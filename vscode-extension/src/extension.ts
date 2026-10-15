import * as fs from 'node:fs';
import * as path from 'node:path';
import { execFile, execFileSync } from 'node:child_process';
import { promisify } from 'node:util';
import * as vscode from 'vscode';
import {
    LanguageClient,
    LanguageClientOptions,
    ServerOptions,
    Trace,
} from 'vscode-languageclient/node';
import { stageServerBinary } from './serverStaging';
import { preserveCompletionRanking } from './completionRanking';
import { CPP_LANGUAGES, FORMATTER_DEFAULT_COLUMN_LIMIT, equalArrays, findHeimdallConfig, resolveColumnLimit, rulersForColumnLimit } from './columnRulers';
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
let output: vscode.LogOutputChannel;
let sourceWatcher: vscode.FileSystemWatcher | undefined;
let indexStatus: vscode.StatusBarItem | undefined;
// Dimmed ranges of `#if`-excluded code, keyed by document URI string.
const inactiveRanges = new Map<string, vscode.Range[]>();
let inactiveDecoration: vscode.TextEditorDecorationType | undefined;

interface InactiveRegionsParams {
    uri: string;
    regions: { start: { line: number; character: number }; end: { line: number; character: number } }[];
}

function dimInactiveCodeEnabled(): boolean {
    return vscode.workspace.getConfiguration('heimdall').get<boolean>('dimInactiveCode', true);
}

function ensureInactiveDecoration(): vscode.TextEditorDecorationType {
    if (!inactiveDecoration) {
        inactiveDecoration = vscode.window.createTextEditorDecorationType({ opacity: '0.5' });
    }
    return inactiveDecoration;
}

function applyInactiveDecorations(editor?: vscode.TextEditor): void {
    if (!inactiveDecoration) {
        return;
    }
    const enabled = dimInactiveCodeEnabled();
    const editors = editor ? [editor] : vscode.window.visibleTextEditors;
    for (const visible of editors) {
        const ranges = enabled ? (inactiveRanges.get(visible.document.uri.toString()) ?? []) : [];
        visible.setDecorations(inactiveDecoration, ranges);
    }
}

function handleInactiveRegions(params: InactiveRegionsParams): void {
    try {
        const ranges = (params.regions ?? []).map(
            region =>
                new vscode.Range(
                    region.start.line,
                    region.start.character,
                    region.end.line,
                    region.end.character,
                ),
        );
        if (ranges.length === 0) {
            inactiveRanges.delete(params.uri);
        } else {
            inactiveRanges.set(params.uri, ranges);
        }
        if (!dimInactiveCodeEnabled()) {
            return;
        }
        ensureInactiveDecoration();
        for (const visible of vscode.window.visibleTextEditors) {
            if (visible.document.uri.toString() === params.uri) {
                visible.setDecorations(inactiveDecoration!, ranges);
            }
        }
    } catch (error) {
        output.appendLine(`Heimdall could not apply inactive regions: ${String(error)}`);
    }
}
// Rulers this extension wrote, keyed by `${folder}|${language}`, so user-owned
// `editor.rulers` values are never touched and ours can be withdrawn again.
const appliedRulers = new Map<string, number[]>();

interface IndexStatus {
    state: 'indexing' | 'ready';
    done: number;
    total: number;
    elapsedMs: number;
}

function formatElapsed(milliseconds: number): string {
    return milliseconds < 1000 ? `${milliseconds} ms` : `${(milliseconds / 1000).toFixed(1)} s`;
}

function showIndexStatus(status: IndexStatus): void {
    if (!indexStatus) {
        return;
    }
    const elapsed = formatElapsed(status.elapsedMs);
    if (status.state === 'indexing') {
        indexStatus.text = `$(sync~spin) Heimdall: indexing ${status.done}/${status.total} (${elapsed})`;
        indexStatus.tooltip = 'Heimdall is indexing the workspace for Go to Symbol in Workspace.';
    } else {
        indexStatus.text = `$(check) Heimdall: ${status.total} files indexed in ${elapsed}`;
        indexStatus.tooltip = `Go to Symbol in Workspace is ready: ${status.total} files indexed in ${elapsed}.`;
    }
    indexStatus.show();
}

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

