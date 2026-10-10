#include "WorkspaceSymbolIndex.hpp"

#include <algorithm>
#include <array>
#include <compare>
#include <cstdint>
#include <mutex>
#include <ranges>
#include <utility>

namespace heimdall::lsp
{

    namespace
    {

        constexpr std::size_t kCancellationStride = 64;
        constexpr std::size_t kReserveCap         = 1024;
        constexpr int kNoMatch                    = -1;
        constexpr std::uint32_t kGlobalContainer  = 0;
        constexpr std::string_view kScopeSeparator = "::";

        // Bit layout of CharacterMask: a-z, 0-9, '_', ':' then 26 hash buckets for the rest.
        constexpr unsigned kLetterCount = 26;
        constexpr unsigned kDigitBase   = kLetterCount;
        constexpr unsigned kDigitCount  = 10;
        constexpr unsigned kUnderscore  = kDigitBase + kDigitCount;
        constexpr unsigned kColon       = kUnderscore + 1;
        constexpr unsigned kOtherBase   = kColon + 1;

        enum Tier : int
        {
            kExact,
            kPrefix,
            kSubstring,
            kNameSubsequence,
            kQualifiedSubsequence
        };

        [[nodiscard]] char Lower(char c) noexcept
        {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        }

        [[nodiscard]] std::string LowerCopy(std::string_view text)
        {
            std::string lowered(text);
            std::ranges::transform(lowered, lowered.begin(), Lower);
            return lowered;
        }

        [[nodiscard]] unsigned BitOf(char folded) noexcept
        {
            if (folded >= 'a' && folded <= 'z')
            {
                return static_cast<unsigned>(folded - 'a');
            }

            if (folded >= '0' && folded <= '9')
            {
                return kDigitBase + static_cast<unsigned>(folded - '0');
            }

            if (folded == '_')
            {
                return kUnderscore;
            }

            if (folded == ':')
            {
                return kColon;
            }

            return kOtherBase +
                   static_cast<unsigned>(static_cast<unsigned char>(folded)) % kLetterCount;
        }

        [[nodiscard]] bool EqualsInsensitive(std::string_view needle,
                                             std::string_view text) noexcept
        {
            return needle.size() == text.size() &&
                   std::ranges::equal(needle, text, [](char a, char b) { return a == Lower(b); });
        }

        [[nodiscard]] bool PrefixInsensitive(std::string_view needle,
                                             std::string_view text) noexcept
        {
            return text.size() >= needle.size() &&
                   EqualsInsensitive(needle, text.substr(0, needle.size()));
        }

        [[nodiscard]] bool SubstringInsensitive(std::string_view needle,
                                                std::string_view text) noexcept
        {
            for (std::size_t at = 0; at + needle.size() <= text.size(); ++at)
            {
                if (EqualsInsensitive(needle, text.substr(at, needle.size())))
                {
                    return true;
                }
            }

            return false;
        }

        // `needle` appears in order, not necessarily adjacent, across the concatenated pieces.
        [[nodiscard]] bool SubsequenceInsensitive(
            std::string_view needle, std::span<const std::string_view> pieces) noexcept
        {
            std::size_t matched = 0;
            for (const std::string_view piece : pieces)
            {
                for (const char c : piece)
                {
                    if (matched < needle.size() && Lower(c) == needle[matched])
                    {
                        ++matched;
                    }
                }
            }

            return matched == needle.size();
        }

        struct Request
        {
            std::string_view needle;
            std::uint64_t mask = 0;
            bool qualified     = false;
        };

        [[nodiscard]] int RankQualified(const Request& request, const SymbolView& symbol)
        {
            // Reused across symbols of this thread: no allocation per candidate.
            thread_local std::string text;
            text.assign(symbol.container);
            if (!text.empty())
            {
                text += kScopeSeparator;
            }

            text += symbol.name;
            if (EqualsInsensitive(request.needle, text))
            {
                return kExact;
            }

            if (PrefixInsensitive(request.needle, text))
            {
                return kPrefix;
            }

            if (SubstringInsensitive(request.needle, text))
            {
                return kSubstring;
            }

            const std::array<std::string_view, 1> whole = {text};
            return SubsequenceInsensitive(request.needle, whole) ? kNameSubsequence : kNoMatch;
        }

