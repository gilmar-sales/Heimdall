#include "CompilerProbe.hpp"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

#include <Heimdall/Trace.hpp>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <unistd.h>

#include <sys/wait.h>
#endif

namespace heimdall::detail
{
	namespace
	{
		constexpr std::size_t kMaxProbeBytes = 4 * 1024 * 1024;
		constexpr auto kPollInterval         = std::chrono::milliseconds(10);

		CompilerProbeResult Run(
			const std::string& command,
			std::stop_token stop,
			std::chrono::milliseconds timeout)
		{
			CompilerProbeResult result;
			const auto deadline = std::chrono::steady_clock::now() + timeout;
			std::array<char, 4096> buffer {};
#if defined(_WIN32)
			SECURITY_ATTRIBUTES attributes {sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
			HANDLE reader = nullptr;
			HANDLE writer = nullptr;
			if (!CreatePipe(&reader, &writer, &attributes, 0))
			{
				return result;
			}

			SetHandleInformation(reader, HANDLE_FLAG_INHERIT, 0);
			const auto job = CreateJobObjectW(nullptr, nullptr);
			JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
			limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
			if (!job ||!SetInformationJobObject(job, JobObjectExtendedLimitInformation,
				&limits, sizeof(limits)))
			{
				if (job)
				{
					CloseHandle(job);
				}

				CloseHandle(reader);
				CloseHandle(writer);
				return result;
			}

			STARTUPINFOA startup {};
			startup.cb         = sizeof(startup);
			startup.dwFlags    = STARTF_USESTDHANDLES;
			startup.hStdOutput = writer;
			startup.hStdError  = writer;
			startup.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
			PROCESS_INFORMATION process {};
			std::string line = "cmd.exe /d /s /c " + command;
			const bool launched = CreateProcessA(nullptr, line.data(), nullptr, nullptr, TRUE,
				CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &startup, &process);
			CloseHandle(writer);
			if (!launched)
			{
				CloseHandle(job);
				CloseHandle(reader);
				return result;
			}

			if (!AssignProcessToJobObject(job, process.hProcess))
			{
				TerminateProcess(process.hProcess, 1);
			}
			else
			{
				ResumeThread(process.hThread);
			}

			while (!stop.stop_requested() && std::chrono::steady_clock::now() < deadline)
			{
				DWORD available = 0;
				if (!PeekNamedPipe(reader, nullptr, 0, nullptr, &available, nullptr))
				{
					DWORD code = 1;
					const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
						std::max(deadline - std::chrono::steady_clock::now(),
						std::chrono::steady_clock::duration::zero()));
					if (WaitForSingleObject(process.hProcess, static_cast<DWORD>(remaining.count())) ==
						WAIT_OBJECT_0 &&
						GetExitCodeProcess(process.hProcess, &code))
					{
						result.completed = code == 0;
					}

					break;
				}

				if (available != 0)
				{
					DWORD count = 0;
					if (!ReadFile(reader, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr))
					{
						break;
					}

					result.output.append(buffer.data(), count);
					if (result.output.size() > kMaxProbeBytes)
					{
						break;
					}
				}
				else if (WaitForSingleObject(process.hProcess, 0) == WAIT_OBJECT_0)
				{
					DWORD late = 0;
					if (PeekNamedPipe(reader, nullptr, 0, nullptr, &late, nullptr) && late != 0)
					{
						continue;
					}

					DWORD code = 1;
					GetExitCodeProcess(process.hProcess, &code);
					result.completed = code == 0;
					break;
				}
				else
				{
					std::this_thread::sleep_for(kPollInterval);
				}
			}

			CloseHandle(job);
			WaitForSingleObject(process.hProcess, INFINITE);
			CloseHandle(process.hThread);
			CloseHandle(process.hProcess);
			CloseHandle(reader);
#else
			int handles[2];
			if (pipe(handles) != 0)
			{
				return result;
			}

			const auto child = fork();
			if (child == 0)
			{
				setpgid(0, 0);
				dup2(handles[1], STDOUT_FILENO);
				dup2(handles[1], STDERR_FILENO);
				close(handles[0]);
				close(handles[1]);
				execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char*>(nullptr));
				_exit(127);
			}

			close(handles[1]);
			if (child < 0)
			{
				close(handles[0]);
				return result;
			}

			setpgid(child, child);
			fcntl(handles[0], F_SETFL, O_NONBLOCK);
			int status  = 0;
			bool exited = false;
			while (!stop.stop_requested() && std::chrono::steady_clock::now() < deadline)
			{
				const auto count = read(handles[0], buffer.data(), buffer.size());
				if (count > 0)
				{
					result.output.append(buffer.data(), static_cast<std::size_t>(count));
					if (result.output.size() > kMaxProbeBytes)
					{
						break;
					}

					continue;
				}

				if (!exited)
				{
					exited = waitpid(child, &status, WNOHANG) == child;
				}

				if (exited && count == 0)
				{
					result.completed = WIFEXITED(status) && WEXITSTATUS(status) == 0;
					break;
				}

				std::this_thread::sleep_for(kPollInterval);
			}