// Locally built CLI (dev loop): workspace build output, then PATH. Mirrors
// findLocalServer but for the `heimdall` CLI (`build/src/`, `build/`, ...)
// instead of `heimdall-lsp` (`build/lsp/`, ...).
function findLocalCli(extensionPath: string): string | undefined {
    const executable = cliExecutableName();
    const roots = [
        ...(vscode.workspace.workspaceFolders ?? []).map(folder => folder.uri.fsPath),
        path.dirname(extensionPath),
    ];
    for (const root of [...new Set(roots)]) {
        const candidates = [
            path.join(root, 'build', 'src', executable),
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

function cliExecutableName(platform: string = process.platform): string {
    return platform === 'win32' ? 'heimdall.exe' : 'heimdall';
}

// Resolves the `heimdall` CLI used by `heimdall init`. Order: sibling of an
// explicitly configured server binary (release zips ship both), versioned
// install dir, local build output, PATH. Never triggers a server download.
function findCliBinary(context: vscode.ExtensionContext, serverPath?: string): string | undefined {
    const executable = cliExecutableName();
    if (serverPath) {
        const sibling = path.join(path.dirname(serverPath), executable);
        if (fs.existsSync(sibling)) {
            return sibling;
        }
    }
    const version = serverVersion(context);
    if (version) {
        const installed = findInstalledServer(context.globalStorageUri.fsPath, version);
        if (installed) {
            const sibling = path.join(path.dirname(installed), executable);
            if (fs.existsSync(sibling)) {
                return sibling;
            }
        }
    }
    return findLocalCli(context.extensionPath);
}

async function runInitConfig(context: vscode.ExtensionContext): Promise<void> {
    const folders = vscode.workspace.workspaceFolders ?? [];
    if (folders.length === 0) {
        void vscode.window.showErrorMessage('Heimdall: open a folder first, then run "Heimdall: Initialize Config File".');
        return;
    }
    let folder: vscode.WorkspaceFolder | undefined = folders[0];
    if (folders.length > 1) {
        const picked = await vscode.window.showQuickPick(
            folders.map(item => ({ label: item.name, description: item.uri.fsPath, folder: item })),
            { placeHolder: 'Select the folder to create .heimdall.json in' },
        );
        if (!picked) {
            return;
        }
        folder = picked.folder;
    }
    if (!folder) {
        return;
    }
    const targetDir = folder.uri.fsPath;
    const configPath = path.join(targetDir, '.heimdall.json');

    let hint: string | undefined;
    const configuredPath = vscode.workspace.getConfiguration('heimdall').get<string>('serverPath', 'heimdall-lsp');
    if (configuredPath && configuredPath !== 'heimdall-lsp') {
        try {
            if (fs.statSync(configuredPath).isFile()) {
                hint = configuredPath;
            }
        } catch {
            // Explicit serverPath that is not a direct file (e.g. on PATH):
            // fall through to the install-dir / local-build / PATH lookup.
        }
    }
    const cli = findCliBinary(context, hint);
    if (!cli) {
        const message = 'Heimdall CLI not found. Build it (`cmake --build build --target heimdall`), put `heimdall` on PATH, or install the language server first.';
        output.appendLine(message);
        void vscode.window.showErrorMessage(message);
        return;
    }

    let force = false;
    if (fs.existsSync(configPath)) {
        const overwrite = 'Overwrite';
        const openExisting = 'Open existing file';
        const choice = await vscode.window.showWarningMessage(
            `Heimdall: ${configPath} already exists.`,
            overwrite,
            openExisting,
        );
        if (choice === openExisting) {
            void vscode.window.showTextDocument(vscode.Uri.file(configPath));
            return;
        }
        if (choice !== overwrite) {
            return;
        }
        force = true;
    }

    const args = ['init', targetDir];
    if (force) {
        args.push('--force');
    }
    output.appendLine(`Running ${cli} ${args.join(' ')}`);
    const execFileAsync = promisify(execFile);
    try {
        const { stdout } = await execFileAsync(cli, args);
        const created = stdout.trim() || `created ${configPath}`;
        output.appendLine(created);
        syncRulers();
        const openFile = 'Open File';
        const choice = await vscode.window.showInformationMessage(`Heimdall: ${created}`, openFile);
        if (choice === openFile) {
            void vscode.window.showTextDocument(vscode.Uri.file(configPath));
        }
    } catch (error) {
        const stderr = (error as { stderr?: unknown })?.stderr;
        const message = `Heimdall init failed: ${String(stderr ?? error)}`;
        output.appendLine(message);
        void vscode.window.showErrorMessage(message);
    }
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
    indexStatus?.dispose();
    indexStatus = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Left, 0);
    indexStatus.name = 'Heimdall symbol index';
    context.subscriptions.push(indexStatus);
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
            enableSemantic: configuration.get<boolean>('enableSemantic', true),
            workspaceDiagnostics: configuration.get<boolean>('workspaceDiagnostics', true),
            workspaceSymbols: configuration.get<boolean>('workspaceSymbols', true),
            inlayHints: configuration.get<boolean>('inlayHints', true),
            indexStatus: true,
            compileCommands,
            workspaceRoot,
        },
        outputChannel: output,
        middleware: {
            provideCompletionItem: async (document, position, completionContext, token, next) => {
                const result = await next(document, position, completionContext, token);
                if (!result || token.isCancellationRequested ||
                    !vscode.workspace.getConfiguration('heimdall', document.uri).get<boolean>('completion.serverRanking', true)) {
                    return result;
                }
                const range = document.getWordRangeAtPosition(position);
                const prefix = range ? document.getText(new vscode.Range(range.start, position)) : '';
                const items = preserveCompletionRanking(Array.isArray(result) ? result : result.items, prefix);
                return items ? new vscode.CompletionList(items, true) : result;
            },
        },
    };

    client = new LanguageClient('heimdall', 'Heimdall', serverOptions, clientOptions);
    client.onNotification('heimdall/indexStatus', (status: IndexStatus) => showIndexStatus(status));
    client.onNotification('textDocument/inactiveRegions', (params: InactiveRegionsParams) =>
        handleInactiveRegions(params),
    );
    if (dimInactiveCodeEnabled()) {
        ensureInactiveDecoration();
    }
    client.setTrace(trace === 'verbose' ? Trace.Verbose : trace === 'messages' ? Trace.Messages : Trace.Off);
    context.subscriptions.push(client);

    try {
        // The status-bar message covers the time until the server answers `initialize`; the
        // server then reports its own background indexing as LSP work-done progress.
        await vscode.window.withProgress(
            { location: vscode.ProgressLocation.Window, title: 'Heimdall: starting language server' },
            async () => {
                await client!.start();
            },
        );
        output.appendLine(`Started ${stagedServerPath}`);
    } catch (error) {
        const message = `Could not start heimdall-lsp at '${serverPath}': ${String(error)}`;
        output.appendLine(message);
        void vscode.window.showErrorMessage(message);
    }
}

