const path = require('node:path');
const esbuild = require('esbuild');

esbuild.buildSync({
    absWorkingDir: path.resolve(__dirname, '..'),
    entryPoints: ['src/extension.ts'],
    bundle: true,
    platform: 'node',
    format: 'cjs',
    target: 'node20',
    external: ['vscode'],
    outfile: 'out/extension.js',
    sourcemap: true,
});
