# Data-Oriented Design Migration Guide: AoS → SoA for `GrammarNode`

## Overview
`ParseTree` stores its grammar nodes as **SoA** (Structure of Arrays) instead of
`std::vector<GrammarNode>` (AoS):

- **Before**: `std::vector<GrammarNode>` — `sizeof(GrammarNode)` is **20 bytes**
  (1-byte `kind` padded to 4, plus four `uint32_t`), all fields interleaved.
- **After**: `GrammarNodeSoA` with 5 parallel columns (`kind` u8, `first_token`,
  `token_count`, `parent`, `subtree_end` u32) — **17 bytes/node**, no padding.
  A pass that needs only `kind` or `parent` streams one dense column.

## Status

All production code is migrated; `Nodes()` remains only in cold paths and tests.

| File | Status |
|------|--------|
| `core/src/Completion.cpp` | DONE |
| `core/src/Navigation.cpp` | DONE |
| `semantic/src/Binder.cpp` | DONE |
| `semantic/src/ConstantAnalysis.cpp` | DONE |
| `semantic/src/SemanticRules.cpp` | DONE |
| `semantic/src/ApiRules.cpp` | DONE |
| `semantic/src/Typer.cpp` (member `m_nodes`) | DONE |
| `semantic/src/Flow.cpp` (member `nodes`) | DONE |
| `semantic/src/ModernizeConst.cpp` | DONE |
| `semantic/src/IncludeWhatYouUse.cpp` | DONE |
| `semantic/src/detail/TokenView.hpp` | DONE |
| `bench/src/ParserBench.cpp`, `bench/src/DocumentMemoryBench.cpp` | DONE (memory bench now sums the 5 column capacities and no longer forces the AoS build) |
| `src/Pipeline.cpp` | Kept on `Nodes()` — one-shot export of every field (`--parse` output), cold |
| `bench/src/ParserTruncationFuzz.cpp` | Kept on `Nodes()` — cold |
| `test/src/*Spec.cpp` using `ParseTree::Nodes()` | Kept on `Nodes()` on purpose: they exercise the compatibility layer. `ParseTreeSpec` additionally asserts SoA ≡ AoS |

Not affected (the earlier draft of this guide listed them by mistake):
`SyntaxTreeBench.cpp`, `SyntaxTreeSpec.cpp`. They use `SyntaxTree::Nodes()`
(`GreenNode`), a different type.

---

## Migration Patterns

`GrammarNodeSoA` has single-column accessors so call sites stay readable and
touch only the column they need:

```cpp
const auto &soa = tree.NodesSoA();
soa.Kind(i);        // GrammarKind (no manual cast)
soa.FirstToken(i);  // uint32_t
soa.TokenCount(i);
soa.Parent(i);
soa.SubtreeEnd(i);
soa.size();  soa.empty();
```

### Pattern 1: Kind filter
```cpp
// ❌ BEFORE: strided access over 20-byte structs
for (size_t n = 0; n < tree.Nodes().size(); ++n)
    if (tree.Nodes()[n].kind == GrammarKind::DeclaredName) { ... }

// ✅ AFTER: contiguous u8 column
const auto &soa = tree.NodesSoA();
for (size_t n = 0; n < soa.size(); ++n)
    if (soa.Kind(n) == GrammarKind::DeclaredName) { ... }
```

### Pattern 2: Parent walk
```cpp
const auto &soa = tree.NodesSoA();
std::uint32_t current = soa.Parent(node);
for (size_t d = 0; d < 8 && current < soa.size(); ++d) {
    const GrammarKind kind = soa.Kind(current);   // loads kind + parent only
    current = soa.Parent(current);
}
```

### Pattern 3: Several fields of one node → `View`
```cpp
const auto grammar = tree.NodesSoA()[node];   // GrammarNodeSoA::View, by value
grammar.GetFirstToken();  grammar.GetTokenCount();  grammar.GetKind();
```
Note `View` is a *value* (reference + index): write `const auto v = soa[i];`,
not `const GrammarNode &g = ...`.

### Pattern 4: Subtree iteration
```cpp
const auto &soa = tree.NodesSoA();
const size_t end = std::min<size_t>(soa.SubtreeEnd(parent), soa.size());
for (size_t i = parent + 1; i < end; ++i)
    if (soa.Parent(i) == parent) { ... }
```

### Pattern 5: Aliasing in classes
Store `const GrammarNodeSoA &nodes` (see `Flow.cpp`, `Typer.cpp`) and use
`nodes.Kind(i)`; the SoA lives as long as the `ParseTree`.

---

## Compatibility Layer
`ParseTree::Nodes()` still returns `const std::vector<GrammarNode>&`:

- **Lazy**: first call builds the AoS copy from the SoA (`O(N)`, once).
- Rebuilt only while `m_nodes_aos_dirty` is set by the parser.
- **Use for**: cold paths, tests, external consumers.
- **Thread-safety caveat**: the lazy build mutates `mutable` members inside a
  `const` accessor. Calling `Nodes()` for the first time concurrently on a tree
  shared between threads (LSP worker pool) is a data race. Hot/shared code must
  use `NodesSoA()`, which is read-only.

Do **not** remove `GrammarNode` — it is public API.

---

## Auxiliary Token Indices (built once per parse by `BuildAuxiliary()`)

```cpp
const std::vector<std::uint8_t>  &TokenKindMask() const noexcept;   // 1 = trivia
const std::vector<std::uint32_t> &IdentifierTokens() const noexcept;
const std::vector<std::uint32_t> &DirectiveTokens() const noexcept; // tokens inside directives
```

`DirectiveTokens()` locates each directive's token range by binary search over
the offset-ordered token vector (`O(D log T + matched)`); the first draft scanned
all tokens per directive (`O(D·T)`).

```cpp
for (std::uint32_t idx : tree.IdentifierTokens()) {
    const Token &tok = tree.Tokens()[idx];
    ...
}
```

---

## Verification Checklist
- [x] No `Nodes()` in hot loops of `core/`, `semantic/`, `lsp/`
- [x] Kind comparisons through `soa.Kind(i)` / `View::GetKind()`
- [x] `ParseTreeSpec.SoaColumnsMatchTheAosCompatibilityView`
- [x] `ParseTreeSpec.AuxiliaryTokenIndicesMatchTokenKinds`
- [x] Full suite: `ctest --test-dir build` (build with `-DHEIMDALL_BUILD_TESTS=ON`)
- [ ] Run `ParserBench` / `CompletionBench` before and after on the target machine

---

## Performance Targets (aspirational — not yet measured)
| Metric | Target |
|--------|--------|
| `kind` filter loop (10k nodes) | several × faster (single dense u8 column) |
| Parent walk (depth 8) | fewer cache lines per step (kind + parent only) |
| Memory/node | 17 B (SoA) vs 20 B (AoS); AoS only allocated if `Nodes()` is called |

Record measured numbers here once the benchmarks have been run.

---

## Notes
- **Prefer** `NodesSoA()` and column accessors in all new code.
- **Prefer** `IdentifierTokens()` / `TokenKindMask()` for token-level queries.
- `GrammarNodeSoA::push_back/resize` keep all five columns the same length;
  never push to a single column.
