#include <Heimdall/SourceSnapshot.hpp>

#include <algorithm>
#include <unordered_set>

namespace heimdall
{
    std::expected<SourceSnapshot, SourceEditError> SourceSnapshot::Create(std::string_view source)
    {
        if (source.size() > kMaxSourceBytes)
        {
            return std::unexpected(SourceEditError::SizeLimit);
        }
        auto storage = std::make_shared<Storage>();
        storage->pieces.reserve((source.size() + kChunkBytes - 1) / kChunkBytes);
        storage->ends.reserve(storage->pieces.capacity());
        for (std::size_t offset = 0; offset < source.size(); offset += kChunkBytes)
        {
            auto text = std::make_shared<const std::string>(source.substr(offset, kChunkBytes));
            Append(*storage, { text, 0, text->size() });
        }
        storage->copiedBytes = source.size();
        return SourceSnapshot(std::move(storage));
    }

    void SourceSnapshot::Append(Storage& storage, Piece piece)
    {
        if (piece.length == 0)
        {
            return;
        }
        const auto end = (storage.ends.empty() ? 0 : storage.ends.back()) + piece.length;
        if (!storage.pieces.empty())
        {
            auto& previous = storage.pieces.back();
            if (previous.text == piece.text && previous.offset + previous.length == piece.offset)
            {
                previous.length += piece.length;
                storage.ends.back() = end;
                return;
            }
        }
        storage.pieces.push_back(std::move(piece));
        storage.ends.push_back(end);
    }

    void SourceSnapshot::AppendRange(Storage& destination, std::size_t offset,
                                     std::size_t length) const
    {
        auto index = static_cast<std::size_t>(
            std::upper_bound(mStorage->ends.begin(), mStorage->ends.end(), offset) -
            mStorage->ends.begin());
        while (length != 0 && index < ChunkCount())
        {
            const auto& piece = mStorage->pieces[index];
            const auto  local = offset - ChunkOffset(index);
            const auto  count = std::min(length, piece.length - local);
            Append(destination, { piece.text, piece.offset + local, count });
            offset += count;
            length -= count;
            ++index;
        }
    }

    std::expected<SourceSnapshot, SourceEditError> SourceSnapshot::Edit(
        std::size_t offset, std::size_t oldLength, std::string_view replacement) const
    {
        if (offset > Size() || oldLength > Size() - offset)
        {
            return std::unexpected(SourceEditError::InvalidRange);
        }
        if (replacement.size() > kMaxSourceBytes - (Size() - oldLength))
        {
            return std::unexpected(SourceEditError::SizeLimit);
        }
        if (oldLength == 0 && replacement.empty())
        {
            return *this;
        }
        auto storage = std::make_shared<Storage>();
        storage->pieces.reserve(ChunkCount() +
                                (replacement.size() + kChunkBytes - 1) / kChunkBytes + 2);
        storage->ends.reserve(storage->pieces.capacity());
        AppendRange(*storage, 0, offset);
        for (std::size_t i = 0; i < replacement.size(); i += kChunkBytes)
        {
            auto text = std::make_shared<const std::string>(replacement.substr(i, kChunkBytes));
            Append(*storage, { text, 0, text->size() });
        }
        AppendRange(*storage, offset + oldLength, Size() - offset - oldLength);
        storage->copiedBytes                     = replacement.size();
        constexpr std::size_t kFragmentAllowance = 64;
        const auto            chunks =
            (Size() - oldLength + replacement.size() + kChunkBytes - 1) / kChunkBytes;
        if (storage->pieces.size() > chunks * 2 + kFragmentAllowance)
        {
            Compact(*storage);
        }
        return SourceSnapshot(std::move(storage));
    }

    void SourceSnapshot::Compact(Storage& storage)
    {
        Storage compact;
        compact.copiedBytes = storage.copiedBytes;
        std::string pending;
        const auto  flush = [&]
        {
            if (!pending.empty())
            {
                auto text = std::make_shared<const std::string>(std::move(pending));
                compact.copiedBytes += text->size();
                Append(compact, { text, 0, text->size() });
                pending.clear();
            }
        };
        for (const auto& piece : storage.pieces)
        {
            if (piece.length >= kChunkBytes / 2)
            {
                flush();
                Append(compact, piece);
            }
            else
            {
                if (pending.size() + piece.length > kChunkBytes)
                {
                    flush();
                }
                pending.append(*piece.text, piece.offset, piece.length);
            }
        }
        flush();
        storage.pieces      = std::move(compact.pieces);
        storage.ends        = std::move(compact.ends);
        storage.copiedBytes = compact.copiedBytes;
    }

