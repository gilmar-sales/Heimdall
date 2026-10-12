#include <Heimdall/IndexStore.hpp>

#include <algorithm>

namespace heimdall::index
{
	void IndexStore::SetOpen(const std::string& path, std::shared_ptr<const FileShard> shard)
	{
		const std::lock_guard lock(mMutex);
		mOpen[path] = std::move(shard);
		++mRevision;
		mSnapshot.reset();
	}

	void IndexStore::ClearOpen(const std::string& path)
	{
		const std::lock_guard lock(mMutex);
		if (mOpen.erase(path) != 0)
		{
			++mRevision;
			mSnapshot.reset();
		}
	}

	void IndexStore::SetDisk(const std::string& path, std::shared_ptr<const FileShard> shard)
	{
		const std::lock_guard lock(mMutex);
		mDisk[path] = std::move(shard);
		++mRevision;
		mSnapshot.reset();
	}

	void IndexStore::RemoveDisk(const std::string& path)
	{
		const std::lock_guard lock(mMutex);
		if (mDisk.erase(path) != 0)
		{
			++mRevision;
			mSnapshot.reset();
		}
	}

	void IndexStore::RemoveDiskUnder(const std::string& path)
	{
		const std::lock_guard lock(mMutex);
		const auto removed = std::erase_if(mDisk,
			[&](const auto& entry)
			{
				const auto& key = entry.first;
				return key == path ||
				(key.size() > path.size() && key.starts_with(path) && key[path.size()] == '/');
		});
		if (removed != 0)
		{
			++mRevision;
			mSnapshot.reset();
		}
	}

	std::shared_ptr<const IndexSnapshot> IndexStore::Snapshot() const
	{
		const std::lock_guard lock(mMutex);
		if (!mSnapshot)
		{
			std::vector<std::shared_ptr<const FileShard>> shards;
			shards.reserve(mDisk.size() + mOpen.size());
			for (const auto& [path, shard] : mDisk)
			{
				if (!mOpen.contains(path))
				{
					shards.push_back(shard);
				}
			}

			for (const auto& entry : mOpen)
			{
				shards.push_back(entry.second);
			}

			// Deterministic shard order independent of hash-map iteration.
			std::ranges::sort(shards,
				[](const auto& a, const auto& b)
				{
					return a->Path() < b->Path();
			});
			mSnapshot = IndexSnapshot::Build(std::move(shards), mRevision);
		}

		return mSnapshot;
	}

	std::size_t IndexStore::FileCount() const
	{
		const std::lock_guard lock(mMutex);
		std::size_t count = mDisk.size();
		for (const auto& entry : mOpen)
		{
			count += mDisk.contains(entry.first) ? 0 : 1;
		}

		return count;
	}
} // namespace heimdall::index
