#include "Server.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <shared_mutex>
#include <stop_token>
#include <syncstream>
#include <unordered_map>
#include <unordered_set>

#include <Heimdall/Completion.hpp>
#include <Heimdall/CompletionRanking.hpp>
#include <Heimdall/Doxygen.hpp>
#include <Heimdall/Formatter.hpp>
#include <Heimdall/IncludeAnalyzer.hpp>
#include <Heimdall/IncludeInsertion.hpp>
#include <Heimdall/IndexRename.hpp>
#include <Heimdall/IndexStore.hpp>
#include <Heimdall/Lexer.hpp>
#include <Heimdall/ModuleUnit.hpp>
#include <Heimdall/Navigation.hpp>
#include <Heimdall/ParseTree.hpp>
#include <Heimdall/Profiling.hpp>
#include <Heimdall/Refactoring.hpp>
#include <Heimdall/RuleConfig.hpp>
#include <Heimdall/RuleEngine.hpp>
#include <Heimdall/SemanticAnalyzer.hpp>
#include <Heimdall/SemanticHighlight.hpp>
#include <Heimdall/SemanticRules.hpp>
#include <Heimdall/ShardBuilder.hpp>
#include <Heimdall/SymbolOutline.hpp>
#include <Heimdall/SyntaxHighlight.hpp>
#include <Heimdall/Trace.hpp>
#include <Heimdall/TypeHierarchy.hpp>

#include "Document.hpp"
#include "JsonRpc.hpp"
#include "MacroProtocol.hpp"
#include "SemanticTokens.hpp"
#include "SymbolProtocol.hpp"
#include "WorkspaceFiles.hpp"

namespace heimdall::lsp
{

	namespace
	{
		constexpr std::size_t kDocumentWorkers = 3;
		constexpr std::size_t kParseWorkers    = 2;

		std::uint64_t PositionNumber(simdjson::dom::object position, const char* key);

		// "850 ms" below a second, "12.3 s" above.
		std::string FormatDuration(std::uint64_t milliseconds)
		{
			constexpr std::uint64_t kMillisecondsPerSecond = 1000;
			constexpr std::uint64_t kTenthsPerSecond       = 10;
			if (milliseconds < kMillisecondsPerSecond)
			{
				return std::to_string(milliseconds) + " ms";
			}

			const std::uint64_t tenths =
				milliseconds * kTenthsPerSecond / kMillisecondsPerSecond;
			return std::to_string(tenths / kTenthsPerSecond) + "." +
				std::to_string(tenths % kTenthsPerSecond) + " s";
		}

		// A signature fix (api/pass-by-const-reference, api/pass-by-value) must
		// land on the declaration too, or the program no longer compiles. The
		// diagnostic only covers the definition, so the sibling declarations
		// are resolved through the workspace index and edited with the same
		// type surgery on their own spelling.
		struct ParamSite
		{
			heimdall::SymbolId owner    = heimdall::kNone;
			std::size_t ordinal         = 0; // positional, among ParameterDeclarations
			std::size_t paramCount      = 0;
			std::uint32_t funcNameToken = heimdall::kNone;
			std::string name;
		};

		std::unordered_map<std::uint32_t, heimdall::SymbolId> ParamFunctionMap(
			const heimdall::SymbolTable& symbols)
		{
			std::unordered_map<std::uint32_t, heimdall::SymbolId> map;
			for (heimdall::SymbolId symbol = 0; symbol < symbols.Size(); ++symbol)
			{
				if (symbols.kind[symbol] == heimdall::SymbolKind::Function)
				{
					map[symbols.decl_node[symbol]] = symbol;
				}
			}

			return map;
		}

		heimdall::SymbolId ParamOwner(
			const heimdall::SemanticModel& model,
			const std::unordered_map<std::uint32_t, heimdall::SymbolId>& function_of,
			std::uint32_t node)
		{
			const auto& soa       = model.Tree().NodesSoA();
			std::uint32_t current = node;
			for (std::size_t steps = 0; steps <= soa.size(); ++steps)
			{
				const auto kind = soa.Kind(current);
				if (kind == heimdall::GrammarKind::FunctionDefinition)
				{
					const auto found = function_of.find(current);
					return found == function_of.end() ? heimdall::kNone : found->second;
				}

				if (kind == heimdall::GrammarKind::LambdaExpression)
				{
					return heimdall::kNone;
				}

				const auto parent = soa.Parent(current);
				if (parent >= soa.size() || parent == current)
				{
					return heimdall::kNone;
				}

				current = parent;
			}

			return heimdall::kNone;
		}

		// Positional ParameterDeclaration children of a function's suffix, for
		// definitions and pure declarations alike. Unnamed parameters have no
		// symbol, so the ordinal must be positional to stay aligned.
		std::vector<std::uint32_t> SuffixParams(
			const heimdall::SemanticModel& model,
			heimdall::SymbolId function)
		{
			const auto& soa     = model.Tree().NodesSoA();
			const auto& symbols = model.Symbols();
			for (const auto child : model.ChildrenOf(symbols.decl_node[function]))
			{
				if (soa.Kind(child) != heimdall::GrammarKind::Declarator)
				{
					continue;
				}

				for (const auto suffix : model.ChildrenOf(child))
				{
					if (soa.Kind(suffix) != heimdall::GrammarKind::FunctionSuffix)
					{
						continue;
					}

					std::vector<std::uint32_t> parameters;
					for (const auto parameter : model.ChildrenOf(suffix))
					{
						if (soa.Kind(parameter) == heimdall::GrammarKind::ParameterDeclaration)
						{
							parameters.push_back(parameter);
						}
					}

					return parameters;
				}
			}

			return {};
		}

		ParamSite FindParamSite(
			const heimdall::ParseTree& tree,
			const heimdall::SemanticModel& model,
			std::size_t byteOffset)
		{
			const auto& symbols      = model.Symbols();
			const auto& tokens       = tree.Tokens();
			heimdall::SymbolId param = heimdall::kNone;
			for (heimdall::SymbolId symbol = 0; symbol < symbols.Size(); ++symbol)
			{
				if (symbols.kind[symbol] != heimdall::SymbolKind::Parameter ||
					symbols.name[symbol] == heimdall::kNone)
				{
					continue;
				}

				const auto token = symbols.decl_token[symbol];
				if (token < tokens.size() && tokens[token].offset == byteOffset)
				{
					param = symbol;
					break;
				}
			}

			if (param == heimdall::kNone)
			{
				return {};
			}

			const auto function_of = ParamFunctionMap(symbols);
			const auto owner       = ParamOwner(model, function_of, symbols.decl_node[param]);
			if (owner == heimdall::kNone)
			{
				return {};
			}

			const auto parameters = SuffixParams(model, owner);
			for (std::size_t i = 0; i < parameters.size(); ++i)
			{
				if (parameters[i] == symbols.decl_node[param])
				{
					return {owner, i, parameters.size(), symbols.decl_token[owner],
						std::string(model.Names().Text(symbols.name[param]))};
				}
			}

			return {};
		}

		bool IsSpace(char c) noexcept
		{
			return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
				c == '\v';
		}

		std::string_view Trimmed(std::string_view text)
		{
			while (!text.empty() && IsSpace(text.front()))
			{
				text.remove_prefix(1);
			}

			while (!text.empty() && IsSpace(text.back()))
			{
				text.remove_suffix(1);
			}

			return text;
		}

		// `const Foo &` -> `Foo`: drops `&` and standalone `const` words,
		// preserving every other spelling (notably `::` qualification).
		std::string StripConstRef(std::string_view raw)
		{
			const auto isWord = [](char c)
			{
				return (c >= 'a' && c <= 'z') ||(c >= 'A' && c <= 'Z') ||
					(c >= '0' && c <= '9') || c == '_';
			};
			struct Piece
			{
				bool word = false;
				std::string text;
			};

			std::vector<Piece> pieces;
			std::string current;
			bool currentWord = false;
			const auto push = [&]
			{
				if (!current.empty())
				{
					pieces.push_back({currentWord, current});
					current.clear();
				}
			};
			for (const char c : raw)
			{
				if (c == '&')
				{
					push();
					continue;
				}

				if (IsSpace(c))
				{
					push();
					continue;
				}

				const bool word = isWord(c);
				if (!current.empty() && word != currentWord)
				{
					push();
				}

				current += c;
				currentWord = word;
			}

			push();
			std::string out;
			std::string previous;
			for (const auto& piece : pieces)
			{
				if (piece.word && (piece.text == "const" || piece.text == "volatile"))
				{
					continue;
				}

				if (!out.empty() && previous != "::" && previous != "<" &&
					piece.text != "::" && piece.text != "," && piece.text != ">" &&
					piece.text != ">>")
				{
					out += ' ';
				}

				out += piece.text;
				previous = piece.text;
			}

			return out;
		}

		struct DeclEdit
		{
			std::size_t begin = 0;
			std::size_t end   = 0;
			std::string text;
			std::string rawType; // trimmed type spelling before the surgery
		};

		// `const Foo &` and `Foo` spell the same type: drops whitespace,
		// cv-qualifiers, reference markers and a leading `::` so sibling
		// declarations can be matched across spelling variations.
		std::string NormalizedType(std::string_view raw)
		{
			std::string out;
			std::string word;
			const auto flush = [&]
			{
				if (!word.empty() && word != "const" && word != "volatile")
				{
					out += word;
				}

				word.clear();
			};
			for (const char c : raw)
			{
				if (c == '&' || c == '*')
				{
					flush();
					continue;
				}

				if ((c >= 'a' && c <= 'z') ||(c >= 'A' && c <= 'Z') ||
					(c >= '0' && c <= '9') || c == '_')
				{
					word += c;
				}
				else
				{
					flush();
					if (!IsSpace(c))
					{
						out += c;
					}
				}
			}

			flush();
			while (out.starts_with("::"))
			{
				out.erase(0, 2);
			}

			return out;
		}

		// Recomputes the definition's type surgery on one sibling declaration,
		// keeping that declaration's own type spelling. False means "leave this
		// declaration alone" (complex spelling, arity drift, already matching).
		bool SiblingParamEdit(
			const heimdall::ParseTree& tree,
			const heimdall::SemanticModel& model,
			std::uint32_t funcNameByteOffset,
			std::size_t ordinal,
			std::size_t paramCount,
			bool toConstRef,
			std::string_view defParamName,
			DeclEdit& edit)
		{
			const auto& symbols         = model.Symbols();
			const auto& tokens          = tree.Tokens();
			heimdall::SymbolId function = heimdall::kNone;
			for (heimdall::SymbolId symbol = 0; symbol < symbols.Size(); ++symbol)
			{
				if (symbols.kind[symbol] != heimdall::SymbolKind::Function)
				{
					continue;
				}

				const auto token = symbols.decl_token[symbol];
				if (token < tokens.size() && tokens[token].offset == funcNameByteOffset)
				{
					function = symbol;
					break;
				}
			}

			if (function == heimdall::kNone)
			{
				return false;
			}

			const auto parameters = SuffixParams(model, function);
			if (parameters.size() != paramCount || ordinal >= parameters.size())
			{
				return false;
			}

			const auto node  = parameters[ordinal];
			const auto& soa  = tree.NodesSoA();
			const auto first = soa.FirstToken(node);
			const auto count = soa.TokenCount(node);
			if (count == 0 || first >= tokens.size())
			{
				return false;
			}

			const auto last = std::min(first + count, static_cast<std::uint32_t>(tokens.size()));
			// The binder only keeps parameter symbols for definitions, so a
			// pure declaration (`int Read(Snapshot);`) has no symbol to read
			// the name from: resolve it from the tokens instead. The span ends
			// before any default argument; brackets make it complex.
			std::uint32_t spanEnd = last;
			{
				std::size_t depth = 0;
				for (auto i = first; i < last; ++i)
				{
					const auto tok = tokens[i].tok;
					if (tok == heimdall::Tok::LParen || tok == heimdall::Tok::LBracket ||
						tok == heimdall::Tok::LBrace)
					{
						++depth;
					}
					else if (tok == heimdall::Tok::RParen || tok == heimdall::Tok::RBracket ||
						tok == heimdall::Tok::RBrace)
					{
						depth = depth == 0 ? 0 : depth - 1;
					}
					else if (tok == heimdall::Tok::Eq && depth == 0)
					{
						spanEnd = i;
						break;
					}
				}
			}

			const auto isPlainIdent = [&](std::uint32_t i)
			{
				return tokens[i].kind == heimdall::TokenKind::Identifier &&
					tokens[i].tok == heimdall::Tok::None;
			};
			// Preferred: the definition's own spelling, still at depth 0 and
			// behind an actual type (a lone match is the type itself, as in an
			// undecorated `Snapshot`).
			std::uint32_t name = heimdall::kNone;
			{
				std::size_t depth       = 0;
				std::uint32_t firstSig  = heimdall::kNone;
				std::uint32_t candidate = heimdall::kNone;
				for (auto i = first; i < spanEnd; ++i)
				{
					const auto tok = tokens[i].tok;
					if (tok == heimdall::Tok::LParen || tok == heimdall::Tok::LBracket ||
						tok == heimdall::Tok::LBrace)
					{
						++depth;
					}
					else if (tok == heimdall::Tok::RParen || tok == heimdall::Tok::RBracket ||
						tok == heimdall::Tok::RBrace)
					{
						depth = depth == 0 ? 0 : depth - 1;
					}
					else if (tokens[i].kind == heimdall::TokenKind::Whitespace)
					{
						continue;
					}
					else
					{
						if (firstSig == heimdall::kNone)
						{
							firstSig = i;
						}

						if (depth == 0 && isPlainIdent(i) &&
							tree.Text(tokens[i]) == defParamName)
						{
							candidate = i;
						}
					}
				}

				if (candidate != heimdall::kNone && candidate != firstSig)
				{
					name = candidate;
				}
			}

			std::string paramName;
			std::uint32_t scanEnd = spanEnd;
			if (name != heimdall::kNone)
			{
				scanEnd   = name;
				paramName = std::string(defParamName);
			}
			else
			{
				// Last significant token of the span: a trailing identifier
				// beside a real type is a renamed parameter (`Snapshot s`);
				// a lone identifier or one behind only cv-qualifiers is the
				// type itself (`Snapshot`, `const Foo`) and stays unnamed.
				std::uint32_t trailing    = heimdall::kNone;
				std::uint32_t significant = 0;
				bool realType             = false;
				for (auto i = first; i < spanEnd; ++i)
				{
					const auto& token = tokens[i];
					if (token.kind == heimdall::TokenKind::Whitespace)
					{
						continue;
					}

					++significant;
					trailing = i;
					if (token.tok != heimdall::Tok::KwConst &&
						token.tok != heimdall::Tok::KwVolatile)
					{
						realType = true;
					}
				}

				if (trailing != heimdall::kNone && significant > 1 && realType &&
					isPlainIdent(trailing))
				{
					// Not the definition's spelling, but unmistakably a name:
					// keep it instead of renaming.
					bool typeBefore = false;
					for (auto i = first; i < trailing; ++i)
					{
						if (tokens[i].kind != heimdall::TokenKind::Whitespace)
						{
							typeBefore = true;
							break;
						}
					}

					if (typeBefore)
					{
						name      = trailing;
						scanEnd   = trailing;
						paramName = std::string(tree.Text(tokens[trailing]));
					}
				}
			}

			bool hasConst = false, hasRef = false, hasRealType = false, veto = false;
			std::uint32_t typeToken = heimdall::kNone;
			std::size_t lastSigEnd  = 0;
			for (auto i = first; i < scanEnd; ++i)
			{
				const auto& token = tokens[i];
				if (token.kind == heimdall::TokenKind::Whitespace)
				{
					continue;
				}

				if (token.kind == heimdall::TokenKind::LineComment ||
					token.kind == heimdall::TokenKind::BlockComment)
				{
					return false;
				}

				if (token.tok == heimdall::Tok::Eq)
				{
					break;
				}

				if (token.tok == heimdall::Tok::LBracket ||
					token.tok == heimdall::Tok::LParen ||
					token.tok == heimdall::Tok::Ellipsis)
				{
					veto = true;
					break;
				}

				if (token.tok == heimdall::Tok::KwConst)
				{
					hasConst = true;
				}

				if (token.tok == heimdall::Tok::Amp || token.tok == heimdall::Tok::AmpAmp)
				{
					hasRef = true;
				}

				if (token.tok == heimdall::Tok::Star)
				{
					veto = true;
					break;
				}

				if (token.tok != heimdall::Tok::KwConst &&
					token.tok != heimdall::Tok::KwVolatile)
				{
					hasRealType = true;
				}

				if (typeToken == heimdall::kNone)
				{
					typeToken = i;
				}

				lastSigEnd = token.offset + token.length;
			}

			// A lone qualifier (`const Foo` read as type `const`, name `Foo`)
			// has no type to rewrite: leave it alone.
			if (veto || typeToken == heimdall::kNone || lastSigEnd == 0 ||!hasRealType)
			{
				return false;
			}

			const std::string_view source = tree.Source();
			const std::size_t typeBegin   = tokens[typeToken].offset;
			if (typeBegin > source.size() || lastSigEnd > source.size())
			{
				return false;
			}

			if (toConstRef)
			{
				if (hasRef)
				{
					return false;
				}

				// Named: `Type name` -> `const Type& name`. Unnamed: the type
				// itself is the whole range (`Type` -> `const Type&`).
				const bool named = name != heimdall::kNone;
				const std::size_t typeEnd =
					named ? tokens[name].offset : lastSigEnd;
				const std::size_t rangeEnd =
					named ? tokens[name].offset + tokens[name].length : lastSigEnd;
				if (typeEnd < typeBegin || rangeEnd < typeBegin || rangeEnd > source.size())
				{
					return false;
				}

				const std::string raw(Trimmed(source.substr(typeBegin, typeEnd - typeBegin)));
				if (raw.empty())
				{
					return false;
				}

				std::string replacement = hasConst ? raw : "const " + raw;
				replacement += '&';
				if (named)
				{
					replacement += ' ';
					replacement += paramName;
				}

				edit = {typeBegin, rangeEnd, std::move(replacement), raw};
				return true;
			}

			if (!hasRef)
			{
				return false;
			}

			// Named: `const Type& name` -> `Type name`. Unnamed: the type span
			// up to the last significant token (stopping before any `=`).
			const bool named = name != heimdall::kNone;
			const std::size_t typeEnd =
				named ? tokens[name].offset : lastSigEnd;
			const std::size_t rangeEnd =
				named ? tokens[name].offset + tokens[name].length : lastSigEnd;
			if (typeEnd < typeBegin || rangeEnd < typeBegin || rangeEnd > source.size())
			{
				return false;
			}

			const std::string raw(Trimmed(source.substr(typeBegin, typeEnd - typeBegin)));
			const std::string stripped =
				StripConstRef(raw);
			if (stripped.empty())
			{
				return false;
			}

			std::string replacement = stripped;
			if (named)
			{
				replacement += ' ';
				replacement += paramName;
			}

			edit = {typeBegin, rangeEnd, std::move(replacement), raw};
			return true;
		}

		// Interactive handlers + two for background work (compiler probing).
		// HEIMDALL_LSP_THREADS overrides the default when set (clamped to 2..16),
		// so constrained CI boxes and large workstations can both meet <50ms.
		std::size_t PoolSize()
		{
			std::size_t threads = std::thread::hardware_concurrency();
			if (const char * env = std::getenv("HEIMDALL_LSP_THREADS"))
			{
				char* end                  = nullptr;
				const unsigned long parsed = std::strtoul(env, &end, 10);
				if (end != env && parsed >= 2 && parsed <= 16)
				{
					threads = static_cast<std::size_t>(parsed);
				}
			}

			return std::clamp<std::size_t>(threads, 3, 8);
		}

	} // namespace

	thread_local const LanguageServer::RequestContext* LanguageServer::t_context = nullptr;

	LanguageServer::LanguageServer()
	: mPool(PoolSize()), mParsePool(kParseWorkers), mDocumentPool(kDocumentWorkers) {}

	LanguageServer::~LanguageServer()
	{
		// Producers must finish before the pools they submit continuations to.
		mDocumentPool.WaitIdle();
		mDocumentPool.Shutdown();
		mIndexWorker.request_stop();
		mScanWorker.request_stop();
		mSymbolWorker.request_stop();
		mIndexCv.notify_all();
		mDiagWorker.request_stop();
		{
			const std::lock_guard<std::mutex> lock(mDiagMutex);
			mDiagStop.request_stop();
		}
		mDiagCv.notify_all();
		if (mDiagWorker.joinable())
		{
			mDiagWorker.join();
		}

		if (mIndexWorker.joinable())
		{
			mIndexWorker.join();
		}

		if (mScanWorker.joinable())
		{
			mScanWorker.join();
		}

		if (mSymbolWorker.joinable())
		{
			mSymbolWorker.join();
		}

		DrainRequests();
		mParsePool.Shutdown();
		mPool.Shutdown();
	}

	bool LanguageServer::Run()
	{
		const auto tracePath     = heimdall::profiling::OutputPathFromEnvironment();
		mProfilingSession        = tracePath ? heimdall::profiling::Session::Start(*tracePath) : nullptr;
		const auto& traceSession = mProfilingSession;
		if (tracePath)
		{
			std::osyncstream(std::cerr)
			<<(traceSession ? "heimdall: recording Perfetto trace to " + tracePath->string()
				: std::string("heimdall: HEIMDALL_TRACE ignored, build with -DHEIMDALL_PROFILING=ON"))
			<< '\n';
		}

		HEIMDALL_TRACE_THREAD("lsp-io");
		HEIMDALL_TRACE_SCOPE(Lsp, "LanguageServer::Run");
		mIndexWorker = std::jthread([this](std::stop_token stop)
			{
				IndexWorkerMain(stop);
			});
		mDiagWorker = std::jthread([this](std::stop_token stop)
			{
				DiagWorkerMain(stop);
			});

		mPool.Submit([]
			{
				HEIMDALL_TRACE_SCOPE_DETAIL(Compiler, "SystemIncludes", "c++");
				heimdall::IncludeIndex::SystemIncludes("c++");
			},
			ThreadPool::Priority::Background);

		auto stop_workers = [&]
		{
			std::vector<std::stop_source> sources;
			{
				const std::lock_guard lock(m_inflight_mu);
				sources.reserve(m_inflight.size());
				for (const auto& [key, source] : m_inflight)
				{
					sources.push_back(source);
				}
			}
			for (auto& source : sources)
			{
				source.request_stop();
			}

			mDocumentPool.WaitIdle();
			mDocumentPool.Shutdown();
			mIndexWorker.request_stop();
			mDiagWorker.request_stop();
			{
				const std::lock_guard<std::mutex> lock(mDiagMutex);
				mDiagStop.request_stop();
			}
			mScanWorker.request_stop();
			mSymbolWorker.request_stop();
			mIndexCv.notify_all();
			mDiagCv.notify_all();
			if (mDiagWorker.joinable())
			{
				mDiagWorker.join();
			}

			if (mIndexWorker.joinable())
			{
				mIndexWorker.join();
			}

			if (mScanWorker.joinable())
			{
				mScanWorker.join();
			}

			if (mSymbolWorker.joinable())
			{
				mSymbolWorker.join();
			}

			DrainRequests();
			mParsePool.Shutdown();
			mPool.Shutdown();
			if (traceSession)
			{
				traceSession->Stop();
			}
		};

		std::string body;

		simdjson::dom::parser parser;
		while (ReadMessage(body))
		{
			simdjson::dom::element request;
			if (parser.parse(body).get(request))
			{
				continue;
			}

			simdjson::dom::object object;
			if (request.get_object().get(object))
			{
				continue;
			}

			std::string_view method;
			if (!GetString(object, "method", method))
			{
				continue;
			}

			simdjson::dom::element id;
			const bool has_id   = !object["id"].get(id);
			std::string id_json = has_id ? simdjson::minify(id) : "null";

			// A handler that throws on the I/O thread (bad_alloc, filesystem_error,
			// ...) must not unwind out of Run and end the session.
			try
			{
				if (method == "initialize")
				{
					HEIMDALL_TRACE_SCOPE(Startup, "initialize");
					LoadInitializationOptions(request);
					const std::lock_guard profilingLock(mProfilingMutex);
					if (traceSession)
					{
						const std::lock_guard lock(m_init_mu);
						traceSession->SetProjectRoot(mWorkspaceRoot);
						std::osyncstream(std::cerr)
						<< "heimdall: recording Perfetto trace to "
						<< traceSession->Output().string() << '\n';
					}

					const std::string rename_capability =
						mVersionedEdits ? "{\"prepareProvider\":true}" : "false";
					const std::string workspace_symbol_capability =
						mWorkspaceSymbols.load(std::memory_order_relaxed) ? "true" : "false";
					Respond(id_json,
						"{\"capabilities\":{\"textDocumentSync\":2,"
						"\"documentFormattingProvider\":true,"
						"\"documentRangeFormattingProvider\":true,"
						"\"codeActionProvider\":{\"codeActionKinds\":[\"quickfix\",\"source."
						"fixAll\","
						"\"refactor.extract.variable\",\"refactor.extract.function\","
						"\"refactor.inline.variable\",\"refactor.inline.function\",\"refactor.inline.macro\",\"refactor.extract.constant\",\"refactor.extract.method\"]},\"hoverProvider\":true,"
						"\"definitionProvider\":true,\"implementationProvider\":true,"
						"\"documentLinkProvider\":{\"resolveProvider\":false},"
						"\"documentSymbolProvider\":true,\"typeHierarchyProvider\":true,\"workspaceSymbolProvider\":" +
						workspace_symbol_capability +
						",\"referencesProvider\":true,\"renameProvider\":" +
						rename_capability +
						","
						"\"inlayHintProvider\":true,"
						"\"semanticTokensProvider\":{\"legend\":" +
						SemanticTokensLegend() + ",\"range\":true,\"full\":true},"
						"\"signatureHelpProvider\":{\"triggerCharacters\":[\"(\",\",\",\"{\"],"
						"\"retriggerCharacters\":[\")\"]},"
						"\"completionProvider\":{\"triggerCharacters\":[\".\",\">\",\":\","
						"\"#\",\"/\",\"<\",\"\\\"\"],"
						"\"resolveProvider\":false}},"
						"\"serverInfo\":{\"name\":\"Heimdall\",\"version\":\"0.1.0\"}}");
				}
				else if (method == "initialized")
				{
					HEIMDALL_TRACE_INSTANT(Startup, "initialized");
					if (mWorkspaceScan.load(std::memory_order_relaxed))
					{
						mScanWorker =
							std::jthread([this](std::stop_token stop)
							{
								WorkspaceScanMain(stop);
							});
					}

					mSymbolWorker =
						std::jthread([this](std::stop_token stop)
						{
							SymbolIndexMain(stop);
						});
				}
				else if (method == "shutdown")
				{
					// Answer every request received so far before acknowledging.
					mDocumentPool.WaitIdle();
					FlushDiagnostics();
					DrainRequests();
					Respond(id_json, "null");
				}
				else if (method == "exit")
				{
					stop_workers();
					return true;
				}
				else if (method == "textDocument/didOpen")
				{
					QueueDocument(body, request);
				}
				else if (method == "textDocument/didChange")
				{
					QueueDocument(body, request);
				}
				else if (method == "textDocument/didClose")
				{
					QueueDocument(body, request);
				}
				else if (method == "textDocument/formatting")
				{
					Dispatch(body, request, id_json,
						[this](simdjson::dom::element request, std::string_view id)
						{
							FormatDocument(request, id);
						});
				}
				else if (method == "textDocument/rangeFormatting")
				{
					Dispatch(body, request, id_json,
						[this](simdjson::dom::element request, std::string_view id)
						{
							RangeFormatDocument(request, id);
						});
				}
				else if (method == "textDocument/codeAction")
				{
					Dispatch(body, request, id_json,
						[this](simdjson::dom::element request, std::string_view id)
						{
							CodeActions(request, id);
						});
				}
				else if (method == "textDocument/completion")
				{
					Dispatch(body, request, id_json,
						[this](simdjson::dom::element request, std::string_view id)
						{
							CompleteDocument(request, id);
						});
				}
				else if (method == "textDocument/references" ||
					method == "textDocument/prepareRename" || method == "textDocument/rename")
				{
					const int mode = method == "textDocument/references" ? 0
					: method == "textDocument/prepareRename" ? 1
					: 2;
					Dispatch(body, request, id_json,
						[this, mode](simdjson::dom::element request, std::string_view id)
						{
							RenameDocument(request, id, mode);
						});
				}
				else if (method == "textDocument/documentLink")
				{
					Dispatch(body, request, id_json,
						[this](simdjson::dom::element request, std::string_view id)
						{
							DocumentLinks(request, id);
						});
				}
				else if (method == "textDocument/definition")
				{
					Dispatch(body, request, id_json,
						[this](simdjson::dom::element request, std::string_view id)
						{
							GotoDocument(request, id, false);
						});
				}
				else if (method == "textDocument/implementation")
				{
					Dispatch(body, request, id_json,
						[this](simdjson::dom::element request, std::string_view id)
						{
							GotoDocument(request, id, true);
						});
				}
				else if (method == "textDocument/signatureHelp")
				{
					Dispatch(body, request, id_json,
						[this](simdjson::dom::element request, std::string_view id)
						{
							SignatureHelpDocument(request, id);
						});
				}
				else if (method == "textDocument/hover")
				{
					Dispatch(body, request, id_json,
						[this](simdjson::dom::element request, std::string_view id)
						{
							HoverDocument(request, id);
						});
				}
				else if (method == "textDocument/inlayHint")
				{
					Dispatch(body, request, id_json,
						[this](simdjson::dom::element request, std::string_view id)
						{
							InlayHints(request, id);
						});
				}
				else if (method == "textDocument/semanticTokens/full" ||
					method == "textDocument/semanticTokens/range")
				{
					const bool ranged = method == "textDocument/semanticTokens/range";
					Dispatch(body, request, id_json,
						[this, ranged](simdjson::dom::element request, std::string_view id)
						{
							SemanticTokensRequest(request, id, ranged);
						});
				}
				else if (method == "textDocument/documentSymbol")
				{
					Dispatch(body, request, id_json,
						[this](simdjson::dom::element request, std::string_view id)
						{
							DocumentSymbols(request, id);
						});
				}
				else if (method == "textDocument/prepareTypeHierarchy")
				{
					Dispatch(body, request, id_json,
						[this](simdjson::dom::element request, std::string_view id)
						{
							TypeHierarchyRequest(request, id, TypeHierarchyMode::Prepare);
						});
				}
				else if (method == "typeHierarchy/supertypes")
				{
					Dispatch(body, request, id_json,
						[this](simdjson::dom::element request, std::string_view id)
						{
							TypeHierarchyRequest(request, id, TypeHierarchyMode::Supertypes);
						});
				}
				else if (method == "typeHierarchy/subtypes")
				{
					Dispatch(body, request, id_json,
						[this](simdjson::dom::element request, std::string_view id)
						{
							TypeHierarchyRequest(request, id, TypeHierarchyMode::Subtypes);
						});
				}
				else if (method == "workspace/symbol")
				{
					Dispatch(body, request, id_json,
						[this](simdjson::dom::element request, std::string_view id)
						{
							WorkspaceSymbols(request, id);
						});
				}
				else if (method == "workspace/didChangeWatchedFiles")
				{
					WatchedFilesChanged(request);
				}
				else if (method == "$/cancelRequest")
				{
					simdjson::dom::object params;
					if (GetObject(request, "params", params))
					{
						simdjson::dom::element cancel_id;
						if (!params["id"].get(cancel_id))
						{
							// The request is running (or queued) on the pool; the
							// I/O thread is free, so the token flips right away.
							std::optional<std::stop_source> source;
							{
								const std::lock_guard lock(m_inflight_mu);
								if (const auto found = m_inflight.find(simdjson::minify(cancel_id));
									found != m_inflight.end())
								{
									source = found->second;
								}
							}
							if (source)
							{
								source->request_stop();
							}
						}
					}
				}
				else if (has_id)
				{
					Respond(id_json, "null");
				}
			}
			catch (...)
			{
				if (has_id)
				{
					RespondInternalError(id_json);
				}
			}
		}

		stop_workers();
		return false;
	}

