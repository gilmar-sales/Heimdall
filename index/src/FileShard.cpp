#include <Heimdall/FileShard.hpp>

#include <algorithm>
#include <numeric>

namespace heimdall::index
{
	StringId StringTable::Intern(std::string_view text)
	{
		const auto id = static_cast<StringId>(mBegin.size());
		mBegin.push_back(static_cast<std::uint32_t>(mBlob.size()));
		mLength.push_back(static_cast<std::uint32_t>(text.size()));
		mBlob.append(text);
		return id;
	}

	std::string_view StringTable::Text(StringId id) const noexcept
	{
		return id < mBegin.size() ? std::string_view(mBlob).substr(mBegin[id], mLength[id])
		: std::string_view {};
	}

	std::size_t StringTable::Bytes() const noexcept
	{
		return mBlob.size() +(mBegin.size() + mLength.size()) * sizeof(std::uint32_t);
	}

	std::uint64_t CharacterMask(std::string_view text) noexcept
	{
		constexpr unsigned kLetters    = 26;
		constexpr unsigned kDigitBase  = 26;
		constexpr unsigned kUnderscore = 36;
		constexpr unsigned kOther      = 37;
		std::uint64_t mask             = 0;
		for (const char raw : text)
		{
			const auto c = static_cast<unsigned char>(raw);
			unsigned bit = kOther;
			if (c >= 'a' && c <= 'z')
			{
				bit = c - 'a';
			}
			else if (c >= 'A' && c <= 'Z')
			{
				bit = c - 'A';
			}
			else if (c >= '0' && c <= '9')
			{
				bit = kDigitBase +(c - '0');
			}
			else if (c == '_')
			{
				bit = kUnderscore;
			}

			static_assert(kLetters == kDigitBase);
			mask |= 1ull << bit;
		}

		return mask;
	}

	std::uint64_t HashBytes(std::string_view bytes, std::uint64_t seed) noexcept
	{
		constexpr std::uint64_t kPrime = 0x100000001b3ull;
		std::uint64_t hash             = seed;
		for (const char byte : bytes)
		{
			hash ^= static_cast<unsigned char>(byte);
			hash *= kPrime;
		}

		return hash;
	}

	std::span<const std::uint32_t> FileShard::OccurrencesOf(std::uint32_t symbol) const noexcept
	{
		if (symbol + 1 >= mSymbolOccurrenceBegin.size())
		{
			return {};
		}

		return {mSymbolOccurrenceList.data() + mSymbolOccurrenceBegin[symbol],
			mSymbolOccurrenceBegin[symbol + 1] - mSymbolOccurrenceBegin[symbol]};
	}

	std::uint32_t FileShard::OccurrenceAt(std::uint32_t offset) const noexcept
	{
		const auto& offsets = mOccurrences.offset;
		auto next           = std::ranges::upper_bound(offsets, offset);
		if (next == offsets.begin())
		{
			return kInvalid;
		}

		const auto index = static_cast<std::uint32_t>(next - offsets.begin()) - 1;
		return offset < offsets[index] + mOccurrences.length[index] ? index : kInvalid;
	}

	std::uint32_t FileShard::SymbolAt(std::uint32_t offset) const noexcept
	{
		const auto index = OccurrenceAt(offset);
		return index == kInvalid ? kInvalid : mOccurrences.symbol[index];
	}

	std::pair<std::size_t, std::size_t> FileShard::RelationsFrom(
		SymbolHash subject) const noexcept
	{
		const auto range = std::ranges::equal_range(mRelations.subject, subject);
		return {static_cast<std::size_t>(range.begin() - mRelations.subject.begin()),
			static_cast<std::size_t>(range.end() - mRelations.subject.begin())};
	}

	std::span<const std::uint32_t> FileShard::RelationsTo(SymbolHash object) const noexcept
	{
		const auto range = std::ranges::equal_range(mRelationsByObject, object, {},
			[&](std::uint32_t index)
			{
				return mRelations.object[index];
		});
		return {range.begin(), range.end()};
	}

