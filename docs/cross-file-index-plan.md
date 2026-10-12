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

Type names written in type positions are now recorded as uses (resolved in-file when the lookup succeeds, otherwise name-matched).