        // Tiers that only look at the name.
        [[nodiscard]] int RankByName(const Request& request,
                                     const SymbolView& symbol,
                                     std::uint64_t nameMask)
        {
            if ((request.mask & ~nameMask) != 0)
            {
                return kNoMatch;
            }

            if (EqualsInsensitive(request.needle, symbol.name))
            {
                return kExact;
            }

            if (PrefixInsensitive(request.needle, symbol.name))
            {
                return kPrefix;
            }

            if (SubstringInsensitive(request.needle, symbol.name))
            {
                return kSubstring;
            }

            const std::array<std::string_view, 1> name = {symbol.name};
            return SubsequenceInsensitive(request.needle, name) ? kNameSubsequence : kNoMatch;
        }

        [[nodiscard]] bool MatchesQualifiedSubsequence(const Request& request,
                                                       const SymbolView& symbol)
        {
            const std::string_view qualified[] = {symbol.container, kScopeSeparator, symbol.name};
            return !symbol.container.empty() && SubsequenceInsensitive(request.needle, qualified);
        }

        // Anonymous scopes are invisible to lookup, so they are not part of the qualified name:
        // `a::(anonymous namespace)::f` is listed as `a::f`.
        [[nodiscard]] std::string WithoutAnonymousScopes(std::string_view container)
        {
            constexpr std::string_view kMarker = "(anonymous";
            std::string result(container);
            for (std::size_t at = result.find(kMarker); at != std::string::npos;
                 at             = result.find(kMarker))
            {
                const std::size_t close = result.find(')', at);
                if (close == std::string::npos)
                {
                    break;
                }

                std::size_t end   = close + 1;
                std::size_t begin = at;
                if (result.compare(end, kScopeSeparator.size(), kScopeSeparator) == 0)
                {
                    end += kScopeSeparator.size();
                }
                else if (begin >= kScopeSeparator.size() &&
                         result.compare(begin - kScopeSeparator.size(), kScopeSeparator.size(),
                                        kScopeSeparator) == 0)
                {
                    begin -= kScopeSeparator.size();
                }

                result.erase(begin, end - begin);
            }

            return result;
        }

        // `key` orders by rank, then name length: most comparisons never read any text.
        struct Candidate
        {
            std::uint64_t key;
            std::uint32_t file;
            std::uint32_t symbol;
        };

        constexpr unsigned kRankShift = 32;

        [[nodiscard]] std::uint64_t KeyOf(int rank, std::size_t nameLength) noexcept
        {
            return (static_cast<std::uint64_t>(rank) << kRankShift) | nameLength;
        }

    } // namespace

    std::uint64_t CharacterMask(std::string_view text) noexcept
    {
        std::uint64_t mask = 0;
        for (const char c : text)
        {
            mask |= std::uint64_t {1} << BitOf(Lower(c));
        }

        return mask;
    }

    SymbolView IndexedFile::View(std::size_t index) const noexcept
    {
        const IndexedSymbol& symbol = symbols[index];
        return {std::string_view(names).substr(symbol.nameOffset, symbol.nameLength),
                containers[symbol.container],
                symbol.kind,
                {symbol.startLine, symbol.startCharacter},
                {symbol.endLine, symbol.endCharacter}};
    }

