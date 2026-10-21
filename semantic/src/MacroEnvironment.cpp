#include <Heimdall/MacroEnvironment.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include <Heimdall/IncludeIndex.hpp>
#include <Heimdall/Trace.hpp>

#include "CompilerProbe.hpp"

namespace heimdall
{

	namespace
	{

		constexpr std::size_t kProbeBufferBytes = 4096;
		constexpr std::uint64_t kFnvBasis       = 14695981039346656037ull;
		constexpr std::uint64_t kFnvPrime       = 1099511628211ull;

		[[nodiscard]] std::string DriverOf(const CompileCommand* command)
		{
			if (command != nullptr && !command->arguments.empty() &&
				!command->arguments.front().empty())
			{
				return command->arguments.front();
			}

			return "c++";
		}

		bool IsMsvcDriver(std::string_view driver)
		{
			const std::string name = std::filesystem::path(driver).stem().string();
			return name == "cl" || name == "clang-cl";
		}

		// Flags that change which macros a compiler predefines.
		std::string ProbeFlags(const CompileCommand* command)
		{
			std::string flags;
			bool hasStandard = false;
			if (command != nullptr)
			{
				for (std::size_t i = 1; i < command->arguments.size(); ++i)
				{
					const std::string& argument = command->arguments[i];
					const bool keep = argument.starts_with("-std=") ||
						argument.starts_with("-stdlib=") || argument.starts_with("--target") ||
						argument.starts_with("-nostd") || argument == "-pthread" ||
						argument == "-freflection" || argument.starts_with("-fno-") ||
						argument.starts_with("-fexceptions") || argument.starts_with("-frtti") ||
						argument.starts_with("-fPIC") || argument.starts_with("-fPIE") ||
						argument.starts_with("-O") || argument.starts_with("-m");
					if (!keep)
					{
						continue;
					}

					hasStandard = hasStandard || argument.starts_with("-std=");
					flags += " \"" + argument + "\"";
				}
			}

			if (!hasStandard)
			{
				const CppStandard standard =
					command != nullptr ? command->standard : CppStandard::Cpp20;
				flags += standard == CppStandard::Cpp26 ? " -std=c++26"
				: standard == CppStandard::Cpp23 ? " -std=c++23"
				: " -std=c++20";
			}

			return flags;
		}

		[[nodiscard]] std::string CppVersion(const CompileCommand* command)
		{
			const CppStandard standard = command != nullptr ? command->standard : CppStandard::Cpp20;
			return standard == CppStandard::Cpp26 ? "202400L"
			: standard == CppStandard::Cpp23 ? "202302L"
			: "202002L";
		}

		// What a compiler is known to predefine when it cannot be run.
		[[nodiscard]] MacroTable FallbackBuiltins(const CompileCommand* command, bool msvc)
		{
			MacroTable table;
			const auto define = [&table](std::string_view text)
			{
				if (auto parsed = MacroTable::ParseDefinition(text))
				{
					parsed->origin = MacroOrigin::Builtin;
					table.Define(std::move(*parsed));
				}
			};
			const std::string version = CppVersion(command);
			define("__cplusplus " + version);
			define("__STDC_HOSTED__ 1");
			if (msvc)
			{
				define("_MSVC_LANG " + version);
				define("_MSC_VER 1944");
				define("_WIN32 1");
				define("_MSC_FULL_VER 194435211");
			}

			return table;
		}

		[[nodiscard]] std::string RunProbe(
			const std::string& driver,
			const std::string& flags,
			std::stop_token stop)
		{
#if defined(_WIN32)
			const std::string line = "\"\"" + driver + "\" -x c++ -dM -E" + flags + " NUL 2>nul\"";
#else
			const std::string line =
				"\"" + driver + "\" -x c++ -dM -E" + flags + " /dev/null 2>/dev/null";
#endif
			return detail::QueryCompiler(driver, line, stop).output;
		}

		[[nodiscard]] MacroTable ParseProbe(std::string_view text)
		{
			MacroTable table;
			std::size_t pos = 0;
			while (pos < text.size())
			{
				std::size_t end      = text.find('\n', pos);
				end                  = end == std::string_view::npos ? text.size() : end;
				std::string_view row = text.substr(pos, end - pos);
				pos                  = end + 1;
				while (!row.empty() && row.back() == '\r')
				{
					row.remove_suffix(1);
				}

				constexpr std::string_view kDefine = "#define ";
				if (!row.starts_with(kDefine))
				{
					continue;
				}

				if (auto parsed = MacroTable::ParseDefinition(row.substr(kDefine.size())))
				{
					parsed->origin = MacroOrigin::Builtin;
					table.Define(std::move(*parsed));
				}
			}

			return table;
		}

