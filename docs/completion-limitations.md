# Completion limitations & roadmap

Known completion-engine gaps, why they exist, and what fixing each takes.
Symptom-level notes also live in `README.md` / `vscode-extension/README.md`;
this file is the engineering record so the details are not lost.

## 1. Headers are read from disk, open buffers are ignored

- **Files:** `lsp/Server.cpp` (`CompleteDocument`), `semantic/src/IncludeIndex.cpp`
  (`ReadFile`), `semantic/include/Heimdall/IncludeIndex.hpp`.
- **Cause:** `IncludeIndex::Build` opens every header with `ifstream` from
  disk. Only the main file's dirty text (from `m_documents`) feeds local
  symbols; header buffers are never consulted.
- **Impact:** editing a header without saving hides its new symbols from
  completion in includers; not-yet-saved headers do not resolve at all.
- **Fix:** overlay filesystem — pass a `read(path) -> optional<string>`
  callback into `Build`/`ReadFile` that checks `m_documents` (path to URI)
  first and falls back to disk. Include the open buffer's version in
  `CacheKey`, otherwise the cache hides the change. Small (~30 lines).

## 2. No MSVC (`cl.exe`) default include paths

- **Files:** `semantic/src/IncludeIndex.cpp` (`SystemIncludes`).
- **Cause:** system include discovery shells out to
  `<driver> -xc++ -E -v` and parses the `#include <...> search starts here:`
  block — a GCC/Clang protocol. `cl.exe` has no equivalent flag.
- **Impact:** MSVC projects whose `compile_commands.json` lacks explicit STL /
  Windows SDK `-I` entries get an empty index for `<vector>` etc.
- **Fix:** branch on the driver name in `SystemIncludes`: for `cl.exe`, run
  `cl /nologo /E /showIncludes` on a scratch file and parse
  `Note: including file:` lines, or locate the SDK via `%WindowsSdkDir%` /
  `vswhere -latest`. Needs a real Windows+MSVC machine to validate. Medium.

## 3a. `using namespace` directives are ignored

- **Files:** `core/src/Completion.cpp` (`CompleteExpression`).
- **Cause:** unqualified lookup only considers file-visible names; nothing
  records that `using namespace foo;` makes `foo`'s members nominable
  unqualified at the cursor's scope.
- **Impact:** `using namespace std;` + `vec` suggests nothing (must still
  type `std::vec`).
- **Fix:** collect using-directives visible at the cursor scope and union
  their scopes' members into unqualified candidates (dedupe already handles
  collisions). Cheap.

## 3b. Type aliases are not resolvable as qualifiers

- **Files:** `core/src/Completion.cpp` (`QualifierBefore`, `ResolveScope`).
- **Cause:** `using Vec = std::vector<int>;` registers `Vec` (as a Type via
  `UsingDeclaration`), but nothing maps the alias to the scope path
  `{std, vector}`, so `Vec::...` resolves to no target.
- **Impact:** `Vec::` after an alias is empty; same for `typedef` aliases.
- **Fix:** resolve the alias's underlying type expression to a scope path
  (recursively, through chains of aliases). Requires type-expression parsing
  plus lookup — notably harder than 3a. Medium-hard.

## 4. No member access (`.` / `->`) completion

- **Files:** `core/src/Completion.cpp` (`ClassifyContext` returns
  `MemberAccess`, `Complete` answers `[]`; pinned by
  `SuppressesMemberAccessUntilMembersAreModeled`).
- **Cause:** `ns::`/`Type::` resolve scopes *named in source*. `obj.member`
  needs the *type of the expression* `obj`: variable-to-type mapping through
  `typedef`/`using`/templates, then the member list of the corresponding
  `RecordDefinition`, possibly in another header. That is type inference, not
  lexical lookup.
- **Impact:** `s.` / `ptr->` intentionally return nothing rather than guess.
- **Fix:** a minimal expression evaluator (identifier, call, `*`/`&`,
  chained access) plus a type-to-members table per TU, joined with the header
  index. Suggested slicing: (1) `var.` / `var->` for same-file simple-type
  locals (covers the common case), (2) inherited members, (3) templates.
  Hard; the natural next milestone after 1–3.
