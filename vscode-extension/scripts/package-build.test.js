const assert = require('node:assert/strict');
const { spawnSync } = require('node:child_process');
const path = require('node:path');
const test = require('node:test');
const esbuild = require('esbuild');
const fs = require('node:fs');
const os = require('node:os');

function loadColumnRulers() {
    const extensionDir = path.resolve(__dirname, '..');
    const file = path.join(extensionDir, 'src', 'columnRulers.ts');
    const source = fs.readFileSync(file, 'utf8');
    const ts = require('typescript');
    const { outputText } = ts.transpileModule(source, {
        compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2022 },
    });
    const module = new (require('node:module').Module)(file, null);
    module._compile(outputText, file);
    return module.exports;
}

test('dynamic ruler follows the repository .heimdall.json column limit', () => {
    const extensionDir = path.resolve(__dirname, '..');
    const manifest = JSON.parse(fs.readFileSync(path.join(extensionDir, 'package.json'), 'utf8'));

    assert.equal(manifest.contributes.configuration.properties['heimdall.dynamicRulers'].default, true);

    const rulers = loadColumnRulers();
    assert.deepEqual([...rulers.CPP_LANGUAGES].sort(), ['c', 'cpp', 'cuda', 'objective-c', 'objective-cpp']);
    assert.equal(rulers.FORMATTER_DEFAULT_COLUMN_LIMIT, 100);

    assert.equal(rulers.parseColumnLimit('{"format":{"column-limit":120}}'), 120);
    assert.equal(rulers.parseColumnLimit('{"format":{"column-limit":0}}'), 0);
    assert.equal(rulers.parseColumnLimit('{"format":{}}'), undefined);
    assert.equal(rulers.parseColumnLimit('{}'), undefined);
    assert.equal(rulers.parseColumnLimit('{"format":{"column-limit":-1}}'), undefined);
    assert.equal(rulers.parseColumnLimit('{"format":{"column-limit":"120"}}'), undefined);
    assert.equal(rulers.parseColumnLimit('{"format":{"column-limit":99.9}}'), 99);
    assert.equal(rulers.parseColumnLimit('not json'), undefined);

    assert.deepEqual(rulers.rulersForColumnLimit(undefined), undefined);
    assert.deepEqual(rulers.rulersForColumnLimit(0), []);
    assert.deepEqual(rulers.rulersForColumnLimit(120), [120]);

    assert.equal(rulers.equalArrays([100], [100]), true);
    assert.equal(rulers.equalArrays([100], [120]), false);
    assert.equal(rulers.equalArrays(undefined, undefined), true);
    assert.equal(rulers.equalArrays([], undefined), false);

    const root = fs.mkdtempSync(path.join(os.tmpdir(), 'heimdall-rulers-'));
    try {
        const project = path.join(root, 'project');
        const nested = path.join(project, 'src');
        fs.mkdirSync(nested, { recursive: true });
        assert.equal(rulers.resolveColumnLimit(nested, fs), undefined);

        fs.writeFileSync(path.join(project, '.heimdall.json'), '{"format":{"column-limit":120}}');
        assert.equal(rulers.resolveColumnLimit(nested, fs), 120);

        fs.writeFileSync(path.join(nested, '.heimdall.json'), '{"format":{"column-limit":80}}');
        assert.equal(rulers.resolveColumnLimit(nested, fs), 80);

        fs.writeFileSync(path.join(nested, '.heimdall.json'), '{"root":true}');
        // A root file blocks inheritance, so the project limit no longer applies.
        assert.equal(rulers.resolveColumnLimit(nested, fs), undefined);

        fs.writeFileSync(path.join(nested, '.heimdall.json'), '{"root":true,"format":{"column-limit":80}}');
        assert.equal(rulers.resolveColumnLimit(nested, fs), 80);

        fs.writeFileSync(path.join(nested, '.heimdall.json'), 'broken');
        assert.equal(rulers.resolveColumnLimit(nested, fs), 120);

        fs.rmSync(path.join(nested, '.heimdall.json'));
        assert.equal(rulers.findHeimdallConfig(nested, fs), path.join(project, '.heimdall.json'));
        fs.rmSync(path.join(project, '.heimdall.json'));
        assert.equal(rulers.findHeimdallConfig(nested, fs), undefined);
        assert.equal(rulers.resolveColumnLimit(nested, fs), undefined);
    } finally {
        fs.rmSync(root, { recursive: true, force: true });
    }
});

test('column-limit ruler defaults to the formatter width for C++ languages', () => {
    const extensionDir = path.resolve(__dirname, '..');
    const manifest = JSON.parse(fs.readFileSync(path.join(extensionDir, 'package.json'), 'utf8'));
    const rulers = loadColumnRulers();

    for (const language of ['[c]', '[cpp]', '[cuda]', '[objective-c]', '[objective-cpp]']) {
        assert.deepEqual(manifest.configurationDefaults?.[language]?.['editor.rulers'], [rulers.FORMATTER_DEFAULT_COLUMN_LIMIT]);
    }
});

test('semantic diagnostics are enabled by default in settings and client initialization', () => {
    const extensionDir = path.resolve(__dirname, '..');
    const manifest = JSON.parse(fs.readFileSync(path.join(extensionDir, 'package.json'), 'utf8'));
    const source = fs.readFileSync(path.join(extensionDir, 'src/extension.ts'), 'utf8');

    assert.equal(manifest.contributes.configuration.properties['heimdall.enableSemantic'].default, true);
    assert.match(source, /enableSemantic:\s*configuration\.get<boolean>\('enableSemantic', true\)/);
});

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
