#pragma once

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>

namespace heimdall
{
	class SourceSnapshot;

	// Construct before publishing through shared_ptr<const SourceOverlay>.
	// Open buffers win over disk, including buffers of not-yet-created files.
	class SourceOverlay
	{
	public:
		void Add(const std::filesystem::path& path, std::shared_ptr<const std::string> source);

		/** Retains immutable pieces and materializes only when the header is read. */
		void AddSnapshot(
			const std::filesystem::path& path,
			std::shared_ptr<const SourceSnapshot> source);

		std::shared_ptr<const std::string> Find(const std::filesystem::path& path) const;

		std::string Fingerprint() const;

		std::string Fingerprint(std::span<const std::filesystem::path> paths) const;

		/** Exact path membership, independent of buffer contents and materialization. */
		[[nodiscard]] std::string PathFingerprint() const;

		bool Empty() const noexcept
		{
			return m_sources.empty();
		}

	private:
		static std::string Key(const std::filesystem::path& path);

		struct Source
		{
			std::shared_ptr<const std::string> text;
			std::shared_ptr<const SourceSnapshot> pieces;
		};

		std::unordered_map<std::string, Source> m_sources;
	};
} // namespace heimdall