	void LanguageServer::DrainRequests()
	{
		// Handlers can produce parse jobs, which produce handler continuations.
		// A single pool-idle observation cannot cover both directions.
		for (;;)
		{
			mPool.WaitIdle();
			mParsePool.WaitIdle();
			mPool.WaitIdle();
			const std::lock_guard<std::mutex> lock(m_inflight_mu);
			if (m_inflight.empty())
			{
				break;
			}
		}
	}

	void LanguageServer::QueueDocument(std::string_view body, simdjson::dom::element request)
	{
		// Notifications and snapshot pins use the document's FIFO lane.
		// No text editing, lexing or line-index rebuilding runs on transport I/O.
		std::string_view uri;
		simdjson::dom::object document;
		if (!DocumentParams(request, uri, document))
		{
			return;
		}

		mDocumentPool.SubmitSerial(std::string(uri),
			[this, body = std::string(body)]
			{
				simdjson::dom::parser parser;
				simdjson::dom::element request;
				if (parser.parse(body).get(request))
				{
					return;
				}

				std::string_view method;
				if (request["method"].get_string().get(method))
				{
					return;
				}

				if (method == "textDocument/didOpen")
				{
					OpenDocument(request);
				}
				else if (method == "textDocument/didChange")
				{
					ChangeDocument(request);
				}
				else if (method == "textDocument/didClose")
				{
					CloseDocument(request);
				}
			});
	}

	void LanguageServer::Dispatch(
		std::string_view body,
		simdjson::dom::element request,
		const std::string& id,
		RequestHandler handler)
	{
		auto job                 = std::make_shared<RequestJob>();
		job->body                = body;
		job->id                  = id;
		job->handler             = std::move(handler);
		job->context.interactive = true;
		std::stop_source source;
		job->context.stop = source.get_token();
		std::string_view uri;
		simdjson::dom::object document;
		if (DocumentParams(request, uri, document))
		{
			job->context.document_uri = uri;
		}
		else if (simdjson::dom::object params, item, data;
			GetObject(request, "params", params) && GetObject(params, "item", item) &&
			GetObject(item, "data", data) && GetString(data, "uri", uri))
		{
			job->context.document_uri = uri;
		}

		bool overloaded = false;
		{
			const std::lock_guard lock(m_inflight_mu);
			constexpr std::size_t kMaxInflightRequests = 256;
			overloaded                                 = m_inflight.size() >= kMaxInflightRequests;
			if (!overloaded)
			{
				m_inflight[id] = source;
			}
		}
		if (overloaded)
		{
			Send("{\"jsonrpc\":\"2.0\",\"id\":" + id +
				",\"error\":{\"code\":-32000,\"message\":\"request queue is full\"}}");
			return;
		}

		// Pin after prior notifications, before later ones. Cancellation was
		// registered above, so transport can cancel even while this pin is queued.
		auto pin = [this, job]
		{
			if (job->context.stop.stop_requested())
			{
				mPool.SubmitCancellable([this, job]
					{
						ExecuteRequest(job);
						}, job->context.stop,
					[this, job]
					{
						job->context.pinned.reset();
						job->context.interest.reset();
						job->context.analysis = {};
						job->context.parsed.reset();
						RespondCancelled(job->id);
						const std::lock_guard lock(m_inflight_mu);
						m_inflight.erase(job->id);
					});
				return;
			}

			if (!job->context.document_uri.empty())
			{
				if (auto snapshot = GetDocument(job->context.document_uri))
				{
					snapshot->lifetime->Retain();
					job->context.interest = std::shared_ptr<DocumentVersion>(
						snapshot->lifetime.get(),
						[lifetime = snapshot->lifetime](DocumentVersion*)
						{
							lifetime->Release();
						});
					job->context.pinned =
						PinnedDocument {job->context.document_uri, std::move(*snapshot)};
				}
			}

			job->context.analysis = m_workspace.Snapshot();
			mPool.SubmitCancellable([this, job]
				{
					ExecuteRequest(job);
					}, job->context.stop,
				[this, job]
				{
					job->context.pinned.reset();
					job->context.interest.reset();
					job->context.analysis = {};
					job->context.parsed.reset();
					RespondCancelled(job->id);
					const std::lock_guard lock(m_inflight_mu);
					m_inflight.erase(job->id);
				});
		};
		std::string_view method;
		if (request["method"].get_string().get(method))
		{
			method = {};
		}

		if (method == "textDocument/rename" || method == "textDocument/prepareRename" ||
			method == "textDocument/codeAction")
		{
			mDocumentPool.SubmitBarrier(std::move(pin));
		}
		else
		{
			mDocumentPool.SubmitSerial(job->context.document_uri, std::move(pin));
		}
	}

	void LanguageServer::ExecuteRequest(const std::shared_ptr<RequestJob>& job)
	{
		struct ContextScope
		{
			~ContextScope()
			{
				t_context = nullptr;
			}
		} scope;
		t_context      = &job->context;
		bool suspended = false;
		try
		{
			if (RequestCancelled())
			{
				RespondCancelled(job->id);
			}
			else
			{
				if (job->context.parse_failure)
				{
					std::rethrow_exception(job->context.parse_failure);
				}

				thread_local simdjson::dom::parser parser;
				simdjson::dom::element element;
				if (parser.parse(job->body).get(element))
				{
					Respond(job->id, "null");
				}
				else
				{
					job->handler(element, job->id);
				}
			}
		}
		catch (const ParsePending& pending)
		{
			auto waiter = std::make_shared<ParseSlot::Waiter>();
			waiter->resume = [this, job, slot = pending.slot]
			{
				// Keep the finished result with this request even if didChange
				// or another version has meanwhile evicted the document cache.
				mPool.Submit([this, job, slot]
					{
						{
							const std::lock_guard<std::mutex> lock(slot->mu);
							job->context.parsed        = slot->tree;
							job->context.parse_failure = slot->failure;
						}
						ExecuteRequest(job);
					});
			};
			std::weak_ptr<ParseSlot::Waiter> weak = waiter;
			waiter->cancellation.emplace(job->context.stop, std::function<void() >([weak]
				{
					if (auto waiting = weak.lock())
					{
						waiting->Wake();
					}
				}));
			bool completed;
			{
				const std::lock_guard<std::mutex> lock(pending.slot->mu);
				completed = pending.slot->ready.load(std::memory_order_acquire) ||
					pending.slot->failure || pending.slot->cancelled;
				if (!completed)
				{
					pending.slot->waiters.push_back(waiter);
				}
			}
			suspended = true;
			if (completed)
			{
				waiter->Wake();
			}
		}
		catch (...)
		{
			RespondInternalError(job->id);
		}

		if (!suspended)
		{
			job->context.interest.reset();
			const std::lock_guard<std::mutex> lock(m_inflight_mu);
			m_inflight.erase(job->id);
		}
	}

	std::optional<LanguageServer::DocumentSnapshot> LanguageServer::GetDocument(
		const std::string& uri)
	{
		if (t_context != nullptr && t_context->interactive && t_context->document_uri == uri)
		{
			return t_context->pinned ? std::optional(t_context->pinned->snapshot) : std::nullopt;
		}

		std::shared_lock<std::shared_mutex> lock(m_docs_mu);
		if (const auto found = m_documents.find(uri); found != m_documents.end())
		{
			auto snapshot = found->second;
			lock.unlock();
			snapshot.text   = snapshot.buffer->Materialize();
			snapshot.tokens = snapshot.tokenBuffer->Materialize();
			return snapshot;
		}

		return std::nullopt;
	}

	const heimdall::CompileCommand* LanguageServer::CommandFor(const std::string& uri)
	{
		// m_compile_database is written once by `initialize` on the I/O thread.
		// Guard the read with m_init_mu so workers never race the write; the
		// lock is uncontended after initialization.
		const std::lock_guard<std::mutex> lock(m_init_mu);
		if (!m_compile_database)
		{
			return nullptr;
		}

		const std::filesystem::path path      = PathFromUri(uri);
		const heimdall::CompileCommand* exact = m_compile_database->Find(path);
		return exact != nullptr ? exact : m_compile_database->FindOrNearest(path);
	}

	std::stop_token LanguageServer::CurrentStop()
	{
		return t_context != nullptr ? t_context->stop : std::stop_token {};
	}

	bool LanguageServer::RequestCancelled()
	{
		return t_context != nullptr && t_context->stop.stop_requested();
	}

	void LanguageServer::RespondCancelled(std::string_view id)
	{
		// LSP RequestCancelled.
		Send("{\"jsonrpc\":\"2.0\",\"id\":" + std::string(id) +
			",\"error\":{\"code\":-32800,\"message\":\"Request cancelled\"}}");
	}

	void LanguageServer::RespondInternalError(std::string_view id)
	{
		Send("{\"jsonrpc\":\"2.0\",\"id\":" + std::string(id) +
			",\"error\":{\"code\":-32603,\"message\":\"Internal error\"}}");
	}

	void LanguageServer::LoadInitializationOptions(simdjson::dom::element request)
	{
		HEIMDALL_TRACE_SCOPE(Startup, "LoadInitializationOptions");
		simdjson::dom::object params;
		simdjson::dom::object options;
		const bool has_params = GetObject(request, "params", params);
		simdjson::dom::object capabilities, workspace_capabilities, edit_capabilities;
		if (has_params && GetObject(params, "capabilities", capabilities) &&
			GetObject(capabilities, "workspace", workspace_capabilities) &&
			GetObject(workspace_capabilities, "workspaceEdit", edit_capabilities))
		{
			bool supported = false;
			if (!edit_capabilities["documentChanges"].get_bool().get(supported))
			{
				mVersionedEdits = supported;
			}
		}

		simdjson::dom::object window_capabilities;
		if (has_params && GetObject(params, "capabilities", capabilities) &&
			GetObject(capabilities, "window", window_capabilities))
		{
			bool progress = false;
			if (!window_capabilities["workDoneProgress"].get_bool().get(progress))
			{
				mWorkDoneProgress.store(progress, std::memory_order_relaxed);
			}
		}

		simdjson::dom::object text_document_capabilities, symbol_capabilities;
		if (has_params && GetObject(params, "capabilities", capabilities) &&
			GetObject(capabilities, "textDocument", text_document_capabilities) &&
			GetObject(text_document_capabilities, "documentSymbol", symbol_capabilities))
		{
			bool hierarchical = false;
			if (!symbol_capabilities["hierarchicalDocumentSymbolSupport"].get_bool().get(hierarchical))
			{
				mHierarchicalSymbols.store(hierarchical, std::memory_order_relaxed);
			}
		}

		std::filesystem::path workspace_root;
		if (has_params)
		{
			simdjson::dom::array folders;
			if (!params["workspaceFolders"].get_array().get(folders))
			{
				for (const auto folder_element : folders)
				{
					simdjson::dom::object folder;
					std::string_view uri;
					if (!folder_element.get_object().get(folder) && GetString(folder, "uri", uri))
					{
						workspace_root = PathFromUri(uri);
						break;
					}
				}
			}

			if (workspace_root.empty())
			{
				std::string_view uri;
				std::string_view root_path;
				if (GetString(params, "rootUri", uri) && !uri.empty())
				{
					workspace_root = PathFromUri(uri);
				}
				else if (GetString(params, "rootPath", root_path) && !root_path.empty())
				{
					workspace_root = std::filesystem::path(root_path);
				}
			}
		}

		// A default-constructed simdjson object is not safe to index: only touch
		// `params` / `options` when they were actually present.
		const bool has_options = has_params && GetObject(params, "initializationOptions", options);
		if (has_options)
		{
			if (workspace_root.empty())
			{
				std::string_view configured_root;
				if (GetString(options, "workspaceRoot", configured_root) &&
					!configured_root.empty())
				{
					workspace_root = std::filesystem::path(configured_root);
				}
			}
		}

		{
			// Only an explicit workspace is scanned; the cwd fallback below is not.
			const std::lock_guard<std::mutex> lock(m_init_mu);
			mWorkspaceRoot = workspace_root;
		}
		std::error_code cwd_error;
		const auto cwd = std::filesystem::current_path(cwd_error);
		if (workspace_root.empty())
		{
			workspace_root = cwd_error ? std::filesystem::path {}: cwd;
		}

		if (!workspace_root.empty())
		{
			auto loaded_rules = heimdall::FindRuleOptions(workspace_root);
			if (!loaded_rules)
			{
				const std::lock_guard<std::mutex> lock(m_init_mu);
				mInitializationError = loaded_rules.error();
			}
			else if (*loaded_rules)
			{
				const std::lock_guard<std::mutex> lock(m_init_mu);
				m_rule_options = std::move(* *loaded_rules);
			}
			else if (!cwd_error && std::filesystem::absolute(workspace_root).lexically_normal() !=
				std::filesystem::absolute(cwd).lexically_normal())
			{
				auto cwd_rules = heimdall::FindRuleOptions(cwd);
				if (!cwd_rules)
				{
					const std::lock_guard<std::mutex> lock(m_init_mu);
					mInitializationError = cwd_rules.error();
				}
				else if (*cwd_rules)
				{
					const std::lock_guard<std::mutex> lock(m_init_mu);
					m_rule_options = std::move(* *cwd_rules);
				}
			}
		}

		bool scan = true;
		simdjson::dom::object semanticCapabilities;
		if (has_params && GetObject(params, "capabilities", capabilities) &&
			GetObject(capabilities, "workspace", workspace_capabilities) &&
			GetObject(workspace_capabilities, "semanticTokens", semanticCapabilities))
		{
			bool refresh = false;
			if (!semanticCapabilities["refreshSupport"].get_bool().get(refresh))
			{
				mSemanticTokensRefresh.store(refresh, std::memory_order_relaxed);
			}
		}

		if (has_options && !options["workspaceDiagnostics"].get_bool().get(scan))
		{
			mWorkspaceScan.store(scan, std::memory_order_relaxed);
		}

		bool symbols = true;
		if (has_options && !options["workspaceSymbols"].get_bool().get(symbols))
		{
			mWorkspaceSymbols.store(symbols, std::memory_order_relaxed);
		}

		bool inlay_hints = true;
		if (has_options && !options["inlayHints"].get_bool().get(inlay_hints))
		{
			mInlayHints.store(inlay_hints, std::memory_order_relaxed);
		}

		bool index_status = false;
		if (has_options && !options["indexStatus"].get_bool().get(index_status))
		{
			mIndexStatus.store(index_status, std::memory_order_relaxed);
		}

		bool enabled = false;
		if (has_options && options["enableSemantic"].get_bool().get(enabled))
		{
			enabled = false;
		}

		mEnableSemantic.store(enabled, std::memory_order_relaxed);

		std::string_view path;
		if (!has_options ||!GetString(options, "compileCommands", path) || path.empty())
		{
			return;
		}

		auto database = [&]
		{
			HEIMDALL_TRACE_SCOPE_DETAIL(Startup, "CompileDatabase::Load", path);
			return heimdall::CompileDatabase::Load(std::filesystem::path(path));
		}
		();
		if (!database)
		{
			const std::lock_guard<std::mutex> lock(m_init_mu);
			mInitializationError = database.error();
			return;
		}

		std::unordered_set<std::string> drivers = {"c++"};
		{
			const std::lock_guard<std::mutex> lock(m_init_mu);
			m_compile_database =
				std::make_shared<const heimdall::CompileDatabase>(std::move(*database));
			m_workspace.SetCompilationDatabase(m_compile_database);
			for (const auto& command : m_compile_database->Commands())
			{
				if (!command.arguments.empty() && !command.arguments.front().empty())
				{
					drivers.insert(command.arguments.front());
				}
			}
		}

		mPool.Submit(ThreadPool::Task([drivers = std::move(drivers)]()
			{
				for (const auto& driver : drivers)
				{
					HEIMDALL_TRACE_SCOPE_DETAIL(Compiler, "SystemIncludes", driver);
					heimdall::IncludeIndex::SystemIncludes(driver);
				}
			}),
			ThreadPool::Priority::Background);
	}

	void LanguageServer::Respond(std::string_view id, std::string_view result)
	{
		Send("{\"jsonrpc\":\"2.0\",\"id\":" + std::string(id) +
			",\"result\":" + std::string(result) + "}");
	}

	void LanguageServer::PublishDiagnostics(
		const std::string& uri,
		std::shared_ptr<const std::string> text,
		std::int64_t version)
	{
		// Single-pass pipeline: one lex + preprocess + grammar pass per version,
		// shared by the rule engine, the semantic pass and the syntax errors
		// (was: 4 lexes + 2 preprocesses of the same buffer per keystroke).
		const heimdall::CompileCommand* command = CommandFor(uri);
		const auto tree                         = CachedParse(uri, text, version, command);
		if (!tree || RequestCancelled() ||!IsCurrentVersion(uri, version))
		{
			return;
		}

		IndexOpenSymbols(uri, text, *tree, version);
		const auto diagnostics = RuleDiagnostics(uri, tree, command);
		if (!IsCurrentVersion(uri, version))
		{
			return;
		}

		std::vector<heimdall::SemanticDiagnostic> semantic_diagnostics;
		if (mEnableSemantic.load(std::memory_order_relaxed) && command != nullptr)
		{
			semantic_diagnostics = heimdall::SemanticAnalyzer().AnalyzeUnusedLocals(*tree, command);
			if (!IsCurrentVersion(uri, version))
			{
				return;
			}
		}

		// Names that a using-directive makes ambiguous (needs header scopes so
		// `using namespace std;` clashes with other namespaces are seen too).
		const HeaderView ambiguity_headers = HeaderScopes(uri, text, command);
		const auto ambiguities = heimdall::Navigation::FindAmbiguities(
			*tree, ambiguity_headers.index ? &ambiguity_headers.index->Scopes() : nullptr);
		if (!IsCurrentVersion(uri, version))
		{
			return;
		}

		// One line-index build per publish; every diagnostic position below is
		// O(log L) (was: an O(file size) scan per diagnostic).
		LineIndex lines;
		lines.Build(*text);
		std::string message = "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/"
			"publishDiagnostics\",\"params\":{\"uri\":";
		QuoteJson(uri, message);
		message += ",\"version\":" + std::to_string(version) + ",\"diagnostics\":[";
		bool first = true;
		for (const auto& diagnostic : diagnostics)
		{
			if (!first)
			{
				message += ',';
			}

			first                = false;
			const Position start = lines.ToPosition(diagnostic.offset);
			const Position end   = lines.ToPosition(diagnostic.offset + diagnostic.length);
			message += "{\"range\":{\"start\":";
			AppendPosition(start, message);
			message += ",\"end\":";
			AppendPosition(end, message);
			message += diagnostic.severity == heimdall::Severity::Error
			? "},\"severity\":1,\"code\":" : "},\"severity\":2,\"code\":";
			QuoteJson(diagnostic.code, message);
			message += ",\"source\":\"heimdall\",\"message\":";
			QuoteJson(diagnostic.message, message);
			if (diagnostic.has_related_information)
			{
				message += ",\"relatedInformation\":[{\"location\":{\"uri\":";
				QuoteJson(uri, message);
				message += ",\"range\":{\"start\":";
				AppendPosition(lines.ToPosition(diagnostic.related_offset), message);
				message += ",\"end\":";
				AppendPosition(
					lines.ToPosition(diagnostic.related_offset + diagnostic.related_length), message);
				message += "}},\"message\":";
				QuoteJson(diagnostic.related_message, message);
				message += "}]";
			}

			message += '}';
		}

		for (const auto& diagnostic : semantic_diagnostics)
		{
			if (!first)
			{
				message += ',';
			}

			first                = false;
			const Position start = lines.ToPosition(diagnostic.offset);
			const Position end   = lines.ToPosition(diagnostic.offset + diagnostic.length);
			message += "{\"range\":{\"start\":";
			AppendPosition(start, message);
			message += ",\"end\":";
			AppendPosition(end, message);
			message += "},\"severity\":2,\"code\":";
			QuoteJson(diagnostic.code, message);
			message += ",\"source\":\"heimdall\",\"message\":";
			QuoteJson(diagnostic.message, message);
			message += '}';
		}

		for (const auto& ambiguity : ambiguities)
		{
			if (!first)
			{
				message += ',';
			}

			first = false;
			std::string text_message =
				"reference to '" + ambiguity.name + "' is ambiguous; candidates: ";
			for (std::size_t i = 0; i < ambiguity.candidates.size(); ++i)
			{
				text_message +=(i == 0 ? "" : ", ") + ambiguity.candidates[i];
			}

			message += "{\"range\":{\"start\":";
			AppendPosition(lines.ToPosition(ambiguity.offset), message);
			message += ",\"end\":";
			AppendPosition(lines.ToPosition(ambiguity.offset + ambiguity.length), message);
			message += "},\"severity\":1,\"code\":\"semantic/"
				"ambiguous-reference\",\"source\":\"heimdall\",\"message\":";
			QuoteJson(text_message, message);
			message += '}';
		}

		for (const auto& diagnostic : tree->Diagnostics())
		{
			if (!first)
			{
				message += ',';
			}

			first                = false;
			const Position start = lines.ToPosition(diagnostic.offset);
			message += "{\"range\":{\"start\":";
			AppendPosition(start, message);
			message += ",\"end\":";
			AppendPosition(start, message);
			message += "},\"severity\":1,\"code\":\"syntax/"
				"parse-error\",\"source\":\"heimdall\",\"message\":";
			QuoteJson(diagnostic.message, message);
			message += '}';
		}

		message += "]}}";
		std::string init_error;
		{
			const std::lock_guard<std::mutex> lock(m_init_mu);
			init_error = mInitializationError;
			mInitializationError.clear();
		}
		if (!init_error.empty())
		{
			std::string notification = "{\"jsonrpc\":\"2.0\",\"method\":\"window/"
				"showMessage\",\"params\":{\"type\":2,\"message\":";
			QuoteJson(init_error, notification);
			notification += "}}";
			Send(notification);
		}

		Send(message);
		PublishInactiveRegions(uri, *text, tree->ActiveRanges(), tree->Directives(), lines);
	}

	void LanguageServer::PublishInactiveRegions(
		const std::string& uri,
		std::string_view text,
		const std::vector<heimdall::ActiveSourceRange>& activeRanges,
		const std::vector<heimdall::PreprocessorDirective>& directives,
		const LineIndex& lines)
	{
		std::string notification =
			"{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/"
			"inactiveRegions\",\"params\":{\"uri\":";
		QuoteJson(uri, notification);
		notification += ",\"regions\":[";
		bool first = true;
		if (!activeRanges.empty())
		{
			std::size_t cursor      = 0;
			std::size_t directiveAt = 0;
			const auto isConditional = ([](heimdall::DirectiveKind kind)
				{
					return kind == heimdall::DirectiveKind::If ||
					kind == heimdall::DirectiveKind::Ifdef || kind == heimdall::DirectiveKind::Ifndef ||
					kind == heimdall::DirectiveKind::Elif || kind == heimdall::DirectiveKind::Else ||
					kind == heimdall::DirectiveKind::Endif;
				});
			const auto emit = [&](std::size_t begin, std::size_t end)
			{
				if (end <= begin || begin >= text.size())
				{
					return;
				}

				end = end > text.size() ? text.size() : end;
				if (end <= begin)
				{
					return;
				}

				if (!first)
				{
					notification += ',';
				}

				first = false;
				notification += "{\"start\":";
				AppendPosition(lines.ToPosition(begin), notification);
				notification += ",\"end\":";
				AppendPosition(lines.ToPosition(end), notification);
				notification += '}';
			};
			// A gap between active ranges covers directive lines too (the
			// standalone preprocessor keeps them out of the ranges). Cut the
			// conditional directives back out: they are always compiled text.
			const auto append = [&](std::size_t begin, std::size_t end)
			{
				if (end <= begin || begin >= text.size())
				{
					return;
				}

				end               = end > text.size() ? text.size() : end;
				std::size_t piece = begin;
				while (directiveAt < directives.size() &&
					directives[directiveAt].offset + directives[directiveAt].length <= piece)
				{
					++directiveAt;
				}

				for (std::size_t scan = directiveAt;
					scan < directives.size() && directives[scan].offset < end; ++scan)
				{
					const auto& directive = directives[scan];
					if (!isConditional(directive.kind))
					{
						continue;
					}

					const std::size_t directiveEnd = directive.offset + directive.length;
					if (directiveEnd <= piece)
					{
						continue;
					}

					if (directive.offset > piece)
					{
						emit(piece, directive.offset);
						piece = directiveEnd;
					}
					else
					{
						piece = directiveEnd > piece ? directiveEnd : piece;
					}
				}

				emit(piece, end);
			};
			for (const auto& range : activeRanges)
			{
				if (range.offset > cursor)
				{
					append(cursor, range.offset);
				}

				const std::size_t rangeEnd = range.offset + range.length;
				if (rangeEnd > cursor)
				{
					cursor = rangeEnd;
				}

				if (cursor >= text.size())
				{
					break;
				}
			}

			if (cursor < text.size())
			{
				append(cursor, text.size());
			}
		}

		notification += "]}}";
		Send(notification);
	}

	bool LanguageServer::DocumentParams(
		simdjson::dom::element request,
		std::string_view& uri,
		simdjson::dom::object& document)
	{
		simdjson::dom::object params;
		if (!GetObject(request, "params", params))
		{
			return false;
		}

		if (!GetObject(params, "textDocument", document))
		{
			return false;
		}

		return GetString(document, "uri", uri);
	}

	void LanguageServer::OpenDocument(simdjson::dom::element request)
	{
		std::string_view uri;
		simdjson::dom::object text_document;
		if (!DocumentParams(request, uri, text_document))
		{
			return;
		}

		std::string_view text;
		if (!GetString(text_document, "text", text))
		{
			return;
		}

		std::int64_t version = 0;
		if (const auto error = text_document["version"].get_int64().get(version); error)
		{
			version = 0;
		}

		const std::string uri_string(uri);
		DocumentSnapshot snapshot;
		auto source = heimdall::SourceSnapshot::Create(text);
		if (!source)
		{
			return;
		}

		snapshot.buffer = std::make_shared<const heimdall::SourceSnapshot>(std::move(*source));
		snapshot.tokenBuffer = std::make_shared<const heimdall::TokenSnapshot>(
			heimdall::TokenSnapshot::Create(heimdall::Lexer(text).Lex()));
		snapshot.version = version;
		auto lines       = std::make_shared<LineIndex>();
		lines->Build(snapshot.buffer);
		snapshot.lines = std::move(lines);
		{
			const std::shared_lock lock(m_docs_mu);
			if (const auto old = m_documents.find(uri_string); old != m_documents.end())
			{
				(void) m_workspace.Close(old->second.document);
			}
		}
		auto opened =
			m_workspace.OpenBuffer(PathFromUri(uri_string), snapshot.buffer, snapshot.tokenBuffer,
			version, ParserOptionsFor(CommandFor(uri_string)));
		if (!opened)
		{
			return;
		}

		snapshot.document = *opened;
		{
			const std::lock_guard lock(m_docs_mu);
			m_documents[uri_string].lifetime->Retire();
			m_documents[uri_string] = std::move(snapshot);
		}
		{
			const std::lock_guard<std::mutex> lock(mParseMutex);
			mParseCache.erase(uri_string);
		}
		EnqueueDiagnostics(uri_string, version);
	}

	bool LanguageServer::ApplyContentChange(
		std::shared_ptr<const heimdall::SourceSnapshot>& current,
		LineIndex& index,
		std::shared_ptr<const heimdall::TokenSnapshot>& tokens,
		EditHull& hull,
		simdjson::dom::object change)
	{
		std::string_view text;
		if (!GetString(change, "text", text))
		{
			return false;
		}

		simdjson::dom::object range;
		if (!GetObject(change, "range", range))
		{
			// Full-document sync (textDocumentSync = 1 fallback).
			auto replacement = heimdall::SourceSnapshot::Create(text);
			if (!replacement)
			{
				return false;
			}

			hull.directivesUnchanged = false;
			hull.Add({0, current->Size(), text.size()});
			current = std::make_shared<const heimdall::SourceSnapshot>(std::move(*replacement));
			index.Build(current);
			tokens = std::make_shared<const heimdall::TokenSnapshot>(
				heimdall::TokenSnapshot::Create(heimdall::Lexer(text).Lex()));
			return true;
		}

		simdjson::dom::object start_object;
		simdjson::dom::object end_object;
		if (!GetObject(range, "start", start_object) ||!GetObject(range, "end", end_object))
		{
			return false;
		}

		auto number = [](simdjson::dom::object object, const char* key)->std::size_t
		{
			std::uint64_t unsigned_value = 0;
			if (!object[key].get_uint64().get(unsigned_value))
			{
				return static_cast<std::size_t>(unsigned_value);
			}

			std::int64_t signed_value = 0;
			if (!object[key].get_int64().get(signed_value) && signed_value > 0)
			{
				return static_cast<std::size_t>(signed_value);
			}

			return 0;
		};
		const Position start = {number(start_object, "line"), number(start_object, "character")};
		const Position end   = {number(end_object, "line"), number(end_object, "character")};
		// F3: the caller keeps one index across the batch (was: a fresh O(N)
		// build per change). Offsets resolve against the running buffer; the
		// index is refreshed once per applied change below.
		std::size_t start_offset = index.OffsetFromPosition(start);
		std::size_t end_offset   = index.OffsetFromPosition(end);
		if (start_offset > current->Size())
		{
			start_offset = current->Size();
		}

		if (end_offset > current->Size())
		{
			end_offset = current->Size();
		}

		if (end_offset < start_offset)
		{
			end_offset = start_offset;
		}

		const heimdall::Lexer::TextEdit edit
		{
			start_offset, end_offset - start_offset,
				text.size()
		};
		hull.directivesUnchanged =
			hull.directivesUnchanged && tokens->PreservesDirectives(*current, edit, text);
		auto replacement = current->Edit(edit.offset, edit.old_length, text);
		if (!replacement)
		{
			return false;
		}

		current = std::make_shared<const heimdall::SourceSnapshot>(std::move(*replacement));
		index.Update(current, edit.offset, edit.old_length, edit.new_length);
		tokens = std::make_shared<const heimdall::TokenSnapshot>(tokens->Relex(*current, edit));
		hull.Add(edit);
		return true;
	}

