# Plan: cross-file index (data-oriented)

## Status

| Phase | State |
|-------|-------|
| 0 Baselines | Not started: no `CoreBench` benchmarks or budgets yet |
| 1 Shard format + builder | Done: `index/` (`FileShard`, `ShardBuilder`), `IndexSpec.cpp` |
| 2 Snapshot + queries | Done: `IndexSnapshot` (`Declarations`, `Definitions`, `References`, `Subtypes`, `Supertypes`, `Search`, `SymbolAt`) and `IndexStore` (open buffers shadow disk). `workspace/symbol` is **not** migrated yet |
| 3 Background indexer | Partial: the LSP feeds `IndexStore` from the existing open-buffer and disk-scan paths. Snapshot is rebuilt in full on change (no delta segment); no priority scheduling |
| 4 LSP features | Partial: `typeHierarchy` supertypes/subtypes and `definition` (declaration to definition in other files, via `Candidates`) use the index; the sibling-file search remains as fallback. `references` appends cross-file uses (name-matched) to the local/workspace result; implementation/hover still on the old paths |
| 5-8 | Not started |

Known gaps in what exists: cross-file uses are matched by written name plus transitive `#include` visibility and are labelled `Unresolved`; bases in other files link by written name and are labelled inexact; occurrences do not record their enclosing symbol yet (needed for call hierarchy); forward-only declarations are not symbols because the binder does not create them.

## 1. Why

Four partial indexes exist today, none of which can answer "what is this symbol everywhere in the project?":

| Component | Layer | Shape | Limit |
|-----------|-------|-------|-------|
| `WorkspaceSymbolIndex` | `lsp/` | per-file columns of outline symbols | syntactic only; names, no identity, no references |
| `ProjectSymbolIndex` | `analysis/` | `vector<IndexedEntity>` / `vector<IndexedOccurrence>` with `std::string` members | AoS, string-heavy; built whole per snapshot, all-or-nothing; explicit `LoadProjectSources`; free functions only |
| `ProjectIndex` / `HeaderSummary` | `semantic/` | SoA, exports and class bases of headers in one include closure | per-file, include-closure only, no occurrences |
| `ScopeIndex` | include/completion | scopes of included headers | per-file, completion only |

Consequences visible in the LSP: `GotoDocument` guesses at most `kMaxGotoCandidates = 4` sibling files; references/rename are single-file; type hierarchy (new) sees only the open TU; completion cannot offer a symbol from a header the file does not include yet.

Goal: one immutable, columnar, incrementally updated index that every feature queries, replacing the four above.

## 2. Design principles (from AGENTS.md)

- **SoA, hot/cold split.** Queries scan integer columns; text lives in side tables read only to render results.
- **No per-entity allocation.** Strings interned into per-shard arenas; ids are `uint32_t` into flat tables.
- **Immutable shards + atomic snapshot publish**, same model as `Workspace` snapshots: readers are lock-free, writers build off-thread.
- **No new third-party dependency in `heimdall_core`.** New target `heimdall_index` (STL only) between `semantic` and `analysis`; `analysis`, `lsp`, `refactor`, `src` consume it. `semantic` must not depend on it (architecture policy test).
- **Honesty about C++.** Resolution state (`Resolved / Ambiguous / Dependent / Unresolved / Unsupported`) is stored per occurrence, never guessed. Destructive consumers (rename) require `Resolved` plus complete coverage.

## 3. Data model

### 3.1 Identity
`SymbolKey` = exact length-delimited string (qualified name, kind, normalized signature, linkage, config class), as in `ProjectSymbolIndex` today. Stored once in a cold table. Hot path uses `SymbolHash` (`uint64_t`) of that key; any hash hit is verified against the cold key during shard merge (collision => both flagged `Ambiguous`, never merged silently). Extend identity beyond free functions: class members (owner key + signature + cv/ref), classes, aliases, enums/enumerators, variables, template primaries (specializations link to their primary through `Specializes`).

