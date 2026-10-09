# AGENTS.md

C++26 linter/formatter/LSP engine for C++. CMake + Ninja + GCC 16+ (or MSVC/Clang).

## Build & Test

```bash
cmake -G Ninja -S . -B build
cmake --build build  --parallel
ctest --test-dir build --output-on-failure
```

- Run a single test binary: `./build/test/Tests_run --gtest_filter='ParseTreeSpec.*'`
- Run a single benchmark: `./build/bench/CoreBench --benchmark_filter='LexerBench.*'`
- LSP smoke tests require Python3 (auto-skipped if not found).
- `compile_commands.json` is auto-generated in `build/` (`CMAKE_EXPORT_COMPILE_COMMANDS=ON`); semantic tests and `--semantic` CLI runs depend on it.

## Architecture

| Target | Dir | Role |
|--------|-----|------|
| `heimdall_core` | `core/` | STL-only engine: lexer, parser, rules, formatter, completion. **No exceptions, no RTTI, no third-party.** |
| `heimdall_semantic` | `semantic/` | Compile DB, type oracle, semantic rules. simdjson PRIVATE. |
| `heimdall_config` | `config/` | `.heimdall.json` rule config. simdjson PRIVATE. |
| `heimdall` (exe) | `src/` | CLI entry point. |
| `heimdall-lsp` (exe) | `lsp/` | LSP JSON-RPC stdio server. simdjson PRIVATE. |
| `Tests_run` | `test/` | GoogleTest suite. |
| `CoreBench` | `bench/` | Google Benchmark suite. |

- `vscode-extension/` is an independent npm/TypeScript project (VS Code extension + LSP client).
- All CMake targets use **explicit source lists** (no GLOB) — new files require editing the relevant `CMakeLists.txt`.
- PCH at `cmake/pch.hpp` is STL-only (no third-party, no Heimdall headers).

## Core dependency policy (enforced)

`heimdall_core` must not link anything outside the C++ standard library. No exceptions, no RTTI — errors use `std::expected` and `ErrorNode`/`Diagnostic`. This is enforced by `test/src/CorePolicySpec.cpp` which fails the build if violated.

## Key conventions

- **simdjson is always PRIVATE** — it must never appear in public headers or propagate through `target_link_libraries(... PUBLIC ...)`.
- **ParseTree uses SoA** (`GrammarNodeSoA`) in hot paths. Use `tree.NodesSoA()` with column accessors (`soa.Kind(i)`, `soa.Parent(i)`, etc.). `Nodes()` (AoS) is a lazy compatibility layer for cold paths/tests only — see `DOD_MIGRATION_GUIDE.md`.
- **No `Nodes()` in hot loops** — it mutates `mutable` members and is not thread-safe.
- **Formatter** only does brace-depth indentation; it does not reflow lines or normalize spacing.
  - Default brace style is **Allman**: `{` starts on its own line (`new Node {` → `new Node\n{`). `};` stays together on the last line.
  - Struct initializers break across lines when they don't fit: `new Node{.a=1,.b=2}` → `new Node\n{ .a = 1, .b = 2 }\n};`
  - Use `--pointer-alignment left|right` and `--reference-alignment left|right` to control `*`/`&` binding.
- **Parser dialect** comes from `compile_commands.json` (`-std=`/`/std:`, `-D` macros), defaulting to C++20. `--std` flag overrides.

## VS Code extension

```bash
cmake --build build --target heimdall-lsp
cd vscode-extension && npm install && npm run compile
```

The extension downloads prebuilt servers from GitHub releases by default. Set `heimdall.serverPath` to use a local build.

## Release

Extension `package.json` `"version"` is the single source of truth. Tag `v*.*.*` must match. See README for the full release checklist.
