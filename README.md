# Heimdall

C++ project: a Biome/Oxlint-style engine with a standalone LSP server for VS Code.
Structure mirrors [Freyr](https://github.com/gilmar-sales/Freyr).

## Dependency policy

- **`heimdall_core` (`core/`, `include/Heimdall/`): standard library only.** No third-party
  code, no exceptions, no RTTI. Enforced by `test/src/CorePolicySpec.cpp`.
- **Outside core, third parties are allowed**: `simdjson` for `compile_commands.json` and LSP JSON-RPC,
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

The development LSP also supports conservative local refactorings: rename and
references for locals/parameters, extraction of complete integral/boolean constant return
expressions to variables, literal-return extraction to internal functions, and
inline of same-type scalar literal locals used directly in returns. These
extract/inline operations require a document without preprocessor directives/macros.
Local rename accepts includes when GCC/Clang preprocessing through
`compile_commands.json` preserves the document's written tokens before and after
the edit. Macro expansion, conditional token removal, dirty included headers,
templates, captures, global rename and general statement extraction remain blocked.
Versioned workspace edits are required. Actions use the specific LSP kinds
`refactor.extract.variable`, `refactor.extract.function` and `refactor.inline.variable`;
parent-kind filters such as `refactor.extract` also work. Floating-point and
user-defined literals are not supported. See
[the implementation status and remaining phases](docs/refactoring-implementation-plan.md#8-estado-da-implementação).

Header completion/indexing reads open buffers through an immutable overlay,
including headers not yet saved to disk. Compiler macro probing still uses disk
headers, so this does not establish full preprocessor fidelity for refactoring.

The engine also provides a snapshot-based semantic project symbol index, with
header/source declaration identities, classified occurrences and explicit coverage
gaps. `Workspace::LoadProjectSources` can import closed compilation units and
transitive includes without overwriting open buffers. This does not enable global
LSP rename yet; see [the index API and supported subset](docs/project-symbol-index.md).

```bash
./build/src/heimdall lint --jobs 8 src include
./build/src/heimdall lint --json src/main.cpp
./build/src/heimdall lint --fix src/main.cpp
./build/src/heimdall lint --fix-unsafe --semantic --compile-commands build/compile_commands.json src
./build/src/heimdall check src include
./build/src/heimdall format src/main.cpp
./build/src/heimdall format --write src include
./build/src/heimdall parse src/main.cpp
./build/src/heimdall parse --json --std c++23 src/main.cpp
./build/src/heimdall lint --semantic --compile-commands build/compile_commands.json src
./build/src/heimdall init
./build/src/heimdall init ./my-project --force
```

See [formatter style options](docs/formatter-style.md) for the Freyr `.clang-format` rule
inventory, Heimdall project settings, and current compatibility status.

`init [directory] [--force]` creates `<directory>/.heimdall.json` (default:
current directory) with `root`, `rules`, `suppressions` and `include-order`
defaults. It refuses to overwrite an existing file unless `--force` (`-f`) is
given and creates missing directories. The JSON Schema for editors lives in
`schemas/heimdall.schema.json`; add `"$schema": "<path-to>/heimdall.schema.json"`
to `.heimdall.json` for completion and validation.

Files are analyzed concurrently with a standard-library `std::jthread` worker
set and an atomic work index; results are emitted in sorted path order. The
formatter normalizes indentation and spacing, separates function/method declarations
and definitions (including constructors), groups consecutive fields and separates
them from methods and access sections,
and puts declared parameters on individual lines when there are more than three.
`check` exits 1 if
lint diagnostics are found; I/O or CLI errors exit 2.

These declaration layout defaults can be configured in `.heimdall.json`:

```json
{
  "format": {
    "blank-line-between-methods": true,
    "max-parameters-per-line": 3
  }
}
```

Set the first option to `false` or the second to `0` to disable it. Parameter
wrapping applies to declarations/definitions, including constructors, not calls. Unary complement stays
attached to its operand, with assignment spacing preserved: `kNone = ~0u;`.

`parse` dumps the grammar tree (node kinds with byte offsets) and reports
syntax errors as `syntax/parse-error` diagnostics; it exits 1 when syntax errors are
found. `lint` and `check` also include `syntax/parse-error` diagnostics from the
same parser. The parser dialect comes from the matching `compile_commands.json`
entry (`-std=`/`/std:`) and its `-D` macros, defaulting to C++20; an explicit
`--std <c++20|c++23|c++26>` overrides the database.

## VS Code extension

The independent extension is in `vscode-extension/` and talks to `heimdall-lsp` over
LSP JSON-RPC stdio. End users don't need to build anything: the extension
downloads the matching prebuilt server from GitHub releases on first use
(`heimdall.autoInstallServer`, `Heimdall: Install Language Server` command).

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
`///`/`/**` documentation popups, and hover information), plus symbol navigation: the
Outline / *Go to Symbol in Editor* of the open file and *Go to Symbol in Workspace*
(`Ctrl+T`) over a background index of the workspace, both syntactic and independent of
`compile_commands.json` (coverage and limits in `vscode-extension/README.md`). Known
completion gaps and their fixes are tracked in `docs/completion-limitations.md`. Set `heimdall.serverPath` if the server is not found in the
workspace build directory or `PATH`. This replaces the lint/format portion of
Microsoft's extension; full IntelliSense (member completion after `.`/`->`),
debugging, and build integration are not implemented yet.

## Release: prebuilt servers + extension

`.github/workflows/release.yml` builds `heimdall` + `heimdall-lsp` in Release
for `win32-x64` (MSVC), `linux-x64` (GCC 16), `darwin-arm64` and `darwin-x64`
(Homebrew LLVM Clang), zips each as `heimdall-<platform>-<arch>.zip`
(plus `LICENSE`, `heimdall.schema.json`, `version.txt`), and — on tags
`v*.*.*` — publishes them with SHA256 checksums and the packaged `.vsix` as a
GitHub Release. The same builds run on pull requests without publishing.
Set the `VSCE_PAT` secret to also publish the extension to the Marketplace.

To cut a release (extension `package.json` `"version"` is the single source
of truth, and the workflow fails the tag otherwise):

```bash
# 1. bump "version" in vscode-extension/package.json and commit
git tag v0.4.0 && git push origin v0.4.0
```

## Layout

```text
CMakeLists.txt          # root: options, ccache, FetchContent, add_subdirectory(core/semantic/src/lsp/test/bench)
cmake/pch.hpp           # STL-only precompiled header (no third-party, no Heimdall headers)
core/                   # heimdall_core: zero-dependency engine (STL only, no exceptions/RTTI)
  include/Heimdall/     # core public headers (Arena, MappedBuffer, SyntaxTree, Formatter, CppStandard, Lexer, ...)
  src/                  # one TU per stage + ParseTree.cpp entry; detail/ holds private bridges
semantic/               # heimdall_semantic: compile DB + local oracle (simdjson PRIVATE)
  include/Heimdall/     # CompileDatabase.hpp, SemanticAnalyzer.hpp (STL + core only)
  src/
analysis/               # shared Workspace, AnalysisSnapshot/Context, features, scheduler and experimental Plugin API
src/                    # heimdall CLI: cli.cpp (main) + CliOptions/FileDiscovery/Init/Pipeline/Reporting
lsp/                    # heimdall-lsp: main.cpp + JsonRpc/Document/Server modules, SymbolProtocol, WorkspaceFiles, WorkspaceSymbolIndex (simdjson PRIVATE)
schemas/                # heimdall.schema.json: JSON Schema for .heimdall.json (used by init docs)
vscode-extension/       # VS Code extension manifest and LSP client
test/                   # GoogleTest suite, incl. core dependency-policy guard (explicit sources)
bench/                  # Google Benchmark suite (explicit sources) + corpus
docs/                   # design notes: linter/formatter architecture, completion limits, mmap I/O
```

See [architectural evolution](docs/architectural-evolution.md) for snapshot lifetime,
incremental invalidation, experimental plugins, benchmarks and remaining roadmap gates.

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
  spaces/tabs; `format/require-final-newline` ensures a final newline;
  `cpp/no-empty-catch` flags empty catch blocks; `cpp/no-duplicate-include`
  flags repeated literal includes outside conditional blocks;
  `cpp/modernize-using` rewrites plain `typedef` declarations as `using`
  aliases; `cpp/no-todo` flags uppercase `TODO`/`FIXME`/`XXX` markers in
  comments (no autofix); `cpp/no-magic-numbers` flags numeric literals other
  than `0`/`1` (any base or suffix spelling) outside comments, literals and
  directives (no autofix); `cpp/sort-includes` (opt-in) flags include blocks out of the
  configured order (`include-order` in `.heimdall.json`) and safely
  reorders each block in batch; `cpp/modernize-emplace` rewrites
  `push_back(T(...))`/`push_back({...})` as `emplace_back(...)` (quick fix);
  `cpp/modernize-make-unique`/`cpp/modernize-make-shared` rewrite
  `unique_ptr<T>(new T(...))` as `make_unique<T>(...)`; `cpp/modernize-smart-ptr`
  rewrites `T* p = new T(...)` as `make_unique` (quick fixes);
  `cpp/no-new-delete` flags remaining direct `new`/`delete` (no fix; placement
  new and `operator new`/`delete` stay out); `cpp/modernize-structured-bindings`
  rewrites `std::tie(a, b) = expr` as `auto [a, b] = expr` and suggests
  bindings for repeated `.first`/`.second` (no fix there);
  `cpp/modernize-algorithms` suggests `accumulate`/`count_if`/`copy_if` for
  matching loops (no fix); `doc/require-comment` and `doc/doxygen-style`
  (both opt-in, `--semantic`) require a Doxygen comment on classes, enums and
  functions, and check it against good practice: a one-sentence `@brief` apart from
  the details, an `@param` per named parameter, `@tparam` per template parameter,
  `@return` for non-void functions, `@throws` when the body throws, no stale or
  repeated tags, Javadoc style (`///`, `/** */`). Both offer editor quick fixes:
  syntax-only repairs also run with `--fix`; missing documentation is filled with
  explicit `TODO` templates and requires `--fix-unsafe`. Obsolete or duplicate
  tags are preserved as `@note` text for review. `"doc": {"scope": "public" |
  "private" | "all"}` picks which declarations they cover (default `public`: public
  and protected members and external linkage; `private`: private members, `static`
  and anonymous-namespace entities). Diagnostics
  carry byte ranges and 1-based line/column, with non-overlapping text edits applied
  from right to left. Rules can be enabled independently.
- **Local semantic rule**: `semantic/no-unused-local` reports unused simple local variables when
  run with `--semantic --compile-commands ...`. It only covers basic declarations
  in function bodies (built-in/current-file types); complex declarators, parameters,
  captures, shadowing and include-provided types are outside the current subset.
  With `--semantic`, the modernize set also runs: `cpp/modernize-span` (pointer +
  size parameters to `std::span`), `cpp/modernize-string-view` (`const std::string`
  by value to `std::string_view`), `cpp/modernize-attributes` (`[[nodiscard]]` for
  query functions) as editor quick fixes, and `cpp/modernize-consteval-constexpr`
  (namespace constants to `constexpr`, safe in `--fix`).
- **Formatter foundation**: brace-depth indentation using lexer tokens, configurable
  spaces/tabs, with scope-aware dedents for access specifiers (`public:`),
  switch labels (`case:`/`default:`) and goto labels, collapsing blank-line
  runs to `max_empty_lines` (default 1, edges trimmed), preserving CRLF,
  comments, literals, and preprocessor directives. It intentionally does
  not yet reflow lines or normalize operator spacing.
- **Compilation database / semantic seed**: `heimdall_semantic` uses `simdjson` directly
  to read `compile_commands.json` (`arguments` or `command`) and
  extracts `-D`, `-U`, and `-I` options. Its local type oracle recognizes built-ins
  and declarations in the current file, so it can distinguish simple `A * b;` forms
  when `A` is known. It does not open include directories or implement full C++ name
  lookup; `--semantic` reports when a per-file compilation context is available.