	void LanguageServer::ChangeDocument(simdjson::dom::element request)
	{
		std::string_view uri;
		simdjson::dom::object text_document;
		if (!DocumentParams(request, uri, text_document))
		{
			return;
		}

		const std::string uri_string(uri);
		simdjson::dom::object params;
		if (!GetObject(request, "params", params))
		{
			return;
		}

		simdjson::dom::array changes;
		if (params["contentChanges"].get_array().get(changes))
		{
			return;
		}

		std::int64_t new_version = 0;
		if (const auto error = text_document["version"].get_int64().get(new_version); error)
		{
			new_version = 0;
		}

		// Which text the parse cache entry has to describe for its tree to be a
		// usable base, and which document version that is.
		const heimdall::SourceSnapshot* current_text = nullptr;
		std::int64_t current_version                 = 0;
		{
			const std::shared_lock<std::shared_mutex> lock(m_docs_mu);
			const auto found = m_documents.find(uri_string);
			if (found == m_documents.end())
			{
				return;
			}

			current_text    = found->second.buffer.get();
			current_version = found->second.version;
		}

		if (new_version <= current_version)
		{
			return;
		}

		// Take the previous parse out of the cache as the base for the next one.
		// Dropping the cache entry releases obsolete buffers. Keep just the
		// tokens/nodes needed for reuse when no other consumer owns the tree.
		std::shared_ptr<const heimdall::ParseTree> base;
		heimdall::ParserOptions base_options;
		EditHull pending;
		{
			const std::lock_guard<std::mutex> lock(mParseMutex);
			if (const auto found = mParseCache.find(uri_string); found != mParseCache.end())
			{
				auto& entry = found->second;
				if (entry.slot && entry.slot->ready.load(std::memory_order_acquire) &&
					entry.buffer.get() == current_text && entry.version == current_version)
				{
					base         = entry.slot->tree;
					base_options = entry.options;
				}
				else if (entry.base && entry.base_version == current_version)
				{
					base                        = entry.base;
					base_options                = entry.base_options;
					pending.valid               = true;
					pending.edit                = entry.base_edit;
					pending.directivesUnchanged = entry.baseDirectivesUnchanged;
				}

				mParseCache.erase(found);
			}
		}
		if (base && base.use_count() == 1)
		{
			// Nobody else sees this tree: it will only be read for its nodes and tokens.
			std::const_pointer_cast<heimdall::ParseTree>(base)->ReleaseSource();
		}

		EditHull hull;
		DocumentSnapshot snapshot;
		{
			const std::shared_lock<std::shared_mutex> lock(m_docs_mu);
			const auto found = m_documents.find(uri_string);
			if (found == m_documents.end())
			{
				return;
			}

			snapshot = found->second;
		}
		// Published snapshots are always immutable. Build the next version
		// outside m_docs_mu; readers continue using the previous one meanwhile.
		auto text   = snapshot.buffer;
		auto lines  = std::make_shared<LineIndex>(*snapshot.lines);
		auto tokens = snapshot.tokenBuffer;
		for (simdjson::dom::element change : changes)
		{
			simdjson::dom::object change_object;
			if (!change.get_object().get(change_object))
			{
				ApplyContentChange(text, *lines, tokens, hull, change_object);
			}
		}

		if (hull.valid)
		{
			hull.directivesUnchanged =
				hull.directivesUnchanged && snapshot.tokenBuffer->PreservesDirectives(
				*snapshot.buffer, hull.edit,
				text->Read(hull.edit.offset, hull.edit.new_length));
		}

		snapshot.version = new_version;
		snapshot.buffer  = std::move(text);
		snapshot.text.reset();
		snapshot.lines       = std::move(lines);
		snapshot.tokenBuffer = std::move(tokens);
		snapshot.tokens.reset();
		snapshot.lifetime = std::make_shared<DocumentVersion>();
		const auto updated = m_workspace.UpdateBuffer(
			snapshot.document, snapshot.buffer, snapshot.tokenBuffer, snapshot.version,
			hull.valid ? hull.edit : heimdall::Lexer::TextEdit {});
		if (!updated)
		{
			return;
		}

		{
			const std::lock_guard lock(m_docs_mu);
			m_documents[uri_string].lifetime->Retire();
			m_documents[uri_string] = std::move(snapshot);
		}
		const std::int64_t version = new_version;
		{
			const std::lock_guard<std::mutex> lock(mParseMutex);
			if (base)
			{
				ParseCacheEntry& entry = mParseCache[uri_string];
				entry.base             = std::move(base);
				entry.base_options     = std::move(base_options);
				entry.baseDirectivesUnchanged =
					hull.directivesUnchanged && pending.directivesUnchanged;
				if (pending.valid && hull.valid)
				{
					entry.base_edit = heimdall::Lexer::Compose(pending.edit, hull.edit);
				}
				else if (pending.valid)
				{
					entry.base_edit = pending.edit;
				}
				else if (hull.valid)
				{
					entry.base_edit = hull.edit;
				}
				else
				{
					entry.base_edit = {};
				}

				entry.base_version = new_version;
			}
		}
		EnqueueDiagnostics(uri_string, version);
	}

	void LanguageServer::CloseDocument(simdjson::dom::element request)
	{
		std::string_view uri;
		simdjson::dom::object text_document;
		if (!DocumentParams(request, uri, text_document))
		{
			return;
		}

		{
			const std::lock_guard<std::shared_mutex> lock(m_docs_mu);
			if (const auto old = m_documents.find(std::string(uri)); old != m_documents.end())
			{
				old->second.lifetime->Retire();
				(void) m_workspace.Close(old->second.document);
			}

			m_documents.erase(std::string(uri));
		}
		{
			const std::lock_guard<std::mutex> lock(m_index_cache_mu);
			m_include_cache.erase(std::string(uri));
		}
		{
			const std::lock_guard<std::mutex> lock(mMacroMutex);
			mMacroEnvironments.erase(std::string(uri));
		}
		{
			const std::lock_guard<std::mutex> lock(m_profile_mu);
			m_include_profiles.erase(std::string(uri));
		}
		{
			const std::lock_guard<std::mutex> lock(mParseMutex);
			mParseCache.erase(std::string(uri));
			mTypeNames.erase(std::string(uri));
		}
		{
			// Nothing to publish for a closed document: abandon a running pass.
			const std::lock_guard<std::mutex> lock(mDiagMutex);
			if (mDiagRunningUri == uri)
			{
				mDiagStop.request_stop();
			}
		}
		const std::filesystem::path closed_path = PathFromUri(uri);
		std::filesystem::path root;
		{
			const std::lock_guard<std::mutex> lock(m_init_mu);
			root = mWorkspaceRoot;
		}
		{
			// The buffer stops shadowing its file: serve what is on disk if the workspace owns it.
			{
				const std::lock_guard<std::mutex> lock(mSymbolMutex);
				mSymbolIndex.ClearOpen(FileKey(closed_path));
			}
			mIndexStore.ClearOpen(closed_path.generic_string());
			if (IsWorkspaceSource(closed_path) && IsUnderRoot(closed_path, root))
			{
				mPool.Submit(ThreadPool::Task([this, closed_path]()
					{
						IndexDiskSymbols(closed_path, std::stop_token {});
					}),
					ThreadPool::Priority::Background);
			}
		}

		if (mWorkspaceScan.load(std::memory_order_relaxed) && IsWorkspaceSource(closed_path) &&
			!IsDependencySource(root, closed_path))
		{
			// The file stays part of the workspace: show what is on disk instead of clearing.
			// Dependency sources are not linted, so closing one clears its diagnostics below.
			mPool.Submit(ThreadPool::Task([this, closed_path]()
				{
					PublishWorkspaceFile(closed_path, std::stop_token {});
				}),
				ThreadPool::Priority::Background);
			return;
		}

		std::string message = "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/"
			"publishDiagnostics\",\"params\":{\"uri\":";
		QuoteJson(uri, message);
		message += ",\"diagnostics\":[]}}";
		Send(message);
		std::string inactive = "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/"
			"inactiveRegions\",\"params\":{\"uri\":";
		QuoteJson(uri, inactive);
		inactive += ",\"regions\":[]}}";
		Send(inactive);
	}

	int LanguageServer::PublishWorkspaceFile(
		const std::filesystem::path& file,
		std::stop_token stop)
	{
		auto contents = ReadWorkspaceFile(file);
		if (!contents)
		{
			return -1;
		}

		auto text             = std::make_shared<std::string>(std::move(*contents));
		const std::string uri = UriFromPath(file);
		const auto is_open = [&]
		{
			const std::shared_lock<std::shared_mutex> lock(m_docs_mu);
			return m_documents.contains(uri);
		};
		if (is_open())
		{
			return -1;
		}

		const heimdall::CompileCommand* command = CommandFor(uri);
		auto tree = std::make_shared<const heimdall::ParseTree>(
			heimdall::ParseTree::ParseSnapshot(
			text, ParserOptionsForText(uri, text, 0, command, false), stop));
		if (tree->Cancelled())
		{
			return -1;
		}

		auto diagnostics = RuleDiagnostics(uri, tree, command);
		{
			// RuleDiagnostics caches a per-document include profile; a one-shot scan must not keep
			// it.
			const std::lock_guard<std::mutex> lock(m_profile_mu);
			m_include_profiles.erase(uri);
		}

		std::vector<heimdall::SemanticDiagnostic> semantic;
		if (mEnableSemantic.load(std::memory_order_relaxed) && command != nullptr)
		{
			semantic = heimdall::SemanticAnalyzer().AnalyzeUnusedLocals(*tree, command);
		}

		LineIndex lines;
		lines.Build(*text);
		std::string message = "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/"
			"publishDiagnostics\",\"params\":{\"uri\":";
		QuoteJson(uri, message);
		message += ",\"diagnostics\":[";
		int count = 0;
		const auto append = [&](std::size_t offset, std::size_t length, int severity,
			std::string_view code, std::string_view text_message)
		{
			if (count++ > 0)
			{
				message += ',';
			}

			message += "{\"range\":{\"start\":";
			AppendPosition(lines.ToPosition(offset), message);
			message += ",\"end\":";
			AppendPosition(lines.ToPosition(offset + length), message);
			message += "},\"severity\":" + std::to_string(severity) + ",\"code\":";
			QuoteJson(code, message);
			message += ",\"source\":\"heimdall\",\"message\":";
			QuoteJson(text_message, message);
			message += '}';
		};
		for (const auto& diagnostic : diagnostics)
		{
			append(diagnostic.offset, diagnostic.length,
				diagnostic.severity == heimdall::Severity::Error ? 1 : 2,
				diagnostic.code, diagnostic.message);
		}

		for (const auto& diagnostic : semantic)
		{
			append(diagnostic.offset, diagnostic.length, 2, diagnostic.code, diagnostic.message);
		}

		for (const auto& diagnostic : tree->Diagnostics())
		{
			append(diagnostic.offset, 0, 1, "syntax/parse-error", diagnostic.message);
		}

		message += "]}}";
		// Opened while analyzing: the editor pass owns the diagnostics now.
		if (is_open())
		{
			return -1;
		}

		Send(message);
		return count;
	}

	void LanguageServer::WorkspaceScanMain(std::stop_token stop)
	{
		std::filesystem::path root;
		{
			const std::lock_guard<std::mutex> lock(m_init_mu);
			root = mWorkspaceRoot;
		}
		std::error_code error;
		if (root.empty() ||!std::filesystem::is_directory(root, error))
		{
			return;
		}

		HEIMDALL_TRACE_THREAD("workspace-scan");
		HEIMDALL_TRACE_SCOPE(Lsp, "WorkspaceScanMain");
		// Diagnostics cover the open project only: dependency sources
		// (`_deps`, `third_party`, `external`, `vendor`) stay indexed for
		// navigation and completion but are not linted.
		const std::vector<std::filesystem::path> files = [&]
		{
			HEIMDALL_TRACE_SCOPE(Lsp, "DiscoverProjectSources");
			return DiscoverProjectSources(root, stop);
		}
		();
		if (stop.stop_requested())
		{
			return;
		}

		std::size_t scanned  = 0;
		std::size_t warnings = 0;
		for (const auto& file : files)
		{
			if (stop.stop_requested())
			{
				return;
			}

			if (const int count = PublishWorkspaceFile(file, stop); count >= 0)
			{
				++scanned;
				warnings += static_cast<std::size_t>(count);
			}
		}

		std::string notification = "{\"jsonrpc\":\"2.0\",\"method\":\"window/"
			"logMessage\",\"params\":{\"type\":3,\"message\":";
		QuoteJson("Heimdall: workspace scan finished: " + std::to_string(scanned) + " files, " +
			std::to_string(warnings) + " diagnostics",
			notification);
		notification += "}}";
		Send(notification);
	}

	std::shared_ptr<const LanguageServer::FormattingResult> LanguageServer::CachedFormatting(
		const std::shared_ptr<const heimdall::ParseTree>& tree,
		heimdall::FormatOptions options,
		std::stop_token stop)
	{
		if (stop.stop_requested())
		{
			return {};
		}

		const auto documentUri = t_context ? t_context->document_uri : std::string {};
		std::shared_ptr<const FormattingResult> previous;
		{
			const std::lock_guard lock(mFormattingMutex);
			for (auto entry = mFormattingCache.begin(); entry != mFormattingCache.end();)
			{
				const auto owner = entry->tree.lock();
				if (!previous && !documentUri.empty() && entry->documentUri == documentUri &&
					entry->options == options)
				{
					previous = entry->result;
				}

				if (!owner && entry->documentUri != documentUri)
				{
					mFormattingBytes -= entry->result->bytes;
					entry = mFormattingCache.erase(entry);
				}
				else if (owner == tree && entry->options == options)
				{
					const auto result = entry->result;
					mFormattingCache.splice(mFormattingCache.begin(), mFormattingCache, entry);
					return result;
				}
				else
				{
					++entry;
				}
			}
		}
		auto result = std::make_shared<FormattingResult>();
		result->checkpoint = heimdall::Formatter(options).FormatIncremental(
			*tree, previous ? &previous->checkpoint : nullptr, stop);
		result->edits = heimdall::Formatter::ComputeEdits(
			tree->Source(), result->checkpoint.Formatted(), stop);
		if (stop.stop_requested() ||!result->checkpoint.Complete())
		{
			return {};
		}

		result->bytes =
			result->checkpoint.StorageBytes() +
			result->edits.capacity() * sizeof(heimdall::FormatEdit);
		for (const auto& edit : result->edits)
		{
			result->bytes += edit.replacement.capacity();
		}

		constexpr std::size_t kFormattingCacheBytes   = 16 * 1024 * 1024;
		constexpr std::size_t kFormattingCacheEntries = 16;
		if (result->bytes <= kFormattingCacheBytes)
		{
			const std::lock_guard lock(mFormattingMutex);
			for (const auto& entry : mFormattingCache)
			{
				if (entry.tree.lock() == tree && entry.options == options)
				{
					return entry.result;
				}
			}

			if (!documentUri.empty())
			{
				for (auto entry = mFormattingCache.begin(); entry != mFormattingCache.end();)
				{
					if (entry->documentUri == documentUri && entry->options == options)
					{
						mFormattingBytes -= entry->result->bytes;
						entry = mFormattingCache.erase(entry);
					}
					else
					{
						++entry;
					}
				}
			}

			mFormattingCache.push_front({tree, options, result, documentUri});
			mFormattingBytes += result->bytes;
			while (mFormattingBytes > kFormattingCacheBytes ||
				mFormattingCache.size() > kFormattingCacheEntries)
			{
				mFormattingBytes -= mFormattingCache.back().result->bytes;
				mFormattingCache.pop_back();
			}
		}

		return result;
	}

	void LanguageServer::FormatDocument(simdjson::dom::element request, std::string_view id)
	{
		std::string_view uri;
		simdjson::dom::object text_document;
		if (!DocumentParams(request, uri, text_document))
		{
			Respond(id, "[]");
			return;
		}

		const std::string uri_string(uri);
		const auto document = GetDocument(uri_string);
		if (!document)
		{
			Respond(id, "[]");
			return;
		}

		const std::shared_ptr<const std::string> text = document->text;
		const std::int64_t version                    = document->version;
		const std::shared_ptr<const LineIndex> lines  = document->lines;
		const heimdall::CompileCommand* command       = CommandFor(uri_string);
		const auto tree                               = CachedParse(uri_string, text, version, command);
		if (!tree)
		{
			RespondCancelled(id);
			return;
		}

		heimdall::FormatOptions format_options;
		if (auto found = heimdall::FindFormatOptions(PathFromUri(uri_string)))
		{
			if (*found)
			{
				format_options = * *found;

				format_options.mainFileName = PathFromUri(uri_string).filename().string();
			}
		}
		else
		{
			const std::lock_guard<std::mutex> lock(m_init_mu);
			mInitializationError = found.error();
		}

		const auto formatting = CachedFormatting(tree, format_options, CurrentStop());
		if (!formatting)
		{
			RespondCancelled(id);
			return;
		}

		const auto& formatted = formatting->checkpoint.Formatted();
		if (RequestCancelled())
		{
			RespondCancelled(id);
			return;
		}

		if (formatted == *text)
		{
			Respond(id, "[]");
			return;
		}

		std::string response = "[{\"range\":{\"start\":{\"line\":0,\"character\":0},\"end\":";
		AppendPosition(lines->ToPosition(text->size()), response);
		response += "},\"newText\":";
		QuoteJson(formatted, response);
		response += "}]";
		Respond(id, response);
	}

	heimdall::AnalysisContext LanguageServer::AnalysisFor(
		const std::string& uri,
		std::shared_ptr<const heimdall::ParseTree> tree,
		heimdall::ParserOptions options)
	{
		auto snapshot =
			t_context && t_context->interactive ? t_context->analysis : m_workspace.Snapshot();
		const auto document = snapshot.Find(PathFromUri(uri));
		if (document != heimdall::InvalidDocument)
		{
			if (auto derived = snapshot.WithSyntax(document, tree, options))
			{
				return heimdall::AnalysisContext(std::move(*derived), document);
			}
		}

		return heimdall::AnalysisContext(heimdall::AnalysisSnapshot::FromSyntax(
			std::move(tree), std::move(options), PathFromUri(uri)),
			0);
	}

	std::vector<heimdall::Diagnostic> LanguageServer::RuleDiagnostics(
		const std::string& uri,
		std::shared_ptr<const heimdall::ParseTree> parsed,
		const heimdall::CompileCommand* command)
	{
		const auto& tree = *parsed;
		heimdall::RuleOptions rule_options;
		{
			const std::lock_guard<std::mutex> lock(m_init_mu);
			rule_options = m_rule_options;
		}
		// Per-file chain: <file dir> -> ... -> git root. A config inside the
		// file's subdirectory wins over the workspace-root one loaded at
		// initialize (which remains the fallback for files without one).
		if (auto per_file = heimdall::FindRuleOptions(PathFromUri(uri)))
		{
			if (*per_file)
			{
				rule_options = std::move(* *per_file);
			}
		}
		else
		{
			const std::lock_guard<std::mutex> lock(m_init_mu);
			mInitializationError = per_file.error();
		}

		rule_options.main_file_name = PathFromUri(uri).filename().string();

		const heimdall::RuleEngine engine(rule_options);
		std::vector<heimdall::Diagnostic> diagnostics;
		if (!mEnableSemantic.load(std::memory_order_relaxed))
		{
			return engine.Analyze(tree);
		}

		// The model is built from the tree already parsed for this version of the
		// document. Most rules on it need no compile command; the project-level
		// ones (include-what-you-use, modernize-final) see the included headers only
		// when there is a profile.
		const std::filesystem::path file_path = PathFromUri(uri);
		std::shared_ptr<const heimdall::IncludeProfile> profile;
		if (command != nullptr)
		{
			const std::string fingerprint =
				heimdall::IncludeAnalyzer::Fingerprint(file_path, tree, command);
			{
				const std::lock_guard<std::mutex> lock(m_profile_mu);
				if (const auto found = m_include_profiles.find(uri);
					found != m_include_profiles.end())
				{
					profile = found->second;
				}
			}

			// The profile reads every transitive header: keep it across keystrokes.
			if (!profile || profile->fingerprint != fingerprint ||
				!heimdall::IncludeAnalyzer::IsFresh(*profile))
			{
				profile = heimdall::IncludeAnalyzer::BuildProfile(file_path, tree, command);
				const std::lock_guard<std::mutex> lock(m_profile_mu);
				m_include_profiles[uri] = profile;
			}
		}

		{
			const heimdall::ProjectContext context
			{
				file_path, profile.get(), command
			};
			diagnostics = heimdall::AnalysisFeatures::Diagnostics(
				AnalysisFor(uri, parsed, ParserOptionsFor(command)), engine, true, context);
			const auto headers = AwaitHeaderScopes(uri, tree.SharedSource(), command);
			if (headers.index)
			{
				const auto analysis = AnalysisFor(uri, parsed, ParserOptionsFor(command));
				auto indexed = engine.ApplyPolicy(
					heimdall::SemanticRules::AnalyzeIndexedCallArgumentTypes(
					analysis.Types(), headers.index->Scopes()), tree);
				diagnostics.insert(diagnostics.end(), std::make_move_iterator(indexed.begin()),
					std::make_move_iterator(indexed.end()));
			}
		}

		if (profile == nullptr)
		{
			return diagnostics;
		}

		auto unused = engine.ApplyPolicy(heimdall::IncludeAnalyzer::Analyze(tree, *profile), tree);
		if (!unused.empty())
		{
			diagnostics.insert(diagnostics.end(), std::make_move_iterator(unused.begin()),
				std::make_move_iterator(unused.end()));
			std::stable_sort(diagnostics.begin(), diagnostics.end(),
				[](const heimdall::Diagnostic& a, const heimdall::Diagnostic& b)
				{
					return a.offset < b.offset;
				});
		}

		return diagnostics;
	}

	heimdall::IndexRenameContext LanguageServer::RenameContext()
	{
		std::filesystem::path root;
		{
			const std::lock_guard<std::mutex> lock(m_init_mu);
			root = mWorkspaceRoot;
		}

		heimdall::IndexRenameContext result;
		result.complete = mIndexComplete.load(std::memory_order_acquire);
		{
			const std::lock_guard<std::mutex> lock(mSymbolMutex);
			result.coverageIssue = mIndexIssue;
		}
		result.requireExactIncludes = true;
		result.editable = [root](const std::string& path)
		{
			if (root.empty() ||!IsUnderRoot(std::filesystem::path(path), root))
			{
				return false;
			}

			const auto relative = std::filesystem::path(path).lexically_relative(root);
			return std::ranges::none_of(relative.parent_path(),[](const std::filesystem::path& part)
				{
					return IsSkippedDirectory(part.string());
				});
		};
		result.document = [this](const std::string& path)->std::optional<heimdall::IndexedDocument>
		{
			const auto uri = UriFromPath(path);
			heimdall::IndexedDocument indexed;
			if (const auto open = GetDocument(uri))
			{
				indexed.text    = open->text;
				indexed.version = open->version;
			}
			else if (auto contents = ReadWorkspaceFile(path))
			{
				indexed.text = std::make_shared<const std::string>(std::move(*contents));
			}
			else
			{
				return std::nullopt;
			}

			const auto* command = CommandFor(uri);
			indexed.options     = ParserOptionsFor(command);
			indexed.configHash  = heimdall::index::ShardBuilder::ConfigurationHash(*indexed.options, command);
			return indexed;
		};
		return result;
	}

	void LanguageServer::RenameDocument(
		simdjson::dom::element request,
		std::string_view id,
		int mode)
	{
		if (mode != 0 && !mVersionedEdits)
		{
			Respond(id, "null");
			return;
		}

		simdjson::dom::object params, text_document, position;
		std::string_view uri;
		if (!GetObject(request, "params", params) ||
			!GetObject(params, "textDocument", text_document) ||
			!GetString(text_document, "uri", uri) ||!GetObject(params, "position", position))
		{
			Respond(id, mode == 0 ? "[]" : "null");
			return;
		}

		const std::string uri_string(uri);
		const auto document = GetDocument(uri_string);
		if (!document)
		{
			Respond(id, mode == 0 ? "[]" : "null");
			return;
		}

		const auto* command = CommandFor(uri_string);
		const auto tree     = CachedParse(uri_string, document->text, document->version, command);
		if (!tree || RequestCancelled())
		{
			RespondCancelled(id);
			return;
		}

		const auto context = AnalysisFor(uri_string, tree, ParserOptionsFor(command));
		const auto offset = document->lines->OffsetFromPosition(
			{static_cast<std::size_t>(PositionNumber(position, "line")),
				static_cast<std::size_t>(PositionNumber(position, "character"))});
		const auto fail = [&](const heimdall::RefactoringError& error)
		{
			if (error.code == heimdall::RefactoringErrorCode::Cancelled)
			{
				RespondCancelled(id);
				return;
			}

			if (mode == 0)
			{
				Respond(id, "[]");
				return;
			}

			std::string response = "{\"jsonrpc\":\"2.0\",\"id\":" + std::string(id) +
				",\"error\":{\"code\":-32803,\"message\":";
			QuoteJson(error.message, response);
			response += "}}";
			Send(response);
		};
		bool include_declaration = true;
		simdjson::dom::object reference_context;
		if (mode == 0 && GetObject(params, "context", reference_context))
		{
			bool value = false;
			if (!reference_context["includeDeclaration"].get_bool().get(value))
			{
				include_declaration = value;
			}
		}

		const auto index_snapshot = mIndexStore.Snapshot();
		const auto self_path      = PathFromUri(uri_string).generic_string();
		const auto self_shard     = index_snapshot->ShardFor(self_path);
		const auto rename_context = RenameContext();
		const bool indexed_target = mode != 0 && self_shard != heimdall::index::kInvalid &&
			index_snapshot->Shards()[self_shard]->OccurrenceAt(static_cast<std::uint32_t>(offset)) != heimdall::index::kInvalid &&
			rename_context.editable && rename_context.editable(self_path);
		const auto refs =
			mode == 2 || indexed_target ? std::expected<heimdall::SymbolOccurrences,
			heimdall::RefactoringError>(
			heimdall::SymbolOccurrences {})
		: heimdall::RefactoringService::LocalReferences(
			context.Snapshot(), context.Document(), offset, include_declaration,
			CurrentStop());
		if (!refs && (mode != 0 ||
			refs.error().code == heimdall::RefactoringErrorCode::Cancelled))
		{
			fail(refs.error());
			return;
		}

		const auto range = [&](const std::shared_ptr<const LineIndex>& lines, std::size_t start,
			std::size_t length, std::string& output)
		{
			output += "{\"start\":";
			AppendPosition(lines->ToPosition(start), output);
			output += ",\"end\":";
			AppendPosition(lines->ToPosition(start + length), output);
			output += '}';
		};
		if (mode == 1)
		{
			if (indexed_target)
			{
				const auto prepared = heimdall::IndexRename::Prepare(*index_snapshot, self_path, offset,
					rename_context, CurrentStop());
				if (!prepared)
				{
					fail(prepared.error());
					return;
				}

				std::string response = "{\"range\":";
				range(document->lines, prepared->offset, prepared->length, response);
				response += ",\"placeholder\":";
				QuoteJson(prepared->name, response);
				response += '}';
				Respond(id, response);
				return;
			}

			std::string response = "{\"range\":";
			range(document->lines, refs->offset, refs->length, response);
			response += ",\"placeholder\":";
			QuoteJson(refs->name, response);
			response += '}';
			Respond(id, response);
			return;
		}

		std::set<std::pair<std::string, std::size_t>> emitted;
		const auto appendWritten = [&](std::string& response)
		{
			const HeaderView headers = AwaitHeaderScopes(uri_string, document->text, command);
			const auto* scopes       = headers.index ? &headers.index->Scopes() : nullptr;
			const auto written = heimdall::Navigation::References(*tree, offset,
				include_declaration, scopes, CurrentStop());
			for (const auto& occurrence : written)
			{
				std::string otherUri = uri_string;
				auto lines           = document->lines;
				if (occurrence.file >= 0)
				{
					if (!headers.index || static_cast<std::size_t>(occurrence.file) >= headers.index->Files().size())
					{
						continue;
					}

					otherUri          = UriFromPath(headers.index->Files()[occurrence.file]);
					const auto source = headers.index->Source(occurrence.file);
					if (!source)
					{
						continue;
					}

					auto built = std::make_shared<LineIndex>();
					built->Build(*source);
					lines = std::move(built);
				}

				if (!emitted.emplace(otherUri, occurrence.offset).second)
				{
					continue;
				}

				if (response.size() > 1)
				{
					response += ',';
				}

				response += "{\"uri\":";
				QuoteJson(otherUri, response);
				response += ",\"range\":";
				range(lines, occurrence.offset, occurrence.length, response);
				response += '}';
			}
		};
		const auto appendIndexed = [&](std::string& response)
		{
			const auto snapshot  = mIndexStore.Snapshot();
			const auto selfPath  = PathFromUri(uri_string).generic_string();
			const auto selfShard = snapshot->ShardFor(selfPath);
			if (selfShard != heimdall::index::kInvalid &&
				snapshot->Shards()[selfShard]->ContentHash() ==
				heimdall::index::HashBytes(*document->text))
			{
				auto anchor = snapshot->SymbolAt(selfShard, static_cast<std::uint32_t>(offset));
				if (!anchor.Valid())
				{
					// A use of something declared in an included file (a header macro, a
					// function): the written name resolves through the include closure.
					const auto& file      = *snapshot->Shards()[selfShard];
					const auto occurrence = file.OccurrenceAt(static_cast<std::uint32_t>(offset));
					if (occurrence != heimdall::index::kInvalid)
					{
						const auto& resolved = snapshot->Resolved(selfShard);
						const auto targets   = resolved.Targets(occurrence);
						if (resolved.state[occurrence] == heimdall::index::Resolution::Resolved &&
							targets.size() == 1)
						{
							anchor = targets.front();
						}
					}
				}

				if (anchor.Valid())
				{
					std::unordered_map<std::uint32_t, std::shared_ptr<const LineIndex>> shardLines;
					for (const auto& use : snapshot->References(snapshot->HashOf(anchor)))
					{
						const bool declaring = use.role != heimdall::index::Role::Reference;
						const auto& shard    = *snapshot->Shards()[use.shard];
						const auto otherUri  = UriFromPath(shard.Path());
						if ((declaring && !include_declaration) ||
							!emitted.emplace(otherUri, use.offset).second)
						{
							continue;
						}

						auto& lines = shardLines[use.shard];
						if (!lines)
						{
							if (auto open = GetDocument(otherUri))
							{
								lines = open->lines;
							}
							else if (auto contents = ReadWorkspaceFile(shard.Path()))
							{
								auto built = std::make_shared<LineIndex>();
								built->Build(*contents);
								lines = std::move(built);
							}
						}

						if (!lines)
						{
							continue;
						}

						if (response.size() > 1)
						{
							response += ',';
						}

						response += "{\"uri\":";
						QuoteJson(otherUri, response);
						response += ",\"range\":";
						range(lines, use.offset, use.length, response);
						response += '}';
					}
				}
			}
		};

		if (mode == 0)
		{
			// Locals and fields resolve in a single tree; free functions and
			// globals resolve across the workspace instead.
			if (!refs)
			{
				auto workspace_refs = heimdall::RefactoringService::WorkspaceReferences(
					context.Snapshot(), context.Document(), offset, include_declaration,
					CurrentStop());
				if (!workspace_refs &&
					workspace_refs.error().code == heimdall::RefactoringErrorCode::Cancelled)
				{
					fail(workspace_refs.error());
					return;
				}

				std::string response = "[";
				for (const auto& document_refs : workspace_refs
					? *workspace_refs
					: std::vector<heimdall::DocumentOccurrences> {})
				{
					const std::string item_uri =
						UriFromPath(context.Snapshot().Path(document_refs.document));
					const auto item_tree = context.Snapshot().Syntax(document_refs.document);
					if (!item_tree)
					{
						continue;
					}

					std::shared_ptr<const LineIndex> item_lines;
					if (auto open = GetDocument(item_uri))
					{
						item_lines = open->lines;
					}
					else
					{
						auto built = std::make_shared<LineIndex>();
						if (auto source = context.Snapshot().Source(document_refs.document))
						{
							built->Build(*source);
						}

						item_lines = std::move(built);
					}

					for (auto token : document_refs.tokens)
					{
						if (token >= item_tree->Tokens().size())
						{
							continue;
						}

						if (response.size() > 1)
						{
							response += ',';
						}

						emitted.emplace(item_uri, item_tree->Tokens()[token].offset);
						response += "{\"uri\":";
						QuoteJson(item_uri, response);
						response += ",\"range\":";
						range(item_lines, item_tree->Tokens()[token].offset,
							item_tree->Tokens()[token].length, response);
						response += '}';
					}
				}

				appendWritten(response);
				if (RequestCancelled())
				{
					RespondCancelled(id);
					return;
				}

				appendIndexed(response);
				response += ']';
				Respond(id, response);
				return;
			}

			std::string response = "[";
			for (auto token : refs->tokens)
			{
				if (response.size() > 1)
				{
					response += ',';
				}

				emitted.emplace(UriFromPath(PathFromUri(uri_string)),
					tree->Tokens()[token].offset);
				response += "{\"uri\":";
				QuoteJson(uri, response);
				response += ",\"range\":";
				range(document->lines, tree->Tokens()[token].offset, tree->Tokens()[token].length, response);
				response += '}';
			}

			appendWritten(response);
			if (RequestCancelled())
			{
				RespondCancelled(id);
				return;
			}

			appendIndexed(response);

			response += ']';
			Respond(id, response);
			return;
		}

		std::string_view new_name;
		if (!GetString(params, "newName", new_name))
		{
			fail({heimdall::RefactoringErrorCode::InvalidName, "Missing newName"});
			return;
		}

		std::expected<heimdall::RefactoringPlan, heimdall::RefactoringError> plan =
			std::unexpected(heimdall::RefactoringError {});
		if (indexed_target)
		{
			plan = heimdall::IndexRename::Rename(
				*index_snapshot, self_path, offset, new_name, rename_context, CurrentStop());
		}
		else
		{
			plan = heimdall::RefactoringService::RenameLocal(
				context.Snapshot(), context.Document(), offset, new_name, CurrentStop());
		}

		if (!plan)
		{
			fail(plan.error());
			return;
		}

		const auto preview = indexed_target
		? heimdall::IndexRename::Preview(*mIndexStore.Snapshot(), *plan, RenameContext(), CurrentStop())
		: heimdall::PreviewRefactoring(m_workspace.Snapshot(), *plan, CurrentStop());
		if (!preview ||!IsCurrentVersion(uri_string, document->version))
		{
			fail(preview
				? heimdall::RefactoringError {heimdall::RefactoringErrorCode::StaleSnapshot,
					"Document changed; request rename again"}
				: preview.error());
			return;
		}

		std::string response = "{\"documentChanges\":[";
		bool first_document  = true;
		for (const auto& plan_document : plan->documents)
		{
			const std::string doc_uri = UriFromPath(plan_document.path);
			// Positions refer to the pre-edit text: the origin document uses
			// its pinned lines, open documents use theirs, and closed files
			// get an index built over the pinned original on the fly.
			std::shared_ptr<const LineIndex> lines;
			std::optional<std::int64_t> version;
			if (doc_uri == uri_string)
			{
				lines   = document->lines;
				version = document->version;
			}
			else if (auto other = GetDocument(doc_uri))
			{
				if (other->version != plan_document.version || *other->text != *plan_document.original ||
					!IsCurrentVersion(doc_uri, plan_document.version))
				{
					fail(heimdall::RefactoringError {
							heimdall::RefactoringErrorCode::StaleSnapshot,
							"Document changed; request rename again"});
					return;
				}

				lines   = other->lines;
				version = plan_document.version;
			}
			else
			{
				auto built = std::make_shared<LineIndex>();
				built->Build(*plan_document.original);
				lines   = std::move(built);
				version = std::nullopt;
			}

			if (!first_document)
			{
				response += ',';
			}

			first_document = false;
			response += "{\"textDocument\":{\"uri\":";
			QuoteJson(doc_uri, response);
			response += ",\"version\":";
			response += version ? std::to_string(*version) : "null";
			response += "},\"edits\":[";
			for (const auto& edit : plan_document.edits)
			{
				if (response.back() != '[')
				{
					response += ',';
				}

				response += "{\"range\":";
				range(lines, edit.offset, edit.length, response);
				response += ",\"newText\":";
				QuoteJson(edit.replacement, response);
				response += '}';
			}

			response += "]}";
		}

		response += "]}";
		if (indexed_target && (mIndexStore.Snapshot()->Revision() != plan->revision ||
			!mIndexComplete.load(std::memory_order_acquire)))
		{
			fail({heimdall::RefactoringErrorCode::StaleSnapshot, "Project changed; request rename again"});
			return;
		}

		Respond(id, response);
	}

