# Heimdall

C++ project: a Biome/Oxlint-style engine with a standalone LSP server for VS Code.
Structure mirrors [Freyr](https://github.com/gilmar-sales/Freyr).

## Dependency policy

- **`heimdall_core` (`core/`, `include/Heimdall/`): standard library only.** No third-party
  code, no exceptions, no RTTI. Enforced by `test/src/CorePolicySpec.cpp`.
- **Outside core, third parties are allowed**: [Skirnir](https://github.com/gilmar-sales/Skirnir)
  with its `simdjson` dependency for `compile_commands.json` and LSP JSON-RPC,
  GoogleTest `v1.17.0` for tests, and Google Benchmark `v1.9.5` for benches — via `FetchContent`.

## Requirements

- C++26 compiler (GCC 16+)
- CMake 3.29+
- Git (used by `FetchContent`)
- Ninja + ccache (recommended)

## Configure / Build / Test / Bench

```bash
cmake -G Ninja -S . -B build
cmake --build build --config Debug
ctest --test-dir build --build-config Debug --output-on-failure
./build/test/Tests_run
./build/bench/CoreBench
```

## CLI

```bash
./build/src/heimdall lint --jobs 8 src include
./build/src/heimdall lint --json src/main.cpp
./build/src/heimdall lint --fix src/main.cpp
./build/src/heimdall check src include
./build/src/heimdall format src/main.cpp
./build/src/heimdall format --write src include
./build/src/heimdall parse src/main.cpp
./build/src/heimdall parse --json --std c++23 src/main.cpp
./build/src/heimdall lint --semantic --compile-commands build/compile_commands.json src
```

Files are analyzed concurrently with a standard-library `std::jthread` worker
set and an atomic work index; results are emitted in sorted path order. The
current formatter only normalizes brace-depth indentation. `check` exits 1 if
lint diagnostics are found; I/O or CLI errors exit 2.

`parse` dumps the grammar tree (node kinds with byte offsets) and reports
syntax errors as `syntax/parse-error` diagnostics; it exits 1 when syntax errors are
found. `lint` and `check` also include `syntax/parse-error` diagnostics from the
same parser. The parser dialect comes from the matching `compile_commands.json`
entry (`-std=`/`/std:`) and its `-D` macros, defaulting to C++20; an explicit
`--std <c++20|c++23|c++26>` overrides the database.

## VS Code extension

The independent extension is in `vscode-extension/` and talks to `heimdall-lsp` over
LSP JSON-RPC stdio:

```bash
cmake --build build --target heimdall-lsp
cd vscode-extension
npm install
npm run compile
```

Open `vscode-extension/` in VS Code and press F5 to launch the Extension Development
Host. It provides C/C++ diagnostics (rule lints plus parser-based `syntax/parse-error`
syntax errors using the file's `compile_commands.json` dialect and macros),
`NULL` quick fixes, the current
brace-indent formatter, and scope-aware code completion (visible locals and
parameters, plus `ns::`, `Type::` and `::global` qualified lookup over the
current file and its transitively included headers, with signature details,
`///`/`/**` documentation popups, and hover information). Known completion gaps and
their fixes are tracked in `docs/completion-limitations.md`. Set `heimdall.serverPath` if the server is not found in the
workspace build directory or `PATH`. This replaces the lint/format portion of
Microsoft's extension; full IntelliSense (member completion after `.`/`->`),
debugging, and build integration are not implemented yet.

## Layout

```text
CMakeLists.txt          # root: options, ccache, FetchContent, add_subdirectory(core/semantic/src/lsp/test/bench)
cmake/pch.hpp           # STL-only precompiled header (no third-party, no Heimdall headers)
core/                   # heimdall_core: zero-dependency engine (STL only, no exceptions/RTTI)
  include/Heimdall/     # core public headers (Arena, MappedBuffer, SyntaxTree, Formatter, CppStandard, Lexer, ...)
  src/                  # one TU per stage + ParseTree.cpp entry; detail/ holds private bridges
semantic/               # heimdall_semantic: compile DB + local oracle (Skirnir/simdjson PRIVATE)
  include/Heimdall/     # CompileDatabase.hpp, SemanticAnalyzer.hpp (STL + core only)
  src/
src/                    # heimdall CLI: cli.cpp (main) + CliOptions/FileDiscovery/Pipeline/Reporting
lsp/                    # heimdall-lsp: main.cpp + JsonRpc/Document/Server modules (simdjson PRIVATE)
vscode-extension/       # VS Code extension manifest and LSP client
test/                   # GoogleTest suite, incl. core dependency-policy guard (explicit sources)
bench/                  # Google Benchmark suite (explicit sources) + corpus
docs/                   # design notes: linter/formatter architecture, completion limits, mmap I/O
```

Binaries land in `build/src/heimdall` and `build/lsp/heimdall-lsp`
(`build/test/Tests_run`, `build/bench/CoreBench`). The VS Code extension
launches a staged copy from its global storage, so relinking while it runs
works on Windows; only a directly launched `heimdall-lsp` (e.g. via `PATH`
outside the extension) still needs to be closed before relinking.

## Implemented core stages

- **Lexer**: byte-lossless token spans with trivia, comments, identifiers, numbers,
  maximal-munch C++ punctuators and ordinary/raw literals; malformed literals/comments
  recover to EOF.
- **Preprocessor subset**: classifies and retains directive spans, evaluates nested
  `#if` / `#ifdef` / `#ifndef` / `#elif` / `#else` / `#endif`, handles `#define` /
  `#undef` for object-like macros, and expands those macros outside comments/literals.
  Integer comparisons (`==`, `!=`, `<`, `<=`, `>`, `>=`) are supported in `#if`.
  `#include` is deliberately opaque: no headers are opened. Function-like macros,
  token pasting/stringification and full C++ preprocessor expression semantics are
  not implemented yet; they are left unexpanded rather than guessed.
- **CST foundation**: iterative delimiter grouping for `()`, `[]`, `{}` over lossless
  lexer tokens. Green nodes are flat immutable-style spans linked by parent index;
  unmatched/mismatched delimiters produce diagnostics and recovery continues. This
  is structural scaffolding, not yet a C++ declaration/expression grammar.
- **Rule engine**: `cpp/no-null` replaces `NULL` with `nullptr` outside comments,
  literals and directives; `format/no-trailing-whitespace` removes trailing
  spaces/tabs; `format/require-final-newline` ensures a final newline. Diagnostics
  carry byte ranges and 1-based line/column, with non-overlapping text edits applied
  from right to left. Rules can be enabled independently.
- **Local semantic rule**: `semantic/no-unused-local` reports unused simple local variables when
  run with `--semantic --compile-commands ...`. It only covers basic declarations
  in function bodies (built-in/current-file types); complex declarators, parameters,
  captures, shadowing and include-provided types are outside the current subset.
- **Formatter foundation**: brace-depth indentation using lexer tokens, configurable
  spaces/tabs, with scope-aware dedents for access specifiers (`public:`),
  switch labels (`case:`/`default:`) and goto labels, collapsing blank-line
  runs to `max_empty_lines` (default 1, edges trimmed), preserving CRLF,
  comments, literals, and preprocessor directives. It intentionally does
  not yet reflow lines or normalize operator spacing.
- **Compilation database / semantic seed**: `heimdall_semantic` uses Skirnir's
  `JsonFileSource` (backed by `simdjson`) to read `compile_commands.json` (`arguments` or `command`) and
  extracts `-D`, `-U`, and `-I` options. Its local type oracle recognizes built-ins
  and declarations in the current file, so it can distinguish simple `A * b;` forms
  when `A` is known. It does not open include directories or implement full C++ name
  lookup; `--semantic` reports when a per-file compilation context is available.