### 3.2 FileShard (one file revision, immutable)
```
FileTable row:   path_id, content_hash(u64), config_hash(u64), version, include_range
Symbol columns:  hash u64 | name u32 | container u32 | kind u8 | flags u32 | access u8
                 decl_begin u32 | decl_len u32 | name_begin u32 | name_len u32 | doc u32(cold)
Occurrence cols: offset u32 | length u16 | symbol u32 (shard-local) | enclosing u32 | role u8 | resolution u8
                 sorted by offset (binary search for "symbol at position"), plus `by_symbol` permutation (u32)
Relation cols:   subject_hash u64 | kind u8 (BaseOf, OverriddenBy, Specializes, Contains) | object_hash u64
                 sorted by subject; reverse permutation sorted by object
Cold:            qualified-name text, signature text, type text, doc comments, string arena
```
About 18 bytes per occurrence hot; 4M occurrences is roughly 72 MB with the reference scan touching only `symbol` (4 B/row). Source: built from one `SemanticModel` + `TypeModel` (`ShardBuilder`), reusing `Symbols()`, `Refs()`, `Bases()`, `signature`.

### 3.3 Global view (`IndexSnapshot`)
`shared_ptr<const IndexSnapshot>` = array of `shared_ptr<const FileShard>` + derived tables:
- `Directory`: sorted `(SymbolHash -> shard list)` postings, so references/definition go straight to the shards that mention a symbol instead of scanning all.
- `NameIndex`: name column + 64-bit character masks (keep the existing `CharacterMask` pre-filter from `WorkspaceSymbolIndex`) for fuzzy `workspace/symbol`; optional trigram postings if profiling demands.
- LSM-style update: big base segment + small delta segment for recently changed files, tombstones by file id; background compaction. A keystroke edit rebuilds one shard and a delta, never the whole directory.
- Open buffers always override disk shards (same rule as `Workspace`); closing restores the disk shard.

### 3.4 Persistence
Shards serialize as flat column blobs with a versioned header (format, Heimdall version, `content_hash`, `config_hash`) and are `mmap`ed (see `docs/mmap-io.md`). Warm start = map files and rebuild the directory, no parsing. Cache under a per-workspace cache dir; stale shards (hash mismatch) are rebuilt lazily. Corrupt/unknown version = ignore and rebuild.

## 4. Indexing pipeline
1. **Discovery**: `WorkspaceFiles` + `CompileDatabase` (already present), `didChangeWatchedFiles`.
2. **Schedule**: priority queue (open files, recently edited, same directory, rest); cancellable on `ThreadPool`; memory and file-count limits (mirror `SymbolIndexLimits`); `$/progress` reporting.
3. **Build per file**: parse -> bind -> type -> `ShardBuilder`. A header is indexed once per `(content_hash, config_hash)`, not once per includer; TU-specific facts (macros, include order) stay in the TU shard.
4. **Publish**: swap `IndexSnapshot` atomically. Queries never block on indexing and never see a partial import.
5. **Staleness guard**: results carry the shard's `content_hash`; if the open document's text differs, the live analysis answers instead of the stored range.

## 5. Who should use it

### LSP requests
| Request | Today | With the index |
|---------|-------|----------------|
| `definition` / `declaration` / `implementation` | open file + <=4 guessed siblings | declaration<->definition joined by `SymbolHash`; implementations via reverse `OverriddenBy`/`BaseOf` |
| `references` | single file | posting lookup -> occurrences, filterable by role (read/write/call/decl), honours `includeDeclaration` |
| `prepareRename` / `rename` | single file | multi-file `WorkspaceEdit` with content-hash preconditions, collision check, refuse on non-`Resolved` or incomplete coverage |
| `typeHierarchy` | open TU only (just added) | supertypes/subtypes across files from `BaseOf` relations |
| `callHierarchy` (new) | none | incoming = `Call` occurrences grouped by `enclosing`; outgoing = calls inside the definition range |
| `workspace/symbol` | separate syntactic index | same `NameIndex`, adds identity, container, recency/proximity ranking |
| `completion` | symbols of included headers (`ScopeIndex`) | + not-yet-included project symbols with `additionalTextEdits` that insert the right `#include` |
| `hover` | local + header summary | declaration docs/definition from other files, `TypeLayout`, reference count |
| `codeLens` (new) | none | "N references / N implementations / overridden by" |
| `signatureHelp` (new capability) | not advertised | overload set from all declarations of the callee |
| `inlayHint` | local callee only | parameter names for callees declared in other headers |
| `codeAction` | local quick-fixes | "add #include" for `UndeclaredType` and friends, move/extract with cross-file call-site updates |
| `documentSymbol` | `SymbolOutline` | unchanged; feeds the shard builder |

