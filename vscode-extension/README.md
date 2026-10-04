# Heimdall for VS Code

Independent C/C++ language tooling powered by the Heimdall Language Server. It
provides diagnostics, quick fixes for supported lint rules, brace-depth
formatting, and scope-aware code completion (visible locals/parameters plus
`ns::`, `Type::` and `::global` qualified lookup, including namespaces from
`#include`d headers such as `std::`, with signature details, `///`/`/**`
documentation popups, and hover information). It does not currently provide
debugging, build-system integration, or full C++ navigation (member completion
after `.`/`->` is not yet modeled).

## Language server: automatic install

On activation the extension resolves `heimdall-lsp` in this order:

1. `heimdall.serverPath`, when set to something other than the default.
2. The versioned install in the extension global storage
   (`bin/heimdall-<version>/`), downloaded from the GitHub release matching
   `heimdall.serverVersion` (default: the extension's own version).
3. A local workspace build (`build/lsp/`, `build/`, `build/Debug/`), then
   `heimdall-lsp` on `PATH` (dev loop).
4. Otherwise it downloads the matching release zip
   (`heimdall-<platform>-<arch>.zip`) and installs it — unless
   `heimdall.autoInstallServer` is `false`.

Run the `Heimdall: Install Language Server` command to (re)install on demand.
Before launching, the extension copies the server into its global storage and
runs the copy, so rebuilding while VS Code is open never hits a Windows file
lock — just rebuild and Reload Window to pick the new binary up.

## Dev loop: `npm run package:dev`

```powershell
cd vscode-extension
npm run package:dev
```

This builds `heimdall-lsp` from the repo root (configuring `../build` on first
run) and packages `heimdall-vscode-<version>-dev.vsix`. The dev vsix carries a
`.heimdall-dev` marker so the extension runs in dev mode: it resolves the
server from `serverPath` → local build (`build/lsp/`, `build/`, `build/Debug/`)
→ `PATH` → installed copy, and **never downloads** from GitHub releases.
The same local-first order applies automatically under the F5 Extension
Development Host. Install it with
`code --install-extension heimdall-vscode-<version>-dev.vsix`.

| Setting | Default | Description |
| --- | --- | --- |
| `heimdall.serverPath` | `heimdall-lsp` | Explicit server path. Overrides everything when changed. |
| `heimdall.serverVersion` | `""` (= extension version) | Release version to download, e.g. `"0.3.1"`. Tags are `v<version>`. |
| `heimdall.autoInstallServer` | `true` | Download the server from GitHub releases when none is found. |

## Build the server

From the repository root:

```powershell
cmake -G Ninja -S . -B build
cmake --build build --target heimdall-lsp --config Debug
```

The extension searches `build/` and `build/Debug/` in the opened workspace, then
falls back to `heimdall-lsp` on `PATH`. Override the path with the VS Code setting
`heimdall.serverPath`. Before launching, the extension copies the server into its
global storage and runs the copy, so rebuilding while VS Code is open never hits
a Windows file lock — just rebuild and Reload Window to pick the new binary up.

Optional local semantic diagnostics can be enabled with `heimdall.enableSemantic`.
Set `heimdall.compileCommands` to the database path (default:
`build/compile_commands.json`); the extension passes it to the server during
initialization.

## Build the extension

```powershell
cd vscode-extension
npm install
npm run compile
```

Open this folder in VS Code and press F5 to launch an Extension Development Host.
The server communicates over the standard LSP JSON-RPC stdio transport.
