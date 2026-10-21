#pragma once

#include <filesystem>
#include <memory>
#include <optional>

namespace heimdall::profiling
{
	/** Legacy trace request, reported as unsupported in builds without profiling. */
	inline constexpr const char* kTraceEnvironmentVariable = "HEIMDALL_TRACE";

	/** Whether this build carries the Perfetto backend (HEIMDALL_PROFILING=ON). */
	[[nodiscard]] bool Available() noexcept;

	/**
	 * Returns heimdall.pftrace in the supplied workspace root. With no root, searches
	 * ancestors of the working directory for .git, .heimdall.json or CMakeLists.txt.
	 * Falls back to the working directory if no project marker is found.
	 */
	[[nodiscard]] std::filesystem::path ResolveOutputPath(const std::filesystem::path& projectRoot);

	/** Returns the automatic profiling destination; disabled builds only read HEIMDALL_TRACE. */
	[[nodiscard]] std::optional<std::filesystem::path> OutputPathFromEnvironment();

	/**
	 * One in-process Perfetto recording. While alive, heimdall::trace events are collected;
	 * destruction (or Stop) ends the recording and writes the trace file.
	 */
	class Session
	{
	public:
		/** Begins recording; nullptr when the backend is missing, already running or failed. */
		[[nodiscard]] static std::unique_ptr<Session> Start(std::filesystem::path output);

		/** Sets the destination after the editor supplies its workspace root, before Stop. */
		void SetProjectRoot(const std::filesystem::path& projectRoot);

		~Session();

		Session(const Session&) = delete;

		Session& operator= (const Session&) = delete;

		/** Ends the recording and writes the file; returns false when nothing was written. */
		bool Stop();

		[[nodiscard]] const std::filesystem::path& Output() const noexcept
		{
			return mOutput;
		}

	private:
		struct Impl;

		Session(std::filesystem::path output, std::unique_ptr<Impl> impl);

		std::filesystem::path mOutput;
		std::unique_ptr<Impl> mImpl;
	};
}