### Linter / semantic rules (`semantic/`, run from `analysis` with the snapshot)
- `ApiRules` override/virtual checks and `ModernizeFinal` currently use `ProjectIndex::HasDerived` limited to an include closure; use project-wide `BaseOf` relations (exact "no derived class anywhere" before suggesting `final`).
- `IncludeWhatYouUse` / unused-include: replace `ProjectIndex`/`HeaderSummary` exports with symbol-at-header occurrences; exact usage instead of name matching.
- New whole-project rules: ODR / duplicate external definitions, unused external-linkage function/global (no references anywhere), inconsistent declaration vs definition signatures, dead virtual overrides.
- `UndeclaredType`: attach the "declared in X, add include" fix.

### Refactoring engine (`refactor/`)
Rename, change-signature, inline-function (needs definition from another file), safe-delete (needs zero references), move-to-header. Gate on `HasCompleteCoverageFor(entity)` and file content-hash preconditions.

### CLI and extension
- CLI: `heimdall index`, `heimdall refs <symbol>`, `heimdall lint --project` (cross-file rules in CI), sharing the on-disk cache.
- VS Code: index progress/status-bar item, `heimdall.index.enabled`, cache location and memory-limit settings, "Rebuild index" command.

## 6. Phases and exit criteria

| Phase | Deliverable | Exit criteria |
|-------|-------------|---------------|
| 0 | Corpus + `CoreBench` baselines (Heimdall itself plus one large OSS project); budgets | Measured: cold index files/s, RSS per 1M occurrences, `workspace/symbol` p95, refs p95 |
| 1 | `heimdall_index`: `SymbolKey`, `FileShard`, `ShardBuilder` from `SemanticModel`; `IndexSpec.cpp` | Round-trip tests: symbols, occurrences, relations, resolution states; no per-occurrence allocation |
| 2 | `IndexSnapshot`, `Directory`, `NameIndex`, queries (`Definitions`, `References`, `Subtypes`, `SymbolAt`) | Parity with `WorkspaceSymbolIndex` tests; `workspace/symbol` migrated and old code removed |
| 3 | Background indexer, overlay of open buffers, invalidation, progress | Edit keeps requests served from old snapshot; cancellation and limit tests; concurrency smoke test |
| 4 | LSP: cross-file definition, references, implementation, typeHierarchy, hover | Smoke tests per request; `kMaxGotoCandidates` heuristic deleted |
| 5 | mmap persistence | Warm start without parsing; corruption/version-mismatch tests |
| 6 | Rename (multi-file), callHierarchy, codeLens, completion with auto-include, signatureHelp | Rename refuses incomplete coverage; no edit applied against stale hash |
| 7 | Whole-project lint rules + CLI | Rules in `.heimdall.json` schema; CI example |
| 8 | Retire `ProjectSymbolIndex`, `ProjectIndex`, `ScopeIndex` duplicates | Single source of truth; docs updated |

Benchmark every hot-path change against the Phase 0 numbers before claiming a speedup.

## 7. Risks
- **Parser fidelity**: no full preprocessing or overload resolution. Mitigation: `Ambiguous/Dependent/Unsupported` stay first-class, rename and safe-delete only trust `Resolved` + complete coverage, gaps are reported (as `ProjectSymbolIndex` does now).
- **Header variants / macros**: one header, several configs. Key shards by `config_hash`; record the gap instead of pretending a single model.
- **Memory**: budget and eviction of cold columns (mmap pages are reclaimable); limits surfaced in settings.
- **Hash collisions**: verified against cold keys.
- **Stale ranges**: content-hash check before answering from a stored range.
- **Layering**: policy tests keep `core`/`semantic` free of the index; add a spec for `heimdall_index` dependencies.

