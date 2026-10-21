#include <Heimdall/ModuleUnit.hpp>

#include <cstddef>
#include <string_view>
#include <utility>

namespace heimdall
{

	namespace
	{

		constexpr std::string_view kExportKeyword = "export";
		constexpr std::string_view kPrivateWord   = "private";

		struct NameSpan
		{
			std::string text;
			std::uint32_t offset = 0;
			std::uint32_t length = 0;
			bool valid           = false;
		};

		// The tokens of one declaration with whitespace and comments removed.
		class Statement
		{
		public:
			Statement(const ParseTree& tree, std::size_t node) : mTree(tree)
			{
				const auto& soa    = tree.NodesSoA();
				const auto& tokens = tree.Tokens();
				const auto first   = soa.FirstToken(node);
				const auto last    = first + soa.TokenCount(node);
				for (auto index = first; index < last && index < tokens.size(); ++index)
				{
					const auto kind = tokens[index].kind;
					if (kind != TokenKind::Whitespace && kind != TokenKind::LineComment &&
						kind != TokenKind::BlockComment)
					{
						mTokens.push_back(index);
					}
				}
			}

			[[nodiscard]] std::size_t Size() const noexcept
			{
				return mTokens.size();
			}

			[[nodiscard]] const Token& At(std::size_t position) const
			{
				return mTree.Tokens()[mTokens[position]];
			}

			[[nodiscard]] std::string_view Text(std::size_t position) const
			{
				return position < mTokens.size() ? mTree.Text(At(position)) : std::string_view {};
			}

			[[nodiscard]] bool Is(std::size_t position, std::string_view text) const
			{
				return position < mTokens.size() && At(position).kind != TokenKind::StringLiteral &&
					Text(position) == text;
			}

			// `a.b.c` starting at `position`; `position` is advanced past the name.
			[[nodiscard]] NameSpan ReadName(std::size_t& position) const
			{
				NameSpan name;
				bool expectIdentifier = true;
				while (position < mTokens.size())
				{
					const auto& token = At(position);
					if (expectIdentifier && token.kind == TokenKind::Identifier)
					{
						if (name.text.empty())
						{
							name.offset = token.offset;
						}

						name.text.append(Text(position));
						name.length      = token.offset + token.length - name.offset;
						expectIdentifier = false;
					}
					else if (!expectIdentifier && Is(position, "."))
					{
						name.text.push_back('.');
						expectIdentifier = true;
					}
					else
					{
						break;
					}

					++position;
				}

				name.valid = !name.text.empty() && !expectIdentifier;
				return name;
			}

		private:
			const ParseTree& mTree;
			std::vector<std::uint32_t> mTokens;
		};

		void AddProblem(
			ModuleUnitInfo& info,
			std::uint32_t offset,
			std::uint32_t length,
			std::string message)
		{
			info.problems.push_back({offset, length, std::move(message)});
		}

		struct Fragment
		{
			std::size_t node = 0;
			bool present     = false;
		};

		class Extractor
		{
		public:
			explicit Extractor(const ParseTree& tree) : mTree(tree), mSoa(tree.NodesSoA()) {}

			[[nodiscard]] ModuleUnitInfo Run()
			{
				for (std::size_t node = 1; node < mSoa.size(); ++node)
				{
					switch (static_cast<GrammarKind>(mSoa.kind[node]))
					{
					case GrammarKind::ModuleDeclaration:
						ReadModuleDeclaration(node);
						break;
					case GrammarKind::ImportDeclaration:
						ReadImport(node);
						break;
					default:
						break;
					}
				}

				Validate();
				return std::move(mInfo);
			}

		private:
			[[nodiscard]] std::uint32_t TokenOffset(std::size_t node) const
			{
				const auto first = mSoa.FirstToken(node);
				return first < mTree.Tokens().size() ? mTree.Tokens()[first].offset : 0;
			}

			[[nodiscard]] std::uint32_t NodeLength(std::size_t node) const
			{
				const auto count = mSoa.TokenCount(node);
				if (count == 0)
				{
					return 0;
				}

				const auto& last = mTree.Tokens()[mSoa.FirstToken(node) + count - 1];
				return last.offset + last.length - TokenOffset(node);
			}

