#include <Heimdall/Workspace.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <unordered_map>
#include <unordered_set>

namespace heimdall
{
	namespace detail
	{
		struct BoundStorage
		{
			std::shared_ptr<const ParseTree> tree;
			SemanticModel model;

			explicit BoundStorage(std::shared_ptr<const ParseTree> syntax) :
				tree(std::move(syntax)), model(Binder::Bind(*tree)) {}
		};

		struct TypedStorage
		{
			std::shared_ptr<const SemanticModel> model;
			TypeModel types;

			explicit TypedStorage(std::shared_ptr<const SemanticModel> semantic) :
				model(std::move(semantic)), types(Typer::Type(*model)) {}
		};

		struct SyntaxStorage
		{
			mutable std::mutex mutex;
			mutable std::condition_variable ready;
			mutable bool computing = false;
			mutable std::chrono::steady_clock::time_point lastUsed;
			mutable std::shared_ptr<const ParseTree> tree;
			mutable std::shared_ptr<const ParseTree> base;
			Lexer::TextEdit edit;
			bool directivesUnchanged = false;
			mutable AnalysisMetrics metrics;
		};

		struct DocumentAnalysis
		{
			std::filesystem::path path;
			std::shared_ptr<const std::string> source;
			std::shared_ptr<const SourceSnapshot> sourceBuffer;
			std::shared_ptr<const TokenSnapshot> tokenBuffer;
			std::int64_t version = 0;
			ParserOptions options;
			std::vector<std::string> include_targets;
			std::shared_ptr<SyntaxStorage> syntax = std::make_shared<SyntaxStorage>();
			mutable std::mutex mutex;
			mutable std::condition_variable ready;
			mutable bool binding     = false;
			mutable bool typing      = false;
			mutable bool summarizing = false;
			mutable std::chrono::steady_clock::time_point lastUsed;
			mutable std::shared_ptr<const SemanticModel> semantic;
			mutable std::shared_ptr<const TypeModel> types;
			mutable std::shared_ptr<const HeaderSummary> summary;
			mutable AnalysisMetrics metrics;

			[[nodiscard]] std::shared_ptr<const std::string> Source() const
			{
				return sourceBuffer ? sourceBuffer->Materialize() : source;
			}
		};

		struct ProjectStorage
		{
			std::mutex mutex;
			std::condition_variable ready;
			bool computing = false;
			std::shared_ptr<const ProjectIndex> index;
			AnalysisMetrics metrics;
		};

		struct SymbolIndexStorage
		{
			std::mutex mutex;
			std::condition_variable ready;
			bool computing = false;
			std::shared_ptr<const ProjectSymbolIndex> index;
			AnalysisMetrics metrics;
		};

		class DocumentPages
		{
		public:
			using Entry                            = std::shared_ptr<const DocumentAnalysis>;
			static constexpr std::size_t kPageSize = 64;

			[[nodiscard]] std::size_t size() const noexcept
			{
				return mSize;
			}

			[[nodiscard]] const Entry & operator[](std::size_t index) const noexcept
			{
				return (*mPages[index / kPageSize])[index % kPageSize];
			}

			void Set(std::size_t index, Entry entry)
			{
				auto& page = mPages[index / kPageSize];
				if (!page.unique())
				{
					page = std::make_shared<Page>(*page);
				}

				(*page)[index % kPageSize] = std::move(entry);
			}

			void push_back(Entry entry)
			{
				if (mSize % kPageSize == 0)
				{
					mPages.push_back(std::make_shared<Page>());
				}

				Set(mSize++, std::move(entry));
			}

			class Iterator
			{
			public:
				const Entry & operator * () const noexcept
				{
					return (*mOwner)[mIndex];
				}

				Iterator & operator++() noexcept
				{
					++mIndex;
					return *this;
				}

				bool operator==(const Iterator&) const noexcept = default;
				const DocumentPages* mOwner;
				std::size_t mIndex;
			};

			Iterator begin() const noexcept
			{
				return {this, 0};
			}

			Iterator end() const noexcept
			{
				return {this, mSize};
			}

		private:
			using Page = std::array<Entry, kPageSize>;
			std::vector<std::shared_ptr<Page>> mPages;
			std::size_t mSize = 0;
		};

		template <typename T>
		class SharedMetadata
		{
		public:
			[[nodiscard]] const T& Read() const noexcept
			{
				return *mData;
			}

			T& Write()
			{
				if (!mData.unique())
				{
					mData = std::make_shared<T>(*mData);
				}

				return *mData;
			}

		private:
			std::shared_ptr<T> mData = std::make_shared<T>();
		};

		struct WorkspaceState
		{
			WorkspaceState() = default;

			WorkspaceState(const WorkspaceState& other) :
				revision(other.revision), documents(other.documents), paths(other.paths),
				dependencies(other.dependencies), dependents(other.dependents),
				explicit_dependencies(other.explicit_dependencies), compilation(other.compilation),
				project(other.project) {}

			std::uint64_t revision = 0;
			DocumentPages documents;
			SharedMetadata<std::unordered_map<std::string, DocumentId>> paths;
			SharedMetadata<std::vector<std::vector<DocumentId>>> dependencies;
			SharedMetadata<std::vector<std::vector<DocumentId>>> dependents;
			SharedMetadata<std::vector<bool>> explicit_dependencies;
			std::shared_ptr<const CompileDatabase> compilation;
			std::shared_ptr<const ProjectIndex> project;
			std::shared_ptr<ProjectStorage> project_cache = std::make_shared<ProjectStorage>();
			std::shared_ptr<SymbolIndexStorage> symbol_cache =
				std::make_shared<SymbolIndexStorage>();
		};
	} // namespace detail

	namespace
	{
		using Clock = std::chrono::steady_clock;

		template <typename Result, typename Compute, typename Publish, typename Hit>
		Result Query(
			std::mutex& mutex,
			std::condition_variable& ready,
			bool& computing,
			Result& cache,
			Compute compute,
			Publish publish,
			Hit hit)
		{
			std::unique_lock lock(mutex);
			ready.wait(lock,[&]
				{
					return !computing || static_cast<bool>(cache);
				});
			if (cache)
			{
				hit();
				return cache;
			}

			computing = true;
			lock.unlock();
			try
			{
				const auto start = Clock::now();
				auto result      = compute();
				lock.lock();
				if (!cache)
				{
					cache = std::move(result);
					publish(start);
				}

				computing = false;
				result    = cache;
				lock.unlock();
				ready.notify_all();
				return result;
			}
			catch (...)
			{
				if (!lock.owns_lock())
				{
					lock.lock();
				}

				computing = false;
				lock.unlock();
				ready.notify_all();
				throw;
			}
		}