			kill(-child, SIGKILL);
			if (!exited)
			{
				waitpid(child, &status, 0);
			}

			close(handles[0]);
#endif
			if (!result.completed)
			{
				result.output.clear();
			}

			return result;
		}

		struct ProbeSlot
		{
			std::mutex mutex;
			std::condition_variable_any ready;
			CompilerProbeResult result;
			bool finished = false;
		};
	}

	CompilerProbeResult QueryCompiler(
		std::string_view compiler,
		const std::string& command,
		std::stop_token stop,
		std::chrono::milliseconds timeout)
	{
		HEIMDALL_TRACE_SCOPE_DETAIL(Compiler, "QueryCompiler", command);
		if (stop.stop_requested()) return {};
		std::string key = command;
		for (const auto name :
			{
				"PATH", "CPATH", "CPLUS_INCLUDE_PATH", "INCLUDE", "SDKROOT"
		})
		{
			const auto value = std::getenv(name);
			key += '\n';
			key += name;
			key += '=';
			if (value)
			{
				key += value;
			}
		}

		std::error_code error;
		key += '\n' + std::filesystem::current_path(error).generic_string();
		std::filesystem::path executable(compiler);
		if (executable.is_relative())
		{
			if (executable.has_parent_path())
			{
				executable = std::filesystem::absolute(executable, error);
			}
			else if (const auto search = std::getenv("PATH"))
			{
				std::string_view paths(search);
#if defined(_WIN32)
				constexpr char kSeparator = ';';
#else
				constexpr char kSeparator = ':';
#endif
				while (!paths.empty())
				{
					const auto end = paths.find(kSeparator);
					auto candidate = std::filesystem::path(paths.substr(0, end)) / executable;
#if defined(_WIN32)
					if (!candidate.has_extension())
					{
						candidate += ".exe";
					}
#endif
					if (std::filesystem::is_regular_file(candidate, error))
					{
						executable = std::filesystem::absolute(candidate, error);
						break;
					}

					if (end == std::string_view::npos)
					{
						break;
					}

					paths.remove_prefix(end + 1);
				}
			}
		}

		if (executable.is_absolute())
		{
			key += '\n' + executable.generic_string();
			key += std::to_string(std::filesystem::last_write_time(executable,
				error).time_since_epoch().count());
			key += std::to_string(std::filesystem::file_size(executable, error));
		}

		static std::mutex cacheMutex;
		static std::unordered_map<std::string, std::shared_ptr<ProbeSlot>> cache;
		std::shared_ptr<ProbeSlot> slot;
		bool producer = false;
		{
			HEIMDALL_TRACE_LOCK(cacheMutex, "CompilerProbeCache::Find");
			auto[entry, inserted] = cache.try_emplace(key);
			if (inserted)
			{
				entry->second = std::make_shared<ProbeSlot>();
			}

			slot     = entry->second;
			producer = inserted;
		}
		if (producer)
		{
			CompilerProbeResult result;
			try
			{
				result = Run(command, stop, timeout);
			}
			catch (...)
			{
				result = {};
			}

			{
				const std::lock_guard lock(slot->mutex);
				slot->result   = std::move(result);
				slot->finished = true;
			}
			slot->ready.notify_all();
			HEIMDALL_TRACE_LOCK(cacheMutex, "CompilerProbeCache::Store");
			constexpr std::size_t kMaxProbes = 128;
			if (!slot->result.completed)
			{
				cache.erase(key);
			}

			if (cache.size() > kMaxProbes)
			{
				for (auto it = cache.begin(); it != cache.end() && cache.size() > kMaxProbes;)
				{
					const auto candidate = it->second;
					const std::lock_guard entryLock(candidate->mutex);
					if (it->first != key && candidate->finished)
					{
						it = cache.erase(it);
					}
					else
					{
						++it;
					}
				}
			}
		}

		std::unique_lock lock(slot->mutex);
		if (!slot->ready.wait(lock, stop,[&]
			{
				return slot->finished;
				})) return {};
		auto result = slot->result;
		lock.unlock();
		if (!producer && !result.completed && !stop.stop_requested())
		{
			{
				const std::lock_guard cacheLock(cacheMutex);
				if (const auto found = cache.find(key); found != cache.end() && found->second == slot)
				{
					cache.erase(found);
				}
			}
			return QueryCompiler(compiler, command, stop, timeout);
		}

		return result;
	}
}