		bool IsUnder(const std::filesystem::path& file, const std::filesystem::path& directory)
		{
			auto f = file.begin();
			for (auto d = directory.begin(); d != directory.end(); ++d, ++f)
			{
				if (f == file.end() || *f != *d)
				{
					return false;
				}
			}

			return true;
		}

		[[nodiscard]] std::filesystem::path Normalized(const std::filesystem::path& path)
		{
			std::error_code ec;
			auto absolute = std::filesystem::absolute(path, ec);
			return (ec ? path : absolute).lexically_normal();
		}

		// What entering one header reads from, and does to, the macro state. Replayed instead
		// of scanning the header again while everything it read still holds.
		struct EffectRead
		{
			std::string name;
			/// `Signature` of the definition the header saw, 0 when the name was undefined.
			std::uint64_t signature = 0;
			MacroPtr definition;
		};

		struct EffectFile
		{
			std::string path;
			std::uint64_t stamp = 0;
			std::string guard;
			bool pragmaOnce = false;
		};

		struct Effect
		{
			std::vector<EffectRead> reads;
			/// Net result per name; a null definition means the name ends up undefined.
			std::vector<std::pair<std::string, MacroPtr>> net;
			/// The header first, then every header it entered, in order.
			std::vector<EffectFile> files;
			struct Query
			{
				IncludeRequest request;
				std::filesystem::path directory;
				std::optional<std::filesystem::path> result;
			};

			std::vector<Query> queries;
			std::vector<std::string> definedNames;
			std::size_t depth = 1;
		};

		std::string QueryKey(
			const IncludeRequest& request,
			const std::filesystem::path& directory)
		{
			const auto path = directory.generic_string();
			return std::to_string(path.size()) + ":" + path +(request.angled ? "A" : "Q") +
				(request.next ? "N" : "F") + request.header;
		}

		std::uint64_t Signature(const MacroDef* macro) noexcept
		{
			return macro == nullptr ? 0 : (macro->hash | 1u);
		}

		std::uint64_t Mix(std::uint64_t hash, std::uint64_t value) noexcept
		{
			return (hash ^ value) * kFnvPrime;
		}

		// Identifies the exact contents a header had: a hash of an open buffer, or the size
		// and modification time of the file on disk.
		std::uint64_t Stamp(const std::filesystem::path& path, const SourceOverlay* overlay)
		{
			std::uint64_t hash = kFnvBasis;
			if (overlay != nullptr)
			{
				if (const auto buffer = overlay->Find(path))
				{
					for (const char c : *buffer)
					{
						hash = Mix(hash, static_cast<unsigned char>(c));
					}

					return Mix(hash, 1);
				}
			}

			std::error_code ec;
			const auto size = std::filesystem::file_size(path, ec);
			const auto time = std::filesystem::last_write_time(path, ec);
			hash            = Mix(hash, static_cast<std::uint64_t>(size));
			return Mix(hash, static_cast<std::uint64_t>(time.time_since_epoch().count()));
		}

		class EffectCache
		{
		public:
			static EffectCache& Instance()
			{
				static EffectCache cache;
				return cache;
			}

			std::vector<std::shared_ptr<const Effect>> Find(const std::string& key)
			{
				const std::lock_guard<std::mutex> lock(mMutex);
				const auto found = mEffects.find(key);
				if (found == mEffects.end())
				{
					return {};
				}

				mOrder.splice(mOrder.begin(), mOrder, found->second.position);
				return found->second.variants;
			}

			void Store(const std::string& key, std::shared_ptr<const Effect> effect)
			{
				const std::lock_guard<std::mutex> lock(mMutex);
				std::size_t bytes = sizeof(Effect) + key.size();
				for (const auto& file : effect->files)
				{
					bytes += sizeof(file) + file.path.size() + file.guard.size();
				}

				for (const auto& read : effect->reads)
				{
					bytes += sizeof(read) + read.name.size();
					if (read.definition)
					{
						bytes += sizeof(MacroDef) + read.definition->body.size() + read.definition->file.size() +
							read.definition->tokens.capacity() * sizeof(MacroBodyToken);
						for (const auto& parameter : read.definition->params)
						{
							bytes += sizeof(parameter) + parameter.size();
						}
					}
				}

				for (const auto& [name, macro] : effect->net)
				{
					bytes += sizeof(MacroDef) + name.size();
					if (macro)
					{
						bytes += macro->body.size() + macro->file.size() +
							macro->tokens.capacity() * sizeof(MacroBodyToken);
						for (const auto& param : macro->params)
						{
							bytes += sizeof(param) + param.size();
						}
					}
				}

				for (const auto& query : effect->queries)
				{
					bytes += sizeof(query) + query.request.header.size() +
						query.directory.native().size() * sizeof(std::filesystem::path::value_type);
					if (query.result)
					{
						bytes += query.result->native().size() * sizeof(std::filesystem::path::value_type);
					}
				}

				for (const auto& name : effect->definedNames)
				{
					bytes += sizeof(name) + name.size();
				}

				if (bytes > kMaxBytes)
				{
					return;
				}

				auto found = mEffects.find(key);
				if (found != mEffects.end() && found->second.variants.size() == kMaxVariants)
				{
					const auto removed = found->second.variantBytes.back();
					mBytes -= removed;
					found->second.bytes -= removed;
					found->second.variantBytes.pop_back();
					found->second.variants.pop_back();
				}

				while (!mOrder.empty() && (mBytes + bytes > kMaxBytes || mEffects.size() >= kMaxEntries))
				{
					const auto victim = mEffects.find(mOrder.back());
					mBytes -= victim->second.bytes;
					mEffects.erase(victim);
					mOrder.pop_back();
				}

				auto[entry, inserted] = mEffects.try_emplace(key);
				if (inserted)
				{
					mOrder.push_front(key);
					entry->second.position = mOrder.begin();
				}

				entry->second.variants.insert(entry->second.variants.begin(), std::move(effect));
				entry->second.variantBytes.insert(entry->second.variantBytes.begin(), bytes);
				entry->second.bytes += bytes;
				mBytes += bytes;
			}

