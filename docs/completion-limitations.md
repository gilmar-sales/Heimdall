# Completion limitations & roadmap

Known completion-engine gaps, why they exist, and what fixing each takes.
Symptom-level notes also live in `README.md` / `vscode-extension/README.md`;
this file is the engineering record so the details are not lost.

## 1. Open header buffers: overlay implemented, compiler macro probing still uses disk

- **Files:** `lsp/Server.cpp` (`CompleteDocument`), `semantic/src/IncludeIndex.cpp`
  (`ReadFile`), `semantic/include/Heimdall/IncludeIndex.hpp`.
- **Implemented:** `SourceOverlay` pins open buffer contents from the request's
  analysis snapshot. ResolveHeaders/Build consult it before disk, including
  transitive includes and files that have not been created. Cache keys and the
  LSP include fingerprint include the exact overlay contents.
- **Verified:** unit coverage for transitive unsaved files, limits and empty
  buffers, plus an LSP test changing a header without saving it.
- **Remaining:** CompilerMacros still invokes the compiler against disk headers;
  changed macro definitions/conditional branches are not a complete compiler
  view of the overlay. Closed-file changes also need cache invalidation. The
  exact-content cache fingerprints trade memory/copying cost for collision-free
  equality and need budgeting before broad workspace indexing. Indexes also
  retain the parsed source buffers so navigation never converts offsets using a
  different version of a header.

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

## 4. Member access (`.` / `->`): implemented, with known limits

- **Files:** `core/src/Completion.cpp` (`MemberResolver`, `CompleteMember`),
  `semantic/src/IncludeIndex.cpp` (`CompilerMacros`).
- **How it works:** the receiver is parsed backwards into a chain
  (`a.b().c[0]->`); each segment resolves to a record path through locals,
  parameters, fields (inherited too), free functions, `auto` initializers,
  `this`, aliases (`using`/`typedef`, including header ones such as
  `std::string` -> `basic_string`) and `using namespace`. `->` on
  `unique_ptr`/`shared_ptr`/`optional` reaches the first template argument and
  `[]` on containers reaches the element type. Members come from the buffer and
  the header index; base classes are recorded per scope (`IndexedScope::bases`).
- **Header fidelity:** headers are parsed one by one without expanding
  `#include`, so the index is built with the macros the compiler reports
  (`c++ -dM -E`, include guards removed) and decoration-only macros
  (`_GLIBCXX_NOEXCEPT`, `EXPORT`) are invisible to the grammar.
- **Not covered (no guesses are made, the list is just empty):** types that
  depend on template parameters (`T::value_type`, `typename C::iterator`),
  iterators, lambdas and `operator->` overloads other than the standard
  smart pointers, structured bindings, range-for variables, macros that expand
  to real code, access control (private members are listed).