		std::uint64_t Elapsed(const Clock::time_point& start)
		{
			return static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
		}

		std::string PathKey(const std::filesystem::path& path)
		{
			std::error_code error;
			auto absolute = std::filesystem::absolute(path, error);
			auto key      = (error ? path : absolute).lexically_normal().generic_string();
#ifdef _WIN32
			// Windows paths are case-insensitive; use ASCII folding without locale.
			for (auto& c : key)
			{
				if (c >= 'A' && c <= 'Z')
				{
					c += 'a' - 'A';
				}
			}
#endif
			return key;
		}

		std::shared_ptr<detail::DocumentAnalysis> NewDocument(const detail::DocumentAnalysis& old)
		{
			auto document             = std::make_shared<detail::DocumentAnalysis>();
			document->path            = old.path;
			document->source          = old.source;
			document->sourceBuffer    = old.sourceBuffer;
			document->tokenBuffer     = old.tokenBuffer;
			document->version         = old.version;
			document->options         = old.options;
			document->include_targets = old.include_targets;
			return document;
		}

		bool SameOptions(const ParserOptions& a, const ParserOptions& b)
		{
			return a.standard == b.standard && a.Macros() == b.Macros() &&
				a.type_names == b.type_names;
		}

		void InvalidateDependents(detail::WorkspaceState& state, DocumentId changed)
		{
			if (state.dependents.Read()[changed].empty())
			{
				return;
			}

			std::vector<bool> affected(state.documents.size(), false);
			std::vector<DocumentId> queue {changed};
			affected[changed] = true;
			for (std::size_t next = 0; next < queue.size(); ++next)
			{
				for (DocumentId id : state.dependents.Read()[queue[next]])
				{
					if (affected[id] ||!state.documents[id])
					{
						continue;
					}

					affected[id] = true;
					queue.push_back(id);
					// Syntax remains valid; only the semantic dependency changed.
					auto document    = NewDocument(*state.documents[id]);
					document->syntax = state.documents[id]->syntax;
					state.documents.Set(id, std::move(document));
				}
			}
		}

		std::vector<std::string> Includes(std::string_view source, const ParserOptions& options)
		{
			std::vector<std::string> includes;
			// Reuse the syntax preprocessor's directive recognition, including
			// inactive branches and opaque raw literals, rather than a regex.
			const auto preprocessing = Preprocessor(options.Macros()).Process(source);
			for (const auto& directive : preprocessing.directives)
			{
				if (directive.kind != DirectiveKind::Include)
				{
					continue;
				}

				auto body = source.substr(directive.offset, directive.length);
				std::vector<Token> significant;
				for (const auto& token : Lexer(body).Lex())
				{
					if (token.kind != TokenKind::Whitespace &&
						token.kind != TokenKind::LineComment &&
						token.kind != TokenKind::BlockComment)
					{
						significant.push_back(token);
					}
				}

				if (significant.size() < 3 ||
					body.substr(significant[1].offset, significant[1].length) != "include")
				{
					continue;
				}

				const auto& header  = significant[2];
				const auto spelling = body.substr(header.offset, header.length);
				if (header.kind == TokenKind::StringLiteral && spelling.size() >= 2 &&
					spelling.front() == '"')
				{
					includes.emplace_back(spelling);
				}
				else if (spelling == "<")
				{
					std::string target = "<";
					for (std::size_t i = 3; i < significant.size(); ++i)
					{
						auto text = body.substr(significant[i].offset, significant[i].length);
						target.append(text);
						if (text == ">")
						{
							includes.push_back(std::move(target));
							break;
						}
					}
				}
			}

			return includes;
		}

		void ReverseEdges(detail::WorkspaceState& state)
		{
			state.dependents.Write().assign(state.documents.size(), {});
			for (DocumentId id = 0; id < state.dependencies.Read().size(); ++id)
			{
				for (auto dependency : state.dependencies.Read()[id])
				{
					state.dependents.Write()[dependency].push_back(id);
				}
			}
		}

		void ResolveIncludes(detail::WorkspaceState& state)
		{
			std::vector<DocumentId> changed;
			for (DocumentId id = 0; id < state.documents.size(); ++id)
			{
				if (!state.documents[id] || state.explicit_dependencies.Read()[id])
				{
					continue;
				}

				const auto& document = *state.documents[id];
				const auto command =
					state.compilation ? state.compilation->FindOrNearest(document.path) : nullptr;
				std::vector<DocumentId> edges;
				for (const auto& target : document.include_targets)
				{
					const auto name = target.substr(1, target.size() - 2);
					std::vector<std::filesystem::path> directories;
					if (target.front() == '"')
					{
						directories.push_back(document.path.parent_path());
						if (command)
						{
							directories.insert(directories.end(),
								command->quote_directories.begin(),
								command->quote_directories.end());
						}
					}

					if (command)
					{
						directories.insert(directories.end(), command->include_directories.begin(),
							command->include_directories.end());
					}

					for (const auto& directory : directories)
					{
						const auto found = state.paths.Read().find(PathKey(directory / name));
						if (found != state.paths.Read().end())
						{
							edges.push_back(found->second);
							break;
						}

						// An earlier header on disk shadows a registered header
						// in a later search directory, even if not tracked here.
						std::error_code error;
						if (std::filesystem::is_regular_file(directory / name, error))
						{
							break;
						}
					}
				}

				std::sort(edges.begin(), edges.end());
				edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
				if (edges == state.dependencies.Read()[id])
				{
					continue;
				}

				state.dependencies.Write()[id] = std::move(edges);
				auto replacement               = NewDocument(document);
				replacement->syntax            = document.syntax;
				state.documents.Set(id, std::move(replacement));
				changed.push_back(id);
			}

			ReverseEdges(state);
			for (auto id : changed)
			{
				InvalidateDependents(state, id);
			}
		}

		ParserOptions CommandOptions(const CompileCommand& command)
		{
			ParserOptions options;
			options.standard          = command.standard;
			options.predefined_macros = command.defines;
			for (const auto& name : command.undefines)
			{
				options.predefined_macros.erase(name);
			}

			return options;
		}
	} // namespace

