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

With the local development server, **Rename Symbol** supports modeled local
variables and parameters. The **Refactor** menu offers restricted integral/boolean
extract/inline actions when their preconditions hold. Extract/inline require
a document without preprocessor directives/macros. Local rename can accept includes
with GCC/Clang and `compile_commands.json` when compiler preprocessing preserves
the written tokens before and after the edit; dirty included headers are blocked.
All operations decline templates, captures, ambiguous/unmodeled uses and global symbols.
They use versioned edits
and reanalysis before returning a plan. This support is not available in older
downloaded server binaries; use a local build to try it.

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

## Symbol navigation

The server answers the standard VS Code symbol features; the extension adds no commands
or shortcuts of its own, so the native ones apply.

| Feature | Default shortcut | Scope |
| --- | --- | --- |
| Outline view, Explorer *Symbols*, breadcrumbs | `Ctrl+Shift+O` for the current file | The open file, as a tree: namespaces → types → members, enumerators under their enum. |
| Go to Symbol in Workspace | `Ctrl+T` | Every C/C++ source and header below the workspace folder. |

Both are **syntactic**: they work with `heimdall.enableSemantic` off and without a
`compile_commands.json`.

What is listed: namespaces (including `inline` and anonymous ones), classes, structs,
unions, enums and their enumerators, functions and methods (each overload separately,
out-of-line definitions as `Type::Member`), constructors, destructors, operators, fields,
variables, `using` aliases, `typedef`s and concepts. Block-local variables and function
parameters are not listed, nor are forward declarations and `friend` declarations.

Workspace symbols come from an index built in the background after start-up (logged as
`symbol index ready`); a search typed before that finishes sees the files indexed so far.

- Files under `build*`, `cmake-build*`, `out`, `node_modules`, `_deps`, `vcpkg_installed` and
  hidden folders are skipped, as are files over 2 MiB and anything past 20 000 files.
- Open editors win over disk: unsaved edits are searchable, and closing an editor shows what
  is on disk again. Files changed outside the editor are re-read through VS Code's file
  watcher.
- Results are case-insensitive, best match first (exact name, prefix, substring, subsequence;
  `Widget::Run` and `ns::` qualified queries work), at most 200 per search.
- Headers outside the workspace folder (system or SDK headers) and generated or
  macro-expanded declarations are not indexed; a construct the parser cannot recover is
  skipped.
- Set `heimdall.workspaceSymbols` to `false` to turn the workspace index off (the Outline
  keeps working).

Measured on a Release build with 40 symbols per file: indexing runs at roughly 1–2 ms per file on
a few helper threads (28-core Windows machine), 20 000 files (the cap) take about 35 s in the background and 90 MB;
a search over them answers in 6–60 ms, and over 1 000 files in under 3 ms.

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
