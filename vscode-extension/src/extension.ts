import * as fs from 'node:fs';
import * as path from 'node:path';
import { execFileSync } from 'node:child_process';
import * as vscode from 'vscode';
import {
    LanguageClient,
    LanguageClientOptions,
    ServerOptions,
    Trace,
} from 'vscode-languageclient/node';
import { stageServerBinary } from './serverStaging';
import {
    findInstalledServer,
    getServerAssetName,
    installServer,
    isDevRuntime,
    releaseDownloadUrl,
} from './serverInstall';

// Source and header files the server indexes for "Go to Symbol in Workspace"; the watcher
// tells it when one changes on disk (saved elsewhere, created, deleted, branch switch).
const SOURCE_FILE_GLOB = '**/*.{c,cc,cpp,cxx,c++,h,hh,hpp,hxx,inl,ipp,tpp,cppm,ixx}';

let client: LanguageClient | undefined;
let output: vscode.OutputChannel;
let sourceWatcher: vscode.FileSystemWatcher | undefined;

function findOnPath(executable: string): string | undefined {
    try {
        // NB: `command -v` is a shell builtin, so POSIX needs an explicit sh.
        const command = process.platform === 'win32' ? 'where' : 'sh';
        const args = process.platform === 'win32'
            ? [executable]
            : ['-c', 'command -v "$0"', executable];
        const stdout = execFileSync(command, args, { encoding: 'utf8', stdio: ['ignore', 'pipe', 'ignore'] });
        const first = stdout.split(/\r?\n/).map(line => line.trim()).find(line => line.length > 0);
        if (first && fs.existsSync(first)) {
            return first;
        }
    } catch {
        // not on PATH
    }
    return undefined;
}

// Locally built server (dev loop): workspace build output, then PATH.
function findLocalServer(extensionPath: string): string | undefined {
    const executable = process.platform === 'win32' ? 'heimdall-lsp.exe' : 'heimdall-lsp';
    const roots = [
        ...(vscode.workspace.workspaceFolders ?? []).map(folder => folder.uri.fsPath),
        path.dirname(extensionPath),
    ];
    for (const root of [...new Set(roots)]) {
        const candidates = [
            path.join(root, 'build', 'lsp', executable),
            path.join(root, 'build', executable),
            path.join(root, 'build', 'Debug', executable),
            path.join(root, 'out', 'build', executable),
        ];
        const found = candidates.find(candidate => fs.existsSync(candidate));
        if (found) {
            return found;
        }
    }
    return findOnPath(executable);
}

function serverVersion(context: vscode.ExtensionContext): string {
    const configured = vscode.workspace.getConfiguration('heimdall').get<string>('serverVersion', '');
    if (configured.trim().length > 0) {
        return configured.trim().replace(/^v/, '');
    }
    const extensionVersion = context.extension.packageJSON?.version;
    return typeof extensionVersion === 'string' ? extensionVersion : '';
}

async function resolveServerPath(context: vscode.ExtensionContext): Promise<string> {
    const configuration = vscode.workspace.getConfiguration('heimdall');
    const configuredPath = configuration.get<string>('serverPath', 'heimdall-lsp');
    if (configuredPath && configuredPath !== 'heimdall-lsp') {
        return configuredPath;
    }

    const version = serverVersion(context);
    const storagePath = context.globalStorageUri.fsPath;
    const dev = isDevRuntime(
        context.extensionPath,
        context.extensionMode === vscode.ExtensionMode.Development,
    );
    if (dev) {
        // Dev loop (F5 host or `npm run package:dev`): local build first,
        // never download. Rebuild + Reload Window picks the new binary up.
        const local = findLocalServer(context.extensionPath);
        if (local) {
            return local;
        }
        if (version) {
            const installed = findInstalledServer(storagePath, version);
            if (installed) {
                return installed;
            }
        }
        throw new Error(
            'Heimdall language server not found. Build it with ' +
            '`cmake --build build --target heimdall-lsp` (or `npm run package:dev`), ' +
            'put heimdall-lsp on PATH, or set heimdall.serverPath.',
        );
    }

    if (version) {
        const installed = findInstalledServer(storagePath, version);
        if (installed) {
            return installed;
        }
    }

    const local = findLocalServer(context.extensionPath);
    if (local) {
        return local;
    }

    // Nothing usable: download the matching release zip (opt-out via
    // heimdall.autoInstallServer).
    if (!configuration.get<boolean>('autoInstallServer', true)) {
        throw new Error(
            'Heimdall language server not found. Run "Heimdall: Install Language Server", ' +
            'build heimdall-lsp, or set heimdall.serverPath.',
        );
    }
    if (!version) {
        throw new Error('Cannot determine Heimdall version to download (extension has no version).');
    }
    const assetName = getServerAssetName();
    if (!assetName) {
        throw new Error(
            `No prebuilt Heimdall server for ${process.platform}-${process.arch}. ` +
            'Build heimdall-lsp and set heimdall.serverPath.',
        );
    }
    await vscode.window.withProgress(
        {
            location: vscode.ProgressLocation.Notification,
            title: `Downloading Heimdall ${version}`,
            cancellable: false,
        },
        async progress => {
            let lastBytes = 0;
            await installServer({
                version,
                globalStoragePath: storagePath,
                log: message => output.appendLine(message),
                onProgress: (downloaded, total) => {
                    const increment = total ? ((downloaded - lastBytes) / total) * 100 : 0;
                    lastBytes = downloaded;
                    progress.report({
                        message: total ? `${(downloaded / 1048576).toFixed(1)} / ${(total / 1048576).toFixed(1)} MB` : `${(downloaded / 1048576).toFixed(1)} MB`,
                        increment: total ? increment : undefined,
                    });
                },
            });
        },
    ).then(
        undefined,
        (error: unknown) => {
            output.appendLine(`Download failed: ${String(error)} (${releaseDownloadUrl(assetName, version)})`);
            throw error;
        },
    );
    const installed = findInstalledServer(storagePath, version);
    if (!installed) {
        throw new Error(`Install finished but the server binary is missing for version ${version}.`);
    }
    return installed;
}

