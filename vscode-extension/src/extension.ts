import * as fs from 'node:fs';
import * as path from 'node:path';
import * as vscode from 'vscode';
import {
    LanguageClient,
    LanguageClientOptions,
    ServerOptions,
    Trace,
} from 'vscode-languageclient/node';
import { stageServerBinary } from './serverStaging';

let client: LanguageClient | undefined;
let output: vscode.OutputChannel;

function findServer(configuredPath: string, extensionPath: string): string {
    if (configuredPath && configuredPath !== 'heimdall-lsp') {
        return configuredPath;
    }

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
    return configuredPath || executable;
}

async function startClient(context: vscode.ExtensionContext): Promise<void> {
    const configuration = vscode.workspace.getConfiguration('heimdall');
    const serverPath = findServer(configuration.get<string>('serverPath', 'heimdall-lsp'), context.extensionPath);
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
    const clientOptions: LanguageClientOptions = {
        documentSelector: [
            { scheme: 'file', language: 'c' },
            { scheme: 'file', language: 'cpp' },
            { scheme: 'file', language: 'cuda' },
            { scheme: 'file', language: 'objective-c' },
            { scheme: 'file', language: 'objective-cpp' },
        ],
        synchronize: { configurationSection: 'heimdall' },
        initializationOptions: {
            enableSemantic: configuration.get<boolean>('enableSemantic', false),
            compileCommands,
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

    context.subscriptions.push(vscode.workspace.onDidChangeConfiguration(async event => {
        if (event.affectsConfiguration('heimdall.serverPath') ||
            event.affectsConfiguration('heimdall.enableSemantic') ||
            event.affectsConfiguration('heimdall.compileCommands')) {
            await client?.stop();
            client = undefined;
            await startClient(context);
        }
    }));
}

export async function deactivate(): Promise<void> {
    await client?.stop();
}