// Mirrors the workspace .heimdall.json `format.column-limit` as a vertical
// `editor.rulers` guide for C++ languages. Values the user set explicitly are
// never touched; ours are withdrawn when the config goes away or the
// `heimdall.dynamicRulers` setting is disabled.
function syncRulers(): void {
    try {
        syncRulersInner();
    } catch (error) {
        output.appendLine(`Heimdall could not sync the column rulers: ${String(error)}`);
    }
}

function syncRulersInner(): void {
    const folders = vscode.workspace.workspaceFolders ?? [];
    const enabled = vscode.workspace.getConfiguration('heimdall').get<boolean>('dynamicRulers', true);
    const multiRoot = folders.length > 1;
    for (const folder of folders) {
        const configFile = findHeimdallConfig(folder.uri.fsPath, fs);
        const limit = enabled ? resolveColumnLimit(folder.uri.fsPath, fs) : undefined;
        let desired = enabled ? rulersForColumnLimit(limit) : undefined;
        if (enabled && desired === undefined && configFile !== undefined) {
            // The repo uses Heimdall but sets no limit: state the formatter
            // default explicitly instead of relying on the packaged default.
            desired = [FORMATTER_DEFAULT_COLUMN_LIMIT];
        }
        output.appendLine(
            `Heimdall rulers for ${folder.uri.fsPath}: ` +
            (enabled ? `column-limit ${limit === undefined ? '(unset)' : limit}` : '(dynamic rulers off)'),
        );
        const target = multiRoot
            ? vscode.ConfigurationTarget.WorkspaceFolder
            : vscode.ConfigurationTarget.Workspace;
        for (const language of CPP_LANGUAGES) {
            const key = `${folder.uri.fsPath}|${language}`;
            const config = vscode.workspace.getConfiguration('editor', { uri: folder.uri, languageId: language });
            const inspected = config.inspect<number[]>('rulers');
            const explicit = inspected?.workspaceValue ?? inspected?.workspaceFolderValue ?? inspected?.globalValue;
            const owned = appliedRulers.get(key);
            if (explicit !== undefined && (owned === undefined || !equalArrays(explicit, owned))) {
                // The user (or another extension) owns this value; leave it alone.
                output.appendLine(`Heimdall rulers: leaving user-configured rulers for ${language} untouched.`);
                appliedRulers.delete(key);
                continue;
            }
            if (desired === undefined) {
                if (owned !== undefined && explicit !== undefined) {
                    void config.update('rulers', undefined, target, true).then(
                        () => {
                            appliedRulers.delete(key);
                            output.appendLine(`Heimdall rulers: cleared ruler for ${language}.`);
                        },
                        (error: unknown) => output.appendLine(`Heimdall could not clear the ruler: ${String(error)}`),
                    );
                } else {
                    appliedRulers.delete(key);
                }
                continue;
            }
            if (explicit !== undefined && equalArrays(explicit, desired)) {
                appliedRulers.set(key, [...desired]);
                continue;
            }
            void config.update('rulers', [...desired], target, true).then(
                () => {
                    appliedRulers.set(key, [...desired]);
                    output.appendLine(`Heimdall rulers: set [${desired.join(', ')}] for ${language}.`);
                },
                (error: unknown) => output.appendLine(`Heimdall could not set the ruler: ${String(error)}`),
            );
        }
    }
}