    IndexedFilePtr BuildIndexedFile(std::string uri,
                                    std::span<const heimdall::OutlineSymbol> symbols,
                                    const LineIndex& lines)
    {
        auto file = std::make_shared<IndexedFile>();
        file->uri = std::move(uri);
        file->symbols.reserve(symbols.size());
        file->nameMasks.reserve(symbols.size());
        file->qualifiedMasks.reserve(symbols.size());
        std::unordered_map<std::string, std::uint32_t> interned = {
            {std::string(), kGlobalContainer}};
        for (const heimdall::OutlineSymbol& symbol : symbols)
        {
            if (symbol.name.empty() || symbol.name.front() == '(')
            {
                continue;
            }

            std::string container = WithoutAnonymousScopes(symbol.container);
            std::uint32_t slot    = kGlobalContainer;
            if (!container.empty())
            {
                const auto found = interned.find(container);
                if (found != interned.end())
                {
                    slot = found->second;
                }
                else
                {
                    slot = static_cast<std::uint32_t>(file->containers.size());
                    file->containers.push_back(container);
                    interned.emplace(std::move(container), slot);
                }
            }

            const Position start = lines.ToPosition(symbol.nameOffset);
            const Position end   = lines.ToPosition(symbol.nameOffset + symbol.nameLength);
            IndexedSymbol record;
            record.nameOffset     = static_cast<std::uint32_t>(file->names.size());
            record.nameLength     = static_cast<std::uint32_t>(symbol.name.size());
            record.container      = slot;
            record.startLine      = static_cast<std::uint32_t>(start.line);
            record.startCharacter = static_cast<std::uint32_t>(start.character);
            record.endLine        = static_cast<std::uint32_t>(end.line);
            record.endCharacter   = static_cast<std::uint32_t>(end.character);
            record.kind           = symbol.kind;
            file->names += symbol.name;
            file->symbols.push_back(record);
            const std::uint64_t nameMask = CharacterMask(symbol.name);
            file->nameMasks.push_back(nameMask);
            file->qualifiedMasks.push_back(
                nameMask | CharacterMask(file->containers[slot]) |
                (slot == kGlobalContainer ? 0 : std::uint64_t {1} << kColon));
        }

        file->names.shrink_to_fit();
        file->symbols.shrink_to_fit();
        file->nameMasks.shrink_to_fit();
        file->qualifiedMasks.shrink_to_fit();
        return file;
    }

    void WorkspaceSymbolIndex::SetDisk(const std::string& key, IndexedFilePtr file)
    {
        const std::lock_guard<std::mutex> lock(mMutex);
        mDisk[key] = std::move(file);
    }

    void WorkspaceSymbolIndex::RemoveDisk(const std::string& key)
    {
        const std::lock_guard<std::mutex> lock(mMutex);
        mDisk.erase(key);
    }

    void WorkspaceSymbolIndex::RemoveDiskUnder(const std::string& key)
    {
        const std::string folder = key + '/';
        const std::lock_guard<std::mutex> lock(mMutex);
        std::erase_if(mDisk, [&](const auto& entry)
        {
            return entry.first == key || entry.first.starts_with(folder);
        });
    }

    void WorkspaceSymbolIndex::SetOpen(const std::string& key, IndexedFilePtr file)
    {
        const std::lock_guard<std::mutex> lock(mMutex);
        mOpen[key] = std::move(file);
    }

    void WorkspaceSymbolIndex::ClearOpen(const std::string& key)
    {
        const std::lock_guard<std::mutex> lock(mMutex);
        mOpen.erase(key);
    }

    std::size_t WorkspaceSymbolIndex::FileCount() const
    {
        const std::lock_guard<std::mutex> lock(mMutex);
        std::size_t count = mOpen.size();
        for (const auto& entry : mDisk)
        {
            count += mOpen.contains(entry.first) ? 0 : 1;
        }

        return count;
    }