	AnalysisSnapshot::AnalysisSnapshot(std::shared_ptr<const detail::WorkspaceState> state) :
		m_state(std::move(state)) {}

	std::shared_ptr<const detail::DocumentAnalysis> AnalysisSnapshot::Document(
		DocumentId document) const
	{
		return Contains(document) ? m_state->documents[document] : nullptr;
	}

	bool AnalysisSnapshot::Contains(DocumentId document) const noexcept
	{
		return m_state && document < m_state->documents.size() && m_state->documents[document];
	}

	std::uint64_t AnalysisSnapshot::Revision() const noexcept
	{
		return m_state ? m_state->revision : 0;
	}

	std::int64_t AnalysisSnapshot::Version(DocumentId document) const noexcept
	{
		return Contains(document) ? m_state->documents[document]->version : -1;
	}

	std::vector<DocumentId> AnalysisSnapshot::Documents() const
	{
		std::vector<DocumentId> ids;
		if (m_state)
		{
			for (DocumentId id = 0; id < m_state->documents.size(); ++id)
			{
				if (Contains(id))
				{
					ids.push_back(id);
				}
			}
		}

		return ids;
	}

	DocumentId AnalysisSnapshot::Find(const std::filesystem::path& path) const
	{
		if (m_state)
		{
			const auto found = m_state->paths.Read().find(PathKey(path));
			if (found != m_state->paths.Read().end())
			{
				return found->second;
			}
		}

		return InvalidDocument;
	}

	std::shared_ptr<const SourceSnapshot> AnalysisSnapshot::SourceBuffer(
		DocumentId document) const
	{
		const auto data = Document(document);
		return data ? data->sourceBuffer : nullptr;
	}

	std::shared_ptr<const std::string> AnalysisSnapshot::Source(DocumentId document) const
	{
		auto data = Document(document);
		return data ? data->Source() : nullptr;
	}

	const std::filesystem::path& AnalysisSnapshot::Path(DocumentId document) const
	{
		static const std::filesystem::path empty;
		return Contains(document) ? m_state->documents[document]->path : empty;
	}

	const ParserOptions& AnalysisSnapshot::Options(DocumentId document) const
	{
		static const ParserOptions empty;
		return Contains(document) ? m_state->documents[document]->options : empty;
	}

	const CompileCommand* AnalysisSnapshot::Command(DocumentId document) const
	{
		return Contains(document) && m_state->compilation
		? m_state->compilation->FindOrNearest(Path(document))
		: nullptr;
	}

	std::span<const CompileCommand> AnalysisSnapshot::CompilationCommands() const
	{
		return m_state && m_state->compilation
		? std::span<const CompileCommand>(m_state->compilation->Commands())
		: std::span<const CompileCommand> {};
	}

	std::span<const DocumentId> AnalysisSnapshot::Dependencies(DocumentId document) const
	{
		return Contains(document)
		? std::span<const DocumentId>(m_state->dependencies.Read()[document])
		: std::span<const DocumentId> {};
	}

	std::shared_ptr<const ParseTree> AnalysisSnapshot::Syntax(DocumentId document) const
	{
		auto data = Document(document);
		if (!data)
			return {};
		return Query(
			data->syntax->mutex, data->syntax->ready, data->syntax->computing, data->syntax->tree,
			[data]
			{
				std::shared_ptr<const ParseTree> base;
				Lexer::TextEdit edit;
				{
					std::lock_guard lock(data->syntax->mutex);
					base = data->syntax->base;
					edit = data->syntax->edit;
				}
				ParseReuse reuse {base.get(), edit.offset, edit.old_length, edit.new_length,
					data->syntax->directivesUnchanged};
				return std::make_shared<const ParseTree>(ParseTree::ParseSnapshot(
				data->Source(), data->options, {},
				data->tokenBuffer ? data->tokenBuffer->Materialize() : nullptr,
				base ? &reuse : nullptr));
			},
			[data](Clock::time_point start)
			{
				data->syntax->base.reset();
				data->syntax->lastUsed = Clock::now();
				++data->syntax->metrics.parse_count;
				data->syntax->metrics.parse_ns += Elapsed(start);
			},
			[data]
			{
				data->syntax->lastUsed = Clock::now();
				++data->syntax->metrics.cache_hits;
			});
	}

	std::shared_ptr<const SemanticModel> AnalysisSnapshot::Semantic(DocumentId document) const
	{
		auto data = Document(document);
		if (!data)
			return {};
		auto syntax = Syntax(document);
		return Query(
			data->mutex, data->ready, data->binding, data->semantic,
			[syntax = std::move(syntax)]
			{
				auto bound = std::make_shared<detail::BoundStorage>(syntax);
				return std::shared_ptr<const SemanticModel>(bound, &bound->model);
			},
			[data](Clock::time_point start)
			{
				data->lastUsed = Clock::now();
				++data->metrics.bind_count;
				data->metrics.bind_ns += Elapsed(start);
			},
			[data]
			{
				data->lastUsed = Clock::now();
				++data->metrics.cache_hits;
			});
	}

	std::shared_ptr<const SemanticModel> AnalysisSnapshot::TrySemantic(DocumentId document) const
	{
		const auto data = Document(document);
		if (!data)
		{
			return {};
		}

		const std::unique_lock lock(data->mutex, std::try_to_lock);
		return lock.owns_lock() ? data->semantic : nullptr;
	}

	std::shared_ptr<const TypeModel> AnalysisSnapshot::Types(DocumentId document) const
	{
		auto data = Document(document);
		if (!data)
			return {};
		auto semantic = Semantic(document);
		return Query(
			data->mutex, data->ready, data->typing, data->types,
			[semantic = std::move(semantic)]
			{
				auto typed = std::make_shared<detail::TypedStorage>(semantic);
				return std::shared_ptr<const TypeModel>(typed, &typed->types);
			},
			[data](Clock::time_point start)
			{
				data->lastUsed = Clock::now();
				++data->metrics.type_count;
				data->metrics.type_ns += Elapsed(start);
			},
			[data]
			{
				data->lastUsed = Clock::now();
				++data->metrics.cache_hits;
			});
	}

