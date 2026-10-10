#pragma once

#include "Document.hpp"
#include "WorkspaceSymbolIndex.hpp"

#include <Heimdall/SymbolOutline.hpp>

#include <cstdint>
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

    /// `window/workDoneProgress/create` request that announces `token` to the client.
    [[nodiscard]] std::string WorkDoneProgressCreate(std::string_view requestId,
                                                     std::string_view token);

    /// `$/progress` notification that starts the progress shown to the user.
    [[nodiscard]] std::string WorkDoneProgressBegin(std::string_view token,
                                                    std::string_view title,
                                                    std::string_view message);

    /// `$/progress` notification with the share of work done, 0 to 100.
    [[nodiscard]] std::string WorkDoneProgressReport(std::string_view token,
                                                     std::string_view message,
                                                     unsigned percentage);

    /// `heimdall/indexStatus` notification for the status-bar item: `state` is "indexing" or
    /// "ready"; `done` of `total` files are indexed after `elapsedMs` milliseconds.
    [[nodiscard]] std::string IndexStatusNotification(std::string_view state,
                                                      std::size_t done,
                                                      std::size_t total,
                                                      std::uint64_t elapsedMs);

    /// `$/progress` notification that dismisses the progress.
    [[nodiscard]] std::string WorkDoneProgressEnd(std::string_view token,
                                                  std::string_view message);

} // namespace heimdall::lsp