	void LanguageServer::CodeActions(simdjson::dom::element request, std::string_view id)
	{
		std::string_view uri;
		simdjson::dom::object text_document;
		if (!DocumentParams(request, uri, text_document))
		{
			Respond(id, "[]");
			return;
		}

		simdjson::dom::object params;
		simdjson::dom::object range;
		simdjson::dom::object start_object;
		simdjson::dom::object end_object;
		if (!GetObject(request, "params", params) ||!GetObject(params, "range", range) ||
			!GetObject(range, "start", start_object) ||!GetObject(range, "end", end_object))
		{
			Respond(id, "[]");
			return;
		}

		const auto start_line = static_cast<std::size_t>(PositionNumber(start_object, "line"));
		auto end_line         = static_cast<std::size_t>(PositionNumber(end_object, "line"));
		if (PositionNumber(end_object, "character") == 0 && end_line > start_line)
		{
			--end_line;
		}

		const std::string uri_string(uri);
		const auto document = GetDocument(uri_string);
		if (!document)
		{
			Respond(id, "[]");
			return;
		}

		const std::shared_ptr<const std::string> text = document->text;
		const std::int64_t version                    = document->version;
		const std::shared_ptr<const LineIndex> lines  = document->lines;
		const heimdall::CompileCommand* command       = CommandFor(uri_string);
		const auto tree                               = CachedParse(uri_string, text, version, command);
		if (!tree)
		{
			RespondCancelled(id);
			return;
		}

		const auto diagnostics = RuleDiagnostics(uri_string, tree, command);
		std::string response   = "[";
		bool first             = true;

		const auto accepts = [&](std::string_view kind)
		{
			simdjson::dom::object action_context;
			simdjson::dom::array only;
			if (!GetObject(params, "context", action_context) ||
				action_context["only"].get_array().get(only))
			{
				return true;
			}

			for (const auto item : only)
			{
				std::string_view wanted;
				if (!item.get_string().get(wanted) &&
					(kind == wanted ||(kind.starts_with(wanted) && kind.size() > wanted.size() &&
					kind[wanted.size()] == '.')))
				{
					return true;
				}
			}

			return false;
		};
		const auto context = AnalysisFor(uri_string, tree, ParserOptionsFor(command));
		const auto selection_start = lines->OffsetFromPosition(
			{static_cast<std::size_t>(PositionNumber(start_object, "line")),
				static_cast<std::size_t>(PositionNumber(start_object, "character"))});
		// A signature fix must also rewrite the sibling declarations, or the
		// program no longer compiles. The extras below are best-effort: any
		// declaration the index cannot resolve or whose spelling is not a
		// plain by-value/const-reference parameter keeps the single-file edit.
		struct ParamEdit
		{
			std::string uri;
			std::size_t offset = 0;
			std::size_t length = 0;
			std::string replacement;
			std::shared_ptr<const LineIndex> lines;
		};

		const auto sibling_param_edits = [&](const heimdall::Diagnostic& diagnostic)
		{
			std::vector<ParamEdit> extras;
			if (diagnostic.code != "api/pass-by-const-reference" &&
				diagnostic.code != "api/pass-by-value")
			{
				return extras;
			}

			const bool toConstRef = diagnostic.code == "api/pass-by-const-reference";
			// The definition's own type spelling, used to confirm a sibling
			// declaration really is the same overload before touching it.
			if (diagnostic.fix.length <= diagnostic.length ||
				diagnostic.fix.offset + diagnostic.fix.length > text->size())
			{
				return extras;
			}

			const std::string wanted = NormalizedType(Trimmed(std::string_view(*text).substr(
				diagnostic.fix.offset, diagnostic.fix.length - diagnostic.length)));
			if (wanted.empty())
			{
				return extras;
			}

			const heimdall::SemanticModel& model = context.Semantic();
			const ParamSite site                 = FindParamSite(*tree, model, diagnostic.offset);
			if (site.owner == heimdall::kNone || site.funcNameToken == heimdall::kNone)
			{
				return extras;
			}

			const std::string self_path = PathFromUri(uri_string).generic_string();
			const auto snapshot         = mIndexStore.Snapshot();
			if (!snapshot)
			{
				return extras;
			}

			const auto self_shard = snapshot->ShardFor(self_path);
			if (self_shard == heimdall::index::kInvalid ||
				snapshot->Shards()[self_shard]->ContentHash() !=
				heimdall::index::HashBytes(*text))
			{
				return extras;
			}

			const auto& selfColumns          = snapshot->Shards()[self_shard]->Symbols();
			const std::uint32_t funcByte     = tree->Tokens()[site.funcNameToken].offset;
			bool have                        = false;
			heimdall::index::SymbolHash hash = 0;
			std::uint32_t selfSymbol         = heimdall::index::kInvalid;
			for (std::uint32_t symbol = 0; symbol < selfColumns.Size(); ++symbol)
			{
				if (selfColumns.kind[symbol] == heimdall::SymbolKind::Function &&
					selfColumns.nameOffset[symbol] == funcByte)
				{
					hash       = snapshot->HashOf({self_shard, symbol});
					selfSymbol = symbol;
					have       = true;
					break;
				}
			}

			if (!have)
			{
				return extras;
			}

			// Exact identities first; then the same qualified name for entities
			// the binder spells differently per file (an out-of-line `A::A`
			// carries no constructor flag, so its hash differs from the
			// in-class declaration's). The normalized-type check below keeps
			// same-named overloads untouched.
			std::vector<heimdall::index::SymbolRef> candidates =
				snapshot->Declarations(hash);
			const std::string qualified(snapshot->Shards()[self_shard]->Qualified(selfSymbol));
			for (const auto& named : snapshot->Named(qualified))
			{
				const auto& columns = snapshot->Shards()[named.shard]->Symbols();
				if (columns.kind[named.symbol] != heimdall::SymbolKind::Function)
				{
					continue;
				}

				if (columns.paramMax[named.symbol] != heimdall::index::kNoArity &&
					(site.paramCount < columns.paramMin[named.symbol] ||
					site.paramCount > columns.paramMax[named.symbol]))
				{
					continue;
				}

				candidates.push_back(named);
			}

			std::set<std::pair<std::string, std::size_t>> seen;
			const auto rename_context = RenameContext();
			for (const auto& declaration : candidates)
			{
				const auto& shard = *snapshot->Shards()[declaration.shard];
				if (shard.Path() == self_path &&
					shard.Symbols().nameOffset[declaration.symbol] == funcByte)
				{
					continue;
				}

				if (rename_context.editable && !rename_context.editable(shard.Path()))
				{
					continue;
				}

				const std::string otherUri = UriFromPath(shard.Path());
				std::shared_ptr<const std::string> otherText;
				std::shared_ptr<const LineIndex> otherLines;
				if (const auto open = GetDocument(otherUri))
				{
					otherText  = open->text;
					otherLines = open->lines;
				}
				else if (auto contents = ReadWorkspaceFile(shard.Path()))
				{
					otherText  = std::make_shared<const std::string>(std::move(*contents));
					auto built = std::make_shared<LineIndex>();
					built->Build(*otherText);
					otherLines = std::move(built);
				}
				else
				{
					continue;
				}

				const auto* otherCommand = CommandFor(otherUri);
				const auto otherTree =
					heimdall::ParseTree::Parse(*otherText, ParserOptionsFor(otherCommand));
				const auto otherModel = heimdall::Binder::Bind(otherTree);
				DeclEdit sibling;
				if (!SiblingParamEdit(otherTree, otherModel,
					shard.Symbols().nameOffset[declaration.symbol], site.ordinal,
					site.paramCount, toConstRef, site.name, sibling) ||
					sibling.end > otherText->size() ||
					NormalizedType(sibling.rawType) != wanted ||
					!seen.emplace(otherUri, sibling.begin).second)
				{
					continue;
				}

				extras.push_back(
					{otherUri, sibling.begin, sibling.end - sibling.begin,
						std::move(sibling.text), std::move(otherLines)});
			}

			return extras;
		};
		const auto selection_end = lines->OffsetFromPosition(
			{static_cast<std::size_t>(PositionNumber(end_object, "line")),
				static_cast<std::size_t>(PositionNumber(end_object, "character"))});
		const auto append_refactor = [&](const auto& plan, std::string_view kind)
		{
			if (!plan || RequestCancelled() ||!IsCurrentVersion(uri_string, version) ||
				!heimdall::PreviewRefactoring(m_workspace.Snapshot(), *plan, CurrentStop()))
			{
				return;
			}

			if (!first)
			{
				response += ',';
			}

			first = false;
			response += "{\"title\":";
			QuoteJson(plan->title, response);
			response += ",\"kind\":";
			QuoteJson(kind, response);
			response += ",\"edit\":{\"documentChanges\":[{\"textDocument\":{\"uri\":";
			QuoteJson(uri, response);
			response += ",\"version\":" + std::to_string(version) + "},\"edits\":[";
			bool first_edit = true;
			for (const auto& edit : plan->documents.front().edits)
			{
				if (!first_edit)
				{
					response += ',';
				}

				first_edit = false;
				response += "{\"range\":{\"start\":";
				AppendPosition(lines->ToPosition(edit.offset), response);
				response += ",\"end\":";
				AppendPosition(lines->ToPosition(edit.offset + edit.length), response);
				response += "},\"newText\":";
				QuoteJson(edit.replacement, response);
				response += '}';
			}

			response += "]}]}}";
		};
		if (mVersionedEdits && selection_end > selection_start)
		{
			// Deterministic collision-free default; the plan still checks lookup.
			std::string name = "extractedValue";
			for (unsigned suffix = 1; suffix < 1000; ++suffix)
			{
				const bool used = std::ranges::any_of(tree->Tokens(),[&](const auto& token)
					{
						return token.kind == heimdall::TokenKind::Identifier &&
						tree->Text(token) == name;
					});
				if (!used)
				{
					break;
				}

				name = "extractedValue" + std::to_string(suffix);
			}

			if (accepts("refactor.extract.variable"))
			{
				append_refactor(heimdall::RefactoringService::ExtractVariable(
					context.Snapshot(), context.Document(), selection_start,
					selection_end - selection_start, name, CurrentStop()),
					"refactor.extract.variable");
			}

			std::string function_name = "extractedFunction";
			for (unsigned suffix = 1; suffix < 1000; ++suffix)
			{
				const bool used = std::ranges::any_of(tree->Tokens(),[&](const auto& token)
					{
						return token.kind == heimdall::TokenKind::Identifier &&
						tree->Text(token) == function_name;
					});
				if (!used)
				{
					break;
				}

				function_name = "extractedFunction" + std::to_string(suffix);
			}

			if (accepts("refactor.extract.function"))
			{
				append_refactor(heimdall::RefactoringService::ExtractFunction(
					context.Snapshot(), context.Document(), selection_start,
					selection_end - selection_start, function_name, CurrentStop()),
					"refactor.extract.function");
			}
		}

		if (mVersionedEdits && accepts("refactor.inline.variable"))
		{
			append_refactor(
				heimdall::RefactoringService::InlineVariable(
				context.Snapshot(), context.Document(), selection_start, CurrentStop()),
				"refactor.inline.variable");
		}

		if (mVersionedEdits && accepts("refactor.inline.macro"))
		{
			// "Expand macro": write back what the preprocessor would make of the call.
			if (const auto word = IdentifierAt(*text, selection_start))
			{
				if (const auto environment = MacroEnvironmentFor(uri_string, text, version, command))
				{
					if (const heimdall::MacroDef * macro = MacroAt(*environment, *text, *word))
					{
						if (const auto edit = MacroExpansionForEdit(*environment, *text, *word, *macro))
						{
							if (!first)
							{
								response += ',';
							}

							first = false;
							response += "{\"title\":";
							QuoteJson("Expand macro " + macro->name, response);
							response += ",\"kind\":\"refactor.inline.macro\",\"edit\":{\"documentChanges\":"
								"[{\"textDocument\":{\"uri\":";
							QuoteJson(uri_string, response);
							response += ",\"version\":" + std::to_string(version) +
								"},\"edits\":[{\"range\":{\"start\":";
							AppendPosition(lines->ToPosition(edit->begin), response);
							response += ",\"end\":";
							AppendPosition(lines->ToPosition(edit->end), response);
							response += "},\"newText\":";
							QuoteJson(edit->text, response);
							response += "}]}]}}";
						}
					}
				}
			}
		}

		if (mVersionedEdits && accepts("refactor.inline.function"))
		{
			append_refactor(
				heimdall::RefactoringService::InlineFunction(
				context.Snapshot(), context.Document(), selection_start, CurrentStop()),
				"refactor.inline.function");
		}

		if (mVersionedEdits && selection_end > selection_start
			&& accepts("refactor.extract.constant"))
		{
			std::string name = "extractedConstant";
			for (unsigned suffix = 1; suffix < 1000; ++suffix)
			{
				const bool used = std::ranges::any_of(tree->Tokens(),[&](const auto& token)
					{
						return token.kind == heimdall::TokenKind::Identifier &&
						tree->Text(token) == name;
					});
				if (!used)
				{
					break;
				}

				name = "extractedConstant" + std::to_string(suffix);
			}

			append_refactor(heimdall::RefactoringService::ExtractConstant(
				context.Snapshot(), context.Document(), selection_start,
				selection_end - selection_start, name, CurrentStop()),
				"refactor.extract.constant");
		}

		if (mVersionedEdits && selection_end > selection_start
			&& accepts("refactor.extract.method"))
		{
			std::string name = "extractedMethod";
			for (unsigned suffix = 1; suffix < 1000; ++suffix)
			{
				const bool used = std::ranges::any_of(tree->Tokens(),[&](const auto& token)
					{
						return token.kind == heimdall::TokenKind::Identifier &&
						tree->Text(token) == name;
					});
				if (!used)
				{
					break;
				}

				name = "extractedMethod" + std::to_string(suffix);
			}

			append_refactor(heimdall::RefactoringService::ExtractMethod(
				context.Snapshot(), context.Document(), selection_start,
				selection_end - selection_start, name, CurrentStop()),
				"refactor.extract.method");
		}

		// Batch fixes follow any explicitly requested refactoring actions.
		// Batch semantics mirror `RuleEngine::ApplyFixes` without unsafe
		// edits: safe, in-bounds and non-overlapping fixes only.
		if (accepts("source.fixAll"))
		{
			std::vector<const heimdall::Diagnostic*> safe;
			safe.reserve(diagnostics.size());
			for (const auto& diagnostic : diagnostics)
			{
				if (diagnostic.has_fix && diagnostic.fix_is_safe)
				{
					safe.push_back(&diagnostic);
				}
			}

			std::sort(safe.begin(), safe.end(),
				[](const heimdall::Diagnostic* a, const heimdall::Diagnostic* b)
				{
					return a->fix.offset > b->fix.offset;
				});

			std::vector<const heimdall::Diagnostic*> selected;
			selected.reserve(safe.size());
			std::size_t previous_start = text->size();
			for (const auto* diagnostic : safe)
			{
				const auto& fix = diagnostic->fix;
				// Safe fixes must stay within their diagnostic (same rule as
				// ApplyFixes), be bounds-checked and not overlap a later fix.
				const bool in_range =
					fix.offset >= diagnostic->offset &&
					fix.offset - diagnostic->offset <= diagnostic->length &&
					fix.length <= diagnostic->length -(fix.offset - diagnostic->offset);
				if (!in_range || fix.offset > text->size() ||
					fix.length > text->size() - fix.offset ||
					fix.offset + fix.length > previous_start)
				{
					continue;
				}

				selected.push_back(diagnostic);
				previous_start = fix.offset;
			}

			if (!selected.empty())
			{
				std::sort(selected.begin(), selected.end(),
					[](const heimdall::Diagnostic* a, const heimdall::Diagnostic* b)
					{
						return a->fix.offset < b->fix.offset;
					});

				if (!first)
				{
					response += ',';
				}

				first = false;
				response += "{\"title\":";
				const std::string fix_all_title =
					"Fix all Heimdall issues (" + std::to_string(selected.size()) +
					(selected.size() == 1 ? " fix)" : " fixes)");
				QuoteJson(fix_all_title, response);
				response += ",\"kind\":\"source.fixAll\",\"edit\":{\"changes\":{";
				QuoteJson(uri_string, response);
				response += ":[";
				for (std::size_t i = 0; i < selected.size(); ++i)
				{
					if (i != 0)
					{
						response += ',';
					}

					const auto& fix = selected[i]->fix;
					response += "{\"range\":{\"start\":";
					AppendPosition(lines->ToPosition(fix.offset), response);
					response += ",\"end\":";
					AppendPosition(lines->ToPosition(fix.offset + fix.length), response);
					response += "},\"newText\":";
					QuoteJson(fix.replacement, response);
					response += '}';
				}

				response += "]}}}";
			}
		}

		for (const auto& diagnostic : diagnostics)
		{
			if (!diagnostic.has_fix ||!accepts("quickfix"))
			{
				continue;
			}

			// Filter by the diagnostic, since its fix may edit another line
			// (for example, inserting an include for a symbol used here).
			const auto diagnostic_start = lines->ToPosition(diagnostic.offset).line;
			const auto diagnostic_end =
				lines
			->ToPosition(
				diagnostic.offset +(diagnostic.length == 0 ? 0 : diagnostic.length - 1))
			.line;
			if (diagnostic_start > end_line || diagnostic_end < start_line)
			{
				continue;
			}

			if (!first)
			{
				response += ',';
			}

			first = false;
			response += "{\"title\":";
			QuoteJson(diagnostic.fix_title.empty()
				? "Fix " + diagnostic.code + ": " + diagnostic.message
				: diagnostic.fix_title,
				response);
			response += ",\"kind\":\"quickfix\"";
			if (diagnostic.fix_is_safe)
			{
				response += ",\"isPreferred\":true";
			}

			response += ",\"edit\":{\"changes\":{";
			QuoteJson(uri_string, response);
			response += ":[{\"range\":{\"start\":";
			AppendPosition(lines->ToPosition(diagnostic.fix.offset), response);
			response += ",\"end\":";
			AppendPosition(lines->ToPosition(diagnostic.fix.offset + diagnostic.fix.length),
				response);
			response += "},\"newText\":";
			QuoteJson(diagnostic.fix.replacement, response);
			response += "}]";
			// Signature fixes also rewrite the sibling declarations resolved
			// through the workspace index; unrelated edits stay single-file.
			for (const auto& extra : sibling_param_edits(diagnostic))
			{
				response += ',';
				QuoteJson(extra.uri, response);
				response += ":[{\"range\":{\"start\":";
				AppendPosition(extra.lines->ToPosition(extra.offset), response);
				response += ",\"end\":";
				AppendPosition(extra.lines->ToPosition(extra.offset + extra.length), response);
				response += "},\"newText\":";
				QuoteJson(extra.replacement, response);
				response += "}]";
			}

			response += "}}}";
		}

		response += ']';
		Respond(id, response);
	}

	namespace
	{

		int ToLspKind(heimdall::CompletionKind kind)
		{
			switch (kind)
			{
			case heimdall::CompletionKind::Function:
				return 3;
			case heimdall::CompletionKind::Variable:
				return 6;
			case heimdall::CompletionKind::Type:
				return 7;
			case heimdall::CompletionKind::Namespace:
				return 9;
			case heimdall::CompletionKind::Macro:
				return 21;
			case heimdall::CompletionKind::Directive:
			case heimdall::CompletionKind::Keyword:
				return 14;
			}

			return 14;
		}

		std::uint64_t PositionNumber(simdjson::dom::object position, const char* key)
		{
			std::uint64_t unsigned_value = 0;
			if (!position[key].get_uint64().get(unsigned_value))
			{
				return unsigned_value;
			}

			std::int64_t signed_value = 0;
			if (!position[key].get_int64().get(signed_value) && signed_value > 0)
			{
				return static_cast<std::uint64_t>(signed_value);
			}

			return 0;
		}

	} // namespace

	void LanguageServer::RespondIncludeCompletion(
		std::string_view id,
		const std::string& uri,
		const std::string& text,
		const LineIndex& lines,
		std::size_t offset,
		const heimdall::IncludeContext& context,
		const heimdall::CompileCommand* command)
	{
		const std::filesystem::path file_path = PathFromUri(uri);
		const std::filesystem::path base_dir =
			file_path.has_parent_path() ? file_path.parent_path() : std::filesystem::path();
		const std::string_view typed =
			std::string_view(text).substr(context.typed_offset, offset - context.typed_offset);
		const auto candidates = heimdall::IncludeIndex::CompleteIncludePath(
			base_dir, context.angled, typed, command, 1000);

		// Only the last path segment is replaced; earlier ones are already typed.
		const std::size_t slash = typed.find_last_of("/\\");
		const std::size_t name_begin =
			context.typed_offset +(slash == std::string_view::npos ? 0 : slash + 1);
		const char closing     = context.angled ? '>' : '"';
		const bool has_closing = offset < text.size() && text[offset] == closing;
		const Position start   = lines.ToPosition(name_begin);
		const Position end     = lines.ToPosition(offset);

		// A full page means the cap may have dropped entries (an empty prefix
		// over a big system directory). The client filters what it has locally
		// as the user types: claim incompleteness so it asks again with the
		// longer prefix instead of never finding `vector`.
		constexpr std::size_t page_size = 1000;
		std::string response =
			candidates.size() >= page_size ? "{\"isIncomplete\":true,\"items\":["
		: "{\"isIncomplete\":false,\"items\":[";
		for (std::size_t i = 0; i < candidates.size(); ++i)
		{
			const auto& candidate = candidates[i];
			if (i != 0)
			{
				response += ',';
			}

			const char* origin = "";
			switch (candidate.origin)
			{
			case heimdall::IncludeOrigin::Local:
				origin = "current directory";
				break;
			case heimdall::IncludeOrigin::Quote:
				origin = "-iquote directory";
				break;
			case heimdall::IncludeOrigin::Include:
				origin = "include directory";
				break;
			case heimdall::IncludeOrigin::System:
				origin = "system include";
				break;
			case heimdall::IncludeOrigin::Absolute:
				origin = "absolute path";
				break;
			}

			char sort[16];
			std::snprintf(sort, sizeof(sort), "%05zu", i);
			std::string new_text = candidate.label;
			if (!candidate.directory && !has_closing)
			{
				new_text += closing;
			}

			response += "{\"label\":";
			QuoteJson(candidate.label, response);
			response +=
				",\"kind\":" + std::string(candidate.directory ? "19" : "17") + ",\"detail\":";
			QuoteJson(origin, response);
			response +=
				",\"sortText\":\"" + std::string(sort) + "\",\"textEdit\":{\"range\":{\"start\":";
			AppendPosition(start, response);
			response += ",\"end\":";
			AppendPosition(end, response);
			response += "},\"newText\":";
			QuoteJson(new_text, response);
			response += '}';
			if (candidate.directory)
			{
				// Keep going: offer the directory's contents right away.
				response += ",\"command\":{\"title\":\"Suggest\",\"command\":\"editor.action."
					"triggerSuggest\"}";
			}

			response += '}';
		}

		response += "]}";
		Respond(id, response);
	}