	std::shared_ptr<const ProjectIndex> AnalysisSnapshot::Project() const
	{
		if (!m_state)
		{
			return nullptr;
		}

		if (m_state->project)
		{
			return m_state->project;
		}

		return Query(
			m_state->project_cache->mutex, m_state->project_cache->ready,
			m_state->project_cache->computing, m_state->project_cache->index,
			[this]
			{
				std::vector<std::shared_ptr<const HeaderSummary>> summaries;
				for (auto id : Documents())
				{
					auto extension = Path(id).extension().string();
					for (auto& c : extension)
					{
						if (c >= 'A' && c <= 'Z')
						{
							c += 'a' - 'A';
						}
					}

					if (extension == ".h" || extension == ".hpp" || extension == ".hh" ||
					extension == ".hxx")
					{
						summaries.push_back(Summary(id));
					}
				}

				return std::make_shared<const ProjectIndex>(
				ProjectIndex::FromSummaries(std::move(summaries)));
			},
			[this](Clock::time_point start)
			{
				++m_state->project_cache->metrics.project_index_count;
				m_state->project_cache->metrics.project_index_ns += Elapsed(start);
			},
			[] {});
	}

	std::expected<std::shared_ptr<const ProjectSymbolIndex>, SymbolIndexError> AnalysisSnapshot::
	SymbolIndex(
		std::stop_token stop) const
	{
		if (stop.stop_requested())
		{
			return std::unexpected(SymbolIndexError::Cancelled);
		}

		if (!m_state)
		{
			auto empty = ProjectSymbolIndex::Build(*this, {}, stop);
			if (!empty)
			{
				return std::unexpected(empty.error());
			}

			return std::make_shared<const ProjectSymbolIndex>(std::move(*empty));
		}

		auto& cache = *m_state->symbol_cache;
		std::unique_lock lock(cache.mutex);
		while (cache.computing && !cache.index && !stop.stop_requested())
		{
			cache.ready.wait_for(lock, std::chrono::milliseconds(10));
		}

		if (stop.stop_requested())
		{
			return std::unexpected(SymbolIndexError::Cancelled);
		}

		if (cache.index)
		{
			return cache.index;
		}

		cache.computing = true;
		lock.unlock();
		try
		{
			const auto start = Clock::now();
			auto index       = ProjectSymbolIndex::Build(*this, {}, stop);
			lock.lock();
			if (index && !stop.stop_requested())
			{
				cache.index = std::make_shared<const ProjectSymbolIndex>(std::move(*index));
				++cache.metrics.symbol_index_count;
				cache.metrics.symbol_index_ns += Elapsed(start);
			}

			cache.computing = false;
			auto result     = cache.index;
			lock.unlock();
			cache.ready.notify_all();
			if (!result)
			{
				return std::unexpected(index ? SymbolIndexError::Cancelled : index.error());
			}

			return result;
		}
		catch (...)
		{
			if (!lock.owns_lock())
			{
				lock.lock();
			}

			cache.computing = false;
			lock.unlock();
			cache.ready.notify_all();
			throw;
		}
	}

	std::shared_ptr<const HeaderSummary> AnalysisSnapshot::Summary(DocumentId document) const
	{
		auto data = Document(document);
		if (!data)
			return {};
		auto semantic = Semantic(document);
		return Query(
			data->mutex, data->ready, data->summarizing, data->summary,
			[&]
			{
				return HeaderSummary::FromModel(*semantic, data->path);
			},
			[](Clock::time_point) {},[] {});
	}

	AnalysisMetrics AnalysisSnapshot::Metrics() const
	{
		AnalysisMetrics total;
		if (!m_state)
		{
			return total;
		}

		for (const auto& data : m_state->documents)
		{
			if (!data)
			{
				continue;
			}

			std::scoped_lock lock(data->mutex, data->syntax->mutex);
			total.parse_count += data->syntax->metrics.parse_count;
			total.parse_ns += data->syntax->metrics.parse_ns;
			total.bind_count += data->metrics.bind_count;
			total.bind_ns += data->metrics.bind_ns;
			total.type_count += data->metrics.type_count;
			total.type_ns += data->metrics.type_ns;
			total.cache_hits += data->metrics.cache_hits + data->syntax->metrics.cache_hits;
			if (data->semantic)
			{
				total.semantic_allocations += data->semantic->ArenaAllocations();
			}

			if (data->types)
			{
				total.type_allocations += data->types->ArenaAllocations();
			}
		}

		{
			std::lock_guard lock(m_state->project_cache->mutex);
			total.project_index_count = m_state->project_cache->metrics.project_index_count;
			total.project_index_ns    = m_state->project_cache->metrics.project_index_ns;
		}
		{
			std::lock_guard lock(m_state->symbol_cache->mutex);
			total.symbol_index_count = m_state->symbol_cache->metrics.symbol_index_count;
			total.symbol_index_ns    = m_state->symbol_cache->metrics.symbol_index_ns;
		}
		return total;
	}

	MemoryBudget AnalysisSnapshot::Memory() const
	{
		MemoryBudget total;
		if (!m_state)
		{
			return total;
		}

		std::unordered_set<const HeaderSummary*> counted;
		std::unordered_set<const void*> sources;
		std::unordered_set<const void*> tokens;
		std::unordered_set<const ParseTree*> trees;
		const auto sourceBytes = [&sources](const std::shared_ptr<const std::string>& source)
		{
			return source && sources.insert(source.get()).second ? source->capacity() : 0;
		};
		const auto treeBytes = [&tokens, &trees](const ParseTree& tree)
		{
			if (!trees.insert(&tree).second)
			{
				return std::size_t {0};
			}

			const auto bytes = tree.StorageBytes();
			return tokens.insert(tree.Tokens().data()).second
			? bytes
			: bytes - tree.Tokens().capacity() * sizeof(Token);
		};
		for (const auto& data : m_state->documents)
		{
			if (!data)
			{
				continue;
			}

			std::scoped_lock lock(data->mutex, data->syntax->mutex);
			total.source_bytes +=
				data->sourceBuffer ? data->sourceBuffer->StorageBytes(sources)
			: data->source ? sourceBytes(data->source)
			: (data->syntax->tree ? data->syntax->tree->Source().size() : 0);
			if (data->tokenBuffer)
			{
				total.syntax_bytes += data->tokenBuffer->StorageBytes(tokens);
			}

			if (const auto & tree = data->syntax->tree)
			{
				total.syntax_bytes += treeBytes(*tree);
			}

			if (const auto & base = data->syntax->base)
			{
				total.retained_base_bytes += treeBytes(*base);
				auto source = base->SharedSource();
				total.retained_base_bytes += source ? sourceBytes(source) : base->Source().size();
			}

			if (data->semantic)
			{
				total.semantic_arena_bytes += data->semantic->ArenaReservedBytes();
			}

			if (data->types)
			{
				total.type_arena_bytes += data->types->ArenaReservedBytes();
			}

			if (data->summary && counted.insert(data->summary.get()).second)
			{
				total.project_string_bytes += data->summary->PoolBytes();
			}
		}

		std::lock_guard lock(m_state->project_cache->mutex);
		auto project = m_state->project ? m_state->project : m_state->project_cache->index;
		if (project)
		{
			for (const auto& summary : project->Summaries())
			{
				if (counted.insert(summary.get()).second)
				{
					total.project_string_bytes += summary->PoolBytes();
				}
			}
		}

		{
			std::lock_guard symbol_lock(m_state->symbol_cache->mutex);
			if (m_state->symbol_cache->index)
			{
				total.symbol_index_bytes = m_state->symbol_cache->index->StorageBytes();
			}
		}

		return total;
	}

