#pragma once

#include <Heimdall/IndexSnapshot.hpp>

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace heimdall::index
{
	/**
	 * Mutable holder of the latest shard per file. Open buffers shadow disk
	 * shards. Snapshots are rebuilt lazily and shared until the next change.
	 */
	class IndexStore
	{
	public:
		/** Shard of an open buffer; replaces the disk shard of the same path. */
		void SetOpen(const std::string& path, std::shared_ptr<const FileShard> shard);

		/** The buffer closed: the disk shard (if any) is served again. */
		void ClearOpen(const std::string& path);

		void SetDisk(const std::string& path, std::shared_ptr<const FileShard> shard);

		void RemoveDisk(const std::string& path);

		/** Drops `path` and, when it is a folder, every disk shard below it. */
		void RemoveDiskUnder(const std::string& path);

		/** Immutable view of the current state; cheap when nothing changed. */
		[[nodiscard]] std::shared_ptr<const IndexSnapshot> Snapshot() const;

		[[nodiscard]] std::size_t FileCount() const;

	private:
		mutable std::mutex mMutex;
		std::unordered_map<std::string, std::shared_ptr<const FileShard>> mOpen;
		std::unordered_map<std::string, std::shared_ptr<const FileShard>> mDisk;
		mutable std::shared_ptr<const IndexSnapshot> mSnapshot;
	};
} // namespace heimdall::index