async function startClient(context: vscode.ExtensionContext): Promise<void> {
    const configuration = vscode.workspace.getConfiguration('heimdall');
    let serverPath: string;
    try {
        serverPath = await resolveServerPath(context);
    } catch (error) {
        const message = `Heimdall language server unavailable: ${String(error)}`;
        output.appendLine(message);
        const install = 'Install Language Server';
        const choice = await vscode.window.showErrorMessage(message, install);
        if (choice === install) {
            await vscode.commands.executeCommand('heimdall.installServer');
        }
        return;
    }
    const stagedServerPath = stageServerBinary(
        serverPath,
        path.join(context.globalStorageUri.fsPath, 'bin'),
        (message): void => output.appendLine(message),
    );
    const trace = configuration.get<string>('trace.server', 'off');
    const workspaceRoot = vscode.workspace.workspaceFolders?.[0]?.uri.fsPath;
    const configuredDatabase = configuration.get<string>('compileCommands', 'build/compile_commands.json');
    let compileCommands = path.isAbsolute(configuredDatabase) || !workspaceRoot
        ? configuredDatabase
        : path.resolve(workspaceRoot, configuredDatabase);
    if (!path.isAbsolute(configuredDatabase) && !fs.existsSync(compileCommands)) {
        const parentBuildDatabase = path.resolve(path.dirname(context.extensionPath), configuredDatabase);
        if (fs.existsSync(parentBuildDatabase)) {
            compileCommands = parentBuildDatabase;
        }
    }

    const serverOptions: ServerOptions = {
        // Launch a staged copy under the extension's global storage
        // (%APPDATA%/Code/User/globalStorage on Windows, ~/.config/Code/... on
        // Linux, ~/Library/Application Support/Code/... on macOS) so rebuilding
        // the server never hits a file lock held by the running instance.
        run: { command: stagedServerPath, args: [] },
        debug: { command: stagedServerPath, args: [] },
    };
    sourceWatcher?.dispose();
    sourceWatcher = vscode.workspace.createFileSystemWatcher(SOURCE_FILE_GLOB);
    context.subscriptions.push(sourceWatcher);
    const clientOptions: LanguageClientOptions = {
        documentSelector: [
            { scheme: 'file', language: 'c' },
            { scheme: 'file', language: 'cpp' },
            { scheme: 'file', language: 'cuda' },
            { scheme: 'file', language: 'objective-c' },
            { scheme: 'file', language: 'objective-cpp' },
        ],
        synchronize: { configurationSection: 'heimdall', fileEvents: sourceWatcher },
        initializationOptions: {
            enableSemantic: configuration.get<boolean>('enableSemantic', false),
            workspaceDiagnostics: configuration.get<boolean>('workspaceDiagnostics', true),
            workspaceSymbols: configuration.get<boolean>('workspaceSymbols', true),
            compileCommands,
            workspaceRoot,
        },
        outputChannel: output,
    };

    client = new LanguageClient('heimdall', 'Heimdall', serverOptions, clientOptions);
    client.setTrace(trace === 'verbose' ? Trace.Verbose : trace === 'messages' ? Trace.Messages : Trace.Off);
    context.subscriptions.push(client);

    try {
        await client.start();
        output.appendLine(`Started ${stagedServerPath}`);
    } catch (error) {
        const message = `Could not start heimdall-lsp at '${serverPath}': ${String(error)}`;
        output.appendLine(message);
        void vscode.window.showErrorMessage(message);
    }
}

export async function activate(context: vscode.ExtensionContext): Promise<void> {
    output = vscode.window.createOutputChannel('Heimdall');
    context.subscriptions.push(output);
    await startClient(context);

    context.subscriptions.push(vscode.commands.registerCommand('heimdall.installServer', async () => {
        const version = serverVersion(context);
        if (!version) {
            void vscode.window.showErrorMessage('Cannot determine Heimdall version to install.');
            return;
        }
        try {
            await vscode.window.withProgress(
                {
                    location: vscode.ProgressLocation.Notification,
                    title: `Installing Heimdall ${version}`,
                    cancellable: false,
                },
                async () => {
                    await installServer({
                        version,
                        globalStoragePath: context.globalStorageUri.fsPath,
                        log: message => output.appendLine(message),
                    });
                },
            );
            void vscode.window.showInformationMessage(`Heimdall ${version} installed. Restarting language server...`);
            await restartClient(context);
        } catch (error) {
            const message = `Heimdall install failed: ${String(error)}`;
            output.appendLine(message);
            void vscode.window.showErrorMessage(message);
        }
    }));

    context.subscriptions.push(vscode.workspace.onDidChangeConfiguration(async event => {
        if (event.affectsConfiguration('heimdall.serverPath') ||
            event.affectsConfiguration('heimdall.serverVersion') ||
            event.affectsConfiguration('heimdall.enableSemantic') ||
            event.affectsConfiguration('heimdall.workspaceDiagnostics') ||
            event.affectsConfiguration('heimdall.workspaceSymbols') ||
            event.affectsConfiguration('heimdall.compileCommands')) {
            await restartClient(context);
        }
    }));
}

async function restartClient(context: vscode.ExtensionContext): Promise<void> {
    await client?.stop();
    client = undefined;
    await startClient(context);
}

export async function deactivate(): Promise<void> {
    await client?.stop();
}