export async function activate(context: vscode.ExtensionContext): Promise<void> {
    output = vscode.window.createOutputChannel('Heimdall', { log: true });
    context.subscriptions.push(output);
    ensureInactiveDecoration();
    context.subscriptions.push(inactiveDecoration!);
    await startClient(context);
    syncRulers();
    applyInactiveDecorations();
    const configWatcher = vscode.workspace.createFileSystemWatcher('**/.heimdall.json');
    context.subscriptions.push(
        configWatcher,
        configWatcher.onDidChange(() => syncRulers()),
        configWatcher.onDidCreate(() => syncRulers()),
        configWatcher.onDidDelete(() => syncRulers()),
        vscode.window.onDidChangeVisibleTextEditors(() => applyInactiveDecorations()),
        vscode.workspace.onDidCloseTextDocument(document => {
            inactiveRanges.delete(document.uri.toString());
        }),
    );

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

    context.subscriptions.push(vscode.commands.registerCommand('heimdall.initConfig', async () => {
        await runInitConfig(context);
    }));

    context.subscriptions.push(vscode.workspace.onDidChangeConfiguration(async event => {
        if (event.affectsConfiguration('heimdall.dimInactiveCode')) {
            if (dimInactiveCodeEnabled()) {
                ensureInactiveDecoration();
            }
            applyInactiveDecorations();
            return;
        }
        if (event.affectsConfiguration('heimdall.dynamicRulers')) {
            syncRulers();
            return;
        }
        if (event.affectsConfiguration('heimdall.serverPath') ||
            event.affectsConfiguration('heimdall.serverVersion') ||
            event.affectsConfiguration('heimdall.enableSemantic') ||
            event.affectsConfiguration('heimdall.workspaceDiagnostics') ||
            event.affectsConfiguration('heimdall.workspaceSymbols') ||
            event.affectsConfiguration('heimdall.inlayHints') ||
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
