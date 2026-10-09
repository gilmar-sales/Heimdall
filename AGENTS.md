# AGENTS.md

C++26 linter/formatter/LSP engine for C++. CMake + Ninja + GCC 16+ (or MSVC/Clang).

## Build & Test

- Locate the active build directory by finding `CMakeCache.txt` before building. If none exists or several are plausible, ask which directory to use. Commands below use `build` as an example; substitute the selected directory and use `--config Debug` / `--build-config Debug` for multi-config generators.
- Use **Ninja** for new CMake configurations. Preserve the selected build's compiler and options.

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

## Code standard (adapted from Freyr)

This repository adopts the reusable C++ coding practices from `C:/dev/Freyr`: `AGENTS.md`, `PATTERNS.md`, `CLAUDE.md`, `.clang-format`, `.clang-tidy`, `.heimdall.json`, and representative headers, implementations, and tests. The rules below are self-contained; access to Freyr is not required for future work.

- Apply this standard to new C++ code and code materially changed by a task. Do not perform unrelated bulk renames or formatting, break public APIs, or rewrite existing tests just to migrate style.
- Heimdall's architecture, core dependency policy, explicit CMake source lists, supported toolchains, and parser dialect rules take precedence. Do **not** import Freyr's ECS types, Skirnir/Perfetto dependencies, namespace macros, source globbing, or reflection requirement.
- These are source-code authoring rules, not new requirements for the formatter engine's output. Do not change formatter behavior or defaults merely to enforce this document.
- The VS Code extension remains TypeScript and follows its existing project conventions; C++ naming and formatting rules do not apply to it.

### Naming & file organization

- Types, concepts, enum values, and functions/methods: `PascalCase` (`TokenKind`, `ParseTree`, `GetToken`).
- Private/protected data members: `mCamelCase` (`mSource`, `mRegistry`), not new `m_snake_case` names.
- Parameters, local variables, and public data-only struct fields: `camelCase` (`sourceText`, `tokenIndex`, `changedTick`).
- Named constants: `kPascalCase` (`kBindingStackBytes`); macros: `UPPER_SNAKE_CASE`. Replace unexplained magic numbers with named constants.
- Keep the existing `heimdall` namespace and module layout. New C++ filenames use `PascalCase.hpp` / `PascalCase.cpp`; tests use `<Module>Spec.cpp` in `test/src/`.
- Headers use `#pragma once`, include what they use, and must not expose private dependencies. Put implementation-only helpers in an unnamed namespace in the `.cpp` file.
- Keep includes deduplicated and grouped according to Heimdall's `.heimdall.json`: angle includes before quoted includes, sorted case-insensitively within groups. Freyr's source files are not consistent enough to override this configured rule.

### Formatting

Use Freyr's actual `.clang-format` settings as the baseline. Its `AGENTS.md` says 120 columns, but the effective configuration says **100**; use **100**. Heimdall currently has no root `.clang-format`; the following records the baseline without requiring an external file:

```yaml
BasedOnStyle: Microsoft
AlignAfterOpenBracket: Align
AlignConsecutiveMacros: true
AlignConsecutiveAssignments: true
AlignConsecutiveDeclarations: true
AlignEscapedNewlines: Right
AlignOperands: true
AlignTrailingComments: true
AllowAllArgumentsOnNextLine: false
ColumnLimit: 100
AllowShortFunctionsOnASingleLine: InlineOnly
AllowShortIfStatementsOnASingleLine: Never
AlwaysBreakTemplateDeclarations: Yes
BreakBeforeTernaryOperators: true
BreakConstructorInitializers: AfterColon
ConstructorInitializerIndentWidth: 4
Cpp11BracedListStyle: false
ExperimentalAutoDetectBinPacking: true
IndentCaseLabels: true
IndentPPDirectives: BeforeHash
IndentWidth: 4
Language: Cpp
NamespaceIndentation: All
PointerAlignment: Left
ReflowComments: true
SpaceAfterCStyleCast: true
SpaceAfterLogicalNot: false
SpaceBeforeCpp11BracedList: true
SpaceBeforeParens: ControlStatements
UseTab: Never
PenaltyIndentedWhitespace: 1
```

