#include <gtest/gtest.h>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{

    std::vector<fs::path> CoreFiles()
    {
        std::vector<fs::path> out;
        const fs::path        root(HEIMDALL_SOURCE_DIR);
        for (const char* sub : { "core/include/Heimdall", "core/src" })
        {
            for (const auto& entry : fs::recursive_directory_iterator(root / sub))
            {
                const auto ext = entry.path().extension();
                if (ext == ".hpp" || ext == ".cpp")
                {
                    out.push_back(entry.path());
                }
            }
        }

        return out;
    }

    std::string ReadFile(const fs::path& path)
    {
        std::ifstream      in(path, std::ios::binary);
        std::ostringstream out;
        out << in.rdbuf();
        return out.str();
    }

    // Strips // line comments, /* */ block comments and "..." / '...'
    // literals so policy scans don't trip on prose.
    std::string StripNoise(const std::string& text)
    {
        std::string out;
        out.reserve(text.size());
        char quote         = '\0';
        bool line_comment  = false;
        bool block_comment = false;
        for (std::size_t i = 0; i < text.size(); ++i)
        {
            const char c    = text[i];
            const char next = i + 1 < text.size() ? text[i + 1] : '\0';
            if (line_comment)
            {
                if (c == '\n')
                {
                    line_comment = false;
                    out += c;
                }

                continue;
            }

            if (block_comment)
            {
                if (c == '*' && next == '/')
                {
                    block_comment = false;
                    ++i;
                }

                continue;
            }

            if (quote != '\0')
            {
                if (c == '\\' && next != '\0')
                {
                    ++i;
                }
                else if (c == quote)
                {
                    quote = '\0';
                }

                continue;
            }

            if (c == '/' && next == '/')
            {
                line_comment = true;
                ++i;
                continue;
            }

            if (c == '/' && next == '*')
            {
                block_comment = true;
                ++i;
                continue;
            }

            if (c == '"' || c == '\'')
            {
                quote = c;
                continue;
            }

            out += c;
        }

        return out;
    }

    bool HasWord(const std::string& text, const std::string& word)
    {
        for (std::size_t pos = text.find(word); pos != std::string::npos;
             pos             = text.find(word, pos + 1))
        {
            const bool left_ok =
                pos == 0 ||
                (!std::isalnum(static_cast<unsigned char>(text[pos - 1])) && text[pos - 1] != '_');
            const std::size_t end = pos + word.size();
            const bool        right_ok =
                end >= text.size() ||
                (!std::isalnum(static_cast<unsigned char>(text[end])) && text[end] != '_');
            if (left_ok && right_ok)
            {
                return true;
            }
        }

        return false;
    }

} // namespace

TEST(CorePolicy, NoThirdPartyQuotedIncludes)
{
    for (const auto& file : CoreFiles())
    {
        std::istringstream in(StripNoise(ReadFile(file)));
        std::string        line;
        while (std::getline(in, line))
        {
            const auto pos = line.find("#include");
            if (pos == std::string::npos)
            {
                continue;
            }

            const auto quote = line.find('"', pos);
            if (quote == std::string::npos)
            {
                continue; // angle <...> include: standard library / platform SDK
            }

            EXPECT_TRUE(line.compare(quote + 1, 9, "Heimdall/") == 0)
                << file << ": quoted include outside Heimdall/: " << line;
        }
    }
}

TEST(CorePolicy, NoSemanticHeadersInCore)
{
    for (const auto& file : CoreFiles())
    {
        const std::string code = StripNoise(ReadFile(file));
        EXPECT_TRUE(code.find("CompileDatabase.hpp") == std::string::npos)
            << file << ": core must not include semantic CompileDatabase.hpp";
        EXPECT_TRUE(code.find("SemanticAnalyzer.hpp") == std::string::npos)
            << file << ": core must not include semantic SemanticAnalyzer.hpp";
    }
}

TEST(CorePolicy, NoExceptionsOrRtti)
{
    for (const auto& file : CoreFiles())
    {
        const std::string code = StripNoise(ReadFile(file));
        for (const char* banned : { "throw", "try", "catch", "typeid", "dynamic_cast" })
        {
            EXPECT_FALSE(HasWord(code, banned))
                << file << ": banned `" << banned << "` in dependency-free core";
        }
    }
}
