#include <Heimdall/IncludeIndex.hpp>
#include <Heimdall/MacroEnvironment.hpp>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace heimdall
{

	namespace
	{

		constexpr std::size_t kProbeBufferBytes = 4096;
		constexpr std::uint64_t kFnvBasis       = 14695981039346656037ull;
		constexpr std::uint64_t kFnvPrime       = 1099511628211ull;

		std::string DriverOf(const CompileCommand* command)
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

		std::string CppVersion(const CompileCommand* command)
		{
			const CppStandard standard = command != nullptr ? command->standard : CppStandard::Cpp20;
			return standard == CppStandard::Cpp26 ? "202400L"
			: standard == CppStandard::Cpp23 ? "202302L"
			: "202002L";
		}

		// What a compiler is known to predefine when it cannot be run.
		MacroTable FallbackBuiltins(const CompileCommand* command, bool msvc)
		{
			MacroTable table;
			const auto define =[&table](std::string_view text)
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

		std::string RunProbe(const std::string& driver, const std::string& flags)
		{
#if defined(_WIN32)
			const std::string line = "\"\"" + driver + "\" -x c++ -dM -E" + flags + " NUL 2>nul\"";
			std::unique_ptr<FILE, decltype(&_pclose) > pipe(_popen(line.c_str(), "r"), _pclose);
#else
			const std::string line =
				"\"" + driver + "\" -x c++ -dM -E" + flags + " /dev/null 2>/dev/null";
			std::unique_ptr<FILE, decltype(&pclose) > pipe(popen(line.c_str(), "r"), pclose);
#endif
			std::string text;
			if (!pipe)
			{
				return text;
			}

			char buffer[kProbeBufferBytes];
			while (std::fgets(buffer, sizeof(buffer), pipe.get()) != nullptr)
			{
				text += buffer;
			}

			return text;
		}

		MacroTable ParseProbe(std::string_view text)
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

		std::filesystem::path Normalized(const std::filesystem::path& path)
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
		};

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

			std::shared_ptr<const Effect> Find(const std::string& key)
			{
				const std::lock_guard<std::mutex> lock(mMutex);
				const auto found = mEffects.find(key);
				return found == mEffects.end() ? nullptr : found->second;
			}

			void Store(const std::string& key, std::shared_ptr<const Effect> effect)
			{
				const std::lock_guard<std::mutex> lock(mMutex);
				if (mEffects.size() >= kMaxEntries)
				{
					mEffects.clear();
				}

				mEffects[key] = std::move(effect);
			}

		private:
			static constexpr std::size_t kMaxEntries = 8192;

			std::mutex mMutex;
			std::unordered_map<std::string, std::shared_ptr<const Effect>> mEffects;
		};

	} // namespace

	class MacroEnvironment::Builder
	{
	public:
		// The reads and writes of the header being scanned, as the table reports them.
		struct Trace
		{
			std::vector<EffectRead> reads;
			std::unordered_set<std::string> readNames;
			std::unordered_set<std::string> touched;
			std::unordered_map<std::string, MacroPtr> net;
			std::vector<std::string> netOrder;
			std::vector<EffectFile> files;
			bool cacheable = true;

			void AddRead(std::string_view name, std::uint64_t signature)
			{
				if (touched.contains(std::string(name)) ||!readNames.emplace(name).second)
				{
					return;
				}

				reads.push_back({std::string(name), signature});
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
			Preprocessor::ErasedSet everDefined;

			void Read(std::string_view name, const MacroDef* found) override
			{
				if (!stack.empty())
				{
					stack.back()->AddRead(name, Signature(found));
				}
			}

			void Defined(const MacroPtr& macro) override
			{
				everDefined.insert(macro->name);
				if (!stack.empty())
				{
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
			for (const auto& dir : IncludeIndex::SystemIncludes(DriverOf(command)))
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
					mConfigKey += "|I" + dir.generic_string();
				}

				for (const auto& dir : command->quote_directories)
				{
					mConfigKey += "|Q" + dir.generic_string();
				}
			}

			mConfigKey += "\n";
		}

		void Run(const std::filesystem::path& file, std::string_view text)
		{
			mTable = Builtins(mCommand, mOptions.probeCompiler);
			mTable.ForEach([this](const MacroDef& macro) { mTracer.everDefined.insert(macro.name); });
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
			callbacks.recordDirectives = true;
			callbacks.checkpoint =[this](std::size_t offset)
			{
				mEnv.mCheckpoints.push_back({offset, mTable.Freeze()});
			};
			mEnv.mMain     = MacroScanner::Scan(text, main.generic_string(), mTable, callbacks, mStop);
			mEnv.mFinal    = mTable.Freeze();
			mEnv.mEverDefined = std::move(mTracer.everDefined);
			mEnv.mComplete = mComplete && !mEnv.mMain.cancelled;
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
		std::vector<std::filesystem::path> SearchPath(const IncludeRequest& request,
			const std::filesystem::path& includerDir) const
		{
			std::vector<std::filesystem::path> dirs;
			if (!request.angled)
			{
				dirs.push_back(includerDir);
				if (mCommand != nullptr)
				{
					dirs.insert(dirs.end(), mCommand->quote_directories.begin(),
						mCommand->quote_directories.end());
				}
			}

			if (mCommand != nullptr)
			{
				for (const auto& dir : mCommand->include_directories)
				{
					dirs.push_back(dir.is_relative() ? mCommand->directory / dir : dir);
				}
			}

			dirs.insert(dirs.end(), mSystem.begin(), mSystem.end());
			return dirs;
		}

		bool Readable(const std::filesystem::path& path) const
		{
			if (mOptions.overlay != nullptr && mOptions.overlay->Find(path))
			{
				return true;
			}

			std::error_code ec;
			return std::filesystem::is_regular_file(path, ec);
		}

		std::optional<std::filesystem::path> Resolve(const IncludeRequest& request,
			const std::filesystem::path& includerDir) const
		{
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

			if (!content)
			{
				std::error_code ec;
				const auto size = std::filesystem::file_size(path, ec);
				std::ifstream in(path, std::ios::binary);
				if (!ec && in && size <= mOptions.maxFileBytes)
				{
					auto text = std::make_shared<std::string>(static_cast<std::size_t>(size), '\0');
					in.read(text->data(), static_cast<std::streamsize>(size));
					content = std::move(text);
				}
			}

			mContents.emplace(key, content);
			return content;
		}

		MacroScanCallbacks CallbacksFor(const std::filesystem::path& includerDir)
		{
			MacroScanCallbacks callbacks;
			callbacks.include =[this, includerDir](const IncludeRequest& request)
			{
				return Enter(request, includerDir, false);
			};
			callbacks.hasInclude =[this, includerDir](const IncludeRequest& request)
			{
				return Resolve(request, includerDir).has_value();
			};
			return callbacks;
		}

		// Scans a header into the live table; false when it cannot be found.
		bool Enter(const IncludeRequest& request, const std::filesystem::path& includerDir,
			bool forced)
		{
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
			if (const auto effect = EffectCache::Instance().Find(cacheKey);
				effect && Valid(*effect))
			{
				Apply(*effect);
				return true;
			}

			const auto content = Read(*path);
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
			const MacroScanResult scanned =
				MacroScanner::Scan(*content, key, mTable, CallbacksFor(path->parent_path()), mStop);
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
			EffectFile self {key, Stamp(*path, mOptions.overlay), scanned.guard,
				scanned.pragmaOnce};
			trace.files.insert(trace.files.begin(), self);
			if (trace.cacheable)
			{
				auto effect   = std::make_shared<Effect>();
				effect->reads = trace.reads;
				for (const std::string& name : trace.netOrder)
				{
					effect->net.emplace_back(name, trace.net[name]);
				}

				effect->files = trace.files;
				EffectCache::Instance().Store(cacheKey, std::move(effect));
			}

			Absorb(trace);
			(void) forced;
			return true;
		}

		bool Valid(const Effect& effect) const
		{
			for (const EffectRead& read : effect.reads)
			{
				if (Signature(mTable.Peek(read.name)) != read.signature)
				{
					return false;
				}
			}

			for (const EffectFile& file : effect.files)
			{
				if (Stamp(file.path, mOptions.overlay) != file.stamp)
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
			if (!mTracer.stack.empty())
			{
				Trace& parent = *mTracer.stack.back();
				for (const EffectRead& read : effect.reads)
				{
					parent.AddRead(read.name, read.signature);
				}

				parent.files.insert(parent.files.end(), effect.files.begin(), effect.files.end());
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
			for (const EffectRead& read : child.reads)
			{
				parent.AddRead(read.name, read.signature);
			}

			for (const std::string& name : child.netOrder)
			{
				parent.SetNet(name, child.net.at(name));
			}

			parent.files.insert(parent.files.end(), child.files.begin(), child.files.end());
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
		std::vector<std::filesystem::path> mDirectories;
		std::vector<std::filesystem::path> mFiles;
		std::unordered_map<std::string, std::shared_ptr<const std::string>> mContents;
		std::unordered_map<std::string, std::string> mGuards;
		std::unordered_set<std::string> mOnce;
		std::unordered_set<std::string> mSeen;
		std::size_t mDepth = 0;
		bool mComplete     = true;
	};

	MacroTable MacroEnvironment::Builtins(const CompileCommand* command, bool probeCompiler)
	{
		const std::string driver = DriverOf(command);
		const bool msvc          = IsMsvcDriver(driver);
		if (!probeCompiler || msvc)
		{
			return FallbackBuiltins(command, msvc);
		}

		const std::string flags = ProbeFlags(command);
		const std::string key   = driver + "\n" + flags;
		static std::mutex mutex;
		static std::unordered_map<std::string, MacroTable> cache;
		{
			const std::lock_guard<std::mutex> lock(mutex);
			if (const auto found = cache.find(key); found != cache.end())
			{
				return found->second;
			}
		}

		MacroTable table = ParseProbe(RunProbe(driver, flags));
		if (table.Size() == 0)
		{
			table = FallbackBuiltins(command, false);
		}

		MacroTable frozen = table.Freeze();
		const std::lock_guard<std::mutex> lock(mutex);
		return cache.emplace(key, std::move(frozen)).first->second;
	}

	MacroEnvironment MacroEnvironment::Build(
		const std::filesystem::path& file,
		std::string_view text,
		const CompileCommand* command,
		const MacroEnvironmentOptions& options,
		std::stop_token stop)
	{
		MacroEnvironment env;
		Builder builder(
			env,
			command,
			options,
			std::move(stop));
		builder.Run(file, text);
		env.mFiles             = builder.TakeFiles();
		env.mSystemDirectories = builder.TakeSystem();
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
		const MacroScanResult& main = mEnvironment->Main();
		mPreprocessing.directives   = main.directives;
		mPreprocessing.active_ranges = main.activeRanges;
		mPreprocessing.macro_events = main.events;
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

	const MacroDef* MacroEnvironmentResolver::Find(std::string_view name,
		std::size_t offset) const noexcept
	{
		if (!mEnvironment->MayDefine(name))
		{
			return nullptr;
		}

		return mEnvironment->Find(name, offset);
	}

} // namespace heimdall
