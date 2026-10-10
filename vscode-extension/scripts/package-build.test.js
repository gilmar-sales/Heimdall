const assert = require('node:assert/strict');
const { spawnSync } = require('node:child_process');
const path = require('node:path');
const test = require('node:test');
const esbuild = require('esbuild');

test('extension compiles with the language client node export', () => {
    const extensionDir = path.resolve(__dirname, '..');
    const compiler = require.resolve('typescript/bin/tsc');

    const result = spawnSync(process.execPath, [compiler, '-p', '.', '--noEmit'], {
        cwd: extensionDir,
        encoding: 'utf8',
    });

    assert.ifError(result.error);
    assert.equal(result.status, 0, result.stdout + result.stderr);
});

test('package bundle includes runtime dependencies except the VS Code API', () => {
    const extensionDir = path.resolve(__dirname, '..');

    const result = esbuild.buildSync({
        absWorkingDir: extensionDir,
        entryPoints: ['src/extension.ts'],
        bundle: true,
        platform: 'node',
        format: 'cjs',
        target: 'node20',
        external: ['vscode'],
        write: false,
        metafile: true,
    });

    const inputs = Object.keys(result.metafile.inputs);
    assert.ok(inputs.some(input => input.includes('vscode-languageclient/')));
    assert.ok(inputs.some(input => input.includes('adm-zip/')));

    const builtins = new Set(require('node:module').builtinModules);
    for (const output of Object.values(result.metafile.outputs)) {
        for (const imported of output.imports) {
            assert.ok(
                imported.path === 'vscode' || builtins.has(imported.path) ||
                    builtins.has(imported.path.replace(/^node:/, '')),
                `Runtime dependency missing from bundle: ${imported.path}`,
            );
        }
    }
});
