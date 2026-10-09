#include "FileDiscovery.hpp"

#include <algorithm>
#include <iostream>
#include <string>
#include <system_error>

namespace heimdall::cli
{
    namespace
    {

        constexpr char kSlash        = '/';
        constexpr char kBackslash    = '\\';
        constexpr char kStar         = '*';
        constexpr char kQuestion     = '?';
        constexpr char kBracketOpen  = '[';
        constexpr char kBracketClose = ']';

        std::string NormalizeSeparators(std::string_view input)
        {
            std::string out(input);
            std::replace(out.begin(), out.end(), kBackslash, kSlash);
            return out;
        }

        bool HasGlobChars(std::string_view text)
        {
            return text.find(kStar) != std::string_view::npos ||
                   text.find(kQuestion) != std::string_view::npos ||
                   text.find(kBracketOpen) != std::string_view::npos;
        }

        bool MatchBracket(std::string_view pattern, std::size_t& pi, char value)
        {
            // pattern[pi] == '[' on entry. Consumes through ']' and reports
            // whether `value` is in the class. Supports [!...]/[^...] negation
            // and [a-z] ranges. Returns false on unterminated classes.
            constexpr std::size_t kClassHeaderLen = 2;
            const std::size_t     open            = pi;
            ++pi;
            if (pi >= pattern.size())
            {
                return false;
            }

            bool negated = false;
            if (pattern[pi] == '!' || pattern[pi] == '^')
            {
                negated = true;
                ++pi;
            }

            bool matched   = false;
            bool has_item  = false;
            char prev      = '\0';
            bool have_prev = false;
            while (pi < pattern.size() && pattern[pi] != kBracketClose)
            {
                if (pattern[pi] == '-' && have_prev && pi + 1 < pattern.size() &&
                    pattern[pi + 1] != kBracketClose)
                {
                    const char lo = prev;
                    const char hi = pattern[pi + 1];
                    if (lo <= value && value <= hi)
                    {
                        matched = true;
                    }

                    pi += kClassHeaderLen;
                    have_prev = false;
                    has_item  = true;
                    continue;
                }

                if (pattern[pi] == value)
                {
                    matched = true;
                }

                prev      = pattern[pi];
                have_prev = true;
                has_item  = true;
                ++pi;
            }

            if (pi >= pattern.size() || !has_item)
            {
                pi = open;
                return false;
            }

            ++pi; // consume ']'
            return negated ? !matched : matched;
        }

        bool MatchGlobRec(
            std::string_view pattern, std::size_t pi, std::string_view text, std::size_t si)
        {
            while (pi < pattern.size())
            {
                const char p = pattern[pi];
                if (p == kStar)
                {
                    const bool is_double = pi + 1 < pattern.size() && pattern[pi + 1] == kStar;
                    if (is_double)
                    {
                        std::size_t after = pi + 1;
                        while (after < pattern.size() && pattern[after] == kStar)
                        {
                            ++after;
                        }

                        if (after >= pattern.size())
                        {
                            return true;
                        }

                        if (pattern[after] == kSlash)
                        {
                            // "**/" matches zero or more directories.
                            if (MatchGlobRec(pattern, after + 1, text, si))
                            {
                                return true;
                            }

                            for (std::size_t k = si; k < text.size(); ++k)
                            {
                                if (text[k] == kSlash &&
                                    MatchGlobRec(pattern, after + 1, text, k + 1))
                                {
                                    return true;
                                }
                            }

                            return false;
                        }

                        for (std::size_t k = si; k <= text.size(); ++k)
                        {
                            if (MatchGlobRec(pattern, after, text, k))
                            {
                                return true;
                            }
                        }

                        return false;
                    }

                    // Single '*': any run without '/'.
                    if (MatchGlobRec(pattern, pi + 1, text, si))
                    {
                        return true;
                    }

                    std::size_t k = si;
                    while (k < text.size() && text[k] != kSlash)
                    {
                        ++k;
                        if (MatchGlobRec(pattern, pi + 1, text, k))
                        {
                            return true;
                        }
                    }

                    return false;
                }

                if (p == kQuestion)
                {
                    if (si >= text.size() || text[si] == kSlash)
                    {
                        return false;
                    }

                    ++pi;
                    ++si;
                    continue;
                }

                if (p == kBracketOpen)
                {
                    if (si >= text.size() || text[si] == kSlash)
                    {
                        return false;
                    }

                    if (!MatchBracket(pattern, pi, text[si]))
                    {
                        return false;
                    }

                    ++si;
                    continue;
                }

                if (si >= text.size() || p != text[si])
                {
                    return false;
                }

                ++pi;
                ++si;
            }

            return si == text.size();
        }