	std::size_t FileShard::StorageBytes() const noexcept
	{
		constexpr std::size_t kSymbolIndexColumns     = 10;
		constexpr std::size_t kOccurrenceIndexColumns = 11;
		constexpr std::size_t kScopeIndexColumns      = 4;
		constexpr std::size_t kUsingIndexColumns      = 4;
		constexpr std::size_t kRelationHashColumns    = 2;
		const auto& s                                 = mSymbols;
		const auto& o                                 = mOccurrences;
		return mPath.size() + mStrings.Bytes() + mParameterTypes.size() + mArgumentTypes.size() +
			s.Size() *
			(sizeof(SymbolHash) + sizeof(std::uint64_t) + kSymbolIndexColumns * sizeof(std::uint32_t) +
			sizeof(heimdall::SymbolKind) + sizeof(Role) + sizeof(std::uint8_t) + sizeof(std::uint8_t)) +
			o.Size() * (kOccurrenceIndexColumns * sizeof(std::uint32_t) + sizeof(Role) +
			sizeof(Resolution) + sizeof(std::uint8_t) + sizeof(std::uint8_t)) +
			mScopes.Size() * kScopeIndexColumns * sizeof(std::uint32_t) +
			mUsings.Size() * (kUsingIndexColumns * sizeof(std::uint32_t) + sizeof(UsingKind)) +
			mRelations.Size() * (kRelationHashColumns * sizeof(SymbolHash) + sizeof(RelationKind) + sizeof(std::uint32_t)) +
			(mIncludes.size() + mIncludeTargets.size() + mIncludeOffsets.size() + mSymbolOccurrenceBegin.size() + mSymbolOccurrenceList.size()) *
			sizeof(std::uint32_t);
	}

	void FileShard::Finalize()
	{
		std::vector<std::uint32_t> order(mRelations.Size());
		std::iota(order.begin(), order.end(), 0u);
		std::ranges::stable_sort(order,
			[&](std::uint32_t a, std::uint32_t b)
			{
				return mRelations.subject[a] < mRelations.subject[b];
		});

		RelationColumns sorted;
		sorted.subject.reserve(order.size());
		sorted.object.reserve(order.size());
		sorted.kind.reserve(order.size());
		sorted.offset.reserve(order.size());
		for (const auto index : order)
		{
			sorted.offset.push_back(mRelations.offset[index]);
			sorted.subject.push_back(mRelations.subject[index]);
			sorted.object.push_back(mRelations.object[index]);
			sorted.kind.push_back(mRelations.kind[index]);
		}

		mRelations = std::move(sorted);

		mRelationsByObject.resize(mRelations.Size());
		std::iota(mRelationsByObject.begin(), mRelationsByObject.end(), 0u);
		std::ranges::stable_sort(mRelationsByObject,
			[&](std::uint32_t a, std::uint32_t b)
			{
				return mRelations.object[a] < mRelations.object[b];
		});

		mSymbolOccurrenceBegin.assign(mSymbols.Size() + 1, 0);
		for (const auto symbol : mOccurrences.symbol)
		{
			if (symbol != kInvalid)
			{
				++mSymbolOccurrenceBegin[symbol + 1];
			}
		}

		std::partial_sum(mSymbolOccurrenceBegin.begin(), mSymbolOccurrenceBegin.end(),
			mSymbolOccurrenceBegin.begin());
		mSymbolOccurrenceList.resize(mSymbolOccurrenceBegin.back());
		auto cursor = mSymbolOccurrenceBegin;
		for (std::uint32_t i = 0; i < mOccurrences.Size(); ++i)
		{
			if (const auto symbol = mOccurrences.symbol[i]; symbol != kInvalid)
			{
				mSymbolOccurrenceList[cursor[symbol] ++] = i;
			}
		}
	}
} // namespace heimdall::index