    std::size_t SourceSnapshot::Size() const noexcept
    {
        return mStorage->ends.empty() ? 0 : mStorage->ends.back();
    }

    std::string SourceSnapshot::Read(std::size_t offset, std::size_t length) const
    {
        if (offset >= Size())
        {
            return {};
        }
        length = std::min(length, Size() - offset);
        std::string result;
        result.reserve(length);
        auto index = static_cast<std::size_t>(
            std::upper_bound(mStorage->ends.begin(), mStorage->ends.end(), offset) -
            mStorage->ends.begin());
        while (length != 0)
        {
            const auto local = offset - ChunkOffset(index);
            const auto chunk = Chunk(index).substr(local, length);
            result.append(chunk);
            offset += chunk.size();
            length -= chunk.size();
            ++index;
        }
        return result;
    }

    std::shared_ptr<const std::string> SourceSnapshot::Materialize() const
    {
        const std::lock_guard lock(mStorage->mutex);
        if (!mStorage->contiguous)
        {
            auto text = std::make_shared<std::string>();
            text->reserve(Size());
            for (std::size_t i = 0; i < ChunkCount(); ++i)
            {
                text->append(Chunk(i));
            }
            mStorage->contiguous = std::move(text);
        }
        return mStorage->contiguous;
    }

    std::size_t SourceSnapshot::ChunkCount() const noexcept
    {
        return mStorage->pieces.size();
    }

    std::string_view SourceSnapshot::Chunk(std::size_t index) const noexcept
    {
        const auto& piece = mStorage->pieces[index];
        return std::string_view(*piece.text).substr(piece.offset, piece.length);
    }

    std::size_t SourceSnapshot::ChunkOffset(std::size_t index) const noexcept
    {
        return index == 0 ? 0 : mStorage->ends[index - 1];
    }

    std::size_t SourceSnapshot::CopiedBytes() const noexcept
    {
        return mStorage->copiedBytes;
    }

    std::size_t SourceSnapshot::LineStart(std::size_t offset) const noexcept
    {
        offset = std::min(offset, Size());
        if (offset == 0)
        {
            return 0;
        }
        auto index = static_cast<std::size_t>(
            std::lower_bound(mStorage->ends.begin(), mStorage->ends.end(), offset) -
            mStorage->ends.begin());
        for (;;)
        {
            const auto start = ChunkOffset(index);
            const auto found = Chunk(index).substr(0, offset - start).rfind('\n');
            if (found != std::string_view::npos)
            {
                return start + found + 1;
            }
            if (index == 0)
            {
                return 0;
            }
            --index;
            offset = mStorage->ends[index];
        }
    }

    std::size_t SourceSnapshot::LineEnd(std::size_t offset) const noexcept
    {
        offset     = std::min(offset, Size());
        auto index = static_cast<std::size_t>(
            std::upper_bound(mStorage->ends.begin(), mStorage->ends.end(), offset) -
            mStorage->ends.begin());
        while (index < ChunkCount())
        {
            const auto start = ChunkOffset(index);
            const auto found = Chunk(index).find('\n', offset - start);
            if (found != std::string_view::npos)
            {
                return start + found;
            }
            offset = mStorage->ends[index++];
        }
        return Size();
    }

    std::size_t SourceSnapshot::StorageBytes() const
    {
        std::unordered_set<const void*> counted;
        return StorageBytes(counted);
    }

    std::size_t SourceSnapshot::StorageBytes(std::unordered_set<const void*>& counted) const
    {
        if (!counted.insert(mStorage.get()).second)
        {
            return 0;
        }
        std::size_t bytes = mStorage->pieces.capacity() * sizeof(Piece) +
                            mStorage->ends.capacity() * sizeof(std::size_t);
        for (const auto& piece : mStorage->pieces)
        {
            if (counted.insert(piece.text.get()).second)
            {
                bytes += piece.text->capacity();
            }
        }
        const std::lock_guard lock(mStorage->mutex);
        if (mStorage->contiguous && counted.insert(mStorage->contiguous.get()).second)
        {
            bytes += mStorage->contiguous->capacity();
        }
        return bytes;
    }
} // namespace heimdall
