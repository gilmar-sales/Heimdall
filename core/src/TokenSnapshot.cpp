#include <Heimdall/TokenSnapshot.hpp>

#include <algorithm>
#include <unordered_set>

namespace heimdall
{
    void TokenSnapshot::Append(Storage& storage, Slice slice)
    {
        if (slice.begin == slice.end)
        {
            return;
        }
        const auto end = (storage.ends.empty() ? 0 : storage.ends.back()) + slice.end - slice.begin;
        if (!storage.slices.empty())
        {
            auto& previous = storage.slices.back();
            if (previous.tokens == slice.tokens && previous.end == slice.begin &&
                previous.shift == slice.shift)
            {
                previous.end        = slice.end;
                storage.ends.back() = end;
                return;
            }
        }
        storage.slices.push_back(std::move(slice));
        storage.ends.push_back(end);
    }

    void TokenSnapshot::AppendTokens(Storage& storage, const std::vector<Token>& tokens)
    {
        for (std::size_t begin = 0; begin < tokens.size(); begin += kBlockTokens)
        {
            const auto end   = std::min(begin + kBlockTokens, tokens.size());
            auto       block = std::make_shared<const std::vector<Token>>(
                tokens.begin() + static_cast<std::ptrdiff_t>(begin),
                tokens.begin() + static_cast<std::ptrdiff_t>(end));
            Append(storage, { block, 0, block->size(), 0 });
        }
        storage.copiedTokens += tokens.size();
    }

    TokenSnapshot TokenSnapshot::Create(std::vector<Token> tokens)
    {
        auto storage = std::make_shared<Storage>();
        AppendTokens(*storage, tokens);
        return TokenSnapshot(std::move(storage));
    }

    std::size_t TokenSnapshot::Size() const noexcept
    {
        return mStorage->ends.empty() ? 0 : mStorage->ends.back();
    }

    Token TokenSnapshot::At(std::size_t index) const noexcept
    {
        const auto sliceIndex = static_cast<std::size_t>(
            std::upper_bound(mStorage->ends.begin(), mStorage->ends.end(), index) -
            mStorage->ends.begin());
        const auto& slice = mStorage->slices[sliceIndex];
        const auto  start = sliceIndex == 0 ? 0 : mStorage->ends[sliceIndex - 1];
        auto        token = (*slice.tokens)[slice.begin + index - start];
        token.offset =
            static_cast<std::uint32_t>(static_cast<std::ptrdiff_t>(token.offset) + slice.shift);
        return token;
    }

    std::size_t TokenSnapshot::LowerBound(std::size_t offset) const noexcept
    {
        std::size_t begin = 0;
        std::size_t end   = Size();
        while (begin < end)
        {
            const auto middle = begin + (end - begin) / 2;
            if (At(middle).offset < offset)
            {
                begin = middle + 1;
            }
            else
            {
                end = middle;
            }
        }
        return begin;
    }

    void TokenSnapshot::AppendRange(Storage& destination, std::size_t begin, std::size_t end,
                                    std::ptrdiff_t shift) const
    {
        auto sliceIndex = static_cast<std::size_t>(
            std::upper_bound(mStorage->ends.begin(), mStorage->ends.end(), begin) -
            mStorage->ends.begin());
        while (begin < end)
        {
            const auto& slice = mStorage->slices[sliceIndex];
            const auto  start = sliceIndex == 0 ? 0 : mStorage->ends[sliceIndex - 1];
            const auto  count = std::min(end, mStorage->ends[sliceIndex]) - begin;
            const auto  local = slice.begin + begin - start;
            Append(destination, { slice.tokens, local, local + count, slice.shift + shift });
            begin += count;
            ++sliceIndex;
        }
    }