			void ReadModuleDeclaration(std::size_t node)
			{
				const Statement statement(mTree, node);
				std::size_t position = 0;
				const bool exported  = statement.Is(position, kExportKeyword);
				if (exported)
				{
					++position;
				}

				++position; // `module`
				if (statement.Is(position, ";"))
				{
					if (mGlobal.present)
					{
						AddProblem(mInfo, TokenOffset(node), NodeLength(node),
							"duplicate global module fragment");
					}
					else
					{
						mGlobal                 = {node, true};
						mInfo.hasGlobalFragment = true;
					}

					return;
				}

				if (statement.Is(position, ":") && statement.Is(position + 1, kPrivateWord))
				{
					if (mPrivate.present)
					{
						AddProblem(mInfo, TokenOffset(node), NodeLength(node),
							"duplicate private module fragment");
					}
					else
					{
						mPrivate                 = {node, true};
						mInfo.hasPrivateFragment = true;
					}

					return;
				}

				if (mNamed.present)
				{
					AddProblem(mInfo, TokenOffset(node), NodeLength(node),
						"a translation unit may contain only one module declaration");
					return;
				}

				mNamed    = {node, true};
				auto name = statement.ReadName(position);
				if (!name.valid)
				{
					AddProblem(mInfo, TokenOffset(node), NodeLength(node), "expected a module name");
					return;
				}

				mInfo.name       = std::move(name.text);
				mInfo.nameOffset = name.offset;
				mInfo.nameLength = name.length;
				bool partition   = false;
				if (statement.Is(position, ":"))
				{
					++position;
					auto part = statement.ReadName(position);
					if (!part.valid)
					{
						AddProblem(mInfo, TokenOffset(node), NodeLength(node),
							"expected a module partition name after ':'");
						return;
					}

					partition        = true;
					mInfo.partition  = std::move(part.text);
					mInfo.nameLength = part.offset + part.length - mInfo.nameOffset;
				}

				if (partition)
				{
					mInfo.kind = exported ? ModuleUnitKind::PartitionInterface
					: ModuleUnitKind::PartitionImplementation;
				}
				else
				{
					mInfo.kind = exported ? ModuleUnitKind::Interface : ModuleUnitKind::Implementation;
				}
			}

			void ReadImport(std::size_t node)
			{
				const Statement statement(mTree, node);
				std::size_t position = 0;
				ModuleImport import;
				import.exported = statement.Is(position, kExportKeyword);
				if (import.exported)
				{
					++position;
				}
				else
				{
					import.exported = IsInsideExportBlock(node);
				}

				++position; // `import`
				if (statement.Is(position, "<"))
				{
					const auto begin  = statement.At(position).offset;
					std::size_t close = position + 1;
					while (close < statement.Size() && !statement.Is(close, ">"))
					{
						++close;
					}

					if (close >= statement.Size())
					{
						AddProblem(mInfo, begin, 1, "expected '>' to close the header name");
						return;
					}

					const auto end = statement.At(close).offset + 1;
					import.header  = std::string(mTree.Source().substr(begin, end - begin));
					import.offset  = begin;
					import.length  = end - begin;
				}
				else if (position < statement.Size() &&
					statement.At(position).kind == TokenKind::StringLiteral)
				{
					const auto& token = statement.At(position);
					import.header     = std::string(mTree.Text(token));
					import.offset     = token.offset;
					import.length     = token.length;
				}
				else
				{
					if (statement.Is(position, ":"))
					{
						const auto colon = statement.At(position).offset;
						++position;
						auto part = statement.ReadName(position);
						if (!part.valid)
						{
							AddProblem(mInfo, TokenOffset(node), NodeLength(node),
								"expected a partition name after ':'");
							return;
						}

						import.partition = std::move(part.text);
						import.offset    = colon;
						import.length    = part.offset + part.length - colon;
					}
					else
					{
						auto name = statement.ReadName(position);
						if (!name.valid)
						{
							AddProblem(mInfo, TokenOffset(node), NodeLength(node),
								"expected a module name");
							return;
						}

						import.module = std::move(name.text);
						import.offset = name.offset;
						import.length = name.length;
					}
				}

				mImportNodes.push_back(node);
				mInfo.imports.push_back(std::move(import));
			}