		private:
			static constexpr std::size_t kMaxEntries  = 8192;
			static constexpr std::size_t kMaxVariants = 4;
			static constexpr std::size_t kMaxBytes    = 64 * 1024 * 1024;
			struct Entry
			{
				std::vector<std::shared_ptr<const Effect>> variants;
				std::vector<std::size_t> variantBytes;
				std::list<std::string>::iterator position;
				std::size_t bytes = 0;
			};

			std::mutex mMutex;
			std::unordered_map<std::string, Entry> mEffects;
			std::list<std::string> mOrder;
			std::size_t mBytes = 0;
		};

		class HeaderContentCache
		{
		public:
			static HeaderContentCache& Instance()
			{
				static HeaderContentCache cache;
				return cache;
			}

			std::shared_ptr<const std::string> Read(
				const std::filesystem::path& path,
				std::uint64_t stamp,
				std::size_t maxBytes,
				MacroEnvironmentMetrics& metrics)
			{
				const auto key = path.generic_string();
				{
					const std::lock_guard lock(mMutex);
					if (const auto found = mContents.find(key); found != mContents.end() &&
						found->second.stamp == stamp)
					{
						mOrder.splice(mOrder.begin(), mOrder, found->second.position);
						return found->second.content->size() <= maxBytes ? found->second.content : nullptr;
					}
				}
				std::error_code error;
				const auto size = std::filesystem::file_size(path, error);
				if (error || size > maxBytes)
				{
					return nullptr;
				}

				std::ifstream input(path, std::ios::binary);
				auto content = std::make_shared<std::string>(static_cast<std::size_t>(size), '\0');
				input.read(content->data(), static_cast<std::streamsize>(size));
				++metrics.metadataQueries;
				if (!input || Stamp(path, nullptr) != stamp)
				{
					return nullptr;
				}

				++metrics.filesRead;
				metrics.bytesRead += content->size();
				constexpr std::size_t kMaxBytes   = 32 * 1024 * 1024;
				constexpr std::size_t kMaxEntries = 4096;
				if (content->size() > kMaxBytes)
				{
					return content;
				}

				const std::lock_guard lock(mMutex);
				if (const auto old = mContents.find(key); old != mContents.end())
				{
					mBytes -= old->second.content->size();
					mOrder.erase(old->second.position);
					mContents.erase(old);
				}

				while (!mOrder.empty() && (mBytes + content->size() > kMaxBytes ||
					mContents.size() >= kMaxEntries))
				{
					const auto victim = mContents.find(mOrder.back());
					mBytes -= victim->second.content->size();
					mContents.erase(victim);
					mOrder.pop_back();
				}

				mOrder.push_front(key);
				mContents.emplace(key, Entry {stamp, content, mOrder.begin()});
				mBytes += content->size();
				return content;
			}

		private:
			struct Entry
			{
				std::uint64_t stamp;
				std::shared_ptr<const std::string> content;
				std::list<std::string>::iterator position;
			};

			std::mutex mMutex;
			std::unordered_map<std::string, Entry> mContents;
			std::list<std::string> mOrder;
			std::size_t mBytes = 0;
		};

	} // namespace

	class MacroEnvironment::Builder
	{
	public:
		// The reads and writes of the header being scanned, as the table reports them.
		struct Trace
		{
			std::vector<EffectRead> reads;
			Preprocessor::ErasedSet readNames;
			Preprocessor::ErasedSet touched;
			std::unordered_map<std::string, MacroPtr> net;
			std::vector<std::string> netOrder;
			std::vector<EffectFile> files;
			std::vector<Effect::Query> queries;
			std::unordered_set<std::string> fileNames;
			std::unordered_set<std::string> queryNames;
			std::unordered_set<std::string> definedNames;
			std::size_t depth = 1;
			bool cacheable    = true;