        bool ExpandGlob(const std::string& raw_input, std::vector<std::filesystem::path>& files)
        {
            const std::string normalized = NormalizeSeparators(raw_input);
            const std::size_t first_glob = normalized.find_first_of("*?[");
            if (first_glob == std::string::npos)
            {
                return false;
            }

            const std::size_t slash_before = normalized.rfind(kSlash, first_glob);
            std::string       base_str;
            std::string       remainder;
            if (slash_before == std::string::npos)
            {
                base_str  = ".";
                remainder = normalized;
            }
            else
            {
                base_str  = normalized.substr(0, slash_before);
                remainder = normalized.substr(slash_before + 1);
                if (base_str.empty())
                {
                    base_str = "/";
                }
            }

            const std::filesystem::path base_path(base_str);
            std::error_code             ec;
            if (!std::filesystem::exists(base_path, ec) || ec ||
                !std::filesystem::is_directory(base_path, ec) || ec)
            {
                std::cerr << "no matches for pattern: " << raw_input << '\n';
                return true;
            }

            const bool recursive = remainder.find("**") != std::string::npos ||
                                   remainder.find(kSlash) != std::string::npos;

            std::vector<std::filesystem::path> matched;
            if (recursive)
            {
                for (std::filesystem::recursive_directory_iterator it(base_path, ec), end;
                     it != end && !ec; it.increment(ec))
                {
                    if (ec)
                    {
                        break;
                    }

                    if (!it->is_regular_file(ec) || ec)
                    {
                        continue;
                    }

                    const auto candidate = it->path();
                    if (!IsSourceFile(candidate))
                    {
                        continue;
                    }

                    auto relative = std::filesystem::relative(candidate, base_path, ec);
                    if (ec)
                    {
                        continue;
                    }

                    const std::string rel_norm = NormalizeSeparators(relative.generic_string());
                    if (MatchGlobPattern(remainder, rel_norm))
                    {
                        matched.push_back(candidate);
                    }
                }
            }
            else
            {
                for (std::filesystem::directory_iterator it(base_path, ec), end; it != end && !ec;
                     it.increment(ec))
                {
                    if (ec)
                    {
                        break;
                    }

                    if (!it->is_regular_file(ec) || ec)
                    {
                        continue;
                    }

                    const auto candidate = it->path();
                    if (!IsSourceFile(candidate))
                    {
                        continue;
                    }

                    auto relative = std::filesystem::relative(candidate, base_path, ec);
                    if (ec)
                    {
                        continue;
                    }

                    const std::string rel_norm = NormalizeSeparators(relative.generic_string());
                    if (MatchGlobPattern(remainder, rel_norm))
                    {
                        matched.push_back(candidate);
                    }
                }
            }

            if (ec)
            {
                std::cerr << "error traversing pattern base " << base_str << ": " << ec.message()
                          << '\n';
                return true;
            }

            if (matched.empty())
            {
                std::cerr << "no matches for pattern: " << raw_input << '\n';
                return true;
            }

            files.insert(files.end(), matched.begin(), matched.end());
            return true;
        }

    } // namespace

    bool IsSourceFile(const std::filesystem::path& path)
    {
        const auto extension = path.extension().string();
        return extension == ".c" || extension == ".cc" || extension == ".cpp" ||
               extension == ".cxx" || extension == ".h" || extension == ".hh" ||
               extension == ".hpp" || extension == ".hxx";
    }

    bool MatchGlobPattern(std::string_view pattern, std::string_view text)
    {
        return MatchGlobRec(NormalizeSeparators(pattern), 0, NormalizeSeparators(text), 0);
    }

    bool CollectFiles(const std::vector<std::filesystem::path>& inputs,
                      std::vector<std::filesystem::path>&       files)
    {
        for (const auto& input : inputs)
        {
            const std::string raw = input.string();
            if (HasGlobChars(NormalizeSeparators(raw)))
            {
                ExpandGlob(raw, files);
                continue;
            }

            std::error_code ec;
            if (!std::filesystem::exists(input, ec) || ec)
            {
                std::cerr << "path does not exist: " << input.string() << '\n';
                return false;
            }

            if (std::filesystem::is_regular_file(input, ec))
            {
                if (IsSourceFile(input))
                {
                    files.push_back(input);
                }
                else
                {
                    std::cerr << "skipping unsupported file: " << input.string() << '\n';
                }
            }
            else if (std::filesystem::is_directory(input, ec))
            {
                for (std::filesystem::recursive_directory_iterator it(input, ec), end;
                     it != end && !ec; it.increment(ec))
                {
                    if (it->is_regular_file(ec) && IsSourceFile(it->path()))
                    {
                        files.push_back(it->path());
                    }
                }

                if (ec)
                {
                    std::cerr << "error traversing directory " << input.string() << ": "
                              << ec.message() << '\n';
                    return false;
                }
            }
            else
            {
                std::cerr << "not a regular file or directory: " << input.string() << '\n';
                return false;
            }
        }

        std::sort(files.begin(), files.end());
        files.erase(std::unique(files.begin(), files.end()), files.end());
        if (files.empty())
        {
            std::cerr << "no C++ source files found\n";
            return false;
        }

        return true;
    }

} // namespace heimdall::cli