	std::expected<AnalysisSnapshot, WorkspaceError> AnalysisSnapshot::WithSyntax(
		DocumentId document,
		std::shared_ptr<const ParseTree> tree,
		ParserOptions options) const
	{
		auto data = Document(document);
		if (!data)
		{
			return std::unexpected(WorkspaceError::InvalidDocument);
		}

		const auto source =
			data->Source() ? std::string_view(*data->Source()) : Syntax(document)->Source();
		if (!tree || tree->Cancelled() || tree->Source() != source ||
			tree->Standard() != options.standard)
		{
			return std::unexpected(WorkspaceError::InvalidSource);
		}

		if (!tree->SharedSource() && data->Source() &&
			tree->Source().data() != data->Source()->data())
		{
			return std::unexpected(WorkspaceError::InvalidSource);
		}

		{
			std::lock_guard lock(data->syntax->mutex);
			if (SameOptions(data->options, options))
			{
				if (!data->syntax->tree)
				{
					data->syntax->tree = std::move(tree);
					data->syntax->base.reset();
					return *this;
				}

				if (data->syntax->tree == tree)
				{
					return *this;
				}
			}
		}
		auto state            = std::make_shared<detail::WorkspaceState>(*m_state);
		auto derived          = NewDocument(*data);
		derived->options      = std::move(options);
		derived->syntax->tree = std::move(tree);
		state->documents.Set(document, std::move(derived));
		state->project.reset();
		return AnalysisSnapshot(std::move(state));
	}

	AnalysisSnapshot AnalysisSnapshot::FromSyntax(
		std::shared_ptr<const ParseTree> tree,
		ParserOptions options,
		std::filesystem::path path)
	{
		if (!tree || tree->Cancelled())
			return {};
		auto state             = std::make_shared<detail::WorkspaceState>();
		auto document          = std::make_shared<detail::DocumentAnalysis>();
		document->path         = std::move(path);
		document->source       = tree->SharedSource();
		options.standard       = tree->Standard();
		document->options      = std::move(options);
		document->syntax->tree = std::move(tree);
		state->paths.Write().emplace(PathKey(document->path), 0);
		state->documents.push_back(std::move(document));
		state->dependencies.Write().emplace_back();
		state->dependents.Write().emplace_back();
		state->explicit_dependencies.Write().push_back(false);
		return AnalysisSnapshot(std::move(state));
	}

	Workspace::Workspace() : m_state(std::make_shared<detail::WorkspaceState>()) {}

	std::expected<ProjectLoadReport, ProjectLoadError> Workspace::LoadProjectSources(
		ProjectLoadLimits limits,
		std::stop_token stop)
	{
		if (stop.stop_requested())
		{
			return std::unexpected(ProjectLoadError::Cancelled);
		}

		std::shared_ptr<const detail::WorkspaceState> base;
		{
			std::lock_guard lock(m_mutex);
			base = m_state;
		}
		struct Input
		{
			std::filesystem::path path;
			std::shared_ptr<const std::string> source;
			ParserOptions options;
			bool existing = false;
		};

		std::vector<Input> inputs;
		std::unordered_set<std::string> visited;
		std::vector<std::filesystem::path> roots;
		std::size_t bytes = 0;
		ProjectLoadReport report;
		for (const auto& document : base->documents)
			if (document)
		{
			if (!document->Source() || document->Source()->size() > limits.source_bytes - bytes)
			{
				return std::unexpected(ProjectLoadError::LimitReached);
			}

			bytes += document->Source()->size();
			inputs.push_back({document->path, document->Source(), document->options, true});
			visited.insert(PathKey(document->path));
		}

		if (inputs.size() > limits.documents)
		{
			return std::unexpected(ProjectLoadError::LimitReached);
		}

		if (base->compilation)
		{
			for (const auto& command : base->compilation->Commands())
			{
				roots.push_back(command.file);
			}
		}

		std::ranges::sort(roots,
			[](const auto& a, const auto& b)
			{
				return PathKey(a) < PathKey(b);
			});
		const auto read =
			[&](const std::filesystem::path& path)->std::expected<void, ProjectLoadError> {
			if (stop.stop_requested())
			{
				return std::unexpected(ProjectLoadError::Cancelled);
			}

			if (!visited.insert(PathKey(path)).second)
				return {};
			std::error_code error;
			const auto size = std::filesystem::file_size(path, error);
			if (error)
			{
				report.unavailable.push_back(path);
				return {};
			}

			if (size > limits.source_bytes - bytes || inputs.size() >= limits.documents)
			{
				return std::unexpected(ProjectLoadError::LimitReached);
			}

			std::ifstream file(path, std::ios::binary);
			auto source = std::make_shared<std::string>(static_cast<std::size_t>(size), '\0');
			file.read(source->data(), static_cast<std::streamsize>(source->size()));
			// Refuse changing files rather than pinning a truncated read.
			if (!file || file.peek() != std::char_traits<char>::eof())
			{
				report.unavailable.push_back(path);
				return {};
			}

			ParserOptions options;
			if (base->compilation)
			{
				if (const auto command = base->compilation->FindOrNearest(path))
				{
					options = CommandOptions(*command);
				}
			}

			bytes += source->size();
			inputs.push_back({path, std::move(source), std::move(options), false});
			return {};
		};
		for (const auto& path : roots)
		{
			const auto loaded = read(path);
			if (!loaded)
			{
				return std::unexpected(loaded.error());
			}
		}

		for (std::size_t next = 0; next < inputs.size(); ++next)
		{
			if (stop.stop_requested())
			{
				return std::unexpected(ProjectLoadError::Cancelled);
			}

			// Copy before appending: the queue can reallocate during read().
			const auto input = inputs[next];
			const auto command =
				base->compilation ? base->compilation->FindOrNearest(input.path) : nullptr;
			for (const auto& target : Includes(*input.source, input.options))
			{
				const auto name = target.substr(1, target.size() - 2);
				std::vector<std::filesystem::path> directories;
				if (target.front() == '"')
				{
					directories.push_back(input.path.parent_path());
					if (command)
					{
						directories.insert(directories.end(), command->quote_directories.begin(),
							command->quote_directories.end());
					}
				}

				if (command)
				{
					directories.insert(directories.end(), command->include_directories.begin(),
						command->include_directories.end());
				}

				bool found = false;
				for (const auto& directory : directories)
				{
					const auto candidate = directory / name;
					std::error_code error;
					if (!visited.contains(PathKey(candidate)) &&
						!std::filesystem::is_regular_file(candidate, error))
					{
						continue;
					}

					const auto loaded = read(candidate);
					if (!loaded)
					{
						return std::unexpected(loaded.error());
					}

					found = true;
					break;
				}

				if (!found)
				{
					report.unavailable.emplace_back(name);
				}
			}
		}

		if (stop.stop_requested())
		{
			return std::unexpected(ProjectLoadError::Cancelled);
		}

		std::lock_guard lock(m_mutex);
		if (m_state != base)
		{
			return std::unexpected(ProjectLoadError::StaleSnapshot);
		}

		auto state = std::make_shared<detail::WorkspaceState>(*base);
		for (auto& input : inputs)
		{
			if (stop.stop_requested())
			{
				return std::unexpected(ProjectLoadError::Cancelled);
			}

			if (input.existing)
			{
				continue;
			}

			if (state->documents.size() >= InvalidDocument)
			{
				return std::unexpected(ProjectLoadError::LimitReached);
			}

			auto document             = std::make_shared<detail::DocumentAnalysis>();
			document->path            = std::move(input.path);
			document->source          = std::move(input.source);
			document->options         = std::move(input.options);
			document->include_targets = Includes(*document->Source(), document->options);
			const auto id             = static_cast<DocumentId>(state->documents.size());
			state->paths.Write().emplace(PathKey(document->path), id);
			state->documents.push_back(std::move(document));
			state->dependencies.Write().emplace_back();
			state->dependents.Write().emplace_back();
			state->explicit_dependencies.Write().push_back(false);
			report.added.push_back(id);
		}

		if (!report.added.empty())
		{
			if (stop.stop_requested())
			{
				return std::unexpected(ProjectLoadError::Cancelled);
			}

			state->project.reset();
			ResolveIncludes(*state);
			++state->revision;
			m_state = std::move(state);
		}

		return report;
	}