			void AddFile(const EffectFile& file)
			{
				if (fileNames.insert(file.path).second)
				{
					files.push_back(file);
				}
			}

			void AddQuery(const Effect::Query& query)
			{
				if (queryNames.insert(QueryKey(query.request, query.directory)).second)
				{
					queries.push_back(query);
				}
			}

			void AddRead(std::string_view name, std::uint64_t signature, MacroPtr definition)
			{
				if (touched.contains(name) ||!readNames.emplace(name).second)
				{
					return;
				}

				reads.push_back({std::string(name), signature, std::move(definition)});
			}

			void SetNet(const std::string& name, MacroPtr macro)
			{
				touched.insert(name);
				if (net.insert_or_assign(name, std::move(macro)).second)
				{
					netOrder.push_back(name);
				}
			}
		};

		class Tracer final : public MacroObserver
		{
		public:
			std::vector<Trace*> stack;
			MacroTable* table = nullptr;
			Preprocessor::ErasedSet everDefined;

			void Read(std::string_view name, const MacroDef* found) override
			{
				if (!stack.empty())
				{
					auto& trace = *stack.back();
					if (trace.touched.contains(name) || trace.readNames.contains(name))
					{
						return;
					}
					MacroPtr definition;
					if (found)
					{
						table->SetObserver(nullptr);
						definition = table->Lookup(name);
						table->SetObserver(this);
					}
					trace.AddRead(name, Signature(found), std::move(definition));
				}
			}

			void Defined(const MacroPtr& macro) override
			{
				everDefined.insert(macro->name);
				if (!stack.empty())
				{
					stack.back()->definedNames.insert(macro->name);
					stack.back()->SetNet(macro->name, macro);
				}
			}

			void Undefined(std::string_view name) override
			{
				if (!stack.empty())
				{
					stack.back()->SetNet(std::string(name), nullptr);
				}
			}
		};

		Builder(
			MacroEnvironment& env,
			const CompileCommand* command,
			const MacroEnvironmentOptions& options,
			std::stop_token stop) :
			mEnv(env), mCommand(command), mOptions(options), mStop(std::move(stop))
		{
			mTracer.table = &mTable;
			for (const auto& dir : IncludeIndex::SystemIncludes(DriverOf(command), mStop))
			{
				mSystem.push_back(Normalized(dir));
			}

			mConfigKey = DriverOf(command) + "|" + ProbeFlags(command) + "|" +
				(options.probeCompiler ? "p" : "f");
			for (const auto& dir : mSystem)
			{
				mConfigKey += "|" + dir.generic_string();
			}

			if (command != nullptr)
			{
				for (const auto& dir : command->include_directories)
				{
					const auto normalized = Normalized(dir.is_relative() ? command->directory / dir : dir);
					mIncludeDirectories.push_back(normalized);
					mConfigKey += "|I" + normalized.generic_string();
				}

				for (const auto& dir : command->quote_directories)
				{
					const auto normalized = Normalized(dir.is_relative() ? command->directory / dir : dir);
					mQuoteDirectories.push_back(normalized);
					mConfigKey += "|Q" + normalized.generic_string();
				}
			}

			mIncludeDirectories.insert(mIncludeDirectories.end(), mSystem.begin(), mSystem.end());

			if (command != nullptr)
			{
				mConfigKey += "|C" + Normalized(command->directory).generic_string();
			}

			mConfigKey += "|L" + std::to_string(options.maxFileBytes) + ":" +
				std::to_string(options.maxMacros) + "\n";
		}

		void Run(const std::filesystem::path& file, std::string_view text)
		{
			mTable = Builtins(mCommand, mOptions.probeCompiler, mStop);
			HEIMDALL_TRACE_SCOPE(Macro, "MacroEnvironment::Run");
			mTable.ForEach([this](const MacroDef& macro)
				{
					mTracer.everDefined.insert(macro.name);
				});
			mTable.SetObserver(&mTracer);
			ApplyCommandLine();
			const std::filesystem::path main = Normalized(file);
			mDirectories.push_back(main.parent_path());
			for (const std::filesystem::path& forced : ForcedIncludes())
			{
				IncludeRequest request;
				request.header = forced.generic_string();
				Enter(request, main.parent_path(), true);
			}

			mEnv.mInitial                = mTable.Freeze();
			MacroScanCallbacks callbacks = CallbacksFor(main.parent_path());
			callbacks.recordDirectives   = true;
			callbacks.checkpoint = [this](std::size_t offset)
			{
				mEnv.mCheckpoints.push_back({offset, mTable.Freeze()});
			};
			{
				HEIMDALL_TRACE_SCOPE(Macro, "ScanMainFile");
				mEnv.mMain = MacroScanner::Scan(text, main.generic_string(), mTable, callbacks, mStop);
			}

			mEnv.mFinal       = mTable.Freeze();
			mEnv.mEverDefined = std::move(mTracer.everDefined);
			mEnv.mComplete    = mComplete && !mEnv.mMain.cancelled;
		}

