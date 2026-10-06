// Synthetic C/C++ documents built from the stb single-header libraries in bench/.
// Shared by the LSP latency, document memory and truncation-fuzz tools (review
// section 7). Each stb header is emitted with its *_IMPLEMENTATION macro defined so
// the parser sees the full implementation, and whole files are repeated (with the
// include guard renamed) until the requested line count is reached, so every
// document stays syntactically balanced.
#pragma once

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace heimdall::bench
{

    struct StbUnit
    {
        const char* file;
        const char* implementation_macro;
        const char* guard;
    };

    inline constexpr StbUnit kStbUnits[] = {
        {
            "stb_c_lexer.h", "STB_C_LEXER_IMPLEMENTATION", "INCLUDE_STB_C_LEXER_H"
        },
        {"stb_tilemap_editor.h", "STB_TILEMAP_EDITOR_IMPLEMENTATION",
            "STB_TILEMAP_INCLUDE_STB_TILEMAP_EDITOR_H"},
        {"stb_image.h", "STB_IMAGE_IMPLEMENTATION", "STBI_INCLUDE_STB_IMAGE_H"},
    };

    inline std::size_t CountLines(std::string_view text)
    {
        return static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n')) +
            (!text.empty() && text.back() != '\n' ? 1 : 0);
    }

    inline std::optional<std::string> ReadFile(const std::string& path)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
        {
            return std::nullopt;
        }

        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    inline void ReplaceAll(std::string& text, std::string_view from, const std::string& to)
    {
        for (std::size_t at = text.find(from); at != std::string::npos; at = text.find(from,
            at + to.size()))
        {
            text.replace(at, from.size(), to);
        }
    }

    // One self-contained copy of a stb header: implementation enabled, guard made
    // unique per `copy` so the preprocessor does not skip repeated copies.
    inline std::optional<std::string> LoadUnit(std::string_view directory, const StbUnit& unit,
        std::size_t copy)
    {
        auto source = ReadFile(std::string(directory) + "/" + unit.file);
        if (!source)
        {
            return std::nullopt;
        }

        if (copy > 0)
        {
            ReplaceAll(*source, unit.guard, std::string(unit.guard) + "_COPY" + std::to_string(copy));
        }

        return "#define " + std::string(unit.implementation_macro) + "\n" + *source + "\n";
    }

    // Builds a document of roughly `target_lines` lines (whole stb files only, so at
    // least 90% of the target), or std::nullopt if a header is missing. Targets
    // cycle lexer (~0.95k lines) -> tilemap (~4.2k) -> image (~8k): 1000 -> 0.95k,
    // 5000 -> 5.1k, 20000 -> 18.3k. Report CountLines() of the result, not the target.
    inline std::optional<std::string> BuildDocument(std::string_view directory,
        std::size_t target_lines)
    {
        std::string document;
        std::size_t copies[std::size(kStbUnits)] = {};
        for (std::size_t index = 0; CountLines(document) * 10 < target_lines * 9; ++index)
        {
            const std::size_t which = index % std::size(kStbUnits);
            auto unit = LoadUnit(directory, kStbUnits[which], copies[which] ++);
            if (!unit)
            {
                return std::nullopt;
            }

            document += *unit;
        }

        return document;
    }

} // namespace heimdall::bench