## 8. Assumptions to confirm
- Index is on by default for workspaces with a `compile_commands.json`, syntactic-only fallback otherwise.
- Cache lives outside the source tree (per-workspace cache directory).
- LSP quality (phases 2-4) ships before whole-project lint rules (phase 7).

Recorded as uses: type names in type specifiers, base-class names (resolved in-file), and type operands of `static_cast`/`dynamic_cast`/`const_cast`/`reinterpret_cast`. Forward declarations (`struct X;`, `class X;`, `enum [class] X [: T];`) are found by a syntactic scan in `ShardBuilder` (namespace/class nesting tracked from braces; none inside function bodies; `friend` skipped) and share the definition's identity. Not covered: forward declarations the binder would need to model (qualified `struct ns::X;`), specializations, friends, macro-generated types.

## 9. Exact cross-file resolution (replacing name matching)

Today a cross-file use is an `Unresolved` occurrence matched to declarations by spelling plus transitive `#include` reachability. Wrong when two reachable files declare the same unqualified name (different namespaces, overloads, shadowing).

### Approach: resolve against the include closure, at index time
1. **Visible-declaration view.** For a shard, build the set of declarations reachable through its include closure (`IndexSnapshot` already has the include graph): per namespace/class scope, name -> candidate symbols. Computed lazily per shard from the shards' own symbol columns, no parsing.
2. **Scoped lookup.** For each `Unresolved` occurrence, replay C++ lookup over indexed data only: enclosing scopes (needs `scope`/`enclosing` per occurrence), `using namespace`, `using X::Y`, aliases, qualified paths (`a::b::T`), ADL not attempted. Result: `Resolved` (one candidate), `Ambiguous` (several; keep all), or stays `Unresolved`.
3. **Persist the result in a second pass**, not in the shard (shards are per-file and immutable): a snapshot-level `ResolvedUse` table (occurrence -> symbol hash) built when the snapshot is built or lazily on first reference query, invalidated per includer when a header changes.
4. **Consumers** switch from name matching to the table; `Unresolved` remains only for what cannot be resolved; rename requires `Resolved` and complete coverage.

### Data the shard must add first (phase A, shard format v2)
- Enclosing scope per occurrence (namespace/class symbol local id) and the written qualifier (`ns::T` parts).
- `using` directives/declarations/aliases as relations (`UsingNamespace`, `UsingDecl`, `AliasOf`).
- Overload signature for call occurrences (argument count; arg types where the typer knows them).
- Base-class qualifier text, so `BaseOfByName` becomes `BaseOf` once resolved.
- Template primary vs specialization link (`Specializes`).

### Phases
| Step | Deliverable | Exit criteria |
|------|-------------|---------------|
| A | Shard v2 columns above, builder + round-trip tests | no per-occurrence allocation; old queries unchanged |
| B | Include-closure visibility + namespace/class scope tables in `IndexSnapshot` | unit tests: same name in two namespaces, shadowing, `using`, aliases |
| C | `ResolvedUse` table and incremental invalidation | editing a header re-resolves only transitive includers; benchmark vs full rebuild |
| D | Move references/definition/typeHierarchy/rename gates to resolved results; keep name match as a labelled fallback | tests that two same-named classes in different namespaces no longer conflate; ambiguity reported |
| E | Overloads and template specializations | call occurrences pick overload by arity then types; `Specializes` used by hierarchy/references |

### Risks and decisions
- **Macros and conditional includes** change visibility; resolve per `config_hash` and report the gap rather than guess. Macros are now indexed (`SymbolKind::Macro`, uses flagged `kOccurrenceMacro`, resolved by name through the include closure; see `docs/macro-support-plan.md`). The shard's `config_hash` still ignores macros that come from headers.
- **ADL, dependent names, complex overloads** stay `Unresolved`/`Ambiguous`; never promoted silently.
- **Cost**: resolution is a hash lookup per occurrence over closure scope tables; benchmark before choosing eager vs lazy (phase 0 numbers needed).
- **Alternative**: let the existing `SemanticModel` bind with the real include closure (`HeaderSummary`/`ScopeIndex`) and store its resolved targets in the shard. More exact, but re-binds every file on any header change and ties the index to the binder; keep as fallback if A-E proves too approximate.