		std::vector<std::filesystem::path> TakeFiles()
		{
			return std::move(mFiles);
		}

		std::vector<std::filesystem::path> TakeSystem()
		{
			return std::move(mSystem);
		}

	private:
		void ApplyCommandLine()
		{
			if (mCommand == nullptr)
			{
				return;
			}

			for (const auto& [name, value] : mCommand->defines)
			{
				if (auto parsed = MacroTable::ParseDefinition(name + " " + value))
				{
					parsed->origin = MacroOrigin::CommandLine;
					mTable.Define(std::move(*parsed));
				}
			}

			for (const std::string& name : mCommand->undefines)
			{
				mTable.Undef(name);
			}
		}

		std::vector<std::filesystem::path> ForcedIncludes() const
		{
			std::vector<std::filesystem::path> forced;
			if (mCommand == nullptr)
			{
				return forced;
			}

			for (std::size_t i = 1; i + 1 < mCommand->arguments.size(); ++i)
			{
				if (mCommand->arguments[i] == "-include" || mCommand->arguments[i] == "-imacros")
				{
					std::filesystem::path path = mCommand->arguments[i + 1];
					if (path.is_relative())
					{
						path = mCommand->directory / path;
					}

					forced.push_back(std::move(path));
				}
			}

			return forced;
		}

		bool InSystem(const std::filesystem::path& file) const
		{
			return std::ranges::any_of(
				mSystem,[&](const std::filesystem::path& dir)
				{
					return IsUnder(file, dir);
				});
		}

		// Directories searched for a header, in compiler order.
		std::vector<std::filesystem::path> SearchPath(
			const IncludeRequest& request,
			const std::filesystem::path& includerDir) const
		{
			std::vector<std::filesystem::path> dirs;
			if (!request.angled)
			{
				dirs.push_back(includerDir);
				dirs.insert(dirs.end(), mQuoteDirectories.begin(), mQuoteDirectories.end());
			}

			dirs.insert(dirs.end(), mIncludeDirectories.begin(), mIncludeDirectories.end());
			return dirs;
		}

		bool Readable(const std::filesystem::path& path) const
		{
			const auto key = path.generic_string();
			if (const auto found = mReadable.find(key); found != mReadable.end())
			{
				return found->second;
			}

			if (mOptions.overlay != nullptr && mOptions.overlay->Find(path))
			{
				mReadable.emplace(key, true);
				return true;
			}

			std::error_code ec;
			++mEnv.mMetrics.resolutionQueries;
			const bool readable = std::filesystem::is_regular_file(path, ec);
			mReadable.emplace(key, readable);
			return readable;
		}

		std::optional<std::filesystem::path> Resolve(
			const IncludeRequest& request,
			const std::filesystem::path& includerDir)
		{
			HEIMDALL_TRACE_SCOPE(Include, "ResolveInclude");
			auto result = ResolveUntracked(request, includerDir);
			if (!mTracer.stack.empty())
			{
				mTracer.stack.back()->AddQuery({request, includerDir, result});
			}

			return result;
		}

		std::optional<std::filesystem::path> ResolveUntracked(
			const IncludeRequest& request,
			const std::filesystem::path& includerDir) const
		{
			const auto key = QueryKey(request, includerDir);
			if (const auto found = mResolutions.find(key); found != mResolutions.end())
			{
				return found->second;
			}

			auto result = ResolvePath(request, includerDir);
			mResolutions.emplace(key, result);
			return result;
		}

		std::optional<std::filesystem::path> ResolvePath(
			const IncludeRequest& request,
			const std::filesystem::path& includerDir) const
		{
			if (mStop.stop_requested())
			{
				return std::nullopt;
			}

			const std::filesystem::path name(request.header);
			if (name.is_absolute())
			{
				return Readable(name) ? std::optional(Normalized(name)) : std::nullopt;
			}

			const auto dirs   = SearchPath(request, includerDir);
			std::size_t first = 0;
			if (request.next)
			{
				const auto at = std::ranges::find_if(dirs,[&](const std::filesystem::path& dir)
					{
						return Normalized(dir) == Normalized(includerDir);
					});
				first = at == dirs.end() ? 0 : static_cast<std::size_t>(at - dirs.begin()) + 1;
			}

			for (std::size_t i = first; i < dirs.size(); ++i)
			{
				if (mStop.stop_requested())
				{
					return std::nullopt;
				}

				const std::filesystem::path candidate = Normalized(dirs[i] / name);
				if (Readable(candidate))
				{
					return candidate;
				}
			}

			return std::nullopt;
		}