	AnalysisSnapshot Workspace::Snapshot() const
	{
		std::lock_guard lock(m_mutex);
		return AnalysisSnapshot(m_state);
	}

	std::expected<DocumentId, WorkspaceError> Workspace::Open(
		std::filesystem::path path,
		std::shared_ptr<const std::string> source,
		std::int64_t version,
		ParserOptions options)
	{
		return OpenImpl(std::move(path), std::move(source), version, std::move(options), {}, {});
	}

	std::expected<DocumentId, WorkspaceError> Workspace::OpenImpl(
		std::filesystem::path path,
		std::shared_ptr<const std::string> source,
		std::int64_t version,
		ParserOptions options,
		std::shared_ptr<const SourceSnapshot> buffer,
		std::shared_ptr<const TokenSnapshot> tokens)
	{
		if (!source)
		{
			return std::unexpected(WorkspaceError::InvalidSource);
		}

		const auto initial = Snapshot();
		if (SameOptions(options, ParserOptions {}) && initial.m_state->compilation)
		{
			if (const auto command = initial.m_state->compilation->FindOrNearest(path))
			{
				options = CommandOptions(*command);
			}
		}

		auto includeTargets = Includes(*source, options);
		auto key            = PathKey(path);
		std::lock_guard lock(m_mutex);
		if (initial.m_state->compilation != m_state->compilation)
		{
			return std::unexpected(WorkspaceError::StaleVersion);
		}

		if (const auto found = m_state->paths.Read().find(key);
			found != m_state->paths.Read().end())
		{
			return std::unexpected(WorkspaceError::StaleVersion);
		}

		if (m_state->documents.size() >= InvalidDocument)
		{
			return std::unexpected(WorkspaceError::IdExhausted);
		}

		auto state                = std::make_shared<detail::WorkspaceState>(*m_state);
		auto document             = std::make_shared<detail::DocumentAnalysis>();
		document->path            = std::move(path);
		document->source          = std::move(source);
		document->version         = version;
		document->options         = std::move(options);
		document->include_targets = std::move(includeTargets);
		document->sourceBuffer    = std::move(buffer);
		document->tokenBuffer     = std::move(tokens);
		if (document->sourceBuffer)
		{
			document->source.reset();
		}

		const auto id = static_cast<DocumentId>(state->documents.size());
		state->documents.push_back(std::move(document));
		state->dependencies.Write().emplace_back();
		state->dependents.Write().emplace_back();
		state->explicit_dependencies.Write().push_back(false);
		state->paths.Write().emplace(std::move(key), id);
		state->project.reset();
		ResolveIncludes(*state);
		++state->revision;
		m_state = std::move(state);
		return id;
	}

	std::expected<DocumentId, WorkspaceError> Workspace::OpenBuffer(
		std::filesystem::path path,
		std::shared_ptr<const SourceSnapshot> source,
		std::shared_ptr<const TokenSnapshot> tokens,
		std::int64_t version,
		ParserOptions options)
	{
		if (!source ||!tokens)
		{
			return std::unexpected(WorkspaceError::InvalidSource);
		}

		auto contiguous = source->Materialize();
		return OpenImpl(std::move(path), std::move(contiguous), version, std::move(options),
			std::move(source), std::move(tokens));
	}