### Status of section 9
| Step | State |
|------|-------|
| A | Done: occurrences carry scope, written qualifier, member flag and call arity; shards carry scope extents (namespaces, classes, out-of-line member bodies), `using` directives/declarations/aliases and per-function required/maximum parameters; base relations carry the written-name offset. |
| B | Done: `IndexSnapshot` keeps a forward include graph, a qualified-name directory and a member directory; lookup walks the enclosing scopes outward (innermost match hides outer ones), honours `using namespace`, `using a::B`, qualified paths and `::`-absolute names. |
| C | Done, lazy: `IndexSnapshot::Resolved(shard)` computes one shard on first use (`std::call_once`) and is shared by all readers of that snapshot. Resolutions are not carried across snapshots: a new snapshot recomputes only the shards that are queried. |
| D | Done for the index consumers: `References` (Resolved / Ambiguous / name-matched `Unresolved` only when nothing visible declares the name), `Candidates` (go to definition), `Subtypes`, `Supertypes` (exact once the base name resolves). Rename still uses the refactoring engine and is not index-based. |
| E | Partial: overloads are filtered by call arity (declared minimum/maximum parameters, defaults and variadics); calls that still fit several overloads are `Ambiguous`. Argument types are not compared (needs the typer). Template specializations share the identity of their primary template, so references to `Box<int>` reach every declaration of `Box`; there is no separate `Specializes` relation. |

Not resolved by design: inherited members looked up unqualified inside derived classes (fall back to name matching), namespace aliases, argument-dependent lookup, dependent names.

Measured (`IndexBench`, synthetic: every source includes three headers, calls members and free functions): building a shard from a bound file about 4 µs; publishing a snapshot 0.40 ms for 250 shards and 2.4 ms for 1,250; resolving one includer adds about 0.02 ms and 0.1 ms; a references query 2 µs and 5.5 µs once resolved. These are first baselines, not budgets.

## 10. Multi-file rename (`refactor/IndexRename`)

`textDocument/rename` uses the index when the symbol is declared or used in more than one file (`IndexRename::SpansFiles`); otherwise the existing single-file engine runs. The rename either applies to every occurrence or is refused with the reason; it never edits on a guess.

Gates, in order:
1. The disk scan finished, was not truncated at `kMaxWorkspaceFiles` and indexed every file it found (`m_index_complete`); declaring files are inside the workspace root.
2. New name is a single non-keyword identifier; the open file's text matches the indexed revision (content hash), as does every file that will be edited.
3. The symbol resolves to exactly one declaration. Namespaces, virtual/override/pure functions, operators, constructors and destructors are refused; a member whose name is also declared by a derived class is refused.
4. Every use returned for the entity is `Resolved` (no ambiguous or name-matched uses).
5. Project-wide coverage: no occurrence of the same spelling in any file is unresolved/ambiguous/dependent; every identifier token of that spelling in a file that contains it is either an indexed occurrence or the declaration of a non-indexed local/parameter; none sits in a preprocessor directive other than `#include`.
6. Classes also rename constructor/destructor names (in-class and `Widget::Widget`, `Widget::~Widget`).
7. Verification: the edited files are parsed again (no new parse errors), re-indexed, and a new snapshot is built; every occurrence in every file must keep the same state and resolve to the same entity (modulo the renamed identities), no two entities may merge and the entity must keep its declaration count. This is what detects collisions, capture by inner declarations and hijacked uses.

What this guarantees and what it cannot: the check covers everything the index sees (workspace files). A new name that clashes with a symbol from a header outside the workspace (system or third-party headers) is not detected, files under skipped directories (`.*`, `build*`, `out`, `node_modules`) are not part of the project, and code in inactive `#if` regions or generated by macros is refused rather than renamed. Occurrences the index does not yet record (for example in templates with dependent names) make the rename refuse instead of skip them.

## 11. Rename hardening: implementation checkpoints