	void LanguageServer::CompleteDocument(simdjson::dom::element request, std::string_view id)
	{
		if (RequestCancelled())
		{
			RespondCancelled(id);
			return;
		}

		simdjson::dom::object params;
		if (!GetObject(request, "params", params))
		{
			Respond(id, "{\"isIncomplete\":false,\"items\":[]}");
			return;
		}

		simdjson::dom::object text_document;
		if (!GetObject(params, "textDocument", text_document))
		{
			Respond(id, "{\"isIncomplete\":false,\"items\":[]}");
			return;
		}

		std::string_view uri;
		if (!GetString(text_document, "uri", uri))
		{
			Respond(id, "{\"isIncomplete\":false,\"items\":[]}");
			return;
		}

		const std::string uri_string(uri);
		const auto document = GetDocument(uri_string);
		if (!document)
		{
			Respond(id, "{\"isIncomplete\":false,\"items\":[]}");
			return;
		}

		const std::shared_ptr<const std::string> text = document->text;
		const std::int64_t version                    = document->version;
		const std::shared_ptr<const LineIndex> lines  = document->lines;
		simdjson::dom::object position;
		if (!GetObject(params, "position", position))
		{
			Respond(id, "{\"isIncomplete\":false,\"items\":[]}");
			return;
		}

		const Position cursor = {static_cast<std::size_t>(PositionNumber(position, "line")),
			static_cast<std::size_t>(PositionNumber(position, "character"))};
		const std::size_t offset = lines->OffsetFromPosition(cursor);

		const heimdall::CompileCommand* command = CommandFor(uri_string);
		if (const auto include_context = heimdall::IncludeIndex::IncludeContextAt(*text, offset))
		{
			RespondIncludeCompletion(
				id, uri_string, *text, *lines, offset, *include_context, command);
			return;
		}

		const heimdall::ParserOptions parser_options = ParserOptionsFor(command);
		const HeaderView headers                     = HeaderScopes(uri_string, text, command);
		const auto tree                              = CachedParse(uri_string, text, version, command);
		if (!tree)
		{
			RespondCancelled(id);
			return;
		}

		if (RequestCancelled())
		{
			RespondCancelled(id);
			return;
		}

		const auto items = heimdall::AnalysisFeatures::Complete(
			AnalysisFor(uri_string, tree, parser_options), offset,
			headers.index ? &headers.index->Scopes() : nullptr);
		const std::string prefix = heimdall::CompletionEngine::PrefixAt(*text, offset);
		const Position start     = lines->ToPosition(offset - prefix.size());
		const Position end       = lines->ToPosition(offset);

		std::vector<CompletionResponseItem> candidates;
		candidates.reserve(items.size());
		for (const auto& item : items)
		{
			std::string response;
			response += "{\"label\":";
			QuoteJson(item.label, response);
			response += ",\"kind\":" + std::to_string(ToLspKind(item.kind)) + ",\"detail\":";
			QuoteJson(item.detail, response);
			if (!item.filter_text.empty())
			{
				response += ",\"filterText\":";
				QuoteJson(item.filter_text, response);
			}

			if (item.deprecated)
			{
				response += ",\"tags\":[1]"; // CompletionItemTag.Deprecated
			}

			if (!item.documentation.empty())
			{
				response += ",\"documentation\":{\"kind\":\"markdown\",\"value\":";
				QuoteJson(item.documentation, response);
				response += '}';
			}

			response += ",\"textEdit\":{\"range\":{\"start\":";
			AppendPosition(start, response);
			response += ",\"end\":";
			AppendPosition(end, response);
			response += "},\"newText\":";
			QuoteJson(item.label, response);
			response += "}}";
			candidates.push_back({item.label, heimdall::CompletionRanking::Evaluate(item, prefix),
					std::move(response)});
		}

		const bool macrosTruncated = AppendMacroCompletions(uri_string, text, version, command,
			offset, prefix, start, end, items, candidates);
		const bool truncated = AppendIncludeInsertionItems(uri_string, *text, offset, command,
			headers, items, *lines, candidates);
		if (RequestCancelled())
		{
			RespondCancelled(id);
			return;
		}

		std::sort(candidates.begin(), candidates.end(),[](const auto& left, const auto& right)
			{
				if (left.score != right.score)
				{
					return left.score > right.score;
				}

				return left.label != right.label ? left.label < right.label : left.json < right.json;
			});
		std::string response;
		constexpr std::size_t kSortTextWidth = 10;
		std::size_t rank                     = 0;
		for (auto& candidate : candidates)
		{
			if (candidate.score <= 0)
			{
				continue;
			}

			if (rank != 0)
			{
				response += ',';
			}

			std::string sortText = std::to_string(rank++);
			sortText.insert(0, kSortTextWidth - std::min(kSortTextWidth, sortText.size()), '0');
			candidate.json.pop_back();
			response += candidate.json;
			response += ",\"sortText\":";
			QuoteJson("heimdall:" + sortText, response);
			response += '}';
		}

		const bool incomplete = !headers.complete || truncated || macrosTruncated;
		Respond(id, (incomplete ? "{\"isIncomplete\":true,\"items\":["
			: "{\"isIncomplete\":false,\"items\":[") + response + "]}");
	}

	// Macros live at the cursor, from any header or the compile command. Not offered after
	// `.`, `->` or `::`, where a macro name cannot appear.
	bool LanguageServer::AppendMacroCompletions(
		const std::string& uri,
		const std::shared_ptr<const std::string>& text,
		std::int64_t version,
		const heimdall::CompileCommand* command,
		std::size_t offset,
		const std::string& prefix,
		const Position& start,
		const Position& end,
		const std::vector<heimdall::CompletionItem>& items,
		std::vector<CompletionResponseItem>& candidates)
	{
		constexpr std::size_t kMaxMacroItems = 100;
		constexpr int kConstantKind          = 21;
		constexpr int kFunctionKind          = 3;
		if (prefix.empty())
		{
			return false;
		}

		const std::size_t prefixStart = offset - prefix.size();
		const char before             = prefixStart > 0 ? (*text)[prefixStart - 1] : ' ';
		const char beforeThat         = prefixStart > 1 ? (*text)[prefixStart - 2] : ' ';
		if (before == '.' || before == ':' ||(before == '>' && beforeThat == '-'))
		{
			return false;
		}

		const auto environment = MacroEnvironmentFor(uri, text, version, command);
		if (!environment)
		{
			return false;
		}

		std::unordered_set<std::string> offered;
		for (const auto& item : items)
		{
			offered.insert(item.label);
		}

		std::vector<heimdall::MacroPtr> matches;
		environment->At(offset).ForEach([&](const heimdall::MacroDef& macro)
			{
				if (macro.name.starts_with(prefix) &&
				(prefix.starts_with('_') ||!macro.name.starts_with("__")) &&
				!offered.contains(macro.name))
				{
					matches.push_back(std::make_shared<const heimdall::MacroDef>(macro));
				}
			});
		std::ranges::sort(matches,[&](const heimdall::MacroPtr& a, const heimdall::MacroPtr& b)
			{
				const float left  = heimdall::CompletionRanking::MatchScore(a->name, prefix, true);
				const float right = heimdall::CompletionRanking::MatchScore(b->name, prefix, true);
				return left != right ? left > right : a->name < b->name;
			});
		const bool truncated = matches.size() > kMaxMacroItems;
		if (matches.size() > kMaxMacroItems)
		{
			matches.resize(kMaxMacroItems);
		}

		for (const auto& macro : matches)
		{
			std::string response;
			response += "{\"label\":";
			QuoteJson(macro->name, response);
			response += ",\"kind\":" +
				std::to_string(macro->functionLike ? kFunctionKind : kConstantKind) + ",\"detail\":";
			QuoteJson(heimdall::lsp::MacroSignature(*macro), response);
			response += ",\"textEdit\":{\"range\":{\"start\":";
			AppendPosition(start, response);
			response += ",\"end\":";
			AppendPosition(end, response);
			response += "},\"newText\":";
			QuoteJson(macro->name, response);
			response += "}}";
			heimdall::CompletionItem item;
			item.label          = macro->name;
			item.kind           = heimdall::CompletionKind::Macro;
			item.completionRole = items.empty() ? heimdall::CompletionRole::Any : items.front().completionRole;
			candidates.push_back({item.label, heimdall::CompletionRanking::Evaluate(item, prefix),
					std::move(response)});
		}

		return truncated;
	}

	bool LanguageServer::AppendIncludeInsertionItems(
		const std::string& uri,
		const std::string& text,
		std::size_t offset,
		const heimdall::CompileCommand* command,
		const HeaderView& headers,
		const std::vector<heimdall::CompletionItem>& items,
		const LineIndex& lines,
		std::vector<CompletionResponseItem>& candidates)
	{
		const heimdall::CompletionContext context =
			heimdall::CompletionEngine::ContextAt(text, offset);
		const bool qualified = context.kind == heimdall::CompletionContextKind::Qualified;
		if (context.kind == heimdall::CompletionContextKind::Other ||
			(!qualified && context.prefix.empty()))
		{
			return false;
		}

		std::string scope;
		for (const auto& part : context.qualifier)
		{
			scope += scope.empty() ? part : "::" + part;
		}

		constexpr std::size_t kIncludeCandidates = 100;
		const std::string query                  = scope.empty() ? context.prefix : scope + "::" + context.prefix;
		const auto hits                          = mSymbolIndex.Query(query, kIncludeCandidates, CurrentStop());

		const std::filesystem::path file = PathFromUri(uri).lexically_normal();
		std::vector<std::filesystem::path> visible {file};
		if (headers.index)
		{
			for (const auto& header : headers.index->Files())
			{
				visible.push_back(header.lexically_normal());
			}
		}

		const auto is_header = [](const std::filesystem::path& path)
		{
			const std::string extension = path.extension().string();
			return extension == ".h" || extension == ".hh" || extension == ".hpp" ||
				extension == ".hxx" || extension == ".h++" || extension == ".inl" ||
				extension == ".ipp" || extension == ".tpp";
		};

		const std::size_t replace_start = offset - context.prefix.size();
		std::vector<std::string> offered;
		for (const auto& hit : hits)
		{
			const SymbolView symbol = hit.Symbol();
			int kind                = 0;
			auto completionKind     = heimdall::CompletionKind::Type;
			switch (symbol.kind)
			{
			case heimdall::OutlineKind::Class:
			case heimdall::OutlineKind::Union:
			case heimdall::OutlineKind::TypeAlias:
				kind = 7; // Class
				break;
			case heimdall::OutlineKind::Struct:
				kind = 22; // Struct
				break;
			case heimdall::OutlineKind::Enum:
				kind = 13; // Enum
				break;
			case heimdall::OutlineKind::Function:
				kind           = 3; // Function
				completionKind = heimdall::CompletionKind::Function;
				break;
			case heimdall::OutlineKind::Variable:
				kind           = 6; // Variable
				completionKind = heimdall::CompletionKind::Variable;
				break;
			case heimdall::OutlineKind::Concept:
				kind = 8; // Interface
				break;
			default:
				break;
			}

			if (kind == 0 || symbol.name.find("::") != std::string_view::npos ||
				heimdall::CompletionRanking::MatchScore(symbol.name, context.prefix) <= 0 ||
				(qualified && symbol.container != scope))
			{
				continue;
			}

			const std::filesystem::path header = PathFromUri(hit.file->uri).lexically_normal();
			if (!is_header(header) ||
				std::find(visible.begin(), visible.end(), header) != visible.end())
			{
				continue;
			}

			std::string qualified_name(symbol.name);
			if (!symbol.container.empty())
			{
				qualified_name = std::string(symbol.container) + "::" + qualified_name;
			}

			// Already visible through the file itself, or already offered.
			const bool known = std::any_of(items.begin(), items.end(),[&](const auto& item)
				{
					return item.label == symbol.name && symbol.container.empty();
				});
			if (known || std::find(offered.begin(), offered.end(), qualified_name) != offered.end())
			{
				continue;
			}

			const std::string spelled = heimdall::IncludeIndex::SpellHeader(header, file, command);
			const auto edit           = heimdall::PlanIncludeInsertion(text, spelled);
			if (!edit)
			{
				continue;
			}

			offered.push_back(qualified_name);
			std::string response;

			response += "{\"label\":";
			QuoteJson(std::string(symbol.name), response);
			response += ",\"kind\":" + std::to_string(kind) + ",\"detail\":";
			QuoteJson(qualified_name + "  (#include " + spelled + ")", response);
			response += ",\"filterText\":";
			QuoteJson(std::string(symbol.name), response);
			response += ",\"textEdit\":{\"range\":{\"start\":";
			AppendPosition(lines.ToPosition(replace_start), response);
			response += ",\"end\":";
			AppendPosition(lines.ToPosition(offset), response);
			response += "},\"newText\":";
			QuoteJson(qualified ? std::string(symbol.name) : qualified_name, response);
			response += "},\"additionalTextEdits\":[{\"range\":{\"start\":";
			AppendPosition(lines.ToPosition(edit->offset), response);
			response += ",\"end\":";
			AppendPosition(lines.ToPosition(edit->offset), response);
			response += "},\"newText\":";
			QuoteJson(edit->text, response);
			response += "}]}";
			heimdall::CompletionItem item;
			item.label              = std::string(symbol.name);
			item.kind               = completionKind;
			item.completionRole     = items.empty() ? context.role : items.front().completionRole;
			item.needsQualification = !qualified && !symbol.container.empty();
			item.scopeDistance      = qualified ? 0 : -1;
			const auto relative     = header.parent_path().lexically_relative(file.parent_path());
			if (!relative.empty())
			{
				constexpr std::int32_t kMaxProximityDistance = 32;
				item.fileDistance                            = 0;
				for (const auto& component : relative)
				{
					if (component != "." && item.fileDistance < kMaxProximityDistance)
					{
						++item.fileDistance;
					}
				}
			}

			candidates.push_back({item.label,
					heimdall::CompletionRanking::Evaluate(item, context.prefix, true), std::move(response)});
		}

		return hits.size() >= kIncludeCandidates;
	}

	void LanguageServer::TouchGlobalIndex(const std::string& key)
	{
		auto found = m_global_indices.find(key);
		if (found == m_global_indices.end())
		{
			return;
		}

		m_lru.erase(found->second.lru);
		m_lru.push_front(key);
		found->second.lru = m_lru.begin();
	}

	LanguageServer::HeaderView LanguageServer::AwaitHeaderScopes(
		const std::string& uri,
		const std::shared_ptr<const std::string>& text,
		const heimdall::CompileCommand* command)
	{
		// Latency fix: never block a pool thread waiting for the index worker.
		// Answer with whatever HeaderScopes has now (stale or empty with
		// complete=false); the client re-requests and the next call hits the
		// freshly built index. The old 25ms-poll loop held a worker for up to
		// 10s and blew the <50ms budget for every concurrent request.
		if (RequestCancelled())
		{
			return {nullptr, false};
		}

		HeaderView view = HeaderScopes(uri, text, command);
		if (view.index && view.index->TypeNames())
		{
			const std::lock_guard<std::mutex> lock(mParseMutex);
			auto& known = mTypeNames[uri];
			if (!known || known->Fingerprint() != view.index->TypeNames()->Fingerprint())
			{
				known = view.index->TypeNames();
			}
		}

		return view;
	}

	LanguageServer::HeaderView LanguageServer::HeaderScopes(
		const std::string& uri,
		const std::shared_ptr<const std::string>& text,
		const heimdall::CompileCommand* command)
	{
		// Two-level header cache. The fingerprint covers only the file's own
		// `#include` block plus search flags: repeat keystrokes hit it with zero
		// disk I/O (previously every completion/hover re-lexed every transitive
		// header from disk just to compute the cache key). A fingerprint miss
		// re-resolves, then re-stats the resolved set; only a real change rebuilds.
		// Built indexes are shared globally by content key, so the same <vector>
		// is parsed once across all open documents (was: once per document).
		// Builds run on a background worker: a miss answers immediately with the
		// last good index (or an empty one) and `isIncomplete=true`; the client
		// re-requests once indexing lands.
		const std::filesystem::path file_path = PathFromUri(uri);
		const std::filesystem::path base_dir =
			file_path.has_parent_path() ? file_path.parent_path() : std::filesystem::path();
		auto overlay = std::make_shared<heimdall::SourceOverlay>();
		const auto snapshot =
			t_context && t_context->interactive ? t_context->analysis : m_workspace.Snapshot();
		for (auto document : snapshot.Documents())
		{
			const auto& path = snapshot.Path(document);
			if (path != file_path)
			{
				if (auto buffer = snapshot.SourceBuffer(document))
				{
					overlay->AddSnapshot(path, std::move(buffer));
				}
				else
				{
					overlay->Add(path, snapshot.Source(document));
				}
			}
		}

		std::vector<std::filesystem::path> headers;
		{
			const std::lock_guard lock(m_index_cache_mu);
			if (const auto found = m_include_cache.find(uri); found != m_include_cache.end())
			{
				headers = found->second.headers;
			}
		}
		const auto includeFingerprint =
			heimdall::IncludeIndex::IncludeFingerprint(base_dir, *text, command);
		const auto pathFingerprint = overlay->PathFingerprint();
		const auto inputFingerprint = std::to_string(includeFingerprint.size()) + ":" +
			includeFingerprint + std::to_string(pathFingerprint.size()) +
			":" + pathFingerprint;
		auto fingerprint = inputFingerprint + overlay->Fingerprint(headers);
		std::string requested_key;
		std::string served_key;
		{
			const std::lock_guard<std::mutex> lock(m_index_cache_mu);
			auto& entry = m_include_cache[uri];
			if (entry.fingerprint != fingerprint)
			{
				entry.fingerprint.clear(); // mark resolving; restored below
				headers.clear();
				requested_key.clear();
				served_key = entry.served_key;
				// Resolve under the caller's eye: do the disk I/O outside the cache lock.
			}
			else
			{
				headers       = entry.headers;
				requested_key = entry.requested_key;
				served_key    = entry.served_key;
			}

			if (!entry.fingerprint.empty())
			{
				// Fast path: fingerprint hit.
				if (!requested_key.empty())
				{
					if (const auto global = m_global_indices.find(requested_key);
						global != m_global_indices.end())
					{
						TouchGlobalIndex(requested_key);
						return {global->second.index, true};
					}

					if (m_index_pending.contains(requested_key))
					{
						if (!served_key.empty())
						{
							if (const auto stale = m_global_indices.find(served_key);
								stale != m_global_indices.end())
							{
								TouchGlobalIndex(served_key);
								return {stale->second.index, false};
							}
						}

						return {nullptr, false};
					}

					// Requested but neither built nor pending (evicted): rebuild.
					m_index_pending.insert(requested_key);
					{
						const std::lock_guard<std::mutex> queue_lock(mIndexMutex);
						m_index_queue.push_back({requested_key, headers, command, overlay});
					}
					mIndexCv.notify_one();
				}

				if (!served_key.empty())
				{
					if (const auto stale = m_global_indices.find(served_key);
						stale != m_global_indices.end())
					{
						TouchGlobalIndex(served_key);
						return {stale->second.index, false};
					}
				}

				return {nullptr, requested_key.empty()};
			}
		}
		// Slow path: fingerprint changed. Resolve + stat outside the lock.
		headers = heimdall::IncludeIndex::ResolveHeaders(
			base_dir, *text, command, {}, nullptr, overlay.get());
		fingerprint = inputFingerprint + overlay->Fingerprint(headers);
		if (headers.empty())
		{
			// No headers: nothing to build, trivially complete (and shared, since
			// the key would be identical for every header-less file).
			const std::lock_guard<std::mutex> lock(m_index_cache_mu);
			auto& entry       = m_include_cache[uri];
			entry.fingerprint = fingerprint;
			entry.headers     = headers;
			entry.requested_key.clear();
			entry.served_key.clear();
			return {nullptr, true};
		}

		requested_key = heimdall::IncludeIndex::CacheKey(headers, command, overlay.get());
		{
			const std::lock_guard<std::mutex> lock(m_index_cache_mu);
			auto& entry         = m_include_cache[uri];
			entry.fingerprint   = fingerprint;
			entry.headers       = headers;
			entry.requested_key = requested_key;
			if (!requested_key.empty())
			{
				if (const auto global = m_global_indices.find(requested_key);
					global != m_global_indices.end())
				{
					entry.served_key = requested_key;
					TouchGlobalIndex(requested_key);
					return {global->second.index, true};
				}

				if (!m_index_pending.contains(requested_key))
				{
					m_index_pending.insert(requested_key);
					{
						const std::lock_guard<std::mutex> queue_lock(mIndexMutex);
						m_index_queue.push_back({requested_key, headers, command, overlay});
					}
					mIndexCv.notify_one();
				}
			}
			else
			{
				entry.served_key.clear();
				return {nullptr, true};
			}

			served_key = entry.served_key;
			if (!served_key.empty())
			{
				if (const auto stale = m_global_indices.find(served_key);
					stale != m_global_indices.end())
				{
					TouchGlobalIndex(served_key);
					return {stale->second.index, false};
				}
			}

			return {nullptr, false};
		}
	}

	// Parser options plus the macros of this text's translation unit, so function-like and
	// header-defined macros parse the way they compile. `remember` keeps the environment for
	// the document's other requests; one-shot scans of closed files pass false.
	heimdall::ParserOptions LanguageServer::ParserOptionsForText(
		const std::string& uri,
		const std::shared_ptr<const std::string>& text,
		std::int64_t version,
		const heimdall::CompileCommand* command,
		bool remember,
		std::stop_token stop)
	{
		heimdall::ParserOptions options = ParserOptionsFor(command);
		if (auto environment = MacroEnvironmentFor(uri, text, version, command, remember, stop))
		{
			if (!remember && std::getenv("HEIMDALL_INDEX_METRICS") != nullptr)
			{
				const auto& metrics = environment->Metrics();
				std::string record  = "{\"file\":";
				QuoteJson(PathFromUri(uri).generic_string(), record);
				record += ",\"totalNs\":" + std::to_string(metrics.totalNs) +
					",\"setupNs\":" + std::to_string(metrics.setupNs) +
					",\"metadataQueries\":" + std::to_string(metrics.metadataQueries) +
					",\"resolutionQueries\":" + std::to_string(metrics.resolutionQueries) +
					",\"filesRead\":" + std::to_string(metrics.filesRead) +
					",\"bytesRead\":" + std::to_string(metrics.bytesRead) +
					",\"effectsReplayed\":" + std::to_string(metrics.effectsReplayed) +
					",\"effectsScanned\":" + std::to_string(metrics.effectsScanned) +
					",\"effectsRejected\":" + std::to_string(metrics.effectsRejected) + "}";
				std::osyncstream(std::cerr) << "heimdall-index-metrics " << record << '\n';
			}

			options.macro_resolver =
				std::make_shared<heimdall::MacroEnvironmentResolver>(std::move(environment));
		}

		return options;
	}

	heimdall::ParserOptions LanguageServer::ParserOptionsFor(
		const heimdall::CompileCommand* command)
	{
		heimdall::ParserOptions options;
		if (command == nullptr)
		{
			return options;
		}

		options.standard = command->standard;
		const std::lock_guard<std::mutex> lock(m_macro_mu);
		if (const auto found = mMacroCache.find(command); found != mMacroCache.end())
		{
			options.shared_macros = found->second;
			return options;
		}

		auto macros = std::make_shared<heimdall::Preprocessor::MacroMap>(command->defines);
		for (const auto& name : command->undefines)
		{
			macros->erase(name);
		}

		mMacroCache.emplace(command, macros);
		options.shared_macros = std::move(macros);
		return options;
	}

	std::shared_ptr<const heimdall::ParseTree> LanguageServer::CachedParse(
		const std::string& uri,
		const std::shared_ptr<const std::string>& text,
		std::int64_t version,
		const heimdall::CompileCommand* command)
	{
		if (t_context && t_context->parsed && t_context->pinned && t_context->pinned->uri == uri &&
			t_context->pinned->snapshot.text.get() == text.get())
		{
			return t_context->parsed;
		}

		const auto document             = GetDocument(uri);
		heimdall::ParserOptions options = ParserOptionsFor(command);
		std::shared_ptr<ParseSlot> slot;
		std::shared_ptr<const heimdall::ParseTree> base;
		heimdall::Lexer::TextEdit base_edit;
		bool baseDirectivesUnchanged = false;
		{
			const std::lock_guard<std::mutex> lock(mParseMutex);
			if (const auto names = mTypeNames.find(uri); names != mTypeNames.end())
			{
				options.type_names = names->second;
			}

			if (const auto found = mParseCache.find(uri);
				found != mParseCache.end() && found->second.version == version &&
				found->second.text.get() == text.get() && found->second.slot &&
				found->second.options.type_names == options.type_names)
			{
				slot = found->second.slot;
			}
			else
			{
				slot        = std::make_shared<ParseSlot>();
				auto& entry = mParseCache[uri];
				if (entry.base && entry.base_version == version &&
					entry.base_options.standard == options.standard &&
					entry.base_options.shared_macros == options.shared_macros &&
					entry.base_options.predefined_macros == options.predefined_macros)
				{
					base                    = entry.base;
					base_edit               = entry.base_edit;
					baseDirectivesUnchanged = entry.baseDirectivesUnchanged;
				}
				else
				{
					entry.base.reset();
				}

				entry.version = version;
				entry.text    = text;
				entry.buffer  = document ? document->buffer : nullptr;
				entry.options = options;
				entry.slot    = slot;
			}
		}
		std::shared_ptr<const std::vector<heimdall::Token>> lexed;
		std::shared_ptr<DocumentVersion> lifetime;
		if (document && document->text.get() == text.get())
		{
			lexed    = document->tokens;
			lifetime = document->lifetime;
		}

		bool start = false;
		{
			const std::lock_guard<std::mutex> lock(slot->mu);
			if (!slot->started)
			{
				slot->started = true;
				start         = true;
			}
		}
		if (start)
		{
			if (auto environment = MacroEnvironmentFor(uri, text, version, command, true,
				lifetime ? lifetime->stop.get_token() : CurrentStop()))
			{
				options.macro_resolver =
					std::make_shared<heimdall::MacroEnvironmentResolver>(std::move(environment));
			}

			// The parse belongs to the version, not to whichever request won
			// the race. Cancelling one consumer cannot invalidate other users.
			mParsePool.SubmitCancellable(
				[this, slot, uri, text, options = std::move(options), lexed, lifetime,
				base = std::move(base), base_edit, baseDirectivesUnchanged]
				{
					std::shared_ptr<const heimdall::ParseTree> tree;
					std::exception_ptr failure;
					try
					{
						const heimdall::ParseReuse reuse
						{
							base.get(), base_edit.offset,
							base_edit.old_length,
							base_edit.new_length,
							baseDirectivesUnchanged
						};
						tree = std::make_shared<heimdall::ParseTree>(
						heimdall::ParseTree::ParseSnapshot(text, options,
						lifetime ? lifetime->stop.get_token()
						: std::stop_token {},
						lexed, base ? &reuse : nullptr));
					}
					catch (...)
					{
						failure = std::current_exception();
					}

					std::vector<std::shared_ptr<ParseSlot::Waiter>> waiters;
					{
						const std::lock_guard<std::mutex> lock(slot->mu);
						slot->tree    = std::move(tree);
						slot->failure = failure;
						slot->ready.store(slot->tree != nullptr, std::memory_order_release);
						waiters.swap(slot->waiters);
					}
					slot->cv.notify_all();
					for (auto& waiter : waiters)
					{
						waiter->Wake();
					}

					const std::lock_guard<std::mutex> lock(mParseMutex);
					if (const auto found = mParseCache.find(uri);
					found != mParseCache.end() && found->second.slot == slot)
					{
						found->second.base.reset();
					}
				},
				lifetime ? lifetime->stop.get_token() : std::stop_token {},
				[slot]
				{
					std::vector<std::shared_ptr<ParseSlot::Waiter>> waiters;
					{
						const std::lock_guard lock(slot->mu);
						slot->cancelled = true;
						waiters.swap(slot->waiters);
					}
					slot->cv.notify_all();
					for (auto& waiter : waiters)
					{
						waiter->Wake();
					}
				});
		}

		std::unique_lock<std::mutex> lock(slot->mu);
		if (!slot->ready.load(std::memory_order_acquire) && !slot->failure && !slot->cancelled)
		{
			if (t_context && t_context->interactive)
				throw ParsePending {slot};
			// Dedicated diagnostics may wait, but remain immediately cancelable.
			if (!slot->cv.wait(lock, CurrentStop(),
				[&]
				{
					return slot->ready.load(std::memory_order_acquire) ||
					slot->failure || slot->cancelled;
				}))
			{
				return nullptr;
			}
		}

		if (slot->failure)
		{
			std::rethrow_exception(slot->failure);
		}

		return slot->tree;
	}

	bool LanguageServer::IsCurrentVersion(const std::string& uri, std::int64_t version)
	{
		const std::shared_lock<std::shared_mutex> lock(m_docs_mu);
		const auto found = m_documents.find(uri);
		return found != m_documents.end() && found->second.version == version;
	}

	void LanguageServer::SignatureHelpDocument(
		simdjson::dom::element request,
		std::string_view id)
	{
		simdjson::dom::object params;
		simdjson::dom::object text_document;
		simdjson::dom::object position;
		std::string_view uri;
		if (!GetObject(request, "params", params) ||
			!GetObject(params, "textDocument", text_document) ||
			!GetString(text_document, "uri", uri) ||!GetObject(params, "position", position))
		{
			Respond(id, "null");
			return;
		}

		const std::string uri_string(uri);
		const auto document = GetDocument(uri_string);
		if (!document)
		{
			Respond(id, "null");
			return;
		}

		const std::shared_ptr<const std::string> text = document->text;
		const std::shared_ptr<const LineIndex> lines  = document->lines;
		const Position cursor = {static_cast<std::size_t>(PositionNumber(position, "line")),
			static_cast<std::size_t>(PositionNumber(position, "character"))};
		const std::size_t offset = lines->OffsetFromPosition(cursor);

		const heimdall::CompileCommand* command      = CommandFor(uri_string);
		const heimdall::ParserOptions parser_options = ParserOptionsFor(command);
		const HeaderView headers                     = HeaderScopes(uri_string, text, command);
		const auto tree                              = CachedParse(uri_string, text, document->version, command);
		if (!tree || RequestCancelled())
		{
			RespondCancelled(id);
			return;
		}

		const heimdall::SignatureHelp help = heimdall::AnalysisFeatures::Signatures(
			AnalysisFor(uri_string, tree, parser_options), offset,
			headers.index ? &headers.index->Scopes() : nullptr);
		if (help.signatures.empty())
		{
			Respond(id, "null");
			return;
		}

		// Parameter ranges are byte offsets into the label; LSP counts UTF-16 units.
		const auto utf16 = [](std::string_view label, std::uint32_t byte)
		{
			return std::to_string(ToPosition(label, byte).character);
		};
		std::string response = "{\"signatures\":[";
		for (std::size_t i = 0; i < help.signatures.size(); ++i)
		{
			const heimdall::SignatureInfo& signature = help.signatures[i];
			response += i == 0 ? "{\"label\":" : ",{\"label\":";
			QuoteJson(signature.label, response);
			if (!signature.documentation.empty())
			{
				response += ",\"documentation\":{\"kind\":\"markdown\",\"value\":";
				QuoteJson(signature.documentation, response);
				response += '}';
			}

			response += ",\"parameters\":[";
			for (std::size_t p = 0; p < signature.parameters.size(); ++p)
			{
				const auto[begin, end] = signature.parameters[p];
				response += p == 0 ? "{\"label\":[" : ",{\"label\":[";
				response += utf16(signature.label, begin) + "," + utf16(signature.label, end);
				response += "]}";
			}

			response += "]}";
		}

		response += "],\"activeSignature\":" + std::to_string(help.active_signature) +
			",\"activeParameter\":" + std::to_string(help.active_parameter) + "}";
		Respond(id, response);
	}