	std::expected<void, WorkspaceError> Workspace::UpdateBuffer(
		DocumentId document,
		std::shared_ptr<const SourceSnapshot> source,
		std::shared_ptr<const TokenSnapshot> tokens,
		std::int64_t version,
		Lexer::TextEdit edit)
	{
		if (!source ||!tokens)
		{
			return std::unexpected(WorkspaceError::InvalidSource);
		}

		auto before   = Snapshot();
		auto prepared = before.Document(document);
		if (!prepared ||!prepared->sourceBuffer ||!prepared->tokenBuffer)
		{
			return std::unexpected(WorkspaceError::InvalidDocument);
		}

		const auto oldSize = prepared->sourceBuffer->Size();
		if (edit.offset > oldSize || edit.old_length > oldSize - edit.offset ||
			edit.offset > source->Size() || edit.new_length > source->Size() - edit.offset ||
			oldSize - edit.old_length != source->Size() - edit.new_length)
		{
			return std::unexpected(WorkspaceError::InvalidSource);
		}

		const bool directivesUnchanged = prepared->tokenBuffer->PreservesDirectives(
			*prepared->sourceBuffer, edit, source->Read(edit.offset, edit.new_length));
		auto includeTargets = directivesUnchanged
		? prepared->include_targets
		: Includes(*source->Materialize(), prepared->options);
		std::lock_guard lock(m_mutex);
		const auto current = AnalysisSnapshot(m_state).Document(document);
		if (!current)
		{
			return std::unexpected(WorkspaceError::InvalidDocument);
		}

		if (version <= current->version || current->sourceBuffer != prepared->sourceBuffer ||
			!SameOptions(current->options, prepared->options))
		{
			return std::unexpected(WorkspaceError::StaleVersion);
		}

		auto state           = std::make_shared<detail::WorkspaceState>(*m_state);
		auto replacement     = NewDocument(*current);
		replacement->version = version;
		replacement->source.reset();
		replacement->sourceBuffer    = std::move(source);
		replacement->tokenBuffer     = std::move(tokens);
		replacement->include_targets = std::move(includeTargets);
		{
			std::lock_guard cacheLock(current->syntax->mutex);
			replacement->syntax->base = current->syntax->tree;
		}
		replacement->syntax->edit                = edit;
		replacement->syntax->directivesUnchanged = directivesUnchanged;
		const bool graphChanged                  = replacement->include_targets != current->include_targets;
		state->documents.Set(document, std::move(replacement));
		state->project.reset();
		InvalidateDependents(*state, document);
		if (graphChanged)
		{
			ResolveIncludes(*state);
		}

		++state->revision;
		m_state = std::move(state);
		return {};
	}

	std::expected<void, WorkspaceError> Workspace::Update(
		DocumentId document,
		std::shared_ptr<const std::string> source,
		std::int64_t version)
	{
		return UpdateImpl(document, std::move(source), version, nullptr);
	}

	std::expected<void, WorkspaceError> Workspace::Update(
		DocumentId document,
		std::shared_ptr<const std::string> source,
		std::int64_t version,
		Lexer::TextEdit edit)
	{
		return UpdateImpl(document, std::move(source), version, &edit);
	}

	std::expected<void, WorkspaceError> Workspace::UpdateImpl(
		DocumentId document,
		std::shared_ptr<const std::string> source,
		std::int64_t version,
		const Lexer::TextEdit* edit)
	{
		if (!source)
		{
			return std::unexpected(WorkspaceError::InvalidSource);
		}

		const auto before   = Snapshot();
		const auto prepared = before.Document(document);
		std::vector<std::string> includeTargets;
		if (prepared && source != prepared->Source())
		{
			includeTargets = Includes(*source, prepared->options);
		}

		std::lock_guard lock(m_mutex);
		if (!AnalysisSnapshot(m_state).Contains(document))
		{
			return std::unexpected(WorkspaceError::InvalidDocument);
		}

		const auto& old = m_state->documents[document];
		if (edit &&
			(edit->offset > old->Source()->size() ||
			edit->old_length > old->Source()->size() - edit->offset ||
			edit->offset > source->size() || edit->new_length > source->size() - edit->offset ||
			old->Source()->size() - edit->old_length != source->size() - edit->new_length))
		{
			return std::unexpected(WorkspaceError::InvalidSource);
		}

		if (version <= old->version)
		{
			return std::unexpected(WorkspaceError::StaleVersion);
		}

		auto state           = std::make_shared<detail::WorkspaceState>(*m_state);
		auto replacement     = NewDocument(*old);
		replacement->version = version;
		bool graph_changed   = false;
		if (source == old->Source() ||(!edit && *source == *old->Source()))
		{
			// Version-only change: all cached data remains valid.
			replacement->syntax = old->syntax;
			std::lock_guard cache_lock(old->mutex);
			replacement->semantic = old->semantic;
			replacement->types    = old->types;
			replacement->summary  = old->summary;
			replacement->metrics  = old->metrics;
			state->project_cache  = m_state->project_cache;
		}
		else
		{
			{
				std::lock_guard cache_lock(old->syntax->mutex);
				replacement->syntax->base = old->syntax->tree;
			}
			if (edit)
			{
				replacement->syntax->edit = *edit;
			}
			else if (replacement->syntax->base)
			{
				// A single edit hull is conservative for arbitrary whole-buffer
				// updates. The grammar already knows which top-level items it
				// can reuse safely (including changes in known type names).
				const auto& before = *old->Source();
				const auto& after  = *source;
				std::size_t prefix = 0;
				while (prefix < before.size() && prefix < after.size() &&
					before[prefix] == after[prefix])
				{
					++prefix;
				}

				std::size_t suffix = 0;
				while (suffix < before.size() - prefix && suffix < after.size() - prefix &&
					before[before.size() - suffix - 1] == after[after.size() - suffix - 1])
				{
					++suffix;
				}

				replacement->syntax->edit = {prefix, before.size() - prefix - suffix,
					after.size() - prefix - suffix};
			}

			replacement->source = std::move(source);
			replacement->sourceBuffer.reset();
			replacement->tokenBuffer.reset();
			state->project.reset();
			replacement->include_targets =
				prepared&& SameOptions(prepared->options, old->options)
			? std::move(includeTargets)
			: Includes(*replacement->Source(), replacement->options);
			graph_changed = replacement->include_targets != old->include_targets;
			InvalidateDependents(*state, document);
		}

		state->documents.Set(document, std::move(replacement));
		if (graph_changed)
		{
			ResolveIncludes(*state);
		}

		++state->revision;
		m_state = std::move(state);
		return {};
	}