		std::shared_ptr<const std::string> Read(const std::filesystem::path& path)
		{
			const std::string key = path.generic_string();
			if (const auto found = mContents.find(key); found != mContents.end())
			{
				return found->second;
			}

			std::shared_ptr<const std::string> content;
			if (mOptions.overlay != nullptr)
			{
				content = mOptions.overlay->Find(path);
			}

			if (!content && !mStop.stop_requested())
			{
				content = HeaderContentCache::Instance().Read(path, FileStamp(key),
					mOptions.maxFileBytes, mEnv.mMetrics);
			}

			mContents.emplace(key, content);
			return content;
		}

		MacroScanCallbacks CallbacksFor(const std::filesystem::path& includerDir)
		{
			MacroScanCallbacks callbacks;
			callbacks.include = [this, includerDir](const IncludeRequest& request)
			{
				return Enter(request, includerDir, false);
			};
			callbacks.hasInclude = [this, includerDir](const IncludeRequest& request)
			{
				return Resolve(request, includerDir).has_value();
			};
			return callbacks;
		}

		// Scans a header into the live table; false when it cannot be found.
		bool Enter(
			const IncludeRequest& request,
			const std::filesystem::path& includerDir,
			bool forced)
		{
			if (mStop.stop_requested())
			{
				mComplete = false;
				return false;
			}

			const auto path = Resolve(request, includerDir);
			if (!path)
			{
				if (!InSystem(includerDir))
				{
					mComplete = false;
					MarkUncacheable();
				}

				return false;
			}

			const std::string key = path->generic_string();
			if (mOnce.contains(key))
			{
				MarkUncacheable();
				return true;
			}

			if (const auto known = mGuards.find(key);
				known != mGuards.end() && !known->second.empty() &&
				mTable.Find(known->second) != nullptr)
			{
				return true;
			}

			if (mTracer.everDefined.size() > mOptions.maxMacros)
			{
				mComplete = false;
				MarkUncacheable();
				return true;
			}

			if (mDepth >= mOptions.maxIncludeDepth)
			{
				mComplete = false;
				MarkUncacheable();
				return true;
			}

			const std::string cacheKey = mConfigKey + key;
			if (mOptions.reuseEffects)
			{
				HEIMDALL_TRACE_SCOPE(Include, "ReplayEffects");
				for (const auto& effect : EffectCache::Instance().Find(cacheKey))
				{
					if (Valid(*effect))
					{
						++mEnv.mMetrics.effectsReplayed;
						Apply(*effect);
						return true;
					}

					++mEnv.mMetrics.effectsRejected;
				}
			}

			const auto content = [&]
			{
				HEIMDALL_TRACE_SCOPE_DETAIL(Include, "ReadHeader", key);
				return Read(*path);
			}();
			if (!content)
			{
				mComplete = false;
				MarkUncacheable();
				return false;
			}

			if (mSeen.insert(key).second)
			{
				mFiles.push_back(*path);
			}

			Trace trace;
			const bool completeBefore = mComplete;
			mComplete                 = true;
			mTracer.stack.push_back(&trace);
			++mDepth;
			++mEnv.mMetrics.effectsScanned;
			const MacroScanResult scanned = [&]
			{
				HEIMDALL_TRACE_SCOPE_DETAIL(Macro, "ScanHeader", key);
				return MacroScanner::Scan(*content, key, mTable, CallbacksFor(path->parent_path()), mStop);
			}();
			--mDepth;
			mTracer.stack.pop_back();
			mGuards[key] = scanned.guard;
			if (scanned.pragmaOnce)
			{
				mOnce.insert(key);
			}

			const bool clean = mComplete && !scanned.cancelled;
			mComplete        = completeBefore && mComplete;
			trace.cacheable  = trace.cacheable && clean;
			EffectFile self {key, FileStamp(key), scanned.guard,
				scanned.pragmaOnce};
			trace.files.insert(trace.files.begin(), self);
			trace.fileNames.insert(key);
			if (trace.cacheable && mOptions.reuseEffects)
			{
				auto effect   = std::make_shared<Effect>();
				effect->reads = trace.reads;
				for (const std::string& name : trace.netOrder)
				{
					effect->net.emplace_back(name, trace.net[name]);
				}

				effect->files   = trace.files;
				effect->queries = trace.queries;
				effect->definedNames.assign(trace.definedNames.begin(), trace.definedNames.end());
				effect->depth = trace.depth;
				std::ranges::sort(effect->definedNames);
				EffectCache::Instance().Store(cacheKey, std::move(effect));
			}

			Absorb(trace);
			(void) forced;
			return true;
		}