	void LanguageServer::HoverDocument(simdjson::dom::element request, std::string_view id)
	{
		simdjson::dom::object params;
		if (!GetObject(request, "params", params))
		{
			Respond(id, "null");
			return;
		}

		simdjson::dom::object text_document;
		if (!GetObject(params, "textDocument", text_document))
		{
			Respond(id, "null");
			return;
		}

		std::string_view uri;
		if (!GetString(text_document, "uri", uri))
		{
			Respond(id, "null");
			return;
		}

		const std::string uri_string(uri);
		const auto document = GetDocument(uri_string);
		if (!document)
		{
			Respond(id, "null");
			return;
		}

		const std::shared_ptr<const std::string> text = document->text;
		const std::int64_t version                    = document->version;
		const std::shared_ptr<const LineIndex> lines  = document->lines;
		simdjson::dom::object position;
		if (!GetObject(params, "position", position))
		{
			Respond(id, "null");
			return;
		}

		const Position cursor = {static_cast<std::size_t>(PositionNumber(position, "line")),
			static_cast<std::size_t>(PositionNumber(position, "character"))};
		const std::size_t offset = lines->OffsetFromPosition(cursor);

		const heimdall::CompileCommand* command = CommandFor(uri_string);
		if (MacroHover(uri_string, text, version, *lines, offset, command, id))
		{
			return;
		}

		const heimdall::ParserOptions parser_options = ParserOptionsFor(command);
		const HeaderView headers                     = AwaitHeaderScopes(uri_string, text, command);
		const auto tree                              = CachedParse(uri_string, text, version, command);
		if (!tree)
		{
			RespondCancelled(id);
			return;
		}

		if (RequestCancelled())
		{
			RespondCancelled(id);
			return;
		}

		const auto hovered =
			heimdall::AnalysisFeatures::Hover(AnalysisFor(uri_string, tree, parser_options), offset,
			headers.index ? &headers.index->Scopes() : nullptr);
		if (!hovered.has_value())
		{
			Respond(id, "null");
			return;
		}

		std::string line = hovered->label;
		if (!hovered->detail.empty() && hovered->detail != hovered->label)
		{
			line = hovered->detail.find(hovered->label) == std::string::npos
			? hovered->label + ": " + hovered->detail
			: hovered->detail;
		}

		std::string value = "```cpp\n" + line + "\n```";
		if (!hovered->type_origin.empty())
		{
			value += "\n\n`<" + hovered->type_origin + ">`";
		}

		if (hovered->has_layout)
		{
			value += "\n\n**Size:** " + std::to_string(hovered->size_bytes) +
				" bytes · **Align:** " + std::to_string(hovered->align_bytes) + " bytes";
		}

		if (hovered->has_field_offset)
		{
			value += "\n\n**Offset:** " + std::to_string(hovered->field_offset) + " bytes";
		}

		if (!hovered->documentation.empty())
		{
			value += "\n\n" + hovered->documentation;
		}

		std::string response = "{\"contents\":{\"kind\":\"markdown\",\"value\":";
		QuoteJson(value, response);
		response += "}}";
		Respond(id, response);
	}

	void LanguageServer::InlayHints(simdjson::dom::element request, std::string_view id)
	{
		if (!mInlayHints.load(std::memory_order_relaxed))
		{
			Respond(id, "[]");
			return;
		}

		std::string_view uri;
		simdjson::dom::object text_document;
		if (!DocumentParams(request, uri, text_document))
		{
			Respond(id, "[]");
			return;
		}

		simdjson::dom::object params, range, start, end;
		if (!GetObject(request, "params", params) ||!GetObject(params, "range", range) ||
			!GetObject(range, "start", start) ||!GetObject(range, "end", end))
		{
			Respond(id, "[]");
			return;
		}

		const std::string uri_string(uri);
		const auto document = GetDocument(uri_string);
		if (!document)
		{
			Respond(id, "[]");
			return;
		}

		const auto text     = document->text;
		const auto lines    = document->lines;
		const auto* command = CommandFor(uri_string);
		const auto tree     = CachedParse(uri_string, text, document->version, command);
		if (!tree)
		{
			RespondCancelled(id);
			return;
		}

		const auto context             = AnalysisFor(uri_string, tree, ParserOptionsFor(command));
		const auto& symbols            = context.Symbols();
		const std::size_t first_line   = PositionNumber(start, "line");
		const std::size_t last_line    = PositionNumber(end, "line");
		const std::size_t first_offset = lines->OffsetFromPosition({first_line, 0});
		const std::size_t last_offset  = lines->OffsetFromPosition({last_line + 1, 0});
		const auto& tokens             = tree->Tokens();
		const auto& nodes              = tree->NodesSoA();
		const auto& semantic           = context.Semantic();
		const auto& semantic_symbols   = context.Symbols();
		const HeaderView typeHeaders   = AwaitHeaderScopes(uri_string, text, command);
		const auto* typeIndex          = typeHeaders.index ? &typeHeaders.index->Scopes() : nullptr;
		const auto next_significant = [&](std::size_t token, std::size_t limit)
		{
			while (token < limit && (tokens[token].kind == heimdall::TokenKind::Whitespace ||
				tokens[token].kind == heimdall::TokenKind::LineComment ||
				tokens[token].kind == heimdall::TokenKind::BlockComment))
			{
				++token;
			}

			return token;
		};
		const auto spell_type = [&](std::uint32_t type_begin, std::size_t limit)
		{
			if (type_begin >= tokens.size())
			{
				return std::string {};
			}

			std::size_t type_end     = type_begin;
			const auto template_open = next_significant(type_begin + 1, limit);
			if (template_open < limit && tokens[template_open].tok == heimdall::Tok::Lt)
			{
				int depth = 0;
				for (auto token = template_open; token < limit; ++token)
				{
					if (tokens[token].tok == heimdall::Tok::Lt)
					{
						++depth;
					}
					else if (tokens[token].tok == heimdall::Tok::Gt && --depth == 0)
					{
						type_end = token;
						break;
					}
					else if (tokens[token].tok == heimdall::Tok::Shr)
					{
						depth -= 2;
						if (depth <= 0)
						{
							type_end = token;
							break;
						}
					}
				}
			}

			const auto begin_offset = tokens[type_begin].offset;
			const auto& last_token  = tokens[type_end];
			return std::string(tree->Source().substr(
				begin_offset, last_token.offset + last_token.length - begin_offset));
		};
		const auto inferred_spelling = [&](heimdall::SymbolId symbol)
		{
			const auto declaration = semantic_symbols.decl_node[symbol];
			if (declaration >= nodes.size())
			{
				return std::string {};
			}

			const auto declaration_begin = nodes.FirstToken(declaration);
			const auto declaration_end = std::min<std::size_t>(
				declaration_begin + nodes.TokenCount(declaration), tokens.size());
			const auto name_token = semantic_symbols.decl_token[symbol];
			bool auto_declared    = false;
			for (auto token = declaration_begin; token < name_token && token < declaration_end; ++token)
			{
				auto_declared |= tokens[token].tok == heimdall::Tok::KwAuto;
			}

			if (!auto_declared)
			{
				return std::string {};
			}

			std::size_t initializer = declaration_end;
			for (auto token = name_token + 1; token < declaration_end; ++token)
			{
				if (tokens[token].tok == heimdall::Tok::Eq)
				{
					initializer = next_significant(token + 1, declaration_end);
					break;
				}
			}

			if (initializer >= declaration_end)
			{
				return std::string {};
			}

			if (tokens[initializer].tok == heimdall::Tok::KwNew)
			{
				const auto allocatedType = next_significant(initializer + 1, declaration_end);
				const auto type          = spell_type(allocatedType, declaration_end);
				return type.empty() ? type : type + "*";
			}

			if (tokens[initializer].kind != heimdall::TokenKind::Identifier)
			{
				return std::string {};
			}

			const auto spelling = tree->Text(tokens[initializer]);
			const auto name     = semantic.Names().Find(spelling);
			const auto candidate = name == heimdall::kNone
			? heimdall::kNone
			: semantic.Lookup(semantic.ScopeOfNode(declaration), name, initializer);
			if (candidate != heimdall::kNone &&
				semantic_symbols.kind[candidate] == heimdall::SymbolKind::Class)
			{
				return spell_type(initializer, declaration_end);
			}

			if (candidate != heimdall::kNone &&
				semantic_symbols.kind[candidate] == heimdall::SymbolKind::TypeAlias)
			{
				const auto alias_type = context.Types().SymbolType(candidate);
				if (context.Types().Types().IsKnown(alias_type))
				{
					return context.Types().Spell(alias_type);
				}
			}

			std::size_t qualified_type = initializer;
			for (auto token = initializer; token < declaration_end; ++token)
			{
				if (tokens[token].tok == heimdall::Tok::LBrace ||
					tokens[token].tok == heimdall::Tok::LParen ||
					tokens[token].tok == heimdall::Tok::Eq ||
					tokens[token].tok == heimdall::Tok::Semi)
				{
					break;
				}

				if (tokens[token].kind == heimdall::TokenKind::Identifier)
				{
					qualified_type = token;
				}
			}

			if (qualified_type != initializer)
			{
				const auto qualified_symbol = semantic.ResolveToken(
					static_cast<std::uint32_t>(qualified_type));
				if (qualified_symbol != heimdall::kNone &&
					semantic_symbols.kind[qualified_symbol] == heimdall::SymbolKind::Class)
				{
					return std::string(semantic.Names().Text(semantic_symbols.name[qualified_symbol]));
				}

				if (qualified_symbol != heimdall::kNone &&
					semantic_symbols.kind[qualified_symbol] == heimdall::SymbolKind::TypeAlias)
				{
					const auto alias_type = context.Types().SymbolType(qualified_symbol);
					if (context.Types().Types().IsKnown(alias_type))
					{
						return context.Types().Spell(alias_type);
					}
				}
			}

			if (qualified_type != initializer)
			{
				const auto namespace_name = semantic.Names().Find(spelling);
				const auto component      = tree->Text(tokens[qualified_type]);
				const auto component_name = semantic.Names().Find(component);
				if (namespace_name != heimdall::kNone && component_name != heimdall::kNone)
				{
					for (heimdall::SymbolId name_space = 0;
						name_space < semantic_symbols.Size(); ++name_space)
					{
						if (semantic_symbols.kind[name_space] != heimdall::SymbolKind::Namespace ||
							semantic_symbols.name[name_space] != namespace_name)
						{
							continue;
						}

						const auto member = semantic.LookupMember(name_space, component_name);
						if (member != heimdall::kNone &&
							semantic_symbols.kind[member] == heimdall::SymbolKind::Class)
						{
							return std::string(semantic.Names().Text(semantic_symbols.name[member]));
						}
					}
				}
			}

			auto sourceSymbol = semantic.ResolveToken(static_cast<std::uint32_t>(initializer));
			if (sourceSymbol == heimdall::kNone && name != heimdall::kNone)
			{
				sourceSymbol = semantic.Lookup(
					semantic.ScopeOfNode(declaration), name,
					static_cast<std::uint32_t>(initializer));
			}

			if (sourceSymbol != heimdall::kNone &&
				(semantic_symbols.kind[sourceSymbol] == heimdall::SymbolKind::Variable ||
				semantic_symbols.kind[sourceSymbol] == heimdall::SymbolKind::Parameter))
			{
				const auto following = next_significant(initializer + 1, declaration_end);
				if (following < declaration_end &&
					(tokens[following].tok == heimdall::Tok::Dot ||
					tokens[following].tok == heimdall::Tok::Arrow ||
					tokens[following].tok == heimdall::Tok::LParen))
				{
					return std::string {};
				}

				const auto declaredType = context.Types().SymbolType(sourceSymbol);
				if (context.Types().Types().IsKnown(declaredType))
				{
					const auto declaredSpelling = context.Types().Spell(declaredType);
					if (declaredSpelling.find("<unknown>") == std::string::npos)
					{
						return declaredSpelling;
					}
				}

				const auto sourceDeclaration = semantic_symbols.decl_node[sourceSymbol];
				const auto sourceName        = semantic_symbols.decl_token[sourceSymbol];
				if (sourceDeclaration < nodes.size() && sourceName < tokens.size())
				{
					const auto typeBegin = nodes.FirstToken(sourceDeclaration);
					if (typeBegin < sourceName)
					{
						const auto beginOffset = tokens[typeBegin].offset;
						const auto endOffset   = tokens[sourceName].offset;
						auto writtenType       = tree->Source().substr(beginOffset, endOffset - beginOffset);
						while (!writtenType.empty() &&
							(writtenType.back() == ' ' || writtenType.back() == '\t'))
						{
							writtenType.remove_suffix(1);
						}

						return std::string(writtenType);
					}
				}
			}

			for (heimdall::SymbolId member = 0; member < semantic_symbols.Size(); ++member)
			{
				if (semantic_symbols.kind[member] != heimdall::SymbolKind::Variable ||
					semantic_symbols.name[member] != name)
				{
					continue;
				}

				const auto memberScope = semantic_symbols.scope[member];
				if (memberScope >= semantic.Scopes().Size() ||
					semantic.Scopes().kind[memberScope] != heimdall::ScopeKind::Class)
				{
					continue;
				}

				const auto memberDeclaration = semantic_symbols.decl_node[member];
				const auto memberName        = semantic_symbols.decl_token[member];
				if (memberDeclaration >= nodes.size() || memberName >= tokens.size())
				{
					continue;
				}

				const auto typeBegin = nodes.FirstToken(memberDeclaration);
				if (typeBegin < memberName)
				{
					const auto beginOffset = tokens[typeBegin].offset;
					const auto endOffset   = tokens[memberName].offset;
					auto writtenType       = tree->Source().substr(beginOffset, endOffset - beginOffset);
					while (!writtenType.empty() &&
						(writtenType.back() == ' ' || writtenType.back() == '\t'))
					{
						writtenType.remove_suffix(1);
					}

					return std::string(writtenType);
				}
			}

			const auto after_initializer = next_significant(initializer + 1, declaration_end);
			if (tokens[initializer].tok == heimdall::Tok::None &&
				after_initializer < declaration_end &&
				tokens[after_initializer].tok == heimdall::Tok::Lt)
			{
				for (heimdall::SymbolId type = 0; type < semantic_symbols.Size(); ++type)
				{
					if (semantic_symbols.kind[type] == heimdall::SymbolKind::Class &&
						semantic_symbols.name[type] == name)
					{
						return spell_type(initializer, declaration_end);
					}
				}
			}

			auto function = semantic.ResolveToken(static_cast<std::uint32_t>(initializer));
			if (function == heimdall::kNone && name != heimdall::kNone)
			{
				function = semantic.Lookup(
					semantic.ScopeOfNode(declaration), name, static_cast<std::uint32_t>(initializer));
			}

			if (function == heimdall::kNone ||
				semantic_symbols.kind[function] != heimdall::SymbolKind::Function)
			{
				return std::string {};
			}

			const auto function_declaration = semantic_symbols.decl_node[function];
			if (function_declaration >= nodes.size())
			{
				return std::string {};
			}

			const auto function_name = semantic_symbols.decl_token[function];
			auto return_begin        = nodes.FirstToken(function_declaration);
			while (return_begin < function_name && tokens[return_begin].tok == heimdall::Tok::KwTemplate)
			{
				while (return_begin < function_name && tokens[return_begin].tok != heimdall::Tok::Gt)
				{
					++return_begin;
				}

				++return_begin;
			}

			if (return_begin >= function_name)
			{
				return std::string {};
			}

			const auto begin_offset = tokens[return_begin].offset;
			const auto end_offset   = tokens[function_name].offset;
			std::string_view return_type = tree->Source().substr(
				begin_offset, end_offset - begin_offset);
			while (!return_type.empty() && (return_type.back() == ' ' || return_type.back() == '\t'))
			{
				return_type.remove_suffix(1);
			}

			return std::string(return_type);
		};
		const auto is_auto_declaration = [&](heimdall::SymbolId symbol)
		{
			const auto declaration = semantic_symbols.decl_node[symbol];
			if (declaration >= nodes.size())
			{
				return false;
			}

			const auto declaration_begin = nodes.FirstToken(declaration);
			const auto declaration_end = std::min<std::size_t>(
				declaration_begin + nodes.TokenCount(declaration), tokens.size());
			for (auto token = declaration_begin;
				token < semantic_symbols.decl_token[symbol] && token < declaration_end; ++token)
			{
				if (tokens[token].tok == heimdall::Tok::KwAuto)
				{
					return true;
				}
			}

			return false;
		};
		const auto structured_binding_type = [&](heimdall::SymbolId symbol, heimdall::TypeId type)
		{
			const auto declaration = semantic_symbols.decl_node[symbol];
			const auto name_token  = semantic_symbols.decl_token[symbol];
			if (declaration >= nodes.size() || name_token >= tokens.size())
			{
				return heimdall::TypeTable::Unknown;
			}

			const auto declaration_begin = nodes.FirstToken(declaration);
			std::size_t open             = name_token;
			for (auto token = declaration_begin; token < name_token; ++token)
			{
				if (tokens[token].tok == heimdall::Tok::LBracket)
				{
					open = token;
					break;
				}
			}

			if (open == name_token)
			{
				return heimdall::TypeTable::Unknown;
			}

			std::size_t close = open + 1;
			while (close < tokens.size() && tokens[close].tok != heimdall::Tok::RBracket)
			{
				++close;
			}

			const auto declaration_end = std::min<std::size_t>(
				nodes.FirstToken(declaration) + nodes.TokenCount(declaration), tokens.size());
			for (auto token = close + 1; token < declaration_end; ++token)
			{
				if (tokens[token].tok != heimdall::Tok::Eq)
				{
					continue;
				}

				const auto initializer = next_significant(token + 1, declaration_end);
				if (initializer < declaration_end)
				{
					const auto source = semantic.ResolveToken(static_cast<std::uint32_t>(initializer));
					if (source != heimdall::kNone &&
						(semantic_symbols.kind[source] == heimdall::SymbolKind::Variable ||
						semantic_symbols.kind[source] == heimdall::SymbolKind::Function))
					{
						const auto source_type = context.Types().SymbolType(source);
						if (context.Types().Types().IsKnown(source_type))
						{
							type = source_type;
						}
					}
				}

				break;
			}

			std::size_t binding_index = 0;
			for (auto token = open + 1; token < name_token; ++token)
			{
				if (tokens[token].kind == heimdall::TokenKind::Identifier)
				{
					++binding_index;
				}
			}

			const auto& types = context.Types().Types();
			if (types.Kind(type) == heimdall::TypeKind::Array)
			{
				return types.Element(type);
			}

			if (types.Kind(type) != heimdall::TypeKind::Class)
			{
				return heimdall::TypeTable::Unknown;
			}

			const auto class_symbol = types.Arg(type);
			if (class_symbol >= semantic_symbols.Size())
			{
				return heimdall::TypeTable::Unknown;
			}

			const auto member_scope = semantic_symbols.member_scope[class_symbol];
			for (heimdall::SymbolId member = 0; member < semantic_symbols.Size(); ++member)
			{
				if (semantic_symbols.kind[member] != heimdall::SymbolKind::Variable ||
					semantic_symbols.scope[member] != member_scope)
				{
					continue;
				}

				if (binding_index == 0)
				{
					return context.Types().SymbolType(member);
				}

				--binding_index;
			}

			return heimdall::TypeTable::Unknown;
		};
		const auto is_structured_binding = [&](heimdall::SymbolId symbol)
		{
			const auto declaration = semantic_symbols.decl_node[symbol];
			const auto name_token  = semantic_symbols.decl_token[symbol];
			if (declaration >= nodes.size() || name_token >= tokens.size())
			{
				return false;
			}

			for (auto token = nodes.FirstToken(declaration); token < name_token; ++token)
			{
				if (tokens[token].tok == heimdall::Tok::LBracket)
				{
					return true;
				}
			}

			return false;
		};
		std::string response = "[";
		bool first           = true;
		struct OrderedHint
		{
			std::size_t offset = 0;
			std::string label;
			int kind = 1;
		};

		std::vector<OrderedHint> orderedHints;
		const auto genericLambdaElement = [&](std::size_t lambdaFirst, std::size_t lambdaLast)
		{
			std::size_t call     = nodes.size();
			std::size_t smallest = std::numeric_limits<std::size_t>::max();
			for (std::size_t candidate = 0; candidate < nodes.size(); ++candidate)
			{
				if (nodes.Kind(candidate) != heimdall::GrammarKind::CallExpression)
				{
					continue;
				}

				const auto begin = nodes.FirstToken(candidate);
				const auto end   = std::min<std::size_t>(begin + nodes.TokenCount(candidate), tokens.size());
				if (begin <= lambdaFirst && end >= lambdaLast && end - begin < smallest)
				{
					call     = candidate;
					smallest = end - begin;
				}
			}

			if (call == nodes.size())
			{
				return std::string {};
			}

			const auto callBegin = nodes.FirstToken(call);
			const auto callEnd   = std::min<std::size_t>(callBegin + nodes.TokenCount(call), tokens.size());
			std::size_t open     = callEnd;
			for (auto token = callBegin; token < lambdaFirst; ++token)
			{
				if (tokens[token].tok != heimdall::Tok::LParen)
				{
					continue;
				}

				std::size_t depth = 1;
				for (auto next = token + 1; next < callEnd; ++next)
				{
					if (tokens[next].tok == heimdall::Tok::LParen)
					{
						++depth;
					}
					else if (tokens[next].tok == heimdall::Tok::RParen && --depth == 0)
					{
						if (next >= lambdaFirst)
						{
							open = token;
						}

						break;
					}
				}

				if (open != callEnd)
				{
					break;
				}
			}

			if (open == callEnd)
			{
				return std::string {};
			}

			const auto argument = next_significant(open + 1, lambdaFirst);
			if (argument >= lambdaFirst || tokens[argument].kind != heimdall::TokenKind::Identifier)
			{
				return std::string {};
			}

			auto sourceSymbol = semantic.ResolveToken(static_cast<std::uint32_t>(argument));
			if (sourceSymbol == heimdall::kNone)
			{
				const auto name = semantic.Names().Find(tree->Text(tokens[argument]));
				if (name != heimdall::kNone)
				{
					sourceSymbol = semantic.Lookup(semantic.ScopeOfNode(call), name,
						static_cast<std::uint32_t>(argument));
				}
			}

			if (sourceSymbol == heimdall::kNone ||
				(semantic_symbols.kind[sourceSymbol] != heimdall::SymbolKind::Variable &&
				semantic_symbols.kind[sourceSymbol] != heimdall::SymbolKind::Parameter))
			{
				return std::string {};
			}

			const auto containerType = context.Types().Types().Strip(
				context.Types().SymbolType(sourceSymbol));
			std::string spelling    = context.Types().Spell(containerType);
			const auto templateOpen = spelling.find('<');
			if (templateOpen != std::string::npos)
			{
				std::size_t depth = 0;
				for (std::size_t index = templateOpen; index < spelling.size(); ++index)
				{
					if (spelling[index] == '<')
					{
						++depth;
					}
					else if (spelling[index] == '>' && --depth == 0)
					{
						spelling = spelling.substr(templateOpen + 1, index - templateOpen - 1);
						break;
					}
					else if (spelling[index] == ',' && depth == 1)
					{
						spelling = spelling.substr(templateOpen + 1, index - templateOpen - 1);
						break;
					}
				}
			}

			return spelling;
		};
		for (heimdall::SymbolId symbol = 0; symbol < symbols.Size(); ++symbol)
		{
			if (symbols.kind[symbol] != heimdall::SymbolKind::Variable)
			{
				continue;
			}

			const auto scope = symbols.scope[symbol];
			if (scope < context.Semantic().Scopes().Size() &&
				context.Semantic().Scopes().kind[scope] == heimdall::ScopeKind::Class)
			{
				continue;
			}

			const auto token_index = symbols.decl_token[symbol];
			if (token_index >= tree->Tokens().size())
			{
				continue;
			}

			const auto& token              = tree->Tokens()[token_index];
			const std::size_t token_offset = token.offset;
			if (token_offset < first_offset || token_offset >= last_offset)
			{
				continue;
			}

			if (!is_auto_declaration(symbol))
			{
				continue;
			}

			const auto type               = context.Types().SymbolType(symbol);
			const bool structured_binding = is_structured_binding(symbol);
			std::string type_spelling;
			if (context.Types().Types().IsKnown(type))
			{
				type_spelling = context.Types().Spell(type);
				if (type_spelling.find("<unknown>") != std::string::npos ||
					type_spelling == "auto")
				{
					type_spelling.clear();
				}
			}

			const auto binding_type = structured_binding_type(symbol, type);
			if (binding_type != heimdall::TypeTable::Unknown)
			{
				type_spelling = context.Types().Spell(binding_type);
			}

			if (type_spelling.empty())
			{
				if (!structured_binding)
				{
					type_spelling = inferred_spelling(symbol);
				}
			}

			if (type_spelling.empty())
			{
				type_spelling = heimdall::CompletionEngine::InferredType(*tree, token_index, typeIndex);
			}

			if (type_spelling == "auto")
			{
				type_spelling.clear();
			}

			if (type_spelling.empty())
			{
				continue;
			}

			orderedHints.push_back(
				{token_offset + token.length, ": " + type_spelling, 1});
		}

		for (std::size_t node = 0; node < nodes.size(); ++node)
		{
			if (nodes.Kind(node) != heimdall::GrammarKind::LambdaExpression)
			{
				continue;
			}

			const auto first_token = nodes.FirstToken(node);
			const auto token_end = std::min<std::size_t>(
				first_token + nodes.TokenCount(node), tokens.size());
			if (first_token >= token_end)
			{
				continue;
			}

			std::size_t paren_depth     = 0;
			std::size_t bracket_depth   = 0;
			std::size_t body_open       = token_end;
			std::size_t parameter_close = token_end;
			std::size_t capture_close   = token_end;
			bool has_trailing_return    = false;
			for (auto token = first_token; token < token_end; ++token)
			{
				const auto kind = tokens[token].tok;
				if (kind == heimdall::Tok::LBrace && paren_depth == 0 && bracket_depth == 0)
				{
					body_open = token;
					break;
				}

				has_trailing_return |= kind == heimdall::Tok::Arrow;
				if (kind == heimdall::Tok::LParen)
				{
					++paren_depth;
				}
				else if (kind == heimdall::Tok::RParen && paren_depth > 0)
				{
					--paren_depth;
					if (paren_depth == 0 && bracket_depth == 0)
					{
						parameter_close = token;
					}
				}
				else if (kind == heimdall::Tok::LBracket)
				{
					++bracket_depth;
				}
				else if (kind == heimdall::Tok::RBracket && bracket_depth > 0)
				{
					--bracket_depth;
					if (bracket_depth == 0)
					{
						capture_close = token;
					}
				}
			}

			if (body_open == token_end || tokens[body_open].offset < first_offset ||
				tokens[body_open].offset >= last_offset)
			{
				continue;
			}

			const std::string elementType = genericLambdaElement(first_token, token_end);
			if (!elementType.empty())
			{
				for (heimdall::SymbolId parameter = 0; parameter < semantic_symbols.Size(); ++parameter)
				{
					if (semantic_symbols.kind[parameter] != heimdall::SymbolKind::Parameter)
					{
						continue;
					}

					const auto nameToken = semantic_symbols.decl_token[parameter];
					if (nameToken <= first_token || nameToken >= body_open || nameToken >= tokens.size())
					{
						continue;
					}

					const auto declaration = semantic_symbols.decl_node[parameter];
					if (declaration >= nodes.size())
					{
						continue;
					}

					const auto declarationBegin = nodes.FirstToken(declaration);
					if (declarationBegin < first_token || declarationBegin >= nameToken)
					{
						continue;
					}

					std::string_view declaredType = tree->Source().substr(
						tokens[declarationBegin].offset,
						tokens[nameToken].offset - tokens[declarationBegin].offset);
					const auto autoPosition = declaredType.find("auto");
					if (autoPosition == std::string_view::npos)
					{
						continue;
					}

					while (!declaredType.empty() &&
						(declaredType.back() == ' ' || declaredType.back() == '\t'))
					{
						declaredType.remove_suffix(1);
					}

					std::string inferredType(declaredType);
					inferredType.replace(autoPosition, 4, elementType);
					orderedHints.push_back({tokens[nameToken].offset + tokens[nameToken].length,
							": " + inferredType, 1});
				}
			}

			if (has_trailing_return)
			{
				continue;
			}

			const auto return_type = context.Types().LambdaReturnType(
				static_cast<std::uint32_t>(node));
			if (!context.Types().Types().IsKnown(return_type))
			{
				continue;
			}

			const auto spelling = context.Types().Spell(return_type);
			if (spelling.empty() || spelling == "auto" ||
				spelling.find("<unknown>") != std::string::npos)
			{
				continue;
			}

			const auto signature_end = parameter_close < body_open ? parameter_close : capture_close;
			const auto hint_offset = signature_end < body_open
			? tokens[signature_end].offset + tokens[signature_end].length
			: tokens[body_open].offset;
			orderedHints.push_back({hint_offset, " -> " + spelling, 1});
		}

		{
			const auto IsBareName = [&](std::size_t argumentNode, std::string_view name)
			{
				if (name.empty() ||
					nodes.Kind(argumentNode) != heimdall::GrammarKind::IdentifierExpression)
				{
					return false;
				}

				if (nodes.TokenCount(argumentNode) != 1)
				{
					return false;
				}

				const auto token = nodes.FirstToken(argumentNode);
				return token < tokens.size() && tree->Text(tokens[token]) == name;
			};

			const HeaderView headers = AwaitHeaderScopes(uri_string, text, command);
			const heimdall::ScopeIndex* external =
				headers.index ? &headers.index->Scopes() : nullptr;
			struct ParamHint
			{
				std::size_t offset = 0;
				std::string label;
			};

			std::vector<ParamHint> parameterHints;
			const auto rangeStart = lines->OffsetFromPosition(
				{first_line, PositionNumber(start, "character")});
			const auto rangeEnd = lines->OffsetFromPosition(
				{last_line, PositionNumber(end, "character")});
			constexpr std::size_t kMaxParamHints = 500;
			for (std::size_t node = 0;
				node < nodes.size() && parameterHints.size() < kMaxParamHints; ++node)
			{
				if (nodes.Kind(node) != heimdall::GrammarKind::CallExpression)
				{
					continue;
				}

				if (RequestCancelled())
				{
					RespondCancelled(id);
					return;
				}

				std::vector<std::size_t> argumentNodes;
				for (const std::size_t child : tree->DirectChildren(node))
				{
					argumentNodes.push_back(child);
				}

				if (argumentNodes.empty())
				{
					continue;
				}

				std::vector<std::size_t> argumentOffsets;
				argumentOffsets.reserve(argumentNodes.size());
				bool anyInRange = false;
				for (const std::size_t argument : argumentNodes)
				{
					const auto firstToken = nodes.FirstToken(argument);
					const std::size_t argumentOffset = firstToken < tokens.size()
					? tokens[firstToken].offset
					: std::string_view::npos;
					argumentOffsets.push_back(argumentOffset);
					anyInRange |= argumentOffset >= rangeStart && argumentOffset < rangeEnd;
				}

				if (!anyInRange || argumentOffsets.back() == std::string_view::npos)
				{
					continue;
				}

				const auto help = heimdall::AnalysisFeatures::Signatures(
					context, argumentOffsets.back(), external);
				auto callOpen = nodes.FirstToken(argumentNodes.front());
				while (callOpen > nodes.FirstToken(node))
				{
					--callOpen;
					if (next_significant(callOpen, tokens.size()) == callOpen)
					{
						break;
					}
				}

				if (help.signatures.empty() || callOpen >= tokens.size() ||
					tokens[callOpen].tok != heimdall::Tok::LParen ||
					help.call_open != tokens[callOpen].offset)
				{
					continue;
				}

				const auto& names = help.signatures.front().parameterNames;

				for (std::size_t i = 0;
					i < argumentNodes.size() && i < names.size() &&
					parameterHints.size() < kMaxParamHints;
					++i)
				{
					if (names[i].empty() || argumentOffsets[i] == std::string_view::npos ||
						argumentOffsets[i] < rangeStart || argumentOffsets[i] >= rangeEnd ||
						IsBareName(argumentNodes[i], names[i]))
					{
						continue;
					}

					if (!std::ranges::all_of(help.signatures,[&](const auto& signature)
						{
							return i < signature.parameterNames.size() &&
							signature.parameterNames[i] == names[i];
						}))
					{
						continue;
					}

					parameterHints.push_back({argumentOffsets[i], names[i] + ":"});
				}
			}

			for (const auto& hint : parameterHints)
			{
				orderedHints.push_back({hint.offset, hint.label, 2});
			}

			std::stable_sort(orderedHints.begin(), orderedHints.end(),
				[](const OrderedHint& left, const OrderedHint& right)
				{
					return left.offset < right.offset;
				});
			for (const auto& hint : orderedHints)
			{
				if (!first)
				{
					response += ',';
				}

				first = false;
				response += "{\"position\":";
				AppendPosition(lines->ToPosition(hint.offset), response);
				response += ",\"label\":";
				QuoteJson(hint.label, response);
				response += hint.kind == 1 ? ",\"kind\":1,\"paddingLeft\":true}"
				: ",\"kind\":2,\"paddingRight\":true}";
			}
		}

		response += ']';
		Respond(id, response);
	}

