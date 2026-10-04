import * as crypto from 'node:crypto';
import * as fs from 'node:fs';
import * as path from 'node:path';

export type StageLog = (message: string) => void;

interface StageReceipt {
    source: string;
    size: number;
    mtimeMs: number;
}

function hashSource(source: string): string {
    return crypto.createHash('sha256').update(source).digest('hex').slice(0, 16);
}

// Copies the language server to a private staging directory and returns the
// staged path to launch. On Windows the build output stays locked while a
// server is running, which breaks relinking; launching a copy keeps the build
// output writable (reads of a running .exe are allowed, writes are not).
// The copy is skipped while a receipt shows the staged binary is current.
// Any failure falls back to launching `source` directly.
export function stageServerBinary(source: string, stagingDir: string, log: StageLog = (): void => {}): string {
    let stat: fs.Stats;
    try {
        stat = fs.statSync(source);
    } catch {
        return source; // not a direct file (e.g. resolved via PATH): launch as-is
    }
    if (!stat.isFile()) {
        return source;
    }

    const base = `heimdall-lsp-${hashSource(source)}${process.platform === 'win32' ? '.exe' : ''}`;
    const staged = path.join(stagingDir, base);
    if (staged === source) {
        return source;
    }

    try {
        const receipt: StageReceipt = JSON.parse(fs.readFileSync(`${staged}.json`, 'utf8'));
        if (
            receipt.source === source &&
            receipt.size === stat.size &&
            receipt.mtimeMs === stat.mtimeMs &&
            fs.existsSync(staged)
        ) {
            return staged;
        }
    } catch {
        // Missing or corrupt receipt: copy below.
    }

    try {
        fs.mkdirSync(stagingDir, { recursive: true });
        fs.copyFileSync(source, staged);
        if (process.platform !== 'win32') {
            fs.chmodSync(staged, stat.mode | 0o111);
        }
        const receipt: StageReceipt = { source, size: stat.size, mtimeMs: stat.mtimeMs };
        fs.writeFileSync(`${staged}.json`, JSON.stringify(receipt));
        log(`Staged language server copy at ${staged}`);
    } catch (error) {
        log(`Could not stage language server copy, launching ${source} directly: ${String(error)}`);
        return source;
    }
    return staged;
}
