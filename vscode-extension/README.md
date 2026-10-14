# Heimdall for VS Code

Heimdall helps you spot problems and keep your C++ code consistent while you work.
It adds warnings, quick fixes, formatting, code suggestions, and symbol search to
VS Code, powered by the Heimdall language server.

[Official repository](https://github.com/gilmar-sales/Heimdall) ·
[Report an issue](https://github.com/gilmar-sales/Heimdall/issues) ·
[Releases](https://github.com/gilmar-sales/Heimdall/releases)

**You do not need to build Heimdall to get started.** The extension automatically
installs its matching language server when needed.

## Get started

1. Install **Heimdall - C++ Lint and Format** in VS Code.
2. Open your project with **File → Open Folder**, then open a C++ source or header file.
3. Allow the language server installation to finish. Automatic installation downloads
   the matching release from GitHub, so it needs an internet connection and an
   available release for your platform.
4. Start editing. Warnings appear in the editor and the **Problems** panel.

Heimdall indexes your project in the background. The status bar shows its progress;
workspace symbol search may return only some results until indexing finishes.

If installation does not start or you want to reinstall, open the Command Palette
and run **Heimdall: Install Language Server**.

> Open the Command Palette with `Ctrl+Shift+P` on Windows/Linux or `Cmd+Shift+P`
> on macOS. You can find all the actions below there by name, without memorizing shortcuts.

## Use Heimdall day to day

### Find problems and apply fixes

Open **View → Problems** to see diagnostics, then select one to jump to its location.
Hover over an underline to read the message. When a fix is available, use the
lightbulb or **Quick Fix** action to review and apply it.

- Checks run as you edit. Background workspace diagnostics are also enabled by
  default, so the Problems panel can include files you have not opened.
- Not every warning has an automatic fix.
- Dependency folders such as `_deps`, `third_party`, `external`, and `vendor` are
  excluded from background linting.

Heimdall's checks complement your compiler; a clean Problems panel is **not** a
guarantee that your program will compile or behave correctly.

### Format your code

Run **Format Document** in a C++ file. If VS Code asks which formatter to use,
choose Heimdall. To make that choice permanent, run
**Format Document With… → Configure Default Formatter**.

For automatic formatting when you save, add this to your workspace
`.vscode/settings.json`:

```json
{
    "[cpp]": {
        "editor.defaultFormatter": "gilmar-sales.heimdall-vscode",
        "editor.formatOnSave": true
    }
}
```

Formatting follows your project's `.heimdall.json`. The default brace style is
Allman, with opening braces on their own line. Expect brace-depth indentation and
supported style adjustments, **not a general-purpose line-reflow engine**.

### Get code suggestions and hover details

Use **Trigger Suggest** while typing to see completion candidates. Heimdall supports
visible local variables and parameters, plus qualified names such as `ns::`,
`Type::`, and `::global`. This includes namespaces from included headers, such as
`std::`, when those headers can be resolved.

Hover over supported symbols to see information about them. Completion details can
include signatures and documentation written with `///` or `/** ... */` comments.
Inlay hints show inferred types for `auto` variables and parameter names at calls.

Code excluded by `#if`/`#ifdef` conditional compilation is dimmed in the editor:
the server reports inactive regions (`textDocument/inactiveRegions`, the same
notification clangd sends) and marks the same spans with the `inactive` semantic
token modifier, so the whole region fades instead of only its keywords. Set
`heimdall.dimInactiveCode` to `false` to turn the dimming off.

**Member completion after `.` or `->` is not currently modeled.** Do not expect the
same completion coverage as a full compiler-backed C++ language service.

### Find symbols in your project

| What you want to do | VS Code feature |
| --- | --- |
| Browse the current file's declarations | **Outline** view or breadcrumbs |
| Jump to a declaration listed in the current file | **Go to Symbol in Editor** |
| Search declarations across your project | **Go to Symbol in Workspace** |

These features work without a compile database or semantic diagnostics. They list
declarations such as namespaces, types, functions, methods, fields, and aliases;
block-local variables and function parameters are not listed.

Workspace searches accept names such as `Widget::Run` and `ns::`. Unsaved changes
in open editors are included. System/SDK headers outside the workspace and
macro-expanded declarations are not indexed. Build folders, hidden folders, and
other excluded locations are skipped; indexing is limited to 20,000 files, with
files larger than 2 MiB skipped.

### Rename and refactor: local-build preview

With a current local development server, **Rename Symbol** supports modeled local
variables and parameters. The **Refactor** menu can offer restricted extract/inline
actions for integral and boolean expressions when their preconditions hold.
These features are not available in older downloaded server binaries.

This is deliberately limited support, not project-wide C++ refactoring:

- Global symbols, templates, captures, and ambiguous or unmodeled uses are declined.
- Extract/inline requires a document without preprocessor directives or macros.
- Local rename with includes requires GCC/Clang and `compile_commands.json`, with
  compiler preprocessing that preserves the written tokens before and after the edit.
  Unsaved changes in included headers block the operation.

## Configure your project

### Choose lint and formatting rules

Run **Heimdall: Initialize Config File** from the Command Palette to create a
`.heimdall.json` in your project folder. The command asks before overwriting an
existing file. Edit this file to customize the project's lint and formatting rules.

The command requires the Heimdall CLI, which is included in the release installation.
If it reports that the CLI is missing, install the language server first.

A vertical editor guide follows the configuration's `format.column-limit` (100 by
default). A value of `0` removes the guide. Heimdall respects an explicit
`editor.rulers` setting; you can also disable this behavior with
`heimdall.dynamicRulers`.

### Give Heimdall your compiler settings

Semantic diagnostics are enabled by default. Local checks, including argument-count
checks for functions declared in the same file, work without extra setup.

For project-specific compiler options, include paths, macros, and the C++ language
version, provide a **compilation database**: a `compile_commands.json` file describing
how your source files are compiled.

Heimdall looks for `build/compile_commands.json` by default. If yours is elsewhere,
set its path in `.vscode/settings.json`:

```json
{
    "heimdall.compileCommands": "out/build/compile_commands.json"
}
```

Relative paths are resolved from the first workspace folder. Absolute paths also
work. For CMake projects using Ninja, you can generate the database by enabling
`CMAKE_EXPORT_COMPILE_COMMANDS` in your existing configuration.

### Useful settings

Open **Settings** and search for `heimdall`, or edit `.vscode/settings.json`.

| Setting | Default | What it controls |
| --- | --- | --- |
| `heimdall.enableSemantic` | `true` | Enable semantic diagnostics. |
| `heimdall.workspaceDiagnostics` | `true` | Check project files in the background, not just open files. |
| `heimdall.workspaceSymbols` | `true` | Enable project-wide symbol indexing. Turning it off keeps the current file's Outline available. |
| `heimdall.inlayHints` | `true` | Show inferred types and parameter name hints. |
| `heimdall.dimInactiveCode` | `true` | Dim code excluded by `#if`/`#ifdef` conditional compilation. |
| `heimdall.compileCommands` | `"build/compile_commands.json"` | Locate your compilation database. |
| `heimdall.dynamicRulers` | `true` | Follow the configured column limit with an editor guide. |
| `heimdall.autoInstallServer` | `true` | Download a matching server when none is found. |
| `heimdall.serverPath` | `"heimdall-lsp"` | Use an explicit server executable when changed from the default. |
| `heimdall.serverVersion` | `""` | Choose a release version; empty uses the extension's version. |
| `heimdall.trace.server` | `"off"` | Log language-server traffic (`"messages"` or `"verbose"`) for troubleshooting. |

## What to expect—and what not to expect

Heimdall focuses on linting, formatting, and the supported editing features above.
It does **not** provide a debugger, build-system integration, or full C++ navigation.
Its parser and symbol model may not understand every C++ construct; unsupported
constructs can be skipped, and refactorings can be refused rather than risk an
incorrect edit.

You can keep your existing build and debugging tools. If you also use another C++
language extension, overlapping features may show duplicate diagnostics or
suggestions. Choose your default formatter explicitly and adjust the extensions'
settings to keep only the features you want.

## Troubleshooting

| Problem | What to try |
| --- | --- |
| The server does not start or download | Open **View → Output**, select **Heimdall**, and read the error. Run **Heimdall: Install Language Server**. If automatic installation is disabled, install manually or set `heimdall.serverPath`. |
| No diagnostics appear | Make sure the file is recognized as C/C++ and check the Heimdall Output channel. Confirm the relevant rules are enabled in `.heimdall.json`. |
| Compiler-dependent checks lack project context | Check that `heimdall.compileCommands` points to an existing, up-to-date database. |
| Formatting uses another extension | Run **Format Document With…** and select Heimdall as the default formatter. |
| Workspace symbol results are missing | Wait for indexing to finish and confirm `heimdall.workspaceSymbols` is enabled. Check whether the file is outside the workspace or excluded from indexing. |
| Background checks are too noisy | Customize `.heimdall.json`, or set `heimdall.workspaceDiagnostics` to `false` to limit diagnostics to open files. |
| The column guide is unexpected | Check `format.column-limit`, your language-specific `editor.rulers`, and `heimdall.dynamicRulers`. |
| A rebuilt server is not being used | Set `heimdall.serverPath` to the rebuilt executable's absolute path, then run **Developer: Reload Window**. |

## Advanced: use a local server or develop the extension

Most users can skip this section.

### Use a server you built yourself

Build with a supported C++ toolchain from the repository root. Reuse your existing
CMake build directory and options; for a new Ninja build:

```sh
cmake -G Ninja -S . -B build
cmake --build build --target heimdall-lsp heimdall
```

Set `heimdall.serverPath` to the absolute path of the resulting `heimdall-lsp`
executable (usually `build/lsp/heimdall-lsp`, or `heimdall-lsp.exe` on Windows),
then run **Developer: Reload Window**. Building the `heimdall` CLI also enables
configuration initialization.

An explicit server path takes priority. Otherwise, the regular extension prefers
the matching installed release, then a local workspace build or a server on `PATH`,
and finally downloads a release if automatic installation is enabled.

The extension runs a staged copy of the server, so you can rebuild without locking
the original binary on Windows. Reload the window after rebuilding to use the new copy.

### Develop or package the extension

```sh
cd vscode-extension
npm install
npm run compile
```

Open the extension folder in VS Code and press **F5** to launch an Extension
Development Host.

To build the local server and package a development `.vsix`, run:

```sh
npm run package:dev
code --install-extension heimdall-vscode-<version>-dev.vsix
```

The development package and F5 host prefer a local server and never download one
from GitHub. Their lookup order is explicit `serverPath`, local build, `PATH`, then
an already installed server.