		std::uint64_t FileStamp(const std::string& path) const
		{
			if (const auto found = mStamps.find(path); found != mStamps.end())
			{
				return found->second;
			}

			++mEnv.mMetrics.metadataQueries;
			const auto stamp = Stamp(path, mOptions.overlay);
			mStamps.emplace(path, stamp);
			return stamp;
		}

		bool Valid(const Effect& effect) const
		{
			HEIMDALL_TRACE_SCOPE(Include, "ValidateEffect");
			if (mStop.stop_requested() || effect.depth > mOptions.maxIncludeDepth - mDepth ||
				mTracer.everDefined.size() + effect.definedNames.size() > mOptions.maxMacros)
			{
				return false;
			}

			for (const EffectRead& read : effect.reads)
			{
				const auto current = mTable.Peek(read.name);
				if (Signature(current) != read.signature)
				{
					return false;
				}

				if (current && (!read.definition || current->body != read.definition->body ||
					current->params != read.definition->params || current->functionLike != read.definition->functionLike ||
					current->variadic != read.definition->variadic || current->file != read.definition->file ||
					current->offset != read.definition->offset || current->origin != read.definition->origin))
				{
					return false;
				}
			}

			for (const EffectFile& file : effect.files)
			{
				if (mStop.stop_requested() ||(file.pragmaOnce && mOnce.contains(file.path)) ||
					FileStamp(file.path) != file.stamp)
				{
					return false;
				}
			}

			for (const auto& query : effect.queries)
			{
				if (ResolveUntracked(query.request, query.directory) != query.result)
				{
					return false;
				}
			}

			return true;
		}

		// Replays a cached effect: the writes go through the table, so an enclosing trace
		// records them itself; its reads and files are added here.
		void Apply(const Effect& effect)
		{
			HEIMDALL_TRACE_SCOPE(Include, "ApplyEffect");
			mTracer.everDefined.insert(effect.definedNames.begin(), effect.definedNames.end());
			if (!mTracer.stack.empty())
			{
				Trace& parent = *mTracer.stack.back();
				parent.depth  = std::max(parent.depth, effect.depth + 1);
				parent.definedNames.insert(effect.definedNames.begin(), effect.definedNames.end());
				for (const EffectRead& read : effect.reads)
				{
					parent.AddRead(read.name, read.signature, read.definition);
				}

				for (const auto& file : effect.files)
				{
					parent.AddFile(file);
				}

				for (const auto& query : effect.queries)
				{
					parent.AddQuery(query);
				}
			}

			for (const auto& [name, macro] : effect.net)
			{
				if (macro)
				{
					mTable.Define(macro);
				}
				else
				{
					mTable.Undef(name);
				}
			}

			for (const EffectFile& file : effect.files)
			{
				mGuards[file.path] = file.guard;
				if (file.pragmaOnce)
				{
					mOnce.insert(file.path);
				}

				if (mSeen.insert(file.path).second)
				{
					mFiles.emplace_back(file.path);
				}
			}
		}

		// Folds a scanned header's trace into the one that is scanning its includer.
		void Absorb(const Trace& child)
		{
			if (mTracer.stack.empty())
			{
				return;
			}

			Trace& parent = *mTracer.stack.back();
			parent.depth  = std::max(parent.depth, child.depth + 1);
			parent.definedNames.insert(child.definedNames.begin(), child.definedNames.end());
			for (const EffectRead& read : child.reads)
			{
				parent.AddRead(read.name, read.signature, read.definition);
			}

			for (const std::string& name : child.netOrder)
			{
				parent.SetNet(name, child.net.at(name));
			}

			for (const auto& file : child.files)
			{
				parent.AddFile(file);
			}

			for (const auto& query : child.queries)
			{
				parent.AddQuery(query);
			}

			parent.cacheable = parent.cacheable && child.cacheable;
		}

		void MarkUncacheable()
		{
			if (!mTracer.stack.empty())
			{
				mTracer.stack.back()->cacheable = false;
			}
		}

		Tracer mTracer;
		std::string mConfigKey;
		MacroEnvironment& mEnv;
		const CompileCommand* mCommand;
		const MacroEnvironmentOptions& mOptions;
		std::stop_token mStop;
		MacroTable mTable;
		std::vector<std::filesystem::path> mSystem;
		std::vector<std::filesystem::path> mIncludeDirectories;
		std::vector<std::filesystem::path> mQuoteDirectories;
		std::vector<std::filesystem::path> mDirectories;
		std::vector<std::filesystem::path> mFiles;
		std::unordered_map<std::string, std::shared_ptr<const std::string>> mContents;
		mutable std::unordered_map<std::string, std::uint64_t> mStamps;
		mutable std::unordered_map<std::string, bool> mReadable;
		mutable std::unordered_map<std::string, std::optional<std::filesystem::path>> mResolutions;
		std::unordered_map<std::string, std::string> mGuards;
		std::unordered_set<std::string> mOnce;
		std::unordered_set<std::string> mSeen;
		std::size_t mDepth = 0;
		bool mComplete     = true;
	};