The cross-file engine is not a general C++ semantic proof yet. Connecting it to the
LSP does not close the remaining coverage, configuration and preprocessing gaps.
The following checkpoints track the safety-first implementation order.

| Phase | State | Remaining exit criteria |
|-------|-------|-------------------------|
| 0 Contract and regressions | Started: regression tests for unknown receivers, incompatible arity, local homonyms, using position, reserved names, new-name macros, cancellation and parse errors | Add regressions for real include search order, conditional variants and expansion provenance |
| 1 Reliable index | Partial: receiver spelling plus declaration scope/position, using introduction offset, pinned store generation | Canonical identity verification, compiler-resolved includes, configuration/TU variants, per-file coverage reports and dependent invalidation |
| 2 Shared service and LSP | Partial: `IndexRename::Preview` validates plans without requiring Workspace document IDs | Shared prepare/rename target resolution, routing by entity kind rather than `SpansFiles`, frontend adoption of indexed preview and configuration fingerprints, final version/generation checks |
| 3 Targeted coverage | Partial: independently bound local homonyms no longer veto rename | Prove affected regions for both old and new names; avoid project-wide rejection and reparse only with dependency evidence |
| 4 Nominal entities | Pending | Aliases, enums/enumerators, structured forwards/friends, constructor selection redirected to the class, namespace reopening and Doxygen parity |
| 5 Members and overloads | Partial: simple declared receivers and locally typed classes separate homonymous members; incompatible arities are discarded | Inheritance lookup/hiding, argument types and conversion ranking, cv/ref, address-taking, ADL and hidden friends |
| 6 Virtual families | Pending | Exact override relations, implicit overrides, complete family edits and external-base/derived coverage |
| 7 Templates and lexical constructs | Pending | Primary/specialization identity, instance bindings/constraints, explicit captures, requires scopes, catch scopes and coroutine bindings |
| 8 Preprocessing/configurations | Partial: old/new directive-name and predefined-new-name checks | Spelling/expansion provenance, all relevant configurations, unsaved-buffer/speculative overlays, GCC/Clang/MSVC verification and modules |
| 9 Scale and delivery | Pending | Incremental resolution, cancellation throughout expensive work, rename end-to-end benchmarks, documentation/release parity and migration of remaining old-index consumers |

Implemented safety details:

- A member candidate found only by spelling stays `Unresolved`, even when only
  one member is visible. `References` preserves that proof state instead of
  deriving it from the number of targets.
- Simple receiver declarations are looked up at their declaration scope and
  position, not at the member-use scope. Unsupported receivers still fail closed.
- Arity filtering applies to single candidates and to empty viable sets. It is
  a necessary filter, not complete overload resolution.
- Same-file `using` declarations/directives cannot resolve earlier occurrences.
  Header include order and transitive using visibility still need exact edges.
- Index rename uses the existing non-reserved ASCII identifier validator; it
  checks new-name directive mentions and predefined macros as well as old-name
  occurrences. This does not replace compiler preprocessing verification.
- Edited files with existing parse errors are rejected, rather than comparing
  only the number of diagnostics before and after.
- `IndexStore` generations are pinned in snapshots and rename plans.
  `IndexRename::Preview` checks the current generation, every proof shard's
  content hash, supplied configuration fingerprints, edited document IDs/versions,
  pinned parser options, edit ranges/expected bytes, duplicate paths and overlap.
  It returns transformed sources without writing anything. Frontends must still
  validate their current project/configuration discovery and apply versioned edits;
  a standalone snapshot's default zero generation is not a project-change tracker.

Tests live in `IndexSpec.cpp` and `IndexRenameSpec.cpp`; run the full CTest suite
and LSP smoke tests after changing proof states or edit preconditions.

Measured after receiver typing was added (`Release`, current selected build,
`IndexBench --benchmark_min_time=0.05s`): shard construction about 8.45 microseconds,
snapshot publication about 0.406/2.523 ms for 250/1,250 shards, warmed references
about 1.81/5.35 microseconds. These are observations, not a speedup claim or a
budget; typing cost must be included in future comparable baselines.