	void LanguageServer::RangeFormatDocument(simdjson::dom::element request, std::string_view id)
	{
		std::string_view uri;
		simdjson::dom::object text_document;
		if (!DocumentParams(request, uri, text_document))
		{
			Respond(id, "[]");
			return;
		}

		const std::string uri_string(uri);
		simdjson::dom::object params;
		if (!GetObject(request, "params", params))
		{
			Respond(id, "[]");
			return;
		}

		simdjson::dom::object range;
		if (!GetObject(params, "range", range))
		{
			Respond(id, "[]");
			return;
		}

		simdjson::dom::object start_object;
		simdjson::dom::object end_object;
		if (!GetObject(range, "start", start_object) ||!GetObject(range, "end", end_object))
		{
			Respond(id, "[]");
			return;
		}

		auto start_line = static_cast<std::size_t>(PositionNumber(start_object, "line"));
		auto end_line   = static_cast<std::size_t>(PositionNumber(end_object, "line"));
		if (PositionNumber(end_object, "character") == 0 && end_line > start_line)
		{
			--end_line;
		}

		const auto document = GetDocument(uri_string);
		if (!document)
		{
			Respond(id, "[]");
			return;
		}

		const std::shared_ptr<const std::string> text = document->text;
		const std::shared_ptr<const LineIndex> lines  = document->lines;
		heimdall::FormatOptions format_options;
		if (auto found = heimdall::FindFormatOptions(PathFromUri(uri_string)))
		{
			if (*found)
			{
				format_options = * *found;

				format_options.mainFileName = PathFromUri(uri_string).filename().string();
			}
		}
		else
		{
			const std::lock_guard<std::mutex> lock(m_init_mu);
			mInitializationError = found.error();
		}

		// Diff the fully formatted buffer, then keep only hunks overlapping
		// the requested lines: context outside the range still informs the
		// formatting (brace depth, continuation) but is never rewritten.
		const auto tree = CachedParse(uri_string, text, document->version, CommandFor(uri_string));
		if (!tree)
		{
			RespondCancelled(id);
			return;
		}

		const auto formatting = CachedFormatting(tree, format_options, CurrentStop());
		if (!formatting)
		{
			RespondCancelled(id);
			return;
		}

		const auto& edits = formatting->edits;
		if (RequestCancelled())
		{
			RespondCancelled(id);
			return;
		}

		std::string response = "[";
		bool first           = true;
		for (const auto& edit : edits)
		{
			if (edit.start_line > end_line || edit.end_line <= start_line)
			{
				continue;
			}

			if (RequestCancelled())
			{
				RespondCancelled(id);
				return;
			}

			if (!first)
			{
				response += ',';
			}

			first = false;
			response += "{\"range\":{\"start\":";
			AppendPosition(lines->ToPosition(edit.start_offset), response);
			response += ",\"end\":";
			AppendPosition(lines->ToPosition(edit.end_offset), response);
			response += "},\"newText\":";
			QuoteJson(edit.replacement, response);
			response += '}';
		}

		response += ']';
		Respond(id, response);
	}

	void LanguageServer::IndexWorkerMain(std::stop_token stop)
	{
		std::uint64_t refreshId = 0;
		while (!stop.stop_requested())
		{
			IndexJob job;
			{
				std::unique_lock<std::mutex> lock(mIndexMutex);
				mIndexCv.wait(lock, stop,[&]
					{
						return !m_index_queue.empty();
					});
				if (stop.stop_requested())
				{
					return;
				}

				job = std::move(m_index_queue.front());
				m_index_queue.pop_front();
			}
			std::shared_ptr<const heimdall::IncludeIndex> built;
			try
			{
				built = std::make_shared<const heimdall::IncludeIndex>(
					heimdall::IncludeIndex::Build(job.headers, job.command, {}, job.overlay.get()));
			}
			catch (...)
			{
				// An exception escaping a jthread is std::terminate: it would kill
				// the whole server. Drop the job but release the pending mark, or
				// this key would stay "building" forever.
			}

			{
				const std::lock_guard<std::mutex> lock(m_index_cache_mu);
				m_index_pending.erase(job.key);
				if (!built)
				{
					continue;
				}

				// LRU eviction: the global index used to grow without bounds.
				if (!m_global_indices.contains(job.key) &&
					m_global_indices.size() >= kMaxGlobalIndices && !m_lru.empty())
				{
					m_global_indices.erase(m_lru.back());
					m_lru.pop_back();
				}

				m_lru.remove(job.key);
				m_lru.push_front(job.key);
				m_global_indices[job.key] = {std::move(built), m_lru.begin()};
				for (auto& [uri, entry] : m_include_cache)
				{
					if (entry.requested_key == job.key)
					{
						entry.served_key = job.key;
					}
				}
			}
			if (mSemanticTokensRefresh.load(std::memory_order_relaxed))
			{
				Send("{\"jsonrpc\":\"2.0\",\"id\":\"semantic-refresh-" + std::to_string(++refreshId) +
					"\",\"method\":\"workspace/semanticTokens/refresh\",\"params\":null}");
			}
		}
	}

	void LanguageServer::EnqueueDiagnostics(const std::string& uri, std::int64_t version)
	{
		{
			const std::lock_guard<std::mutex> lock(mDiagMutex);
			mDiaQueue.push_back({uri, version});
			// The running pass (if for this document) is now obsolete.
			if (mDiagRunningUri == uri)
			{
				mDiagStop.request_stop();
			}
		}
		mDiagCv.notify_one();
	}

	void LanguageServer::FlushDiagnostics()
	{
		std::unique_lock<std::mutex> lock(mDiagMutex);
		mDiagCv.wait(lock,[&]
			{
				return mDiaQueue.empty() && !mDiagBusy;
			});
	}

	void LanguageServer::DiagWorkerMain(std::stop_token stop)
	{
		using namespace std::chrono_literals;
		while (!stop.stop_requested())
		{
			DiagJob job;
			bool stale = false;
			{
				std::unique_lock<std::mutex> lock(mDiagMutex);
				mDiagCv.wait(lock, stop,[&]
					{
						return !mDiaQueue.empty();
					});
				if (stop.stop_requested())
				{
					return;
				}

				job = mDiaQueue.front();
				mDiaQueue.pop_front();
				// Coalesce bursts: a newer queued job for the same uri makes
				// this one stale before it even starts.
				for (const auto& queued : mDiaQueue)
				{
					if (queued.uri == job.uri)
					{
						stale = true;
						break;
					}
				}

				if (!stale)
				{
					mDiagBusy = true;
				}
				else
				{
					mDiagCv.notify_all();
				}
			}
			if (stale)
			{
				continue;
			}

			std::stop_token job_stop;
			{
				// Trailing-edge debounce: a keystroke arriving within the window
				// supersedes this version instead of paying for a full publish.
				std::unique_lock<std::mutex> lock(mDiagMutex);
				mDiagCv.wait_for(lock, 50ms,[&]
					{
						if (stop.stop_requested())
						{
							return true;
						}

						for (const auto& queued : mDiaQueue)
						{
							if (queued.uri == job.uri)
							{
								return true;
							}
						}

						return false;
					});
				if (stop.stop_requested())
				{
					mDiagBusy = false;
					mDiagCv.notify_all();
					return;
				}

				bool superseded = false;
				for (const auto& queued : mDiaQueue)
				{
					if (queued.uri == job.uri)
					{
						superseded = true;
						break;
					}
				}

				if (superseded)
				{
					mDiagBusy = false;
					mDiagCv.notify_all();
					continue;
				}

				// Registered under the same lock EnqueueDiagnostics takes, so a
				// keystroke can never slip between "not superseded" and "running".
				mDiagStop       = std::stop_source();
				mDiagRunningUri = job.uri;
				job_stop        = mDiagStop.get_token();
			}
			{
				// Drop versions that are already obsolete (closed or re-edited).
				std::shared_ptr<const std::string> text;
				std::int64_t version = -1;
				if (const auto document = GetDocument(job.uri))
				{
					text    = document->text;
					version = document->version;
				}

				if (text && version == job.version)
				{
					struct Scope
					{
						RequestContext context;
						Scope(std::stop_token stop)
						{
							context.stop = std::move(stop);
							t_context    = &context;
						}
						~Scope()
						{
							t_context = nullptr;
						}
					} scope(
						job_stop);
					try
					{
						PublishDiagnostics(job.uri, std::move(text), version);
						constexpr std::size_t kAnalysisCacheBytes = 128 * 1024 * 1024;
						m_workspace.TrimCaches(kAnalysisCacheBytes);
					}
					catch (...)
					{
						// Must not escape the jthread (std::terminate), and the
						// busy flag below must be cleared or FlushDiagnostics
						// (shutdown) waits forever.
					}
				}

				const std::lock_guard<std::mutex> lock(mDiagMutex);
				mDiagRunningUri.clear();
				mDiagBusy = false;
				mDiagCv.notify_all();
			}
		}
	}

	namespace
	{
		// Source files that may hold the body of something declared in `owner`:
		// siblings sharing its stem (`foo.hpp` -> `foo.cpp`), compile-database
		// entries with that stem, and the open documents.
		constexpr std::string_view kSourceExtensions[] = {".cpp", ".cc", ".cxx",
			".c++", ".cp", ".C"};

		bool IsSourceExtension(const std::filesystem::path& path)
		{
			const std::string extension = path.extension().string();
			for (const auto candidate : kSourceExtensions)
			{
				if (extension == candidate)
				{
					return true;
				}
			}

			return false;
		}

		std::optional<std::string> ReadWholeFile(const std::filesystem::path& path)
		{
			constexpr std::uintmax_t kMaxBytes = 4u << 20;
			std::error_code error;
			const auto size = std::filesystem::file_size(path, error);
			if (error || size > kMaxBytes)
			{
				return std::nullopt;
			}

			std::ifstream stream(path, std::ios::binary);
			if (!stream)
			{
				return std::nullopt;
			}

			std::string content(static_cast<std::size_t>(size), '\0');
			stream.read(content.data(), static_cast<std::streamsize>(size));
			content.resize(static_cast<std::size_t>(stream.gcount()));
			return content;
		}

		// One file's text plus its line index, kept alive together (LineIndex
		// borrows the text).
		struct LoadedFile
		{
			std::string uri;
			std::shared_ptr<const std::string> text;
			std::unique_ptr<LineIndex> lines;
		};

		void AppendLocation(
			const LoadedFile& file,
			std::size_t offset,
			std::size_t length,
			std::string& out)
		{
			if (!out.empty() && out.back() != '[')
			{
				out += ',';
			}

			out += "{\"uri\":";
			QuoteJson(file.uri, out);
			out += ",\"range\":{\"start\":";
			AppendPosition(file.lines->ToPosition(offset), out);
			out += ",\"end\":";
			AppendPosition(file.lines->ToPosition(offset + length), out);
			out += "}}";
		}
	} // namespace

	void LanguageServer::DocumentLinks(simdjson::dom::element request, std::string_view id)
	{
		std::string_view uri;
		simdjson::dom::object textDocument;
		if (!DocumentParams(request, uri, textDocument))
		{
			Respond(id, "[]");
			return;
		}

		const std::string uriString(uri);
		const auto document = GetDocument(uriString);
		if (!document)
		{
			Respond(id, "[]");
			return;
		}

		const auto* command = CommandFor(uriString);
		const auto tree     = CachedParse(uriString, document->text, document->version, command);
		if (!tree)
		{
			RespondCancelled(id);
			return;
		}

		std::string response = "[";
		bool first           = true;
		for (const auto& reference : heimdall::DoxygenReferences(*tree))
		{
			if (RequestCancelled())
			{
				RespondCancelled(id);
				return;
			}

			const auto targets = heimdall::Navigation::Definition(*tree, reference.offset);
			if (targets.size() != 1 || targets.front().file != -1)
			{
				continue;
			}

			const auto target = document->lines->ToPosition(targets.front().offset);
			if (!first)
			{
				response += ',';
			}

			first = false;
			response += "{\"range\":{\"start\":";
			AppendPosition(document->lines->ToPosition(reference.offset), response);
			response += ",\"end\":";
			AppendPosition(document->lines->ToPosition(reference.offset + reference.length), response);
			response += "},\"target\":";
			QuoteJson(uriString + "#L" + std::to_string(target.line + 1) + "," +
				std::to_string(target.character + 1), response);
			response += '}';
		}

		response += ']';
		Respond(id, response);
	}

	void LanguageServer::GotoDocument(
		simdjson::dom::element request,
		std::string_view id,
		bool implementation)
	{
		simdjson::dom::object params;
		simdjson::dom::object text_document;
		simdjson::dom::object position;
		std::string_view uri;
		if (!GetObject(request, "params", params) ||
			!GetObject(params, "textDocument", text_document) ||
			!GetString(text_document, "uri", uri) ||!GetObject(params, "position", position))
		{
			Respond(id, "null");
			return;
		}

		const std::string uri_string(uri);
		const auto document = GetDocument(uri_string);
		if (!document)
		{
			Respond(id, "null");
			return;
		}

		const std::shared_ptr<const std::string> text = document->text;
		const std::int64_t version                    = document->version;
		const std::shared_ptr<const LineIndex> lines  = document->lines;

		const Position cursor = {static_cast<std::size_t>(PositionNumber(position, "line")),
			static_cast<std::size_t>(PositionNumber(position, "character"))};
		const std::size_t offset = lines->OffsetFromPosition(cursor);

		const heimdall::CompileCommand* command = CommandFor(uri_string);

		// On an `#include` line, navigate to the header file itself.
		// CommandFor already falls back to FindOrNearest, so no extra DB lookup.
		if (!implementation)
		{
			const std::filesystem::path self_file = PathFromUri(uri_string);
			const std::filesystem::path header = heimdall::IncludeIndex::ResolveIncludeAt(
				self_file.has_parent_path() ? self_file.parent_path() : std::filesystem::path(),
				*text, offset, command);
			if (!header.empty())
			{
				std::string response = "[{\"uri\":";
				QuoteJson(UriFromPath(header), response);
				response += ",\"range\":{\"start\":{\"line\":0,\"character\":0},"
					"\"end\":{\"line\":0,\"character\":0}}}]";
				Respond(id, response);
				return;
			}
		}

		if (!implementation && MacroDefinition(uri_string, text, version, offset, command, id))
		{
			return;
		}

		const HeaderView headers = AwaitHeaderScopes(uri_string, text, command);
		const auto tree          = CachedParse(uri_string, text, version, command);
		if (!tree)
		{
			RespondCancelled(id);
			return;
		}

		if (RequestCancelled())
		{
			RespondCancelled(id);
			return;
		}

		if (!implementation)
		{
			if (const auto locations = ModuleImportLocations(*tree, offset); !locations.empty())
			{
				Respond(id, locations);
				return;
			}
		}

		const heimdall::ScopeIndex* scopes = headers.index ? &headers.index->Scopes() : nullptr;
		const auto analysis                = AnalysisFor(uri_string, tree, ParserOptionsFor(command));
		std::vector<heimdall::NavTarget> targets =
			implementation ? heimdall::AnalysisFeatures::Implementation(analysis, offset, scopes)
		: heimdall::AnalysisFeatures::Definition(analysis, offset, scopes);

		// File table: this document, header-index files (read on demand) and the
		// source files searched below, each loaded once per request.
		std::unordered_map<std::string, LoadedFile> loaded; // keyed by uri
		// Header locations use the exact source pinned by their index.
		auto load =
			[&](const std::filesystem::path& path, std::string uri_for,
			std::shared_ptr<const std::string> indexed_source = nullptr) -> LoadedFile *
		{
			if (uri_for.empty())
			{
				uri_for = UriFromPath(path);
			}

			const std::string cache_key = indexed_source ? uri_for + "#index" : uri_for;
			if (const auto found = loaded.find(cache_key); found != loaded.end())
			{
				return &found->second;
			}

			std::shared_ptr<const std::string> content = std::move(indexed_source);
			if (!content)
			{
				const auto id = analysis.Snapshot().Find(path);
				if (id != heimdall::InvalidDocument)
				{
					content = analysis.Snapshot().Source(id);
				}
			}

			if (!content)
			{
				auto read = ReadWholeFile(path);
				if (!read)
				{
					return nullptr;
				}

				content = std::make_shared<const std::string>(std::move(*read));
			}

			LoadedFile file;
			file.uri   = uri_for;
			file.text  = std::move(content);
			file.lines = std::make_unique<LineIndex>();
			file.lines->Build(*file.text);
			return &loaded.emplace(cache_key, std::move(file)).first->second;
		};

		LoadedFile* self = nullptr;
		{
			LoadedFile file;
			file.uri   = uri_string;
			file.text  = text;
			file.lines = std::make_unique<LineIndex>();
			file.lines->Build(*text);
			self = &loaded.emplace(uri_string, std::move(file)).first->second;
		}

		struct Resolved
		{
			LoadedFile* file   = nullptr;
			std::size_t offset = 0;
			std::size_t length = 0;
		};

		std::vector<Resolved> resolved;
		auto add_resolved = [&](LoadedFile* file, std::size_t at, std::size_t length)
		{
			// The same header can be loaded twice (indexed bytes vs. bytes read
			// from disk), so deduplicate by URI, not by loader pointer.
			for (const Resolved& existing : resolved)
			{
				if (existing.file->uri == file->uri && existing.offset == at &&
					existing.length == length)
				{
					return;
				}
			}

			resolved.push_back({file, at, length});
		};

		// Candidate files for bodies of things declared in `owner`.
		// Bounded for <50ms: at most kMaxGotoCandidates files per target
		// (siblings first, then compile-DB stems, then open docs). Each
		// candidate parse below is cancellable via CurrentStop().
		auto source_candidates = [&](const std::filesystem::path& owner)
		{
			constexpr std::size_t kMaxGotoCandidates = 4;
			std::vector<std::filesystem::path> candidates;
			auto push = [&](const std::filesystem::path& path)
			{
				if (candidates.size() >= kMaxGotoCandidates)
				{
					return;
				}

				if (path != owner &&
					std::find(candidates.begin(), candidates.end(), path) == candidates.end())
				{
					candidates.push_back(path);
				}
			};
			const std::string stem = owner.stem().string();
			std::error_code error;
			const std::filesystem::path directory = owner.parent_path();
			for (const auto extension : kSourceExtensions)
			{
				if (candidates.size() >= kMaxGotoCandidates || RequestCancelled())
				{
					break;
				}

				const std::filesystem::path sibling = directory /(stem + std::string(extension));
				if (std::filesystem::is_regular_file(sibling, error))
				{
					push(sibling);
				}
			}

			{
				std::vector<std::filesystem::path> db_matches;
				{
					const std::lock_guard<std::mutex> lock(m_init_mu);
					if (m_compile_database)
					{
						for (const auto& entry : m_compile_database->Commands())
						{
							if (entry.file.stem() == owner.stem() && IsSourceExtension(entry.file))
							{
								db_matches.push_back(entry.file);
							}
						}
					}
				}
				for (const auto& path : db_matches)
				{
					push(path);
					if (candidates.size() >= kMaxGotoCandidates)
					{
						break;
					}
				}

				const std::shared_lock<std::shared_mutex> lock(m_docs_mu);
				for (const auto& [open_uri, document] : m_documents)
				{
					if (candidates.size() >= kMaxGotoCandidates || RequestCancelled())
					{
						break;
					}

					const std::filesystem::path open_path = PathFromUri(open_uri);
					if (IsSourceExtension(open_path))
					{
						push(open_path);
					}
				}
			}

			return candidates;
		};

		const heimdall::ParserOptions parser_options = ParserOptionsFor(command);
		const std::filesystem::path self_path        = PathFromUri(uri_string);
		// Bound fan-out: goto rarely needs more than a handful of targets;
		// each extra target multiplies file I/O + parses on this worker.
		constexpr std::size_t kMaxGotoTargets = 8;
		std::size_t handled_targets           = 0;
		bool indexed                          = false;
		if (!implementation)
		{
			const auto snapshot  = mIndexStore.Snapshot();
			const auto selfShard = snapshot->ShardFor(self_path.generic_string());
			const auto here      = snapshot->SymbolAt(selfShard, static_cast<std::uint32_t>(offset));
			std::vector<heimdall::index::SymbolRef> anchors;
			if (selfShard != heimdall::index::kInvalid &&
				snapshot->Shards()[selfShard]->ContentHash() == heimdall::index::HashBytes(*text))
			{
				if (here.Valid())
				{
					anchors.push_back(here);
				}
				else
				{
					anchors = snapshot->Candidates(selfShard, static_cast<std::uint32_t>(offset));
				}
			}

			for (const auto anchor : anchors)
			{
				for (const auto& definition : snapshot->Definitions(snapshot->HashOf(anchor)))
				{
					const auto& shard = *snapshot->Shards()[definition.shard];
					if (definition.shard == selfShard)
					{
						continue;
					}

					LoadedFile* file = load(std::filesystem::path(shard.Path()), {});
					if (file == nullptr)
					{
						continue;
					}

					const auto begin = shard.Symbols().nameOffset[definition.symbol];
					add_resolved(file, begin, shard.Symbols().nameLength[definition.symbol]);
					indexed = true;
				}
			}
		}

		for (const heimdall::NavTarget& target : targets)
		{
			if (RequestCancelled())
			{
				RespondCancelled(id);
				return;
			}

			if (handled_targets++ >= kMaxGotoTargets)
			{
				break;
			}

			LoadedFile* file            = self;
			std::filesystem::path owner = self_path;
			if (target.file >= 0)
			{
				if (!headers.index)
				{
					continue;
				}

				const auto& files = headers.index->Files();
				if (static_cast<std::size_t>(target.file) >= files.size())
				{
					continue;
				}

				owner = files[static_cast<std::size_t>(target.file)];
				file =
					load(owner, {}, headers.index->Source(static_cast<std::size_t>(target.file)));
				if (file == nullptr)
				{
					continue;
				}
			}

			// A function declared without a body: follow it into its source files.
			const bool function = target.kind == heimdall::CompletionKind::Function;
			bool followed       = indexed && function && !target.is_definition;
			if (function && !indexed && !target.is_definition && !target.back_reference)
			{
				for (const auto& candidate : source_candidates(owner))
				{
					if (RequestCancelled())
					{
						RespondCancelled(id);
						return;
					}

					LoadedFile* source = load(candidate, {});
					if (source == nullptr)
					{
						continue;
					}

					const heimdall::ParseTree source_tree =
						heimdall::ParseTree::Parse(*source->text, parser_options, CurrentStop());
					if (source_tree.Cancelled() || RequestCancelled())
					{
						RespondCancelled(id);
						return;
					}

					for (const auto& found : heimdall::Navigation::FindDefinitions(
						source_tree, target.scope, target.name, true, target.param_count))
					{
						add_resolved(source, found.offset, found.length);
						followed = true;
					}
				}
			}

			// Implementation of a method also reports overriders in other files.
			if (implementation && function && !target.scope.empty())
			{
				for (const auto& candidate : source_candidates(owner))
				{
					if (RequestCancelled())
					{
						RespondCancelled(id);
						return;
					}

					LoadedFile* source = load(candidate, {});
					if (source == nullptr)
					{
						continue;
					}

					const heimdall::ParseTree source_tree =
						heimdall::ParseTree::Parse(*source->text, parser_options, CurrentStop());
					if (source_tree.Cancelled() || RequestCancelled())
					{
						RespondCancelled(id);
						return;
					}

					for (const auto& found : heimdall::Navigation::FindOverriders(
						source_tree, target.scope.back(), target.name, target.param_count))
					{
						add_resolved(source, found.offset, found.length);
						followed = true;
					}
				}
			}

			if (!followed)
			{
				add_resolved(file, target.offset, target.length);
			}
		}

		if (resolved.empty())
		{
			Respond(id, "[]");
			return;
		}

		std::string response = "[";
		for (const Resolved& item : resolved)
		{
			AppendLocation(*item.file, item.offset, item.length, response);
		}

		response += ']';
		Respond(id, response);
	}

	void LanguageServer::DocumentSymbols(simdjson::dom::element request, std::string_view id)
	{
		std::string_view uri;
		simdjson::dom::object text_document;
		if (!DocumentParams(request, uri, text_document))
		{
			Respond(id, "[]");
			return;
		}

		const std::string uri_string(uri);
		const auto document = GetDocument(uri_string);
		if (!document)
		{
			Respond(id, "[]");
			return;
		}

		const auto tree =
			CachedParse(uri_string, document->text, document->version, CommandFor(uri_string));
		if (!tree || RequestCancelled())
		{
			RespondCancelled(id);
			return;
		}

		const auto symbols = heimdall::SymbolOutline::Extract(*tree);
		std::string response;
		if (mHierarchicalSymbols.load(std::memory_order_relaxed))
		{
			AppendDocumentSymbols(symbols, *document->lines, response);
		}
		else
		{
			AppendFlatDocumentSymbols(symbols, uri, *document->lines, response);
		}

		Respond(id, response);
	}

	std::shared_ptr<const heimdall::MacroEnvironment> LanguageServer::MacroEnvironmentFor(
		const std::string& uri,
		const std::shared_ptr<const std::string>& text,
		std::int64_t version,
		const heimdall::CompileCommand* command,
		bool remember,
		std::stop_token stop)
	{
		{
			const std::lock_guard<std::mutex> lock(mMacroMutex);
			if (const auto found = mMacroEnvironments.find(uri);
				found != mMacroEnvironments.end() && found->second.version == version)
			{
				return found->second.environment;
			}
		}

		const std::filesystem::path file = PathFromUri(uri);
		auto overlay                     = std::make_shared<heimdall::SourceOverlay>();
		const auto snapshot =
			t_context && t_context->interactive ? t_context->analysis : m_workspace.Snapshot();
		for (auto document : snapshot.Documents())
		{
			const auto& path = snapshot.Path(document);
			if (path == file)
			{
				continue;
			}

			if (auto buffer = snapshot.SourceBuffer(document))
			{
				overlay->AddSnapshot(path, std::move(buffer));
			}
			else
			{
				overlay->Add(path, snapshot.Source(document));
			}
		}

		heimdall::MacroEnvironmentOptions options;
		options.overlay = overlay.get();
		{
			const std::lock_guard<std::mutex> lock(m_init_mu);
			options.maxIncludeDepth = m_rule_options.macro_max_include_depth;
			options.maxMacros       = m_rule_options.macro_max_macros;
		}

		if (t_context && !stop.stop_possible())
		{
			stop = t_context->stop;
		}

		auto environment = std::make_shared<const heimdall::MacroEnvironment>(
			heimdall::MacroEnvironment::Build(file, *text, command, options, stop));
		if (environment->Main().cancelled)
		{
			return nullptr;
		}

		if (remember)
		{
			const std::lock_guard<std::mutex> lock(mMacroMutex);
			mMacroEnvironments[uri] = {version, environment};
		}

		return environment;
	}

	bool LanguageServer::MacroHover(
		const std::string& uri,
		const std::shared_ptr<const std::string>& text,
		std::int64_t version,
		const LineIndex& lines,
		std::size_t offset,
		const heimdall::CompileCommand* command,
		std::string_view id)
	{
		const auto word = IdentifierAt(*text, offset);
		if (!word)
		{
			return false;
		}

		const auto environment = MacroEnvironmentFor(uri, text, version, command);
		if (!environment)
		{
			return false;
		}

		const heimdall::MacroDef* macro = MacroAt(*environment, *text, *word);
		if (macro == nullptr)
		{
			return false;
		}

		const std::string expansion =
			MacroExpansionPreview(*environment, *text, *word, *macro);
		std::string response = "{\"contents\":{\"kind\":\"markdown\",\"value\":";
		QuoteJson(MacroHoverMarkdown(*macro, expansion), response);
		response += "},\"range\":";
		response += "{\"start\":";
		AppendPosition(lines.ToPosition(word->begin), response);
		response += ",\"end\":";
		AppendPosition(lines.ToPosition(word->end), response);
		response += "}}";
		Respond(id, response);
		return true;
	}

	std::string LanguageServer::ModuleImportLocations(
		const heimdall::ParseTree& tree,
		std::size_t offset)
	{
		const auto module  = heimdall::ExtractModuleUnit(tree);
		const auto* import = module.ImportAt(offset);
		if (import == nullptr)
		{
			return {};
		}

		const std::string target = module.ImportTarget(*import);
		if (target.empty())
		{
			return {};
		}

		std::string response = "[";
		bool first           = true;
		for (const auto& hit : mSymbolIndex.Query(target, WorkspaceSymbolIndex::kDefaultLimit,
			CurrentStop()))
		{
			const auto symbol = hit.Symbol();
			if (symbol.kind != heimdall::OutlineKind::Module ||!symbol.exported ||
				symbol.name != target)
			{
				continue;
			}

			response += first ? "" : ",";
			first = false;
			response += "{\"uri\":";
			QuoteJson(hit.file->uri, response);
			response += ",\"range\":{\"start\":";
			AppendPosition(symbol.start, response);
			response += ",\"end\":";
			AppendPosition(symbol.end, response);
			response += "}}";
		}

		return first ? std::string() : response + "]";
	}