			[[nodiscard]] bool IsInsideExportBlock(std::size_t node) const
			{
				for (auto parent = mSoa.Parent(node); parent != 0;
					parent = mSoa.Parent(parent))
				{
					if (static_cast<GrammarKind>(mSoa.kind[parent]) == GrammarKind::ExportDeclaration)
					{
						return true;
					}
				}

				return false;
			}

			void Validate()
			{
				const auto exportDiagnostic = [this](std::size_t node, std::string_view message)
				{
					AddProblem(mInfo, TokenOffset(node), NodeLength(node), std::string(message));
				};

				if (mGlobal.present && !mNamed.present)
				{
					exportDiagnostic(mGlobal.node,
						"a global module fragment must be followed by a module declaration");
				}

				if (mPrivate.present)
				{
					if (!mNamed.present)
					{
						exportDiagnostic(mPrivate.node,
							"a private module fragment requires a module declaration");
					}
					else if (mInfo.kind != ModuleUnitKind::Interface)
					{
						exportDiagnostic(mPrivate.node,
							"a private module fragment is only allowed in a primary module "
							"interface unit");
					}
				}

				if (mNamed.present)
				{
					ValidatePreamble();
				}

				ValidateExports();
			}

			// Only a global module fragment (preprocessing directives) may precede the
			// module declaration.
			void ValidatePreamble()
			{
				for (std::size_t node = 1; node < mNamed.node; node = mSoa.SubtreeEnd(node))
				{
					const auto kind = static_cast<GrammarKind>(mSoa.kind[node]);
					if (kind == GrammarKind::PreprocessorDirective ||
						kind == GrammarKind::MacroInvocation ||
						(mGlobal.present && node == mGlobal.node))
					{
						continue;
					}

					AddProblem(mInfo, TokenOffset(node), NodeLength(node),
						mGlobal.present && node > mGlobal.node
						? "only preprocessing directives may appear in the global module fragment"
						: "a module declaration must be the first declaration of a module unit");
				}
			}

			void ValidateExports()
			{
				for (std::size_t node = 1; node < mSoa.size(); ++node)
				{
					if (static_cast<GrammarKind>(mSoa.kind[node]) != GrammarKind::ExportDeclaration)
					{
						continue;
					}

					if (!mInfo.IsInterface())
					{
						AddProblem(mInfo, TokenOffset(node), 6,
							"an export declaration is only allowed in a module interface unit");
					}
					else if (mPrivate.present && node > mPrivate.node)
					{
						AddProblem(mInfo, TokenOffset(node), 6,
							"an export declaration is not allowed in the private module "
							"fragment");
					}
				}

				for (std::size_t i = 0; i < mInfo.imports.size(); ++i)
				{
					const auto& import = mInfo.imports[i];
					const auto node    = mImportNodes[i];
					if (import.exported && !mInfo.IsInterface() &&
						!IsInsideExportBlock(node))
					{
						AddProblem(mInfo, TokenOffset(node), 6,
							"an export import is only allowed in a module interface unit");
					}

					if (mNamed.present && !import.module.empty() && import.module == mInfo.name &&
						mInfo.partition.empty())
					{
						AddProblem(mInfo, import.offset, import.length,
							"a module cannot import itself");
					}
				}
			}

			const ParseTree& mTree;
			const GrammarNodeSoA& mSoa;
			ModuleUnitInfo mInfo;
			Fragment mGlobal;
			Fragment mPrivate;
			Fragment mNamed;
			std::vector<std::size_t> mImportNodes;
		};

	} // namespace

	ModuleUnitInfo ExtractModuleUnit(const ParseTree& tree)
	{
		return Extractor(tree).Run();
	}

} // namespace heimdall
