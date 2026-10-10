#include "SymbolProtocol.hpp"

#include "JsonRpc.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace heimdall::lsp
{

    namespace
    {

        constexpr std::string_view kUnnamed = "(unnamed)";

        void AppendRange(const Position& start, const Position& end, std::string& out)
        {
            out += "{\"start\":";
            AppendPosition(start, out);
            out += ",\"end\":";
            AppendPosition(end, out);
            out += '}';
        }

        void AppendName(const heimdall::OutlineSymbol& symbol, std::string& out)
        {
            QuoteJson(symbol.name.empty() ? kUnnamed : std::string_view(symbol.name), out);
        }

        // `range` widened where needed so that it contains the name.
        std::pair<std::size_t, std::size_t> ContainingRange(const heimdall::OutlineSymbol& symbol)
        {
            const std::size_t begin = std::min(symbol.rangeOffset, symbol.nameOffset);
            const std::size_t end   = std::max(symbol.rangeOffset + symbol.rangeLength,
                                               symbol.nameOffset + symbol.nameLength);
            return {begin, end};
        }

        // Opens the object and leaves it open so the caller can append `children`.
        void AppendSymbolHead(const heimdall::OutlineSymbol& symbol,
                              const LineIndex& lines,
                              std::string& out)
        {
            const auto [begin, end] = ContainingRange(symbol);
            out += "{\"name\":";
            AppendName(symbol, out);
            if (!symbol.detail.empty())
            {
                out += ",\"detail\":";
                QuoteJson(symbol.detail, out);
            }

            out += ",\"kind\":" + std::to_string(ToLspSymbolKind(symbol.kind));
            out += ",\"range\":";
            AppendRange(lines.ToPosition(begin), lines.ToPosition(end), out);
            out += ",\"selectionRange\":";
            AppendRange(lines.ToPosition(symbol.nameOffset),
                        lines.ToPosition(symbol.nameOffset + symbol.nameLength), out);
        }

        void AppendInformation(const heimdall::OutlineSymbol& symbol,
                               std::string_view uri,
                               const LineIndex& lines,
                               std::string& out)
        {
            out += "{\"name\":";
            AppendName(symbol, out);
            out += ",\"kind\":" + std::to_string(ToLspSymbolKind(symbol.kind));
            out += ",\"location\":{\"uri\":";
            QuoteJson(uri, out);
            out += ",\"range\":";
            const auto [begin, end] = ContainingRange(symbol);
            AppendRange(lines.ToPosition(begin), lines.ToPosition(end), out);
            out += "},\"containerName\":";
            QuoteJson(symbol.container, out);
            out += '}';
        }

    } // namespace

    int ToLspSymbolKind(heimdall::OutlineKind kind) noexcept
    {
        constexpr int kNamespace   = 3;
        constexpr int kClass       = 5;
        constexpr int kMethod      = 6;
        constexpr int kField       = 8;
        constexpr int kConstructor = 9;
        constexpr int kEnum        = 10;
        constexpr int kInterface   = 11;
        constexpr int kFunction    = 12;
        constexpr int kVariable    = 13;
        constexpr int kEnumMember  = 22;
        constexpr int kStruct      = 23;
        constexpr int kOperator    = 25;

        switch (kind)
        {
        case heimdall::OutlineKind::Namespace:
            return kNamespace;
        case heimdall::OutlineKind::Class:
        case heimdall::OutlineKind::TypeAlias:
            return kClass;
        case heimdall::OutlineKind::Struct:
        case heimdall::OutlineKind::Union:
            return kStruct;
        case heimdall::OutlineKind::Enum:
            return kEnum;
        case heimdall::OutlineKind::EnumMember:
            return kEnumMember;
        case heimdall::OutlineKind::Function:
            return kFunction;
        case heimdall::OutlineKind::Method:
        case heimdall::OutlineKind::Destructor:
            return kMethod;
        case heimdall::OutlineKind::Constructor:
            return kConstructor;
        case heimdall::OutlineKind::Operator:
            return kOperator;
        case heimdall::OutlineKind::Field:
            return kField;
        case heimdall::OutlineKind::Variable:
            return kVariable;
        case heimdall::OutlineKind::Concept:
            return kInterface;
        }

        return kVariable;
    }

    void AppendDocumentSymbols(std::span<const heimdall::OutlineSymbol> symbols,
                               const LineIndex& lines,
                               std::string& out)
    {
        std::vector<std::vector<std::size_t>> children(symbols.size());
        std::vector<std::size_t> roots;
        for (std::size_t i = 0; i < symbols.size(); ++i)
        {
            const std::uint32_t parent = symbols[i].parent;
            if (parent < i)
            {
                children[parent].push_back(i);
            }
            else
            {
                roots.push_back(i);
            }
        }

        // Depth-first with an explicit stack: nesting depth is user-controlled.
        struct Frame
        {
            const std::vector<std::size_t>* siblings;
            std::size_t next;
        };

        out += '[';
        std::vector<Frame> stack;
        stack.emplace_back(&roots, 0);
        while (!stack.empty())
        {
            Frame& frame = stack.back();
            if (frame.next == frame.siblings->size())
            {
                stack.pop_back();
                if (!stack.empty())
                {
                    out += "]}";
                }

                continue;
            }

            const std::size_t index = (*frame.siblings)[frame.next];
            if (frame.next++ > 0)
            {
                out += ',';
            }

            AppendSymbolHead(symbols[index], lines, out);
            out += ",\"children\":[";
            stack.emplace_back(&children[index], 0);
        }

        out += ']';
    }

    void AppendFlatDocumentSymbols(std::span<const heimdall::OutlineSymbol> symbols,
                                   std::string_view uri,
                                   const LineIndex& lines,
                                   std::string& out)
    {
        out += '[';
        for (std::size_t i = 0; i < symbols.size(); ++i)
        {
            if (i > 0)
            {
                out += ',';
            }

            AppendInformation(symbols[i], uri, lines, out);
        }

        out += ']';
    }

    void AppendWorkspaceSymbols(std::span<const SymbolHit> hits, std::string& out)
    {
        out += '[';
        for (std::size_t i = 0; i < hits.size(); ++i)
        {
            const SymbolView symbol = hits[i].Symbol();
            if (i > 0)
            {
                out += ',';
            }

            out += "{\"name\":";
            QuoteJson(symbol.name, out);
            out += ",\"kind\":" + std::to_string(ToLspSymbolKind(symbol.kind));
            out += ",\"location\":{\"uri\":";
            QuoteJson(hits[i].file->uri, out);
            out += ",\"range\":";
            AppendRange(symbol.start, symbol.end, out);
            out += "},\"containerName\":";
            QuoteJson(symbol.container, out);
            out += '}';
        }

        out += ']';
    }

} // namespace heimdall::lsp
