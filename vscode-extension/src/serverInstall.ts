import * as fs from 'node:fs';
import * as https from 'node:https';
import * as os from 'node:os';
import * as path from 'node:path';
import AdmZip from 'adm-zip';

// GitHub repository the server zips are published to by
// .github/workflows/release.yml (asset names must stay in sync).
export const HEIMDALL_REPO = 'gilmar-sales/Heimdall';

// Marker stamped into a vsix by `npm run package:dev` (see scripts/package-dev.js).
// A dev runtime never downloads: it only looks at the local build / PATH.
export const DEV_MARKER = '.heimdall-dev';

export function isDevRuntime(extensionPath: string, isDevHost: boolean): boolean {
    if (isDevHost) {
        return true;
    }
    try {
        return fs.statSync(path.join(extensionPath, DEV_MARKER)).isFile();
    } catch {
        return false;
    }
}

// Platforms with published release zips. linux-arm64 / win32-arm64 have no
// CI builds yet; add them here (and to the workflow matrix) when published.
const PUBLISHED_ASSETS: Record<string, Record<string, string>> = {
    win32: { x64: 'heimdall-win32-x64.zip' },
    linux: { x64: 'heimdall-linux-x64.zip' },
    darwin: { x64: 'heimdall-darwin-x64.zip', arm64: 'heimdall-darwin-arm64.zip' },
};

export type InstallProgress = (downloadedBytes: number, totalBytes: number | undefined) => void;

export function getServerAssetName(platform: string = process.platform, arch: string = process.arch): string | undefined {
    return PUBLISHED_ASSETS[platform]?.[arch];
}

export function serverExecutableName(platform: string = process.platform): string {
    return platform === 'win32' ? 'heimdall-lsp.exe' : 'heimdall-lsp';
}

export function releaseDownloadUrl(assetName: string, version: string): string {
    return `https://github.com/${HEIMDALL_REPO}/releases/download/v${version}/${assetName}`;
}

// Versioned install dir so a new extension version never fights a running
// server: <globalStorage>/bin/heimdall-<version>/heimdall-lsp[.exe]
export function installedServerDir(globalStoragePath: string, version: string): string {
    return path.join(globalStoragePath, 'bin', `heimdall-${version}`);
}

export function installedServerPath(globalStoragePath: string, version: string): string {
    return path.join(installedServerDir(globalStoragePath, version), serverExecutableName());
}

export function findInstalledServer(globalStoragePath: string, version: string): string | undefined {
    const candidate = installedServerPath(globalStoragePath, version);
    try {
        if (fs.statSync(candidate).isFile()) {
            return candidate;
        }
    } catch {
        // not installed
    }
    return undefined;
}

// Downloads a release asset following GitHub's redirects (max 5 hops).
export function downloadFile(url: string, destination: string, onProgress: InstallProgress = (): void => {}): Promise<void> {
    return new Promise((resolve, reject) => {
        const get = (currentUrl: string, redirects: number): void => {
            https.get(currentUrl, { headers: { 'User-Agent': 'heimdall-vscode' } }, response => {
                const location = response.headers.location;
                if (response.statusCode !== undefined && response.statusCode >= 300 && response.statusCode < 400 && location) {
                    response.resume();
                    if (redirects <= 0) {
                        reject(new Error(`Too many redirects downloading ${url}`));
                        return;
                    }
                    get(new URL(location, currentUrl).toString(), redirects - 1);
                    return;
                }
                if (response.statusCode !== 200) {
                    response.resume();
                    reject(new Error(`Download failed (${response.statusCode}) for ${currentUrl}`));
                    return;
                }
                const total = response.headers['content-length'] !== undefined
                    ? Number(response.headers['content-length'])
                    : undefined;
                let downloaded = 0;
                response.on('data', (chunk: Buffer) => {
                    downloaded += chunk.length;
                    onProgress(downloaded, Number.isFinite(total) ? total : undefined);
                });
                const file = fs.createWriteStream(destination);
                file.on('error', reject);
                file.on('finish', () => file.close(error => (error ? reject(error) : resolve())));
                response.on('error', reject);
                response.pipe(file);
            }).on('error', reject);
        };
        get(url, 5);
    });
}

// The release zips are flat, but tolerate a single top-level directory.
function locateExecutable(extractDir: string, executable: string): string | undefined {
    const direct = path.join(extractDir, executable);
    if (fs.existsSync(direct)) {
        return direct;
    }
    let entries: fs.Dirent[] = [];
    try {
        entries = fs.readdirSync(extractDir, { withFileTypes: true });
    } catch {
        return undefined;
    }
    for (const entry of entries) {
        if (entry.isDirectory()) {
            const nested = path.join(extractDir, entry.name, executable);
            if (fs.existsSync(nested)) {
                return nested;
            }
        }
    }
    return undefined;
}

function pruneOldVersions(binRoot: string, keepVersion: string): void {
    let entries: fs.Dirent[] = [];
    try {
        entries = fs.readdirSync(binRoot, { withFileTypes: true });
    } catch {
        return;
    }
    for (const entry of entries) {
        if (entry.isDirectory() && entry.name.startsWith('heimdall-') && entry.name !== `heimdall-${keepVersion}`) {
            try {
                fs.rmSync(path.join(binRoot, entry.name), { recursive: true, force: true });
            } catch {
                // best effort: a running server may lock its own copy
            }
        }
    }
}

export interface InstallOptions {
    version: string;
    globalStoragePath: string;
    log?: (message: string) => void;
    onProgress?: InstallProgress;
}

export async function installServer(options: InstallOptions): Promise<string> {
    const { version, globalStoragePath, log = (): void => {}, onProgress = (): void => {} } = options;
    const assetName = getServerAssetName();
    if (!assetName) {
        throw new Error(
            `No prebuilt Heimdall server for ${process.platform}-${process.arch}. ` +
            `Set heimdall.serverPath to a local build (see README).`,
        );
    }
    const executable = serverExecutableName();
    const targetDir = installedServerDir(globalStoragePath, version);
    const targetPath = path.join(targetDir, executable);
    const url = releaseDownloadUrl(assetName, version);

    log(`Downloading Heimdall ${version} (${assetName})...`);
    const tmpZip = path.join(os.tmpdir(), `heimdall-${version}-${Date.now()}.zip`);
    try {
        await downloadFile(url, tmpZip, onProgress);
        log(`Extracting to ${targetDir}...`);
        fs.mkdirSync(targetDir, { recursive: true });
        new AdmZip(tmpZip).extractAllTo(targetDir, true);
        const located = locateExecutable(targetDir, executable);
        if (!located) {
            throw new Error(`Archive ${assetName} does not contain ${executable}`);
        }
        if (located !== targetPath) {
            fs.copyFileSync(located, targetPath);
        }
        if (process.platform !== 'win32') {
            fs.chmodSync(targetPath, 0o755);
        }
        pruneOldVersions(path.join(globalStoragePath, 'bin'), version);
    } finally {
        try {
            fs.rmSync(tmpZip, { force: true });
        } catch {
            // ignore temp cleanup failures
        }
    }
    log(`Heimdall ${version} installed at ${targetPath}`);
    return targetPath;
}