    std::vector<SymbolHit> WorkspaceSymbolIndex::Query(std::string_view query,
                                                       std::size_t limit,
                                                       std::stop_token stop) const
    {
        while (!query.empty() && query.front() == ' ')
        {
            query.remove_prefix(1);
        }

        while (!query.empty() && query.back() == ' ')
        {
            query.remove_suffix(1);
        }

        if (query.empty() || limit == 0)
        {
            return {};
        }

        const std::string needle = LowerCopy(query);
        const Request request {needle, CharacterMask(needle),
                               needle.find(kScopeSeparator) != std::string::npos};

        std::vector<IndexedFilePtr> files;
        {
            const std::lock_guard<std::mutex> lock(mMutex);
            files.reserve(mOpen.size() + mDisk.size());
            for (const IndexedFilePtr& file : mOpen | std::views::values)
            {
                files.push_back(file);
            }

            for (const auto& [key, file] : mDisk)
            {
                if (!mOpen.contains(key))
                {
                    files.push_back(file);
                }
            }
        }

        const auto before = [&files](const Candidate& a, const Candidate& b)
        {
            if (a.key != b.key)
            {
                return a.key < b.key;
            }

            const SymbolView left  = files[a.file]->View(a.symbol);
            const SymbolView right = files[b.file]->View(b.symbol);
            if (const auto order = left.name <=> right.name; order != 0)
            {
                return order < 0;
            }

            if (const auto order = left.container <=> right.container; order != 0)
            {
                return order < 0;
            }

            if (const auto order = files[a.file]->uri <=> files[b.file]->uri; order != 0)
            {
                return order < 0;
            }

            return std::pair {left.start.line, left.start.character} <
                   std::pair {right.start.line, right.start.character};
        };

        // Max-heap of the best `limit` candidates so far: a candidate that does not beat the
        // worst one kept costs a single comparison, however many symbols match.
        std::vector<Candidate> candidates;
        candidates.reserve(std::min<std::size_t>(limit, kReserveCap) + 1);
        const auto consider = [&](int rank, std::size_t fileIndex, std::size_t index,
                                  std::size_t nameLength)
        {
            const Candidate candidate {KeyOf(rank, nameLength),
                                       static_cast<std::uint32_t>(fileIndex),
                                       static_cast<std::uint32_t>(index)};
            if (candidates.size() < limit)
            {
                candidates.push_back(candidate);
                std::ranges::push_heap(candidates, before);
            }
            else if (before(candidate, candidates.front()))
            {
                std::ranges::pop_heap(candidates, before);
                candidates.back() = candidate;
                std::ranges::push_heap(candidates, before);
            }
        };

        // Pass 1 ranks by name (or by the qualified text when the query contains `::`).
        for (std::size_t fileIndex = 0; fileIndex < files.size(); ++fileIndex)
        {
            if (fileIndex % kCancellationStride == 0 && stop.stop_requested())
            {
                return {};
            }

            const IndexedFile& file = *files[fileIndex];
            for (std::size_t i = 0; i < file.symbols.size(); ++i)
            {
                // Every match contains all of the query's characters.
                if ((request.mask & ~file.qualifiedMasks[i]) != 0)
                {
                    continue;
                }

                const SymbolView symbol = file.View(i);
                const int rank = request.qualified ? RankQualified(request, symbol)
                                                   : RankByName(request, symbol, file.nameMasks[i]);
                if (rank != kNoMatch)
                {
                    consider(rank, fileIndex, i, symbol.name.size());
                }
            }
        }

        // Pass 2 only when the name matches did not fill the result: subsequences of the
        // qualified name (`wr` finds `Widget::Run`).
        if (!request.qualified && candidates.size() < limit)
        {
            for (std::size_t fileIndex = 0; fileIndex < files.size(); ++fileIndex)
            {
                if (fileIndex % kCancellationStride == 0 && stop.stop_requested())
                {
                    return {};
                }

                const IndexedFile& file = *files[fileIndex];
                for (std::size_t i = 0; i < file.symbols.size(); ++i)
                {
                    if ((request.mask & ~file.qualifiedMasks[i]) != 0)
                    {
                        continue;
                    }

                    const SymbolView symbol = file.View(i);
                    if (RankByName(request, symbol, file.nameMasks[i]) == kNoMatch &&
                        MatchesQualifiedSubsequence(request, symbol))
                    {
                        consider(kQualifiedSubsequence, fileIndex, i, symbol.name.size());
                    }
                }
            }
        }

        std::ranges::sort_heap(candidates, before);
        std::vector<SymbolHit> hits;
        hits.reserve(candidates.size());
        for (const Candidate& candidate : candidates)
        {
            hits.emplace_back(files[candidate.file], candidate.symbol);
        }

        return hits;
    }

} // namespace heimdall::lsp