- Allman braces for namespaces, classes, functions, and multiline control-flow bodies; four spaces, no tabs; indent namespace contents and case labels.
- Bind pointers and references to the type (`Token* token`, `const Token& token`); Heimdall explicitly configures both alignments as left.
- Use a space before control-statement parentheses (`if (...)`), not function-call parentheses (`Parse(...)`), and before braced initialization (`Token {}`).
- Break template declarations onto their own line; wrap long declarations/calls at 100 columns and align continuation arguments. Align adjacent declarations/assignments when appropriate.
- Only short inline functions may stay on one line; do not put short `if` statements on one line. Constructor initializer lists break after the colon with four-space continuation indentation.
- No trailing whitespace; end files with a newline. Preserve existing comments rather than removing them during style migration.

### Comments & modern C++

- Freyr's default is **no new comments unless requested**: prefer clear names and small functions, and do not add narration, TODOs, or commented-out code. Exception: Heimdall's `.heimdall.json` currently requires public API documentation (`doc/require-comment`, `doc/doxygen-style`, public scope); retain/add required Doxygen documentation. Do not silently change or suppress those rules.
- Use C++26 within the supported toolchain's capabilities; do not introduce mandatory static reflection or compiler-specific features merely because Freyr uses them.
- Prefer `using` aliases, `nullptr`, scoped enums, explicit single-argument constructors, and `= default` / `= delete` for special members. Initialize members explicitly and use `override` for existing polymorphic interfaces.
- Use `const` and const references for read-only access, `auto` when the type is clear, and references in range loops when copying would be unnecessary.
- Mark queries/getters and error-bearing results `[[nodiscard]]`; mark genuinely non-throwing operations `noexcept`. Constrain generic APIs with concepts/`requires`; use `if constexpr` and fold expressions for compile-time branching.
- Use RAII and standard-library ownership (`std::unique_ptr`, shared ownership only when necessary). Raw pointers/references are non-owning; make view/span/string-view lifetimes explicit. Never import `skr::Arc` into Heimdall.
- Keep IDs/indexes in their declared types; avoid signed casts for comparisons, narrowing conversions, dangling views, use-after-move, and unchecked lifetime assumptions.
- Keep platform-specific operations behind existing platform abstractions. No new exceptions, RTTI-based type erasure, or potentially throwing error paths in `heimdall_core`; use its existing result/diagnostic mechanisms.

### Data-oriented design & concurrency

- Prefer compact data-only records and separate processing logic; split hot/cold fields by access pattern. Store auxiliary metadata in parallel columns/side tables when that improves locality.
- Use contiguous SoA columns and `std::span`/views for batch processing. Hoist repeated column/view acquisition out of inner loops and iterate linearly; avoid per-element lookup, hashing, pointer chasing, and virtual dispatch in hot paths.
- Reserve/pre-allocate where sizes are known and reuse buffers. Avoid per-token/per-node allocations. Cache repeated work only with explicit invalidation and ownership rules.
- Consider batching/loop fusion when operations traverse the same data. Use swap-and-pop only when order is not part of the contract; use byte-copy fast paths only for trivially copyable types.
- Prefer concepts/static policies or function-pointer tables when they solve a real extension need without RTTI; do not add design patterns or generic frameworks without a concrete use case.
- Parallel work must operate on independent ranges with clear ownership and completion barriers. Do not mutate shared structure while readers/iterators use it; stage changes until a safe synchronization point when necessary.
- Use RAII synchronization, explicit `std::memory_order` with justified ordering, and avoid false sharing for hot shared atomics (Freyr uses `alignas(64)`). Do not add atomics, padding, locks, or branch hints without a demonstrated need; parallel callbacks must not throw.
- Profile first and verify hot-path changes with the relevant `CoreBench` benchmark and comparable build options/inputs; do not claim a speedup without measurement.

### Tests & static-analysis practices

- **Every bug fix must include a focused regression test**: the smallest reproduction that fails without the fix, next to that module's existing specs. Name it after the broken behavior.
- Use GoogleTest and Arrange-Act-Assert, separated by blank lines rather than explanatory comments. Reuse existing fixtures/helpers and keep tests deterministic; do not depend on unspecified iteration order.
- Run the focused tests during iteration and the applicable suite before finishing. New C++ test/implementation files must be added to the relevant explicit CMake source lists.
- Carry over the intent of Freyr's `.clang-tidy`: bugprone/CERT safety, initialization and narrowing checks, modern C++ idioms, unnecessary-copy/allocation checks, portability, and readability. Its exact check list is tool-version-dependent and is not a requirement to add third-party dependencies or unsupported tooling.
- Keep Heimdall's configured lint rules active. Fix warnings introduced by the change; do not disable checks globally or add broad suppressions to make a change pass.

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
