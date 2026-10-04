// Dev packager: `npm run package:dev` (run from vscode-extension/).
//
// 1. Builds heimdall-lsp from the repo root (configures ../build on first run).
// 2. Stamps the .heimdall-dev marker so the packaged extension runs in dev
//    mode: it resolves the server from the local build / PATH and never
//    downloads from GitHub releases.
// 3. Runs `vsce package` to heimdall-vscode-<version>-dev.vsix.
// 4. Removes the marker again (it only lives inside the dev vsix).
//
// Needs cmake + a C++26 toolchain on PATH. Keeps node_modules untouched.

const { execFileSync } = require('node:child_process');
const fs = require('node:fs');
const path = require('node:path');

const extensionDir = path.resolve(__dirname, '..');
const repoRoot = path.resolve(extensionDir, '..');
const buildDir = path.join(repoRoot, 'build');
const marker = path.join(extensionDir, '.heimdall-dev');
const version = require(path.join(extensionDir, 'package.json')).version;
const outFile = `heimdall-vscode-${version}-dev.vsix`;

function run(cmd, args, cwd) {
    execFileSync(cmd, args, { cwd, stdio: 'inherit' });
}

function serverBinaryExists() {
    const exe = process.platform === 'win32' ? 'heimdall-lsp.exe' : 'heimdall-lsp';
    return [
        path.join(buildDir, 'lsp', exe),
        path.join(buildDir, 'lsp', 'Debug', exe),
        path.join(buildDir, 'lsp', 'Release', exe),
    ].some(candidate => {
        try {
            return fs.statSync(candidate).isFile();
        } catch {
            return false;
        }
    });
}

function buildServer() {
    if (!fs.existsSync(path.join(repoRoot, 'CMakeLists.txt'))) {
        console.log('No CMake project at repo root; skipping server build.');
        return;
    }
    try {
        if (!fs.existsSync(path.join(buildDir, 'CMakeCache.txt'))) {
            console.log('Configuring ../build ...');
            run('cmake', [
                '-S', repoRoot,
                '-B', buildDir,
                '-DCMAKE_BUILD_TYPE=Debug',
                '-DHEIMDALL_BUILD_TESTS=OFF',
                '-DHEIMDALL_BUILD_BENCHMARKS=OFF',
            ], extensionDir);
        }
        console.log('Building heimdall-lsp ...');
        run('cmake', ['--build', buildDir, '--target', 'heimdall-lsp'], extensionDir);
    } catch (error) {
        console.warn(`Server build failed: ${String(error)}`);
    }
    if (!serverBinaryExists()) {
        throw new Error(
            'heimdall-lsp is missing from ../build and the build failed. ' +
            'Install cmake + a C++26 toolchain (GCC 16+) and retry.',
        );
    }
}

function main() {
    buildServer();
    const vsceBin = path.join(extensionDir, 'node_modules', '@vscode', 'vsce', 'vsce');
    if (!fs.existsSync(vsceBin)) {
        throw new Error('vsce not found. Run `npm install` in vscode-extension first.');
    }
    fs.writeFileSync(marker, 'dev\n');
    try {
        // Local vsce via node (no shell/npx lookup, works on win32 + POSIX).
        run(process.execPath, [vsceBin, 'package', '--allow-missing-repository', '--out', outFile], extensionDir);
        console.log(`Dev package ready: ${path.join(extensionDir, outFile)}`);
    } finally {
        try {
            fs.rmSync(marker, { force: true });
        } catch {
            // marker cleanup is best effort (it is gitignored anyway)
        }
    }
}

main();
