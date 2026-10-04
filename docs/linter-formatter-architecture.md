# Linter / formatter architecture: sharing one parser

One frontend (`core/`) serves the linter (`RuleEngine`), the formatter
(`Formatter`), and — since completion/hover landed — the interactive features
(`CompletionEngine`, `SemanticAnalyzer`, `IncludeIndex`). Sharing the parser
saves time and memory, but lint and format pull it in opposite directions.
This file records the four conflicts and how this codebase resolves each one.

## Shared pipeline

```text
source (buffer/mmap)
  └─ Lexer → lossless tokens (trivia included)            ← shared
       ├─ SyntaxTree: iterative ()/[]/{} grouping          ← shared
       ├─ Preprocessor: #if selection + macro substitution ← shared
       │    └─ ParseTree: grammar over the active view     ← shared
       │         ├─ RuleEngine → diagnostics + TextEdit fixes
       │         ├─ SemanticAnalyzer → local semantic rules (opt-in)
       │         └─ CompletionEngine → completion/hover (+ IncludeIndex)
       └─ Formatter → formatted text (works on raw source)
```

The rule is: everything below `Lexer` is shared and side-effect free; every
consumer decides how much of the pipeline to climb.

## 1. Source fidelity (the most important one)

The formatter needs a lossless tree: comments, blank lines, macros exactly as
written, every original byte. A linter mostly does not care, and a clean AST
à la Clang drops comments and pre-expands macros — useless for formatting.
A fully lossless CST, on the other hand, grows memory for everyone.

**Resolution:** the lexer is byte-lossless and *trivia stays in the token
stream* (`Whitespace`, `LineComment`, `BlockComment` are first-class tokens,
`core/include/Heimdall/Lexer.hpp`). Nodes hold `(first_token, token_count)`
spans, never text, so the tree stays compact while every byte remains
addressable. This is the "trivia table" idea with the table inline: simpler
than a side table at the cost of larger token vectors. If profiles ever blame
memory, the migration path is known — move trivia to a side table indexed by
token — but nothing in the node API would need to change.

**Status:** done. `Formatter` copies directives verbatim and never treats
braces inside trivia/literals as structure; `RuleEngine` diagnostics carry
byte offsets convertible to positions.

## 2. The preprocessor: C++'s special problem

The formatter must format the *unexpanded* code, including **all** `#if` /
`#else` branches. A semantic linter wants the *expanded* code for one
configuration. No single tree serves both views (clang-format handles its side
by analyzing `#if`-branch combinations separately).

**Resolution:** the shared layer is the lexer plus the *unexpanded* syntactic
view. Expansion is a separate, explicit step: `Preprocessor::Process`
(`core/include/Heimdall/Preprocessor.hpp`) evaluates nested `#if`/`#ifdef` /
`#elif`/`#else`/`#endif`, substitutes object-like macros, and reports
`active_ranges` (byte ranges of the live configuration) alongside the opaque
`directives`. The grammar (`ParseTree`) parses the active view; the formatter
never looks at it and sees every branch; `SemanticAnalyzer` and the
`semantic/no-unused-local` rule consults only `active_ranges`; the completion index
(`IncludeIndex`) reads headers off to the side without disturbing the parse.

**Status:** mostly done. Known simplifications, all deliberate: function-like
macros, token pasting/stringification and full preprocessor-expression
semantics are left unexpanded rather than guessed; `#include` is opaque to
the parser (headers are indexed separately for completion only).

## 3. Cost for whoever does not need it

If parsing always resolved names or types, running just the formatter would
pay for work nobody asked for.

**Resolution:** on-demand layers. Tokens → CST → semantics, each computed
only when a consumer asks: `Formatter` and syntactic rules never touch the
`SemanticAnalyzer` (gated behind `--semantic` / `enableSemantic`);
completion parses per request; the LSP reuses the compile database entry
without re-reading `compile_commands.json`.

**Status:** layering done, caching incomplete. Today each LSP
`publishDiagnostics` and each completion re-parses from scratch; the include
index is the only cached layer (keyed by resolved headers + flags in
`Server::m_include_indices`). The natural next step is a parse cache keyed by
content hash shared across diagnostics/completion/hover within one document
version.

## 4. Autofix vs. formatting fighting over the source

Lint quick-fixes rewrite code; afterwards the formatter must reformat the
result. If both mutate a tree in place, they invalidate each other.

**Resolution (target):** keep the tree immutable and let both sides produce
edits (`offset + length + replacement`) through one common infrastructure;
after applying fixes, reformat only the affected ranges.

**Status:** half done. The lint side is there: `RuleEngine::Diagnostic`
carries `TextEdit fix`, and `RuleEngine::ApplyFixes` applies non-overlapping
edits right-to-left (`core/src/RuleEngine.cpp`). The formatter side is not:
`Formatter::Format` returns a whole new string, and the LSP synthesizes a
full-document edit around it. Converging means teaching the formatter to emit
a `TextEdit` list (or diffing its output) and adding range-restricted
reformatting — the missing piece before "fix, then format selection" can be
correct and cheap.

## The genuinely hard part

The preprocessor (§2). Everything else is engineering; branch combinations
and macro expansion semantics are where formatters and linters silently
diverge, so prototype `#if`-branch handling and macro policy *before*
investing in grammar or rule coverage. In this codebase that prototype is
`Preprocessor` + `active_ranges` — extend it before extending the grammar.