	MacroTable MacroEnvironment::Builtins(
		const CompileCommand* command,
		bool probeCompiler,
		std::stop_token stop)
	{
		const std::string driver = DriverOf(command);
		const bool msvc          = IsMsvcDriver(driver);
		if (!probeCompiler || msvc)
		{
			return FallbackBuiltins(command, msvc);
		}

		HEIMDALL_TRACE_SCOPE_DETAIL(Compiler, "MacroEnvironment::Builtins", driver);
		const auto output = RunProbe(driver, ProbeFlags(command), stop);
		if (output.empty())
		{
			return FallbackBuiltins(command, false);
		}

		static std::mutex mutex;
		static std::unordered_map<std::string, MacroTable> cache;
		const std::lock_guard lock(mutex);
		if (const auto found = cache.find(output); found != cache.end())
		{
			return found->second;
		}

		auto table                              = ParseProbe(output).Freeze();
		constexpr std::size_t kMaxBuiltinTables = 128;
		if (cache.size() >= kMaxBuiltinTables)
		{
			cache.erase(cache.begin());
		}

		return cache.emplace(output, std::move(table)).first->second;
	}

	MacroEnvironment MacroEnvironment::Build(
		const std::filesystem::path& file,
		std::string_view text,
		const CompileCommand* command,
		const MacroEnvironmentOptions& options,
		std::stop_token stop)
	{
		HEIMDALL_TRACE_SCOPE_DETAIL(Macro, "MacroEnvironment::Build", file.generic_string());
		const auto start = std::chrono::steady_clock::now();
		MacroEnvironment env;
		Builder builder(env, command, options, std::move(stop));
		env.mMetrics.setupNs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now() - start).count());
		builder.Run(file, text);
		env.mFiles             = builder.TakeFiles();
		env.mSystemDirectories = builder.TakeSystem();
		env.mMetrics.totalNs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now() - start).count());
		return env;
	}

	bool MacroEnvironment::IsSystemFile(const std::filesystem::path& file) const
	{
		const std::filesystem::path normalized = Normalized(file);
		return std::ranges::any_of(mSystemDirectories,[&](const std::filesystem::path& dir)
			{
				return IsUnder(normalized, dir);
			});
	}

	const MacroDef* MacroEnvironment::Find(std::string_view name, std::size_t offset) const
	{
		return Snapshot(offset).Find(name);
	}

	const MacroTable& MacroEnvironment::Snapshot(std::size_t offset) const noexcept
	{
		const auto after = std::ranges::upper_bound(
			mCheckpoints, offset, {},[](const Checkpoint& checkpoint)
			{
				return checkpoint.offset;
			});
		if (after == mCheckpoints.begin())
		{
			return mInitial;
		}

		return std::prev(after) -> table;
	}

	MacroTable MacroEnvironment::At(std::size_t offset) const
	{
		return Snapshot(offset);
	}

} // namespace heimdall

namespace heimdall
{

	MacroEnvironmentResolver::MacroEnvironmentResolver(
		std::shared_ptr<const MacroEnvironment> environment) :
		mEnvironment(std::move(environment))
	{
		// Order-independent: the sum of each definition's hash mixed with its name.
		const MacroScanResult& main  = mEnvironment->Main();
		mPreprocessing.directives    = main.directives;
		mPreprocessing.active_ranges = main.activeRanges;
		mPreprocessing.macro_events  = main.events;
		for (const auto& diagnostic : main.diagnostics)
		{
			// A header that cannot be found is the include analysis' business, not a syntax error.
			if (!diagnostic.message.ends_with(" file not found"))
			{
				mPreprocessing.diagnostics.push_back(diagnostic);
			}
		}

		mEnvironment->Final().ForEach(
			[this](const MacroDef& macro)
			{
				std::uint64_t mixed = kFnvBasis;
				for (const char c : macro.name)
				{
					mixed = (mixed ^ static_cast<unsigned char>(c)) * kFnvPrime;
				}

				mFingerprint +=(mixed ^ macro.hash) * kFnvPrime;
			});
	}

	const MacroDef* MacroEnvironmentResolver::Find(
		std::string_view name,
		std::size_t offset) const noexcept
	{
		if (!mEnvironment->MayDefine(name))
		{
			return nullptr;
		}

		return mEnvironment->Find(name, offset);
	}

} // namespace heimdall
