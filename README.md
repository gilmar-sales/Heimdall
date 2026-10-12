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
./build/bench/MacroBench                       # macro expansion, environment, resolver cost
python3 bench/LspMacroLatency.py build/lsp/heimdall-lsp   # highlight latency through the LSP
python3 bench/LspComparison.py --heimdall build/lsp/heimdall-lsp --clangd clangd \
  --lines 1000 5000 --iterations 20 --repeats 3 --output /tmp/opencode/lsp-comparison.json
```

### Heimdall vs clangd

`bench/LspComparison.py` runs the servers sequentially against equivalent fresh workspaces,
including a header under `build/_deps/*-src`, using the same C++20 compile commands.
Each size generates that many one-line functions. Server order alternates between repeats.
It reports process startup/initialize, first open-to-completion, and warm median/p95 latency
for completion, hover, document symbols, semantic tokens and incremental edit-to-completion.
The JSON output retains individual samples, response counts/bytes, executable hashes and settings.

Both servers retain background indexing; Heimdall enables semantic analysis and workspace diagnostics, while
clangd runs without clang-tidy. These are real LSP workloads, not equivalent analysis engines:
token counts, diagnostic coverage and completion quality can differ. Empty/null responses are
reported in the counts, not assumed to be equivalent results. Cold means a fresh process and
workspace, **not** cleared OS filesystem caches. Warm-up requests precede each measured operation;
background index completion is not a measurement barrier. Use comparable optimized builds and
avoid other CPU-heavy work when interpreting results. Python transport/JSON overhead is included.

Linux RSS/high-water memory is sampled every 10 ms; memory is reported as unavailable on other
platforms. `--memory-limit-mib` (default 1024) kills a server if sampled RSS exceeds the limit;
this is a safety monitor, not a hard allocation cap. `--timeout` bounds response waits (default
30 seconds). The comparison benchmark is manual-only: it is not registered with CTest or CI.
Harness tests can also be run manually with `python3 test/LspComparisonSpec.py`.

#### Functional differences and implementation priorities

The comparison also runs seven explicit functional probes in a separate small workspace.
Use `--quality-only` to skip performance measurements:

```bash
python3 bench/LspComparison.py --quality-only --output /tmp/opencode/lsp-quality.json
```

The JSON retains advertised capabilities, server versions, normalized full responses,
expectations and per-case `pass`, `mismatch`, `unsupported` or `error` outcomes.
Expectations are independent of clangd: a nonempty response or advertised capability is
not automatically a pass. Rename is inspected without applying edits.

Local functional run, 2026-10-12, using the existing Release executable and clangd 22.1.8:

| Probe / explicit expectation | Heimdall | clangd |
|---|---|---|
| Definition of dependency member points to `Widget.hpp` | Empty on first run; pass on validation rerun | Pass |
| Definition of `Select(1)` selects only the `int` overload | Returns both overloads | Pass |
| Outer local references exclude the shadowing inner local | Pass | Pass |
| Outer local rename edits only declaration and outer use | Pass | Pass |
| Hover on `Box<Widget>::payload` includes substituted `Widget` type | Reports `T` | Pass |
| Completion after `object.` includes `Value` | Pass | Pass |
| Signature help exposes both `Select` overloads | Pass | Pass |

**Honest assessment:** Heimdall already implements a substantial LSP surface:
completion, hover, navigation, references/rename, signatures, symbols, semantic tokens,
inlay hints, hierarchy, formatting and code actions. These handlers are not proof of
compiler-level semantic parity. The probes demonstrate useful local scope handling and
member completion, but clangd is more consistent in the observed cross-header navigation,
and better at overload selection and template type substitution. The latency baseline below is an advantage on
some narrow workloads, not evidence that Heimdall replaces clangd for arbitrary C++.
Heimdall's configurable lint/format/refactoring integration is a distinct project focus;
this comparison does not measure its superiority (clang-tidy is disabled).

Priorities for future implementation, with concrete acceptance criteria:

1. **Dependency member navigation/readiness:** make `dependency_definition` reliably pass
   from a fresh process; distinguish unfinished indexing from missing bindings, then cover
   unopened headers and separate translation units. This probe varied between local runs.
2. **Overload resolution:** select by argument types rather than returning same-name candidates;
   make `overload_definition` pass, then add conversions, qualifiers, defaults and ambiguity cases.
3. **Template substitution:** propagate instantiated member types into hover and navigation;
   make `template_member_hover` pass, then cover aliases, nested templates and dependent bases.
4. **Expand evidence before claiming parity:** add diagnostics with expected ranges/severity,
   cross-TU references/rename, macros and conditional compilation, completion ranking/include
   insertion, edit invalidation, UTF-16 positions and cancellation. Also test real projects and
   indexing readiness, not just generated functions.

These seven probes are deliberately small, run once per server, and are not a comprehensive
correctness score. Completion checks item presence, not ranking or edit validity; signature
help checks overload count, not active-signature correctness; hover checks type text.
Background indexing has no readiness barrier, so missing navigation may include readiness
effects. No diagnostic, multi-TU, standard-library or refactoring parity claim is made.
Preserve the JSON when updating this table and add a regression case before addressing each gap.

#### Real-project check: Freyr and C++26 reflection — 2026-10-12

`--project` uses an existing compilation database without rewriting its flags or source files.
Cursor lines are one-based; columns are zero-based UTF-16. Edits are full-document LSP
notifications appending a comment in memory only. Servers may write background-index caches.
Project probes retain responses as `nonempty`/`empty`, **not** correctness passes, plus
diagnostic publications, stderr, source/database hashes and a lexical reflection inventory.
Missing diagnostic publications are explicitly `not_observed`, not assumed clean.

Executed against `/home/gilmar/dev/Freyr`, using its `build/compile_commands.json`
(119 entries), existing Heimdall Release executable and clangd 22.1.8. Each run used one
warm-up, three measured requests, one process per server, 60-second request timeout and
4,096 MiB sampled-RSS limit. These are exploratory runs, not a stable performance baseline:

```bash
python3 bench/LspComparison.py --project /home/gilmar/dev/Freyr \
  --source src/Core/Registry.cpp --line 39 --character 47 \
  --warmup 1 --iterations 3 --repeats 1 --timeout 60 --memory-limit-mib 4096 \
  --output /tmp/opencode/freyr-registry-lsp.json
python3 bench/LspComparison.py --project /home/gilmar/dev/Freyr \
  --source include/Freyr/Meta/CallableComponents.hpp --line 80 --character 56 \
  --warmup 1 --iterations 3 --repeats 1 --timeout 60 --memory-limit-mib 4096 \
  --output /tmp/opencode/freyr-reflection-lsp.json
python3 bench/LspComparison.py --project /home/gilmar/dev/Freyr \
  --source include/Freyr/Meta/CallableComponents.hpp --line 131 --character 47 \
  --warmup 1 --iterations 3 --repeats 1 --timeout 60 --memory-limit-mib 4096 \
  --output /tmp/opencode/freyr-splice-lsp.json
```

| Real-source probe | Heimdall observation | clangd observation |
|---|---|---|
| `Registry.cpp`: `IsRunning` member call | Nonempty definition; empty hover/references | Empty definition/hover/references |
| `CallableComponents.hpp`: `^^Entity` | Hover resolves `using Entity = std::uint32_t`; definition/references empty | All three empty |
| Same header: `ComponentsTupleInfo` inside `[: ... :]` | Definition points to the function at line 61; hover/references empty | Incorrect definition points to namespace `meta` at line 14; hover also describes that namespace |

The reflection header contains eight `^^` operators, two splice openers and 35
`std::meta::` mentions (lexical counts only). clangd published errors for unsupported
`-freflection`, `-fmodules-ts`, module-mapper and dependency-format flags, missing
`std::meta` members, and reflection syntax. Its compilation context is therefore degraded;
this run does **not** establish how clangd performs on a compatible Freyr configuration.
Do not silently remove those flags and call it equivalent C++26 analysis.

Heimdall published two lint warnings for this header (indirect `Event.hpp` include and
a magic number), with no published syntax errors in the observed final diagnostics.
That absence is **not** proof of semantic reflection validation. It tolerates enough
reflection syntax for useful editing/navigation in these cases, while this installed
clangd cannot analyze them correctly. The remaining Heimdall work includes reflected-operand
references/definitions, consistent hover inside splices, `std::meta` intrinsic typing,
consteval reflection evaluation and validation of instantiated/generated types.
Future probes should assert the resulting component tuple for an actual callable and
reject invalid reflected operands, rather than merely checking that requests return data.

For context only, Registry edit-to-completion medians were 16.37 ms / 237.14 ms
(Heimdall / clangd), but completion counts after edits were 15 / 0. At the reflected
`Entity` cursor, medians were 15.74 ms / 30.69 ms with 490 / 2 items. These responses
are not equivalent in quality, and no speedup or full-project correctness claim follows.
Workspace indexing readiness and OS/index caches were not controlled; the project was
opened at selected locations, not exhaustively tested.

#### Real-project check without reflection: Boost.Hana — 2026-10-12

Repeated the project comparison against `/home/gilmar/dev/hana`, revision
`bc49ee25638e59d977edff5737b4e6bf12c1e5ea`, with its existing compilation database
(1,306 entries). The selected source files contain no reflection operators or splices.
No source files or compiler flags were modified. Each cursor used two fresh processes per
server (alternating order), two warm-ups and five measured requests per operation/process,
with the same 60-second timeout and 4,096 MiB sampled-RSS limit. Existing index/OS caches
were not cleared. All four locations gave the same qualitative outcomes in both repeats.

Reproduce the four locations with:

```bash
python3 bench/LspComparison.py --project /home/gilmar/dev/hana \
  --source example/at_c.cpp --line 12 --character 20 \
  --warmup 2 --iterations 5 --repeats 2 --timeout 60 --memory-limit-mib 4096 \
  --output /tmp/opencode/hana-tuple-lsp.json
python3 bench/LspComparison.py --project /home/gilmar/dev/hana \
  --source example/at_c.cpp --line 10 --character 16 \
  --warmup 2 --iterations 5 --repeats 2 --timeout 60 --memory-limit-mib 4096 \
  --output /tmp/opencode/hana-deduction-lsp.json
python3 bench/LspComparison.py --project /home/gilmar/dev/hana \
  --source example/accessors.cpp --line 41 --character 34 \
  --warmup 2 --iterations 5 --repeats 2 --timeout 60 --memory-limit-mib 4096 \
  --output /tmp/opencode/hana-macro-lsp.json
python3 bench/LspComparison.py --project /home/gilmar/dev/hana \
  --source example/transform.cpp --line 27 --character 18 \
  --warmup 2 --iterations 5 --repeats 2 --timeout 60 --memory-limit-mib 4096 \
  --output /tmp/opencode/hana-transform-lsp.json
```

| Probe | Heimdall observation | clangd observation |
|---|---|---|
| `hana::at_c<0>(xs)` in `static_assert` | Empty definition, references and hover | Correct definition in `tuple.hpp:281`, three call references, hover with `const int&` return and value `0` |
| Declaration of `xs = hana::make_tuple(...)` | Correct declaration location; hover only `constexpr auto`; no references | Correct location; deduced `const tuple<int, char, double>`; four references including declaration |
| `get_age(john)` inside `BOOST_HANA_RUNTIME_CHECK` | Correct local definition; hover only `constexpr auto`; no references | Same correct local definition; empty hover and references |
| `hana::transform(...)` inside `BOOST_HANA_RUNTIME_CHECK` | Empty definition, hover and references | Also empty definition, hover and references |

clangd published empty final diagnostic lists for all three files, with none of the
unsupported-reflection/module flags seen in Freyr. This is a more appropriate comparison
of ordinary template-heavy C++ editing. It demonstrates a clear clangd advantage in the
tested template instantiation, deduced types, constexpr hover information and namespace-scope
references. It does **not** show that clangd resolves every macro-expanded expression:
both servers missed the tested `transform` location, and clangd did not provide a hover for
the tested `get_age` use. A generic `constexpr auto` hover is not equivalent to a deduced type.

Heimdall also warned that `BOOST_HANA_DEFINE_STRUCT` requires
`<boost/hana/detail/struct_macros.hpp>` even though `accessors.cpp` already includes
`<boost/hana/define_struct.hpp>`. That public header explicitly exports the macro through
the detail header. This is an observed include-what-you-use **false positive**, not a
missing include in Hana. Magic-number warnings are configurable lint policy differences,
not evidence of C++ compilation failures.

Additional implementation priorities established by this run:

1. **Template call binding and return deduction:** resolve the `at_c` overload in `tuple.hpp`
   and expose its instantiated `const int&` return type. Resolve `xs` to the actual tuple type,
   not just `auto`; test these separately from reflection support.
2. **Namespace-scope references:** return the declaration and three uses of `xs` in
   `static_assert`, with exact ranges. Local-scope success in the synthetic corpus is insufficient.
3. **Macro source mapping:** preserve navigation/reference/hover positions through assertion
   macros and generated accessors. Use independent expectations for the `transform` and `get_age`
   cases rather than taking clangd's empty response as the desired behavior.
4. **Public-header ownership in IWYU:** recognize public forwarding/export headers;
   an existing `define_struct.hpp` include must not trigger advice to include private `detail/`.

Performance still needs response-quality context. For the `at_c` call, pooled warm hover
medians were 0.47 ms / 1.15 ms (Heimdall / clangd), but the former returned null and the
latter provided an instantiated signature/value. Edit-to-completion was 3.66 ms / 20.14 ms,
with 0 / 100 items. At the `xs` declaration it was 3.72 ms / 3.57 ms, and at `transform`
13.42 ms / 9.62 ms: Heimdall is not uniformly faster. These cursors are not dedicated
completion-quality tests, so item counts do not establish ranking or edit correctness.
This is a focused real-library check, not an exhaustive 1,306-translation-unit correctness
or indexing benchmark. Preserve both the Freyr and Hana evidence: reflection tolerance
and mature template semantics are different capabilities, and both matter to the roadmap.

#### Isolated regression tests for the comparison findings

`test/LspComparisonRegressionSpec.py` contains 45 strict end-to-end regressions using
small, self-owned C++ fixtures. Every test creates its own temporary workspace, headers,
configuration and compilation database, and starts a fresh Heimdall LSP process.
**No Hana/Freyr checkout, clangd, installed Boost/Skirnir, compiler invocation or network
access is required.** The only requirements are Python and the built Heimdall LSP.
The reduced fixtures model template tuples/factories, nested assertion macros, public
forwarding headers, pointer wrappers and reflection syntax; they do not import those projects.

```bash
python3 test/LspComparisonRegressionSpec.py --heimdall build/lsp/heimdall-lsp
python3 test/LspComparisonRegressionSpec.py --heimdall build/lsp/heimdall-lsp --filter tuple
python3 test/LspComparisonSpec.py
```

The suite checks exact definition/reference ranges, duplicate definitions, shadow-safe
versioned rename edits, instantiated hover types, dependency completion, overload signatures,
macro source mapping, reflection operands/splices, IWYU public-header ownership and
comment-only edit invalidation. Each test waits for its document's versioned diagnostics
and completion-index readiness instead of assuming that a first response means analysis
has finished. Failure to reach readiness is an error, not a passing empty response.

Current isolated run: **26 passed, 19 failed**, with no skipped or expected-failure tests.
Failures remain visible as acceptance criteria for future fixes: overload selection,
template type deduction, macro-call hover/references, smart-pointer hover/references,
cross-header reflected-operand navigation, public-header IWYU and duplicate header definitions.
Some reduced cases already pass even though the original project probe failed; this narrows
the missing behavior to combinations of features or readiness rather than pretending that
all basic namespace-scope references or all reflection syntax are broken.
The standalone strict suite returns a nonzero status while these gaps exist and is
manual-only, not registered with the ordinary CTest/CI suite. Do not weaken assertions or
add `skip`/`expectedFailure` to make it green. `LspComparisonSpec.py` separately tests the
harness helpers (21 passing tests), including UTF-16 positions and stale diagnostic rejection.

#### Baseline — 2026-10-12

Local run on Linux x86-64 / Intel Xeon E5-2697 v3, comparing Heimdall's existing
Release build (`GCC 16.2.1`, `-O3 -DNDEBUG`) with installed clangd `22.1.8`.
Settings: 1,000/5,000 generated functions, 3 warm-ups and 20 measured requests per
operation per process, 3 fresh processes per server/size. Values below are pooled
median latency in milliseconds; lower is better **for this workload only**.

| Operation | Heimdall 1k | clangd 1k | Heimdall 5k | clangd 5k |
|---|---:|---:|---:|---:|
| Completion | 1.09 | 8.97 | 4.94 | 26.37 |
| Hover | 2.30 | 0.57 | 13.08 | 0.82 |
| Document symbols | 3.41 | 23.59 | 17.93 | 205.34 |
| Semantic tokens | 2.63 | 1.34 | 13.19 | 6.39 |
| Edit → completion | 1.99 | 9.18 | 7.84 | 27.06 |

At 5k, edit-to-completion p95 was **8.98 ms / 29.25 ms** (Heimdall / clangd).
Peak sampled RSS across runs was **112.6–115.2 MiB / 368.5–376.9 MiB**;
process-start-to-initialize median was **2.37 ms / 15.71 ms**, and first
open-to-completion median was **43.12 ms / 14.56 ms**. First completion is not
proof that all analysis or background indexing has finished.

Both returned one completion item and non-null hover results; both returned 5,001
document symbols. Semantic token counts differed: **20,006 / 5,005**, so those
latencies do not represent equivalent highlighting coverage. This synthetic
single-document benchmark does not establish large-project indexing performance
or semantic correctness. For future updates, repeat the command above, retain its
JSON output, and add a dated baseline with toolchain/settings and response counts;
do not compare runs with different inputs/options as a speedup.

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

The macro environment built for each translation unit (see
[macro support](docs/macro-support-plan.md)) can be bounded in `.heimdall.json`:

```json
{ "macros": { "max-include-depth": 200, "max-macros": 65536 } }
```

Both keys are optional and nearer configs override farther ones. The defaults are the
include depth GCC and Clang accept and the number of macros the C++ standard asks
implementations to support. Past either bound the environment is marked incomplete and
no more headers are entered.

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
  `#include` is deliberately opaque here: no headers are opened. This standalone
  preprocessor is used when no macro environment is available.
- **Macro environment** (`MacroTable`, `MacroExpander`, `MacroScanner`, `MacroEnvironment`):
  full macro replacement (function-like, `#`, `##`, `__VA_ARGS__`, `__VA_OPT__`,
  hide-sets), conditions evaluated against the live macro table, and a translation-unit
  walk that follows `#include` in order with compiler builtins (`-dM -E`, cached), `-D`/`-U`
  and `-include`. With a compile command it drives parsing of macro-heavy code, macro
  highlighting, hover with expansion, go to definition, completion, references, rename,
  `Expand macro`, `workspace/symbol` and `cpp/include-what-you-use` for macros.
- **CST foundation**: iterative delimiter grouping for `()`, `[]`, `{}` over lossless
  lexer tokens. Green nodes are flat immutable-style spans linked by parent index;
  unmatched/mismatched delimiters produce diagnostics and recovery continues. This
  is structural scaffolding, not yet a C++ declaration/expression grammar.
- **Rule engine**: `cpp/no-null` replaces `NULL` with `nullptr` outside comments,
  literals and directives; `format/no-trailing-whitespace` removes trailing
  spaces/tabs; `format/require-final-newline` ensures a final newline and makes
  CLI/LSP formatting insert it when configured as `warning` or `error`, preserving LF/CRLF;
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