    TokenSnapshot TokenSnapshot::Relex(const SourceSnapshot& source, Lexer::TextEdit edit,
                                       std::stop_token stop) const
    {
        const auto oldSize =
            Size() == 0 ? 0
                        : static_cast<std::size_t>(At(Size() - 1).offset) + At(Size() - 1).length;
        if (edit.offset > oldSize || edit.old_length > oldSize - edit.offset ||
            edit.offset > source.Size() || edit.new_length > source.Size() - edit.offset ||
            oldSize - edit.old_length != source.Size() - edit.new_length)
        {
            return Create(Lexer(*source.Materialize()).Lex(stop));
        }
        constexpr std::size_t kLookbehind = 32;
        const auto windowStart = edit.offset > kLookbehind ? edit.offset - kLookbehind : 0;
        auto       first       = LowerBound(windowStart);
        if (first != 0)
        {
            --first;
        }
        const auto         start       = first < Size() ? At(first).offset : 0;
        const auto         newEnd      = edit.offset + edit.new_length;
        const auto         shift       = static_cast<std::ptrdiff_t>(edit.new_length) -
                                         static_cast<std::ptrdiff_t>(edit.old_length);
        auto               windowBytes = std::min(source.Size() - start, kInitialWindowBytes);
        std::size_t        suffix      = Size();
        std::vector<Token> fresh;
        while (!stop.stop_requested())
        {
            const auto text    = source.Read(start, windowBytes);
            auto       scanned = Lexer(text).Lex(stop);
            bool       matched = false;
            fresh.clear();
            for (auto token : scanned)
            {
                if (start + windowBytes < source.Size() &&
                    static_cast<std::size_t>(token.offset) + token.length == windowBytes)
                {
                    break;
                }
                token.offset += static_cast<std::uint32_t>(start);
                if (token.offset >= newEnd)
                {
                    const auto oldOffset =
                        static_cast<std::size_t>(static_cast<std::ptrdiff_t>(token.offset) - shift);
                    const auto oldIndex = LowerBound(oldOffset);
                    if (oldIndex < Size())
                    {
                        const auto old = At(oldIndex);
                        if (old.offset == oldOffset && old.length == token.length &&
                            old.kind == token.kind && old.tok == token.tok)
                        {
                            suffix  = oldIndex;
                            matched = true;
                            break;
                        }
                    }
                }
                fresh.push_back(token);
            }
            if (matched || start + windowBytes == source.Size())
            {
                break;
            }
            windowBytes = std::min(source.Size() - start, windowBytes * 2);
        }
        auto storage = std::make_shared<Storage>();
        storage->slices.reserve(mStorage->slices.size() + 2);
        storage->ends.reserve(storage->slices.capacity());
        AppendRange(*storage, 0, first, 0);
        AppendTokens(*storage, fresh);
        AppendRange(*storage, suffix, Size(), shift);
        constexpr std::size_t kFragmentAllowance = 64;
        const auto            count              = storage->ends.empty() ? 0 : storage->ends.back();
        const auto            blocks             = (count + kBlockTokens - 1) / kBlockTokens;
        if (storage->slices.size() > blocks * 2 + kFragmentAllowance)
        {
            Compact(*storage);
        }
        return TokenSnapshot(std::move(storage));
    }

    void TokenSnapshot::Compact(Storage& storage)
    {
        Storage compact;
        compact.copiedTokens = storage.copiedTokens;
        std::vector<Token> pending;
        pending.reserve(kBlockTokens);
        const auto flush = [&]
        {
            AppendTokens(compact, pending);
            pending.clear();
        };
        for (const auto& slice : storage.slices)
        {
            if (slice.end - slice.begin >= kBlockTokens / 2)
            {
                flush();
                Append(compact, slice);
                continue;
            }
            for (std::size_t index = slice.begin; index < slice.end; ++index)
            {
                auto token   = (*slice.tokens)[index];
                token.offset = static_cast<std::uint32_t>(
                    static_cast<std::ptrdiff_t>(token.offset) + slice.shift);
                pending.push_back(token);
                if (pending.size() == kBlockTokens)
                {
                    flush();
                }
            }
        }
        flush();
        storage.slices       = std::move(compact.slices);
        storage.ends         = std::move(compact.ends);
        storage.copiedTokens = compact.copiedTokens;
    }