	bool LanguageServer::MacroDefinition(
		const std::string& uri,
		const std::shared_ptr<const std::string>& text,
		std::int64_t version,
		std::size_t offset,
		const heimdall::CompileCommand* command,
		std::string_view id)
	{
		const auto word = IdentifierAt(*text, offset);
		if (!word)
		{
			return false;
		}

		const auto environment = MacroEnvironmentFor(uri, text, version, command);
		if (!environment)
		{
			return false;
		}

		const heimdall::MacroDef* macro = MacroAt(*environment, *text, *word);
		if (macro == nullptr)
		{
			return false;
		}

		if (macro->origin != heimdall::MacroOrigin::File || macro->file.empty())
		{
			Respond(id, "[]");
			return true;
		}

		const std::string targetUri = UriFromPath(std::filesystem::path(macro->file));
		std::shared_ptr<const std::string> targetText;
		if (const auto open = GetDocument(targetUri))
		{
			targetText = open->text;
		}
		else if (std::ifstream in(macro->file, std::ios::binary); in)
		{
			targetText = std::make_shared<const std::string>(
				std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
		}

		if (!targetText)
		{
			Respond(id, "[]");
			return true;
		}

		const auto name = DefinitionNameRange(*targetText, macro->offset, macro->name);
		if (!name)
		{
			Respond(id, "[]");
			return true;
		}

		LineIndex targetLines;
		targetLines.Build(*targetText);
		std::string response = "[{\"uri\":";
		QuoteJson(targetUri, response);
		response += ",\"range\":{\"start\":";
		AppendPosition(targetLines.ToPosition(name->begin), response);
		response += ",\"end\":";
		AppendPosition(targetLines.ToPosition(name->end), response);
		response += "}}]";
		Respond(id, response);
		return true;
	}

	void LanguageServer::SemanticTokensRequest(
		simdjson::dom::element request,
		std::string_view id,
		bool ranged)
	{
		std::string_view uri;
		simdjson::dom::object textDocument;
		if (!DocumentParams(request, uri, textDocument))
		{
			Respond(id, "null");
			return;
		}

		const std::string uriString(uri);
		const auto document = GetDocument(uriString);
		if (!document)
		{
			Respond(id, "null");
			return;
		}

		const auto headers = AwaitHeaderScopes(uriString, document->text, CommandFor(uriString));
		const auto tree =
			CachedParse(uriString, document->text, document->version, CommandFor(uriString));
		if (!tree || RequestCancelled())
		{
			RespondCancelled(id);
			return;
		}

		std::optional<ByteRange> range;
		if (ranged)
		{
			simdjson::dom::object params, rangeObject, start, end;
			if (!GetObject(request, "params", params) ||!GetObject(params, "range", rangeObject) ||
				!GetObject(rangeObject, "start", start) ||!GetObject(rangeObject, "end", end))
			{
				Respond(id, "null");
				return;
			}

			range = ByteRange {document->lines->OffsetFromPosition(
					{PositionNumber(start, "line"), PositionNumber(start, "character")}),
				document->lines->OffsetFromPosition(
					{PositionNumber(end, "line"), PositionNumber(end, "character")})};
		}

		const auto environment =
			MacroEnvironmentFor(uriString, document->text, document->version, CommandFor(uriString));
		heimdall::HighlightContext context;
		if (environment)
		{
			context.findMacro = [&environment](std::string_view name, std::size_t offset)
			{
				return environment->Find(name, offset);
			};
			context.activeRanges = environment->Main().activeRanges;
		}
		else
		{
			// Without a macro environment (cancelled build) still dim what the
			// file's own directives exclude, so `#if 0` works with no database.
			context.activeRanges = tree->ActiveRanges();
		}

		const auto lexical = heimdall::SyntaxHighlight::Extract(*tree, context);
		const auto model   = heimdall::Binder::Bind(*tree);
		const auto spans = heimdall::SemanticHighlight::Extract(
			model, lexical, headers.index ? &headers.index->Highlights() : nullptr);
		if (RequestCancelled())
		{
			RespondCancelled(id);
			return;
		}

		std::string response;
		AppendSemanticTokens(spans, tree->Source(), *document->lines, range, response);
		Respond(id, response);
	}

	void LanguageServer::TypeHierarchyRequest(
		simdjson::dom::element request,
		std::string_view id,
		TypeHierarchyMode mode)
	{
		simdjson::dom::object params;
		std::string_view uri;
		std::size_t offset = 0;
		if (!GetObject(request, "params", params))
		{
			Respond(id, "null");
			return;
		}

		if (mode == TypeHierarchyMode::Prepare)
		{
			simdjson::dom::object textDocument;
			simdjson::dom::object position;
			if (!GetObject(params, "textDocument", textDocument) ||
				!GetString(textDocument, "uri", uri) ||!GetObject(params, "position", position))
			{
				Respond(id, "null");
				return;
			}

			const auto document = GetDocument(std::string(uri));
			if (!document)
			{
				Respond(id, "null");
				return;
			}

			offset = document->lines->OffsetFromPosition(
				{static_cast<std::size_t>(PositionNumber(position, "line")),
					static_cast<std::size_t>(PositionNumber(position, "character"))});
		}
		else
		{
			simdjson::dom::object item;
			simdjson::dom::object data;
			if (!GetObject(params, "item", item) ||!GetObject(item, "data", data) ||
				!GetString(data, "uri", uri))
			{
				Respond(id, "[]");
				return;
			}

			offset = static_cast<std::size_t>(PositionNumber(data, "offset"));
		}

		const char* empty = mode == TypeHierarchyMode::Prepare ? "null" : "[]";
		const std::string uriString(uri);
		const auto document = GetDocument(uriString);
		if (!document)
		{
			Respond(id, empty);
			return;
		}

		const auto* command = CommandFor(uriString);
		const auto tree     = CachedParse(uriString, document->text, document->version, command);
		if (!tree || RequestCancelled())
		{
			RespondCancelled(id);
			return;
		}

		const auto analysis = AnalysisFor(uriString, tree, ParserOptionsFor(command));
		const auto& model   = analysis.Semantic();
		const auto token    = heimdall::TypeHierarchy::TokenAt(model, offset);
		const auto klass =
			token == heimdall::kNone
		? heimdall::kNone
		: heimdall::TypeHierarchy::FindClassAt(model, analysis.Types(), token);
		if (klass == heimdall::kNone)
		{
			Respond(id, empty);
			return;
		}

		std::vector<heimdall::SymbolId> classes;
		switch (mode)
		{
		case TypeHierarchyMode::Prepare:
			classes = {klass};
			break;
		case TypeHierarchyMode::Supertypes:
			classes = heimdall::TypeHierarchy::Parents(model, klass);
			break;
		case TypeHierarchyMode::Subtypes:
			classes = heimdall::TypeHierarchy::Children(model, klass);
			break;
		}

		const auto& symbols  = model.Symbols();
		const auto& tokens   = model.Tree().Tokens();
		std::string response = "[";
		const auto append = [&](std::string_view name, const std::string& itemUri,
			const LineIndex& lines, std::size_t rangeBegin, std::size_t rangeEnd,
			std::size_t nameBegin, std::size_t nameEnd)
		{
			if (response.size() > 1)
			{
				response += ',';
			}

			response += "{\"name\":";
			QuoteJson(name, response);
			response += ",\"kind\":23,\"uri\":";
			QuoteJson(itemUri, response);
			response += ",\"range\":{\"start\":";
			AppendPosition(lines.ToPosition(rangeBegin), response);
			response += ",\"end\":";
			AppendPosition(lines.ToPosition(rangeEnd), response);
			response += "},\"selectionRange\":{\"start\":";
			AppendPosition(lines.ToPosition(nameBegin), response);
			response += ",\"end\":";
			AppendPosition(lines.ToPosition(nameEnd), response);
			response += "},\"data\":{\"uri\":";
			QuoteJson(itemUri, response);
			response += ",\"offset\":" + std::to_string(nameBegin) + "}}";
		};

		for (const auto symbol : classes)
		{
			const auto nameToken = symbols.decl_token[symbol];
			if (nameToken >= tokens.size())
			{
				continue;
			}

			const auto range = analysis.NodeRange(symbols.decl_node[symbol]);
			const auto& name = tokens[nameToken];
			append(model.Names().Text(symbols.name[symbol]), uriString, *document->lines,
				range.offset, range.offset + range.length, name.offset, name.offset + name.length);
		}

		// Classes declared in other files, from the cross-file index. Items for this file
		// are already covered by the model above.
		if (mode != TypeHierarchyMode::Prepare)
		{
			const auto snapshot = mIndexStore.Snapshot();
			const auto selfPath = PathFromUri(uriString).generic_string();
			const auto here = snapshot->SymbolAt(snapshot->ShardFor(selfPath),
				static_cast<std::uint32_t>(offset));
			if (here.Valid())
			{
				const auto hash = snapshot->HashOf(here);
				const auto related = mode == TypeHierarchyMode::Supertypes
				? snapshot->Supertypes(hash)
				: snapshot->Subtypes(hash);
				for (const auto& item : related)
				{
					const auto& shard = *snapshot->Shards()[item.symbol.shard];
					if (shard.Path() == selfPath)
					{
						continue;
					}

					const auto otherUri = UriFromPath(shard.Path());
					std::shared_ptr<const std::string> otherText;
					if (const auto open = GetDocument(otherUri))
					{
						otherText = open->text;
					}
					else if (auto contents = ReadWorkspaceFile(shard.Path()))
					{
						otherText = std::make_shared<const std::string>(std::move(*contents));
					}

					if (!otherText)
					{
						continue;
					}

					LineIndex otherLines;
					otherLines.Build(*otherText);
					const auto begin = shard.Symbols().nameOffset[item.symbol.symbol];
					const auto end   = begin + shard.Symbols().nameLength[item.symbol.symbol];
					append(shard.Name(item.symbol.symbol), otherUri, otherLines, begin, end, begin,
						end);
				}
			}
		}

		response += ']';
		Respond(id, mode == TypeHierarchyMode::Prepare && response.size() == 2 ? "null" : response);
	}

	void LanguageServer::WorkspaceSymbols(simdjson::dom::element request, std::string_view id)
	{
		if (!mWorkspaceSymbols.load(std::memory_order_relaxed))
		{
			Respond(id, "[]");
			return;
		}

		simdjson::dom::object params;
		std::string_view query;
		if (!GetObject(request, "params", params) ||!GetString(params, "query", query))
		{
			Respond(id, "[]");
			return;
		}

		const auto hits =
			mSymbolIndex.Query(query, WorkspaceSymbolIndex::kDefaultLimit, CurrentStop());
		if (RequestCancelled())
		{
			RespondCancelled(id);
			return;
		}

		std::string response;
		AppendWorkspaceSymbols(hits, response);
		AppendHeaderMacroSymbols(query, hits, response);
		Respond(id, response);
	}

	// Macros of the headers the open documents include (system and third-party ones are not
	// part of the workspace index). Macros the index already lists are skipped.
	void LanguageServer::AppendHeaderMacroSymbols(
		std::string_view query,
		std::span<const SymbolHit> hits,
		std::string& response)
	{
		constexpr std::size_t kMaxHeaderMacros = 50;
		if (query.empty())
		{
			return;
		}

		const auto lower = [](std::string text)
		{
			for (char& c : text)
			{
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}

			return text;
		};
		const std::string needle = lower(std::string(query));
		struct Candidate
		{
			std::string name;
			std::string file;
			std::uint32_t offset = 0;
			bool system          = false;
		};

		std::vector<Candidate> candidates;
		std::unordered_set<std::string> seen;
		{
			const std::lock_guard<std::mutex> lock(mMacroMutex);
			for (const auto& [uri, entry] : mMacroEnvironments)
			{
				entry.environment->Final().ForEach([&](const heimdall::MacroDef& macro)
					{
						if (macro.origin != heimdall::MacroOrigin::File || macro.file.empty() ||
						lower(macro.name).find(needle) == std::string::npos)
						{
							return;
						}

						if (seen.insert(macro.file + "#" + macro.name).second)
						{
							candidates.push_back({macro.name, macro.file, macro.offset,
								entry.environment->IsSystemFile(macro.file)});
						}
					});
			}
		}

		for (const auto& hit : hits)
		{
			const SymbolView symbol = hit.Symbol();
			if (symbol.kind == heimdall::OutlineKind::Macro)
			{
				const std::string path = PathFromUri(hit.file->uri).generic_string();
				std::erase_if(candidates,[&](const Candidate& candidate)
					{
						return candidate.name == symbol.name &&
						std::filesystem::path(candidate.file).generic_string() == path;
					});
			}
		}

		std::ranges::sort(candidates,[](const Candidate& a, const Candidate& b)
			{
				if (a.system != b.system)
				{
					return !a.system;
				}

				return a.name.size() != b.name.size() ? a.name.size() < b.name.size() : a.name < b.name;
			});
		if (candidates.size() > kMaxHeaderMacros)
		{
			candidates.resize(kMaxHeaderMacros);
		}

		struct LoadedText
		{
			std::shared_ptr<const std::string> text;
			LineIndex lines;
		};

		std::unordered_map<std::string, LoadedText> loaded;
		std::string extra;
		for (const Candidate& candidate : candidates)
		{
			auto found = loaded.find(candidate.file);
			if (found == loaded.end())
			{
				LoadedText entry;
				if (std::ifstream in(candidate.file, std::ios::binary); in)
				{
					entry.text = std::make_shared<const std::string>(
						std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
					entry.lines.Build(*entry.text);
				}

				found = loaded.emplace(candidate.file, std::move(entry)).first;
			}

			if (!found->second.text)
			{
				continue;
			}

			const auto name = DefinitionNameRange(*found->second.text, candidate.offset, candidate.name);
			if (!name)
			{
				continue;
			}

			if (!extra.empty())
			{
				extra += ',';
			}

			extra += "{\"name\":";
			QuoteJson(candidate.name, extra);
			extra += ",\"kind\":14,\"location\":{\"uri\":";
			QuoteJson(UriFromPath(std::filesystem::path(candidate.file)), extra);
			extra += ",\"range\":{\"start\":";
			AppendPosition(found->second.lines.ToPosition(name->begin), extra);
			extra += ",\"end\":";
			AppendPosition(found->second.lines.ToPosition(name->end), extra);
			extra += "}},\"containerName\":";
			QuoteJson(std::filesystem::path(candidate.file).filename().string() +
				(candidate.system ? " (system)" : " (project)"), extra);
			extra += '}';
		}

		if (extra.empty())
		{
			return;
		}

		if (response.size() <= 2)
		{
			response = "[" + extra + "]";
		}
		else
		{
			response.pop_back();
			response += "," + extra + "]";
		}
	}

	std::shared_ptr<const heimdall::index::FileShard> LanguageServer::IndexShard(
		const heimdall::ParseTree& tree,
		const std::filesystem::path& file,
		const heimdall::CompileCommand* command)
	{
		HEIMDALL_TRACE_SCOPE(Index, "LanguageServer::IndexShard");
		const auto model = [&tree]
		{
			HEIMDALL_TRACE_SCOPE(Semantic, "Binder::Bind");
			return heimdall::Binder::Bind(tree);
		}
		();
		const auto configHash = heimdall::index::ShardBuilder::ConfigurationHash(ParserOptionsFor(command),
			command);
		return heimdall::index::ShardBuilder::Build(model, file.generic_string(), configHash,
			[&tree, &file, command](std::string_view, bool, std::uint32_t offset)->std::optional<std::string>
			{
				const auto target = heimdall::IncludeIndex::ResolveIncludeAt(file.parent_path(),
				tree.Source(), offset, command);
				return target.empty() ? std::nullopt : std::optional(target.lexically_normal().generic_string());
		});
	}

	void LanguageServer::IndexOpenSymbols(
		const std::string& uri,
		const std::shared_ptr<const std::string>& text,
		const heimdall::ParseTree& tree,
		std::int64_t version)
	{
		LineIndex lines;
		lines.Build(*text);
		const auto outline = heimdall::SymbolOutline::Extract(tree);
		auto file          = BuildIndexedFile(uri, outline, lines);
		auto shard         = IndexShard(tree, PathFromUri(uri), CommandFor(uri));
		bool updated       = false;
		{
			const std::lock_guard<std::mutex> lock(mSymbolMutex);
			if (IsCurrentVersion(uri, version))
			{
				if (mWorkspaceSymbols.load(std::memory_order_relaxed))
				{
					mSymbolIndex.SetOpen(FileKey(PathFromUri(uri)), std::move(file));
				}

				mIndexStore.SetOpen(PathFromUri(uri).generic_string(), std::move(shard));
				if (!tree.Diagnostics().empty())
				{
					mIndexComplete.store(false, std::memory_order_release);
				}

				updated = true;
			}
		}
		if (updated && !mIndexComplete.load(std::memory_order_acquire) &&
			mIndexScanFinished.load(std::memory_order_acquire) && mIndexPendingJobs.load(std::memory_order_acquire) == 0)
		{
			RefreshIndexCompleteness(std::stop_token {});
		}
	}

	void LanguageServer::IndexDiskSymbols(
		const std::filesystem::path& file,
		std::stop_token stop,
		const std::function<void(std::string_view) > & reportStage)
	{
		HEIMDALL_TRACE_SCOPE_DETAIL(Index, "IndexDiskSymbols", file.generic_string());
		std::optional<trace::Scope> stageTrace;
		const auto stage = [&](const char* name)
		{
			stageTrace.reset();
			stageTrace.emplace(trace::Category::Index, name);
			if (reportStage)
			{
				reportStage(name);
			}
		};
		const std::string key = FileKey(file);
		try
		{
			stage("Reading");
			// A file that was just written can stay locked for a moment (editor save, virus
			// scan); give an existing one a few tries before dropping it from the index.
			constexpr int kReadAttempts = 4;
			constexpr std::chrono::milliseconds kReadRetryDelay {25};
			auto contents = ReadWorkspaceFile(file);
			std::error_code exists_error;
			for (int attempt = 1; !contents && attempt < kReadAttempts &&
				std::filesystem::exists(file, exists_error) && !stop.stop_requested(); ++attempt)
			{
				std::this_thread::sleep_for(kReadRetryDelay);
				contents = ReadWorkspaceFile(file);
			}

			if (!contents)
			{
				mSymbolIndex.RemoveDisk(key);
				mIndexStore.RemoveDisk(file.generic_string());
				return;
			}

			auto text             = std::make_shared<const std::string>(std::move(*contents));
			const std::string uri = UriFromPath(file);
			stage("Resolving macros and includes");
			const auto options = ParserOptionsForText(uri, text, 0, CommandFor(uri), false, stop);
			stage("Parsing");
			const auto tree = heimdall::ParseTree::ParseSnapshot(
				text, options, stop);
			if (tree.Cancelled())
			{
				return;
			}

			LineIndex lines;
			stage("Extracting symbols");
			lines.Build(*text);
			if (mWorkspaceSymbols.load(std::memory_order_relaxed))
			{
				mSymbolIndex.SetDisk(
					key, BuildIndexedFile(uri, heimdall::SymbolOutline::Extract(tree), lines));
			}

			stage("Building index shard");
			mIndexStore.SetDisk(file.generic_string(),
				IndexShard(tree, file, CommandFor(uri)));
		}
		catch (...)
		{
			// Out of memory or an unreadable tree: keep whatever was indexed before.
			return;
		}
	}

	void LanguageServer::SymbolIndexMain(std::stop_token stop)
	{
		std::filesystem::path root;
		{
			const std::lock_guard<std::mutex> lock(m_init_mu);
			root = mWorkspaceRoot;
		}
		if (root.empty())
		{
			return;
		}

		HEIMDALL_TRACE_THREAD("index-main");
		std::optional<heimdall::trace::Scope> indexTrace;
		if (heimdall::trace::ActiveSink())
		{
			indexTrace.emplace(heimdall::trace::Category::Index, "SymbolIndexMain");
		}

		const auto started = std::chrono::steady_clock::now();
		const auto elapsed_ms = [&started]
		{
			return static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - started).count());
		};
		auto files = [&]
		{
			HEIMDALL_TRACE_SCOPE(Index, "DiscoverWorkspaceSources");
			return DiscoverWorkspaceSources(root, stop);
		}
		();
		HEIMDALL_TRACE_COUNTER(Index, "workspace files", static_cast<std::int64_t>(files.size()));
		const auto projectEnd = std::stable_partition(files.begin(), files.end(),[&](const auto& file)
			{
				return !IsDependencySource(root, file);
			});
		const auto projectFiles = static_cast<std::size_t>(projectEnd - files.begin());
		// Reading files dominates, so a few helpers overlap the I/O; the pool that serves
		// requests is left alone, and shutdown never waits for this scan.
		constexpr std::size_t kMaxIndexThreads  = 4;
		constexpr unsigned kCoresPerIndexThread = 4;
		std::size_t threads = std::clamp<std::size_t>(
			std::thread::hardware_concurrency() / kCoresPerIndexThread, 1, kMaxIndexThreads);
		if (const auto value = std::getenv("HEIMDALL_INDEX_THREADS"))
		{
			std::size_t requested = 0;
			const std::string_view text(value);
			const auto parsed = std::from_chars(text.data(), text.data() + text.size(), requested);
			if (parsed.ec == std::errc {} && parsed.ptr == text.data() + text.size() && requested > 0)
			{
				threads = std::min(requested, kMaxIndexThreads);
			}
		}

		std::atomic<std::size_t> next {0};
		std::atomic<std::size_t> done {0};
		std::atomic<std::size_t> projectDone {0};
		std::mutex report_mutex;
		std::vector<std::string> activities(threads);
		const bool show_progress = mWorkspaceSymbols.load(std::memory_order_relaxed) && mWorkDoneProgress.load(std::memory_order_relaxed) &&
			!files.empty();
		const bool show_status = mWorkspaceSymbols.load(std::memory_order_relaxed) && mIndexStatus.load(std::memory_order_relaxed);
		if (show_status)
		{
			Send(IndexStatusNotification("indexing", 0, files.size(), elapsed_ms()));
		}

		const std::string token = "heimdall/symbol-index";
		if (show_progress)
		{
			Send(WorkDoneProgressCreate(token, token));
			Send(WorkDoneProgressBegin(token, "Heimdall: indexing symbols",
				"0/" + std::to_string(files.size()) + " files"));
		}

		// File milestones supplement the periodic reports for long-running files.
		constexpr std::size_t kPercent = 100;
		const std::size_t report_every = std::max<std::size_t>(1, files.size() / kPercent);
		const auto report = [&]
		{
			const std::lock_guard<std::mutex> lock(report_mutex);
			std::string activity;
			for (const auto& current : activities)
			{
				if (!current.empty())
				{
					if (!activity.empty())
					{
						activity += " | ";
					}

					activity += current;
				}
			}

			const auto finished = done.load(std::memory_order_relaxed);
			if (show_status)
			{
				Send(IndexStatusNotification("indexing", finished, files.size(), elapsed_ms(), activity));
			}

			if (show_progress)
			{
				Send(WorkDoneProgressReport(token,
					std::to_string(finished) + "/" + std::to_string(files.size()) +
					" files (" + FormatDuration(elapsed_ms()) + ") — " + activity,
					static_cast<unsigned>(finished * kPercent / files.size())));
			}
		};
		std::jthread reporter;
		if (show_status || show_progress)
		{
			reporter = std::jthread([&](std::stop_token reportingStop)
				{
					constexpr std::chrono::milliseconds kReportInterval {1000};
					std::mutex timerMutex;
					std::condition_variable_any timer;
					std::unique_lock lock(timerMutex);
					while (!reportingStop.stop_requested() && !stop.stop_requested())
					{
						timer.wait_for(lock, reportingStop, kReportInterval,[]
						{
							return false;
						});
						if (!reportingStop.stop_requested())
						{
							report();
						}
					}
				});
		}

		const auto drain = [&](std::size_t slot)
		{
			HEIMDALL_TRACE_THREAD(slot == 0 ? "index-main" : "index-helper");
			// Relaxed is enough: the counter only hands out distinct file indexes, and each
			// file's result is published through WorkspaceSymbolIndex's own lock.
			for (std::size_t i = next.fetch_add(1, std::memory_order_relaxed);
				i < files.size() && !stop.stop_requested();
				i = next.fetch_add(1, std::memory_order_relaxed))
			{
				const auto relative = files[i].lexically_relative(root).generic_string();
				IndexDiskSymbols(files[i], stop,[&](std::string_view stage)
					{
						const std::lock_guard<std::mutex> lock(report_mutex);
						activities[slot] = std::string(stage) + ": " + relative;
					});
				{
					const std::lock_guard<std::mutex> lock(report_mutex);
					activities[slot].clear();
				}
				const std::size_t finished = done.fetch_add(1, std::memory_order_relaxed) + 1;
				HEIMDALL_TRACE_COUNTER(Index, "indexed files", static_cast<std::int64_t>(finished));
				if (i < projectFiles && projectDone.fetch_add(1, std::memory_order_relaxed) + 1 == projectFiles &&
					!stop.stop_requested() && mWorkspaceSymbols.load(std::memory_order_relaxed))
				{
					HEIMDALL_TRACE_INSTANT(Index, "project files processed");
					std::string message = "{\"jsonrpc\":\"2.0\",\"method\":\"window/logMessage\",\"params\":{\"type\":3,\"message\":";
					QuoteJson("Heimdall: project files processed: " + std::to_string(projectFiles) +
						" files in " + FormatDuration(elapsed_ms()), message);
					Send(message + "}}");
				}

				if ((show_status || show_progress) && finished % report_every == 0 &&
					finished < files.size())
				{
					report();
				}
			}
		};
		{
			std::vector<std::jthread> helpers;
			for (std::size_t i = 1; i < threads; ++i)
			{
				helpers.emplace_back([&, i]
					{
						drain(i);
					});
			}

			drain(0);
		}
		if (!stop.stop_requested())
		{
			{
				const std::lock_guard lock(report_mutex);
				activities[0] = "Checking index completeness";
			}
			report();
			mIndexScanFinished.store(true, std::memory_order_release);
			HEIMDALL_TRACE_INSTANT(Index, "index scan finished");
			RefreshIndexCompleteness(stop);
		}

		if (reporter.joinable())
		{
			reporter.request_stop();
			reporter.join();
		}

		if (!stop.stop_requested())
		{
			indexTrace.reset();
			const std::lock_guard profilingLock(mProfilingMutex);
			if (mProfilingSession)
			{
				if (!mProfilingSession->Stop())
				{
					std::osyncstream(std::cerr) << "heimdall: failed to write Perfetto trace\n";
				}

				mProfilingSession.reset();
			}
		}

		if (show_status)
		{
			Send(IndexStatusNotification(stop.stop_requested() ? "cancelled" : "ready",
				done.load(std::memory_order_relaxed), files.size(), elapsed_ms()));
		}

		if (show_progress)
		{
			Send(WorkDoneProgressEnd(token, std::to_string(done.load(std::memory_order_relaxed)) +
				(stop.stop_requested() ? " files processed before cancellation in " : " files indexed in ") +
				FormatDuration(elapsed_ms())));
		}

		if (stop.stop_requested())
		{
			return;
		}

		std::string notification = "{\"jsonrpc\":\"2.0\",\"method\":\"window/"
			"logMessage\",\"params\":{\"type\":3,\"message\":";
		QuoteJson("Heimdall: symbol index ready: " + std::to_string(files.size()) + " files in " +
			FormatDuration(elapsed_ms()),
			notification);
		notification += "}}";
		if (mWorkspaceSymbols.load(std::memory_order_relaxed))
		{
			Send(notification);
		}
	}

	void LanguageServer::RefreshIndexCompleteness(std::stop_token stop)
	{
		HEIMDALL_TRACE_SCOPE(Index, "RefreshIndexCompleteness");
		std::filesystem::path root;
		{
			const std::lock_guard<std::mutex> lock(m_init_mu);
			root = mWorkspaceRoot;
		}
		const auto discovery = DiscoverWorkspaceSourcesDetailed(root, stop);
		const auto snapshot  = mIndexStore.Snapshot();
		bool complete        = discovery.complete && mIndexScanFinished.load(std::memory_order_acquire);
		std::string issue    = discovery.complete ? std::string {}: "Workspace discovery was cancelled, truncated or failed";
		for (const auto& path : discovery.files)
		{
			const auto relative = path.lexically_relative(root);
			if (std::ranges::any_of(relative.parent_path(),[](const std::filesystem::path& part)
				{
					return IsSkippedDirectory(part.string());
				}))
			{
				continue;
			}

			const auto shard    = snapshot->ShardFor(path.generic_string());
			const auto contents = ReadWorkspaceFile(path);
			if (stop.stop_requested() || shard == heimdall::index::kInvalid ||!contents ||
				snapshot->Shards()[shard]->ContentHash() != heimdall::index::HashBytes(*contents))
			{
				const auto open = GetDocument(UriFromPath(path));
				if (!stop.stop_requested() && shard != heimdall::index::kInvalid && open &&
					snapshot->Shards()[shard]->ContentHash() == heimdall::index::HashBytes(*open->text))
				{
					continue;
				}

				complete = false;
				issue    = "File was skipped or changed before indexing: " + path.generic_string();
				break;
			}
		}

		const std::lock_guard<std::mutex> lock(mSymbolMutex);
		complete &= mIndexPendingJobs.load(std::memory_order_acquire) == 0 &&
			mIndexStore.Snapshot()->Revision() == snapshot->Revision();
		mIndexIssue = std::move(issue);
		mIndexComplete.store(complete, std::memory_order_release);
	}

	void LanguageServer::WatchedFilesChanged(simdjson::dom::element request)
	{
		simdjson::dom::object params;
		simdjson::dom::array changes;
		if (!GetObject(request, "params", params) || params["changes"].get_array().get(changes))
		{
			return;
		}

		std::filesystem::path root;
		{
			const std::lock_guard<std::mutex> lock(m_init_mu);
			root = mWorkspaceRoot;
		}
		constexpr std::int64_t kDeleted = 3;
		for (const auto change : changes)
		{
			simdjson::dom::object entry;
			std::string_view uri;
			std::int64_t type = 0;
			if (change.get_object().get(entry) ||!GetString(entry, "uri", uri) ||
				entry["type"].get_int64().get(type))
			{
				continue;
			}

			const std::filesystem::path path = PathFromUri(uri);
			if (!IsUnderRoot(path, root))
			{
				continue;
			}

			if (type == kDeleted)
			{
				// A removed folder is reported once, not once per file inside it.
				{
					const std::lock_guard<std::mutex> lock(mSymbolMutex);
					mIndexComplete.store(false, std::memory_order_release);
					mSymbolIndex.RemoveDiskUnder(FileKey(path));
					mIndexStore.RemoveDiskUnder(path.generic_string());
				}
				RefreshIndexCompleteness(std::stop_token {});
			}
			else if (IsWorkspaceSource(path))
			{
				{
					const std::lock_guard<std::mutex> lock(mSymbolMutex);
					mIndexPendingJobs.fetch_add(1, std::memory_order_acq_rel);
					mIndexComplete.store(false, std::memory_order_release);
				}
				mPool.Submit(ThreadPool::Task([this, path]()
					{
						IndexDiskSymbols(path, std::stop_token {});
						mIndexPendingJobs.fetch_sub(1, std::memory_order_acq_rel);
						RefreshIndexCompleteness(std::stop_token {});
					}),
					ThreadPool::Priority::Background);
			}
		}
	}

} // namespace heimdall::lsp
