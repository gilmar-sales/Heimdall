#pragma once

#include "Document.hpp"
#include "WorkspaceSymbolIndex.hpp"

#include <Heimdall/SymbolOutline.hpp>

#include <span>
#include <string>
#include <string_view>

namespace heimdall::lsp
{

    /// `SymbolKind` number of the LSP specification.
    [[nodiscard]] int ToLspSymbolKind(heimdall::OutlineKind kind) noexcept;

    /// `DocumentSymbol[]`: the outline as a tree, siblings in source order. Every
    /// `selectionRange` lies inside its `range`.
    void AppendDocumentSymbols(std::span<const heimdall::OutlineSymbol> symbols,
                               const LineIndex& lines,
                               std::string& out);

    /// `SymbolInformation[]` for clients that cannot show a hierarchy; nesting is carried by
    /// `containerName`.
    void AppendFlatDocumentSymbols(std::span<const heimdall::OutlineSymbol> symbols,
                                   std::string_view uri,
                                   const LineIndex& lines,
                                   std::string& out);

    /// `SymbolInformation[]` for `workspace/symbol`.
    void AppendWorkspaceSymbols(std::span<const SymbolHit> hits, std::string& out);

} // namespace heimdall::lsp