    std::shared_ptr<const std::vector<Token>> TokenSnapshot::Materialize() const
    {
        const std::lock_guard lock(mStorage->mutex);
        if (!mStorage->contiguous)
        {
            auto tokens = std::make_shared<std::vector<Token>>();
            tokens->reserve(Size());
            for (const auto& slice : mStorage->slices)
            {
                for (std::size_t i = slice.begin; i < slice.end; ++i)
                {
                    auto token   = (*slice.tokens)[i];
                    token.offset = static_cast<std::uint32_t>(
                        static_cast<std::ptrdiff_t>(token.offset) + slice.shift);
                    tokens->push_back(token);
                }
            }
            mStorage->contiguous = std::move(tokens);
        }
        return mStorage->contiguous;
    }

    std::size_t TokenSnapshot::CopiedTokens() const noexcept
    {
        return mStorage->copiedTokens;
    }

    bool TokenSnapshot::PreservesDirectives(const SourceSnapshot& before, Lexer::TextEdit edit,
                                            std::string_view replacement) const
    {
        constexpr std::string_view kSensitive = "\n\r#\\\"'/";
        if (edit.offset > before.Size() || edit.old_length > before.Size() - edit.offset ||
            edit.old_length > kDirectiveInspectionBytes ||
            replacement.find_first_of(kSensitive) != std::string_view::npos ||
            before.Read(edit.offset, edit.old_length).find_first_of(kSensitive) !=
                std::string::npos)
        {
            return false;
        }
        const auto lineStart = before.LineStart(edit.offset);
        const auto lineEnd   = before.LineEnd(edit.offset);
        if (before.Read(lineStart, lineEnd - lineStart).find('#') != std::string::npos)
        {
            return false;
        }
        if (lineStart != 0 &&
            before.Read(lineStart > 3 ? lineStart - 3 : 0, std::min<std::size_t>(lineStart, 3))
                    .find('\\') != std::string::npos)
        {
            return false;
        }
        const auto start = LowerBound(edit.offset);
        const auto end   = std::min(Size(), LowerBound(edit.offset + edit.old_length) + 2);
        for (std::size_t i = start > 1 ? start - 2 : 0; i < end; ++i)
        {
            const auto token = At(i);
            if (token.kind == TokenKind::LineComment || token.kind == TokenKind::BlockComment ||
                token.kind == TokenKind::RawStringLiteral ||
                token.kind == TokenKind::StringLiteral || token.kind == TokenKind::CharacterLiteral)
            {
                return false;
            }
            if (token.kind != TokenKind::Whitespace &&
                before.Read(token.offset, token.length).find_first_of(kSensitive) !=
                    std::string::npos)
            {
                return false;
            }
        }
        return true;
    }

    std::size_t TokenSnapshot::StorageBytes() const
    {
        std::unordered_set<const void*> counted;
        return StorageBytes(counted);
    }

    std::size_t TokenSnapshot::StorageBytes(std::unordered_set<const void*>& counted) const
    {
        if (!counted.insert(mStorage.get()).second)
        {
            return 0;
        }
        std::size_t bytes = mStorage->slices.capacity() * sizeof(Slice) +
                            mStorage->ends.capacity() * sizeof(std::size_t);
        for (const auto& slice : mStorage->slices)
        {
            if (counted.insert(slice.tokens->data()).second)
            {
                bytes += slice.tokens->capacity() * sizeof(Token);
            }
        }
        const std::lock_guard lock(mStorage->mutex);
        if (mStorage->contiguous && counted.insert(mStorage->contiguous->data()).second)
        {
            bytes += mStorage->contiguous->capacity() * sizeof(Token);
        }
        return bytes;
    }
} // namespace heimdall
