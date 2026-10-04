# Heimdall for VS Code

Independent C/C++ language tooling powered by the Heimdall Language Server. It
provides diagnostics, quick fixes for supported lint rules, brace-depth
formatting, and scope-aware code completion (visible locals/parameters plus
`ns::`, `Type::` and `::global` qualified lookup, including namespaces from
`#include`d headers such as `std::`, with signature details, `///`/`/**`
documentation popups, and hover information). It does not currently provide
debugging, build-system integration, or full C++ navigation (member completion
after `.`/`->` is not yet modeled).

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