	void Workspace::TrimCaches(std::size_t retainedBytes)
	{
		struct Candidate
		{
			std::shared_ptr<const detail::DocumentAnalysis> document;
			Clock::time_point lastUsed;
			std::size_t bytes;
		};

		const auto snapshot = Snapshot();
		std::vector<Candidate> candidates;
		std::size_t total = 0;
		for (const auto& document : snapshot.m_state->documents)
		{
			if (!document)
			{
				continue;
			}

			std::scoped_lock lock(document->mutex, document->syntax->mutex);
			if (document->binding || document->typing || document->summarizing ||
				document->syntax->computing)
			{
				continue;
			}

			std::size_t bytes = document->semantic ? document->semantic->ArenaReservedBytes() : 0;
			bytes += document->types ? document->types->ArenaReservedBytes() : 0;
			bytes += document->syntax->tree ? document->syntax->tree->StorageBytes() : 0;
			bytes += document->syntax->base ? document->syntax->base->StorageBytes() : 0;
			total += bytes;
			candidates.push_back(
				{document, std::max(document->lastUsed, document->syntax->lastUsed), bytes});
		}

		std::sort(candidates.begin(), candidates.end(),[](const auto& left, const auto& right)
			{
				return left.lastUsed < right.lastUsed;
			});
		for (const auto& candidate : candidates)
		{
			if (total <= retainedBytes)
			{
				break;
			}

			const auto& document = candidate.document;
			std::scoped_lock lock(document->mutex, document->syntax->mutex);
			if (document->binding || document->typing || document->summarizing ||
				document->syntax->computing ||
				std::max(document->lastUsed, document->syntax->lastUsed) != candidate.lastUsed)
			{
				continue;
			}

			document->summary.reset();
			document->types.reset();
			document->semantic.reset();
			document->syntax->tree.reset();
			document->syntax->base.reset();
			total -= candidate.bytes;
		}
	}

	std::expected<void, WorkspaceError> Workspace::Close(DocumentId document)
	{
		std::lock_guard lock(m_mutex);
		if (!AnalysisSnapshot(m_state).Contains(document))
		{
			return std::unexpected(WorkspaceError::InvalidDocument);
		}

		auto state = std::make_shared<detail::WorkspaceState>(*m_state);
		InvalidateDependents(*state, document);
		state->paths.Write().erase(PathKey(state->documents[document]->path));
		state->documents.Set(document, {});
		state->project.reset();
		state->dependencies.Write()[document].clear();
		for (auto& edges : state->dependencies.Write())
		{
			std::erase(edges, document);
		}

		ResolveIncludes(*state);
		++state->revision;
		m_state = std::move(state);
		return {};
	}

	std::expected<void, WorkspaceError> Workspace::SetDependencies(
		DocumentId document,
		std::span<const DocumentId> dependencies)
	{
		std::lock_guard lock(m_mutex);
		AnalysisSnapshot snapshot(m_state);
		if (!snapshot.Contains(document))
		{
			return std::unexpected(WorkspaceError::InvalidDocument);
		}

		for (auto id : dependencies)
		{
			if (!snapshot.Contains(id))
			{
				return std::unexpected(WorkspaceError::InvalidDocument);
			}
		}

		std::vector<DocumentId> edges(dependencies.begin(), dependencies.end());
		std::sort(edges.begin(), edges.end());
		edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
		if (edges == m_state->dependencies.Read()[document] &&
			m_state->explicit_dependencies.Read()[document])
			return {};
		auto state                                     = std::make_shared<detail::WorkspaceState>(*m_state);
		state->dependencies.Write()[document]          = std::move(edges);
		state->explicit_dependencies.Write()[document] = true;
		ReverseEdges(*state);
		auto replacement    = NewDocument(*state->documents[document]);
		replacement->syntax = state->documents[document]->syntax;
		state->documents.Set(document, std::move(replacement));
		InvalidateDependents(*state, document);
		++state->revision;
		m_state = std::move(state);
		return {};
	}

	std::expected<void, WorkspaceError> Workspace::SetOptions(
		DocumentId document,
		ParserOptions options)
	{
		std::lock_guard lock(m_mutex);
		if (!AnalysisSnapshot(m_state).Contains(document))
		{
			return std::unexpected(WorkspaceError::InvalidDocument);
		}

		if (SameOptions(m_state->documents[document]->options, options))
			return {};
		auto state                   = std::make_shared<detail::WorkspaceState>(*m_state);
		auto replacement             = NewDocument(*state->documents[document]);
		replacement->options         = std::move(options);
		replacement->include_targets = Includes(*replacement->Source(), replacement->options);
		state->documents.Set(document, std::move(replacement));
		InvalidateDependents(*state, document);
		state->project.reset();
		ResolveIncludes(*state);
		++state->revision;
		m_state = std::move(state);
		return {};
	}

	void Workspace::SetCompilationDatabase(std::shared_ptr<const CompileDatabase> database)
	{
		std::lock_guard lock(m_mutex);
		if (m_state->compilation == database)
		{
			return;
		}

		auto state         = std::make_shared<detail::WorkspaceState>(*m_state);
		state->compilation = std::move(database);
		state->project.reset();
		for (DocumentId id = 0; id < state->documents.size(); ++id)
		{
			const auto& document = state->documents[id];
			if (!document)
			{
				continue;
			}

			auto replacement     = NewDocument(*document);
			replacement->options = {};
			if (state->compilation)
			{
				if (auto command = state->compilation->FindOrNearest(document->path))
				{
					replacement->options = CommandOptions(*command);
				}
			}

			replacement->include_targets = Includes(*replacement->Source(), replacement->options);
			state->documents.Set(id, std::move(replacement));
		}

		ResolveIncludes(*state);
		++state->revision;
		m_state = std::move(state);
	}

	void Workspace::SetProjectIndex(std::shared_ptr<const ProjectIndex> index)
	{
		std::lock_guard lock(m_mutex);
		if (m_state->project == index)
		{
			return;
		}

		auto state     = std::make_shared<detail::WorkspaceState>(*m_state);
		state->project = std::move(index);
		for (DocumentId id = 0; id < state->documents.size(); ++id)
		{
			const auto& document = state->documents[id];
			if (!document)
			{
				continue;
			}

			auto replacement    = NewDocument(*document);
			replacement->syntax = document->syntax;
			state->documents.Set(id, std::move(replacement));
		}

		++state->revision;
		m_state = std::move(state);
	}
} // namespace heimdall
