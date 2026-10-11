# Doxygen in VS Code: rendering and navigation

Research date: 2026-10-10. The rendering and local navigation described below are
implemented and tested by `test/src/DoxygenNavigationSpec.cpp` and
`test/LspDoxygenFlowSmoke.py`.

## What the editor needs

VS Code does not resolve Doxygen commands by itself. The Microsoft C/C++ extension
documents Doxygen hover support, but this is an extension feature, not a universal
VS Code parser or a guarantee that `@ref` supports source navigation.

Heimdall already sends hover/completion documentation as LSP Markdown. Sending raw
`@brief`/`@param` text does not make VS Code interpret those commands. Heimdall must
render the documentation and resolve its symbol references. F12/Go to Definition
requires `textDocument/definition` to accept positions inside documentation;
clickable source-comment links can additionally use `textDocument/documentLink`,
with source ranges and target URIs. Hover links require actual Markdown links,
not raw `@ref` text. There is no mandatory Markdown heading layout for Doxygen.

## Tested command scope

- Documentation forms: `/** */`, `/*! */`, `///`, `//!`.
- Both command prefixes, `@` and `\`, are valid Doxygen syntax.
- `brief`: render the summary; ordinary prose is not automatically a symbol link.
- `param`: render one named bullet per parameter in a separate **Parameters**
  section, with directions when present; resolve the name against the documented
  function, including `[in]`, `[out]`, `[in,out]`, optional whitespace before the
  direction, and comma-separated parameter names.
- `ref`: resolve the explicit target, including qualified names and overload
  signatures; an optional quoted label is display text, not another target.
- `see` and its alias `sa`: render a separate **See also** section and resolve
  referenced symbols, including `Function()`.
- `return` and its alias `returns`: render a separate **Returns** section; an explicit
  `ref` inside it is navigable, but the prose itself is not a symbol target.
- Unresolved names, ordinary comments, and `code` blocks must not produce guessed
  navigation targets.

This covers the commands requested, not the entire Doxygen command language.
Unit tests exercise rendering and symbol resolution through existing engine APIs.
The protocol smoke test verifies Markdown hover, F12 targets, document-link ranges
and file URIs (including UTF-16 positions), and Doxygen edits for parameter/class
rename. Document links currently target uniquely resolved symbols in the same file;
hover text is rendered Markdown, but does not yet contain generated clickable links.
The automated tests do not claim to exercise an actual VS Code UI session.

Rename updates matching `param`, `ref`, and `see`/`sa` targets, and self-name mentions
in the attached `brief`. Function rename uses existing workspace coverage checks.
Class rename is local and remains conservative for includes, templates, constructors,
destructors, and unresolved or ambiguous occurrences.

## Sources

- [VS Code C++ documentation: Doxygen and navigation](https://code.visualstudio.com/docs/cpp/cpp-ide)
- [Microsoft C/C++: see/sa hover support](https://github.com/microsoft/vscode-cpptools/pull/12415)
- [Doxygen special commands](https://www.doxygen.nl/manual/commands.html)
- [Doxygen documentation block forms](https://www.doxygen.nl/manual/docblocks.html)
- [LSP hover](https://microsoft.github.io/language-server-protocol/specifications/lsp/3.17/specification/#textDocument_hover)
- [LSP document links](https://microsoft.github.io/language-server-protocol/specifications/lsp/3.17/specification/#textDocument_documentLink)
