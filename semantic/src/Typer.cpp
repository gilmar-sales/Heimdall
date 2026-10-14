#include <algorithm>
#include <array>
#include <cctype>
#include <optional>
#include <string>

#include <Heimdall/TypeModel.hpp>

#include "detail/TokenView.hpp"

namespace heimdall
{

	// ---- TypeTable ----------------------------------------------------------
	TypeTable::TypeTable(std::pmr::memory_resource* resource) :
		m_kind(resource), m_arg(resource), m_extent(resource), m_index(resource)
	{
		// Id 0 is Unknown.
		m_kind.push_back(TypeKind::Unknown);
		m_arg.push_back(kNone);
		m_extent.push_back(kNone);
	}

	TypeId TypeTable::Intern(TypeKind kind, std::uint32_t arg, std::uint32_t extent)
	{
		const Key key
		{
			arg, extent, kind
		};
		if (const auto found = m_index.find(key); found != m_index.end())
		{
			return found->second;
		}

		const auto id = static_cast<TypeId>(m_kind.size());
		m_kind.push_back(kind);
		m_arg.push_back(arg);
		m_extent.push_back(extent);
		m_index.emplace(key, id);
		return id;
	}

	TypeId TypeTable::Builtin(BuiltinType type)
	{
		return Intern(TypeKind::Builtin, static_cast<std::uint32_t>(type));
	}

	TypeId TypeTable::Class(SymbolId symbol)
	{
		return symbol == kNone ? Unknown : Intern(TypeKind::Class, symbol);
	}

	TypeId TypeTable::InjectedClass(SymbolId symbol)
	{
		return symbol == kNone ? Unknown : Intern(TypeKind::InjectedClass, symbol);
	}

	TypeId TypeTable::Enum(SymbolId symbol)
	{
		return symbol == kNone ? Unknown : Intern(TypeKind::Enum, symbol);
	}

	TypeId TypeTable::External(NameId name)
	{
		return name == kNone ? Unknown : Intern(TypeKind::External, name);
	}

	// A pointer to an unknown type is still known to be a pointer.
	TypeId TypeTable::Pointer(TypeId pointee)
	{
		return Intern(TypeKind::Pointer, pointee);
	}

	TypeId TypeTable::LRef(TypeId referee)
	{
		const auto inner = Value(referee);
		return inner == Unknown ? Unknown : Intern(TypeKind::LRef, inner);
	}

	TypeId TypeTable::RRef(TypeId referee)
	{
		if (Kind(referee) == TypeKind::LRef)
		{
			return referee;
		}

		const auto inner = Value(referee);
		return inner == Unknown ? Unknown : Intern(TypeKind::RRef, inner);
	}

	TypeId TypeTable::Const(TypeId type)
	{
		switch (Kind(type))
		{
		case TypeKind::Unknown:
		case TypeKind::Const:
		case TypeKind::LRef:
		case TypeKind::RRef:
			return type;
		case TypeKind::Array:
			return Array(Const(Arg(type)), Extent(type));
		default:
			return Intern(TypeKind::Const, type);
		}
	}

	TypeId TypeTable::Array(TypeId element, std::uint32_t extent)
	{
		return Intern(TypeKind::Array, element, extent);
	}

	TypeId TypeTable::Parameter(TemplateParameterId parameter)
	{
		return parameter == kNone ? Unknown : Intern(TypeKind::TemplateParameter, parameter);
	}

	TypeId TypeTable::TemplateInstance(std::uint32_t instance, bool dependent)
	{
		return instance == kNone ? Unknown : Intern(TypeKind::TemplateId, instance, dependent);
	}

	TypeId TypeTable::DependentName(NameId spelling, ScopeId definitionScope)
	{
		return spelling == kNone ? Unknown : Intern(TypeKind::DependentName, spelling, definitionScope);
	}

	TypeId TypeTable::DependentMember(TypeId receiver, NameId name)
	{
		return name == kNone ? Unknown : Intern(TypeKind::DependentMember, receiver, name);
	}

	TypeId TypeTable::DecltypeExpression(std::uint32_t node, bool unparenthesized)
	{
		return node == kNone ? Unknown : Intern(TypeKind::DecltypeExpression, node, unparenthesized);
	}

	bool TypeTable::IsDependent(TypeId type) const noexcept
	{
		for (std::size_t steps = 0; steps < Size(); ++steps)
		{
			switch (Kind(type))
			{
			case TypeKind::TemplateParameter:
			case TypeKind::DependentName:
			case TypeKind::DependentMember:
			case TypeKind::DecltypeExpression:
			case TypeKind::InjectedClass:
				return true;
			case TypeKind::TemplateId:
				return Extent(type) != 0;
			case TypeKind::Pointer:
			case TypeKind::LRef:
			case TypeKind::RRef:
			case TypeKind::Const:
			case TypeKind::Array:
				type = Arg(type);
				break;
			default:
				return false;
			}
		}

		return true;
	}

	TypeId TypeTable::Value(TypeId type) const noexcept
	{
		while (Kind(type) == TypeKind::LRef || Kind(type) == TypeKind::RRef)
		{
			type = Arg(type);
		}

		return type;
	}

	TypeId TypeTable::Strip(TypeId type) const noexcept
	{
		type = Value(type);
		return Kind(type) == TypeKind::Const ? Arg(type) : type;
	}

	TypeId TypeTable::Decay(TypeId type)
	{
		return Kind(type) == TypeKind::Array ? Pointer(Arg(type)) : type;
	}

	bool TypeTable::IsInteger(TypeId type) const noexcept
	{
		if (Kind(type) != TypeKind::Builtin)
		{
			return false;
		}

		const auto arg = Arg(type);
		return arg >= static_cast<std::uint32_t>(BuiltinType::Char) &&
			arg <= static_cast<std::uint32_t>(BuiltinType::SizeT);
	}

	bool TypeTable::IsFloating(TypeId type) const noexcept
	{
		if (Kind(type) != TypeKind::Builtin)
		{
			return false;
		}

		const auto arg = Arg(type);
		return arg >= static_cast<std::uint32_t>(BuiltinType::Float) &&
			arg <= static_cast<std::uint32_t>(BuiltinType::LongDouble);
	}

	bool TypeTable::IsArithmetic(TypeId type) const noexcept
	{
		return IsBool(type) || IsInteger(type) || IsFloating(type);
	}

	bool TypeTable::IsConstQualified(TypeId type) const noexcept
	{
		type = Value(type);
		for (std::size_t steps = 0; steps < 16; ++steps)
		{
			if (Kind(type) == TypeKind::Const)
			{
				return true;
			}

			if (Kind(type) != TypeKind::Array)
			{
				return false;
			}

			type = Arg(type);
		}

		return false;
	}

	// ---- TypeModel ----------------------------------------------------------
	TypeModel::TypeModel(const SemanticModel& model, std::size_t arena_hint) :
		m_arena(std::make_unique<Arena>(arena_hint)), m_model(&model),
		m_externals(m_arena->Resource()), m_types(m_arena->Resource()),
		mTemplateArguments(m_arena->Resource()), mInstantiations(m_arena->Resource()),
		mDependentReferences(m_arena->Resource()), mTemplateCalls(m_arena->Resource()),
		m_symbol_type(m_arena->Resource()), m_node_type(m_arena->Resource()),
		m_member_symbol(m_arena->Resource()), mLambdaNodes(m_arena->Resource()),
		mLambdaReturns(m_arena->Resource()) {}

	std::string TypeModel::Spell(TypeId type) const
	{
		static constexpr std::array<std::string_view, 22> kBuiltin {
			"void", "decltype(nullptr)", "bool", "char",
			"signed char", "unsigned char", "wchar_t", "char8_t",
			"char16_t", "char32_t", "short", "unsigned short",
			"int", "unsigned int", "long", "unsigned long",
			"long long", "unsigned long long", "size_t", "float",
			"double", "long double"
		};
		switch (m_types.Kind(type))
		{
		case TypeKind::TemplateParameter:
		{
			const auto parameter = m_types.Arg(type);
			const auto symbol    = m_model->TemplateParameters().symbol[parameter];
			return symbol == kNone ? "<dependent>" : std::string(m_model->Names().Text(m_model->Symbols().name[symbol]));
		}
		case TypeKind::DependentName:
			return std::string(m_externals.Text(m_types.Arg(type)));
		case TypeKind::DependentMember:
			return Spell(m_types.Arg(type)) + "::" + std::string(m_externals.Text(m_types.Extent(type)));
		case TypeKind::DecltypeExpression:
			return "<dependent decltype>";
		case TypeKind::TemplateId:
		{
			const auto instance = m_types.Arg(type);
			const auto owner    = m_model->Templates().owner[mInstantiations.primary[instance]];
			std::string spelling(m_model->Names().Text(m_model->Symbols().name[owner]));
			spelling += '<';
			for (std::uint32_t i = 0; i < mInstantiations.argumentCount[instance]; ++i)
			{
				if (i != 0)
				{
					spelling += ", ";
				}

				spelling += Spell(mTemplateArguments.type[mInstantiations.firstArgument[instance] + i]);
			}

			return spelling + '>';
		}
		case TypeKind::Builtin:
			return std::string(
				kBuiltin[std::min<std::size_t>(m_types.Arg(type), kBuiltin.size() - 1)]);
		case TypeKind::Class:
		case TypeKind::Enum:
		case TypeKind::InjectedClass:
			return std::string(
				m_model->Names().Text(m_model->Symbols().name[m_types.Arg(type)]));
		case TypeKind::External:
			return std::string(m_externals.Text(m_types.Arg(type)));
		case TypeKind::Pointer:
		{
			// A pointer to an array reads `int(*)[2]`.
			if (m_types.Kind(m_types.Arg(type)) == TypeKind::Array)
			{
				std::string dimensions;
				auto element = m_types.Arg(type);
				for (; m_types.Kind(element) == TypeKind::Array; element = m_types.Arg(element))
				{
					dimensions += "[" +
						(m_types.Extent(element) == kNone
						? std::string()
						: std::to_string(m_types.Extent(element))) +
						"]";
				}

				return Spell(element) + "(*)" + dimensions;
			}

			return Spell(m_types.Arg(type)) + "*";
		}
		case TypeKind::LRef:
			return Spell(m_types.Arg(type)) + "&";
		case TypeKind::RRef:
			return Spell(m_types.Arg(type)) + "&&";
		case TypeKind::Const:
			return m_types.Kind(m_types.Arg(type)) == TypeKind::Pointer
			? Spell(m_types.Arg(type)) + " const"
			: "const " + Spell(m_types.Arg(type));
		case TypeKind::Array:
		{
			// Outermost dimension first: `int[2][3]` is an array of 2 arrays of 3 ints.
			std::string dimensions;
			auto element = type;
			for (; m_types.Kind(element) == TypeKind::Array; element = m_types.Arg(element))
			{
				dimensions += "[" +
					(m_types.Extent(element) == kNone
					? std::string()
					: std::to_string(m_types.Extent(element))) +
					"]";
			}

			return Spell(element) + dimensions;
		}
		default:
			return "<unknown>";
		}
	}

	std::string_view TypeModel::ExternalHead(TypeId type) const
	{
		if (m_types.Kind(type) != TypeKind::External)
		{
			return {};
		}

		const auto text = m_externals.Text(m_types.Arg(type));
		return text.substr(0, text.find('<'));
	}

	// ---- Typer --------------------------------------------------------------
	namespace
	{
		constexpr std::uint32_t kMaxTemplateDepth = 32;
		constexpr std::uint64_t kTemplateKeyPrime = 1099511628211ull;

		bool IsStringHead(std::string_view head)
		{
			return head == "std::string" || head == "std::wstring" || head == "std::basic_string" ||
				head == "std::string_view" || head == "std::wstring_view" ||
				head == "std::basic_string_view" || head == "std::u8string" ||
				head == "std::u16string" || head == "std::u32string";
		}

		// Fixed-width integer aliases from <cstdint>/<stdint.h>, resolved exactly
		// like `std::size_t` below. Widths follow LP64 (the primary toolchain): only
		// the spelling differs elsewhere, and every entry keeps its signedness so
		// conversions and diagnostics stay correct.
		std::optional<BuiltinType> FixedWidthInteger(std::string_view text)
		{
			if (text.starts_with("std::"))
			{
				text.remove_prefix(std::string_view("std::").size());
			}

			if (text == "int8_t" ||
				text == "int_least8_t" ||
				text == "int_fast8_t")
			{
				return BuiltinType::SChar;
			}

			if (text == "uint8_t" ||
				text == "uint_least8_t" ||
				text == "uint_fast8_t")
			{
				return BuiltinType::UChar;
			}

			if (text == "int16_t" ||
				text == "int_least16_t")
			{
				return BuiltinType::Short;
			}

			if (text == "uint16_t" ||
				text == "uint_least16_t")
			{
				return BuiltinType::UShort;
			}

			if (text == "int32_t" ||
				text == "int_least32_t")
			{
				return BuiltinType::Int;
			}

			if (text == "uint32_t" ||
				text == "uint_least32_t")
			{
				return BuiltinType::UInt;
			}

			if (text == "int64_t" ||
				text == "int_least64_t" ||
				text == "int_fast64_t" ||
				text == "intmax_t")
			{
				return BuiltinType::LongLong;
			}

			if (text == "uint64_t" ||
				text == "uint_least64_t" ||
				text == "uint_fast64_t" ||
				text == "uintmax_t")
			{
				return BuiltinType::ULongLong;
			}

			if (text == "int_fast16_t" ||
				text == "int_fast32_t" ||
				text == "intptr_t" ||
				text == "ptrdiff_t" ||
				text == "ssize_t")
			{
				return BuiltinType::Long;
			}

			if (text == "uint_fast16_t" ||
				text == "uint_fast32_t" ||
				text == "uintptr_t")
			{
				return BuiltinType::ULong;
			}

			return std::nullopt;
		}

	} // namespace

	bool IsStdContainerHead(std::string_view head)
	{
		static constexpr std::array<std::string_view, 15> kHeads {
			"std::vector",
			"std::deque",
			"std::list",
			"std::array",
			"std::span",
			"std::map",
			"std::set",
			"std::multimap",
			"std::multiset",
			"std::unordered_map",
			"std::unordered_set",
			"std::unordered_multimap",
			"std::unordered_multiset",
			"std::forward_list",
			"std::flat_map"
		};
		return IsStringHead(head) || std::find(kHeads.begin(), kHeads.end(), head) != kHeads.end();
	}

	bool IsStdIndexableHead(std::string_view head)
	{
		return head == "std::vector" || head == "std::deque" || head == "std::array" ||
			head == "std::span" || IsStringHead(head);
	}

	namespace
	{

		constexpr TypeId kPending       = ~0u;
		constexpr std::size_t kMaxDepth = 192;

		bool IsWordChar(char c)
		{
			return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
		}

		void Append(std::string& out, std::string_view piece)
		{
			if (!out.empty() && !piece.empty() && IsWordChar(out.back()) &&
				IsWordChar(piece.front()))
			{
				out += ' ';
			}

			out += piece;
		}

		bool IsBuiltinWord(Tok tok)
		{
			switch (tok)
			{
			case Tok::KwSigned:
			case Tok::KwUnsigned:
			case Tok::KwShort:
			case Tok::KwLong:
			case Tok::KwInt:
			case Tok::KwChar:
			case Tok::KwBool:
			case Tok::KwFloat:
			case Tok::KwDouble:
			case Tok::KwVoid:
			case Tok::KwWchar:
			case Tok::KwChar8:
			case Tok::KwChar16:
			case Tok::KwChar32:
				return true;
			default:
				return false;
			}
		}

		// Specifiers that do not change the type of the declared entity.
		bool IsIgnoredSpecifier(Tok tok)
		{
			switch (tok)
			{
			case Tok::KwStatic:
			case Tok::KwExtern:
			case Tok::KwConstexpr:
			case Tok::KwConstinit:
			case Tok::KwConsteval:
			case Tok::KwInline:
			case Tok::KwThreadLocal:
			case Tok::KwRegister:
			case Tok::KwMutable:
			case Tok::KwVirtual:
			case Tok::KwExplicit:
			case Tok::KwFriend:
			case Tok::KwVolatile:
			case Tok::KwTypedef:
				return true;
			default:
				return false;
			}
		}

		bool IsAssignment(Tok tok)
		{
			switch (tok)
			{
			case Tok::Eq:
			case Tok::PlusEq:
			case Tok::MinusEq:
			case Tok::StarEq:
			case Tok::SlashEq:
			case Tok::PercentEq:
			case Tok::AmpEq:
			case Tok::PipeEq:
			case Tok::CaretEq:
			case Tok::ShlEq:
			case Tok::ShrEq:
				return true;
			default:
				return false;
			}
		}

		int DigitValue(char c)
		{
			if (c >= '0' && c <= '9')
			{
				return c - '0';
			}

			if (c >= 'a' && c <= 'f')
			{
				return c - 'a' + 10;
			}

			if (c >= 'A' && c <= 'F')
			{
				return c - 'A' + 10;
			}

			return -1;
		}

		// Rank for the usual arithmetic conversions.
		int Rank(BuiltinType type)
		{
			switch (type)
			{
			case BuiltinType::Int:
			case BuiltinType::UInt:
				return 1;
			case BuiltinType::Long:
			case BuiltinType::ULong:
				return 2;
			default:
				return 3;
			}
		}

		bool IsUnsignedInt(BuiltinType type)
		{
			return type == BuiltinType::UInt || type == BuiltinType::ULong ||
				type == BuiltinType::ULongLong;
		}

		std::optional<BuiltinType> Promote(BuiltinType type)
		{
			switch (type)
			{
			case BuiltinType::Bool:
			case BuiltinType::Char:
			case BuiltinType::SChar:
			case BuiltinType::UChar:
			case BuiltinType::Short:
			case BuiltinType::UShort:
				return BuiltinType::Int;
			case BuiltinType::WChar:
			case BuiltinType::Char8:
			case BuiltinType::Char16:
			case BuiltinType::Char32:
				return std::nullopt;
			default:
				return type;
			}
		}

	} // namespace

	class TyperImpl
	{
	public:
		TyperImpl(TypeModel& out, const SemanticModel& model) :
		m_out(out), m_model(model), m_tree(model.Tree()), m_types(out.m_types),
			m_sig(model.Significant()), m_symbols(model.Symbols()), m_nodes(model.Tree().NodesSoA()),
			mInstanceCache(out.m_arena->Resource()) {}

		void Run();

	private:
		// Value category of an initializer, as far as the engine can tell: it
		// decides what `auto&&` deduces.
		enum class Category : std::uint8_t
		{
			Unknown,
			LValue,
			PRValue
		};

		struct Base
		{
			TypeId type      = TypeTable::Unknown;
			std::size_t next = 0;
			bool ok          = false;
			bool is_auto     = false;
			bool is_const    = false;
		}

		;

		// ---- token helpers ---------------------------------------------------
		const std::vector<Token>& Tokens() const
		{
			return m_tree.Tokens();
		}

		std::string_view Text(std::uint32_t token) const
		{
			return m_tree.Text(Tokens()[token]);
		}

		std::size_t Pos(std::uint32_t token) const
		{
			return static_cast<std::size_t>(
				std::lower_bound(m_sig.begin(), m_sig.end(), token) - m_sig.begin());
		}

		Tok At(std::size_t pos) const
		{
			return pos < m_sig.size() ? Tokens()[m_sig[pos]].tok : Tok::None;
		}

		bool WordAt(std::size_t pos) const
		{
			return pos < m_sig.size() && Tokens()[m_sig[pos]].kind == TokenKind::Identifier &&
				Tokens()[m_sig[pos]].tok == Tok::None;
		}

		std::string_view TextAt(std::size_t pos) const
		{
			return pos < m_sig.size() ? Text(m_sig[pos]) : std::string_view {};
		}

		std::pair<std::size_t, std::size_t> NodeSig(std::uint32_t node) const
		{
			const auto n     = m_nodes[node];
			const auto begin = Pos(n.GetFirstToken());
			return {begin, std::max(begin, Pos(n.GetFirstToken() + n.GetTokenCount()))};
		}

		std::size_t MatchParen(std::size_t open, std::size_t limit) const;

		std::size_t MatchAngle(std::size_t open, std::size_t limit) const;

		std::uint32_t FindChild(std::uint32_t node, GrammarKind kind) const
		{
			for (const auto child : m_model.ChildrenOf(node))
			{
				if (m_nodes.Kind(child) == kind)
				{
					return child;
				}
			}

			return kNone;
		}

		bool SameStart(std::uint32_t a, std::uint32_t b) const
		{
			return Pos(m_nodes.FirstToken(a)) == Pos(m_nodes.FirstToken(b));
		}

		// ---- declared types --------------------------------------------------
		TypeId SymbolType(SymbolId symbol);

		TypeId ComputeSymbolType(SymbolId symbol);

		TypeId VariableType(SymbolId symbol);

		TypeId ParameterType(std::uint32_t node);

		TypeId LambdaReturnType(std::uint32_t node);

		TypeId ReturnType(SymbolId symbol);

		TypeId AliasTarget(SymbolId symbol);

		TypeId OverloadReturn(SymbolId symbol);

		Base ParseBase(std::size_t begin, std::size_t end, ScopeId scope, std::uint32_t before);

		TypeId ParseName(std::size_t& pos, std::size_t end, ScopeId scope, std::uint32_t before);

		std::vector<TypeId> ParseTemplateArguments(
			std::size_t begin,
			std::size_t end,
			ScopeId scope,
			std::uint32_t before,
			std::vector<std::pair<std::uint32_t, std::uint32_t>>* syntax = nullptr);

		TypeId Instantiate(
			TemplateId declaration,
			std::vector<TypeId> arguments,
			std::vector<std::pair<std::uint32_t, std::uint32_t>> syntax = {},
			SubstitutionContext context = SubstitutionContext::Instantiation);

		void BuildDependentReferences();

		std::uint32_t mInstantiationDepth = 0;

		TypeId SubstituteArguments(
			TypeId type,
			TemplateId declaration,
			std::span<const TypeId> arguments);

		SubstitutionResult CheckedSubstitute(
			TypeId type,
			TemplateId declaration,
			std::span<const TypeId> arguments,
			SubstitutionContext context,
			std::uint32_t remainingDepth = kMaxTemplateDepth);

		SubstitutionResult SubstituteMember(
			TypeId receiver,
			NameId name,
			SubstitutionContext context,
			std::uint32_t remainingDepth);

		SubstitutionState DeduceType(
			TypeId pattern,
			TypeId argument,
			TemplateId declaration,
			std::vector<TypeId>& arguments,
			std::size_t explicitCount,
			std::uint32_t remainingDepth = kMaxTemplateDepth);

		SubstitutionState MatchTypePattern(
			TypeId pattern,
			TypeId argument,
			TemplateId declaration,
			std::vector<TypeId>& bindings,
			bool ordering,
			std::uint32_t remainingDepth);

		SubstitutionResult MatchTypePatterns(
			std::span<const TypeId> patterns,
			std::span<const TypeId> arguments,
			TemplateId declaration,
			std::vector<TypeId>& bindings,
			bool ordering = false);

		struct ConstraintOutcome
		{
			SubstitutionState state   = SubstitutionState::Incomplete;
			bool satisfied            = false;
			SubstitutionReason reason = SubstitutionReason::Unsupported;
		};

		ConstraintOutcome EvaluateConstraintTokens(
			std::size_t begin,
			std::size_t end,
			TemplateId declaration,
			std::span<const TypeId> bindings,
			SubstitutionContext context,
			std::uint32_t remainingDepth);

		ConstraintOutcome EvaluateRequiresBody(
			std::size_t brace,
			std::size_t close,
			TemplateId declaration,
			std::span<const TypeId> bindings,
			SubstitutionContext context,
			std::uint32_t remainingDepth);

		ConstraintOutcome EvaluateTypeRequirement(
			std::uint32_t requirement,
			TemplateId declaration,
			std::span<const TypeId> bindings,
			SubstitutionContext context,
			std::uint32_t remainingDepth);

		ConstraintOutcome EvaluateConceptId(
			std::size_t begin,
			std::size_t end,
			TemplateId declaration,
			std::span<const TypeId> bindings,
			SubstitutionContext context,
			std::uint32_t remainingDepth);

		ConstraintOutcome CheckAssociatedConstraints(
			TemplateId declaration,
			std::span<const TypeId> bindings,
			SubstitutionContext context);

		std::vector<std::string> ConstraintAtoms(TemplateId declaration);

		struct SubstitutedExpression
		{
			SubstitutionResult result;
			Category category = Category::Unknown;
		};

		SubstitutedExpression CheckExpression(
			std::uint32_t node,
			TemplateId declaration,
			std::span<const TypeId> arguments,
			SubstitutionContext context,
			std::uint32_t remainingDepth);

		TypeId ResolveTemplateCall(
			std::uint32_t node,
			SymbolId symbol,
			std::span<const TypeId> explicitArguments,
			bool explicitCall);

		TypeId ApplyOperators(TypeId type, std::size_t begin, std::size_t end);

		TypeId ParseTypeTokens(
			std::size_t begin,
			std::size_t end,
			ScopeId scope,
			std::uint32_t before);

		TypeId DeclaratorType(
			Base base,
			std::uint32_t declarator,
			SymbolKind kind,
			TypeId init_type,
			Category category);

		TypeId DeduceAuto(
			const Base& base,
			std::size_t begin,
			std::size_t end,
			TypeId init,
			Category category);

		TypeId DeducedReturn(std::uint32_t function, bool is_const);

		Category ValueCategory(std::uint32_t node);

		// Type and category of a one-token initializer: a literal or a variable.
		std::pair<TypeId, Category> OperandAt(std::size_t pos, ScopeId scope);

		// Type of the initializer of an `auto` variable, with its value category.
		std::pair<TypeId, Category> AutoInitializer(
			std::uint32_t node,
			std::uint32_t init,
			std::size_t name_pos);

		// Allocated type of a `new`-expression as written after the keyword:
		// `new T...` is a prvalue of type `T*`. `new` has no dedicated node,
		// so this scans the tokens instead. Unknown when unresolvable.
		std::pair<TypeId, Category> NewExpressionType(
			std::size_t new_pos,
			std::size_t end,
			ScopeId scope);

		TypeId NumberType(std::string_view text);

		TypeId CharType(std::string_view text);

		// ---- expressions -----------------------------------------------------
		TypeId NodeType(std::uint32_t node);

		TypeId ComputeNodeType(std::uint32_t node);

		TypeId LiteralType(std::uint32_t node);

		TypeId LiteralTypeAt(std::size_t pos);

		TypeId IdentifierType(std::uint32_t node);

		TypeId UnaryType(std::uint32_t node);

		TypeId BinaryType(std::uint32_t node);

		TypeId ConditionalType(std::uint32_t node);

		TypeId CallType(std::uint32_t node);

		TypeId SubscriptType(std::uint32_t node);

		TypeId MemberType(std::uint32_t node, bool as_call);

		TypeId CastType(std::uint32_t callee);

		TypeId ExternalCall(TypeId object, std::string_view member);

		TypeId Arithmetic(TypeId a, TypeId b);

		TypeId Promoted(TypeId type);

		bool IsScalar(TypeId type) const;

		ScopeId EnclosingClass(ScopeId scope) const;

		TypeModel& m_out;
		const SemanticModel& m_model;
		const ParseTree& m_tree;
		TypeTable& m_types;
		const std::pmr::vector<std::uint32_t>& m_sig;
		const SymbolTable& m_symbols;
		const GrammarNodeSoA& m_nodes;
		std::size_t m_depth = 0;
		std::pmr::unordered_multimap<std::uint64_t, std::uint32_t> mInstanceCache;
	};

	// ---- token helpers --------------------------------------------------------
	std::size_t TyperImpl::MatchParen(std::size_t open, std::size_t limit) const
	{
		const Tok opening = At(open);
		const Tok closing = opening == Tok::LParen ? Tok::RParen
		: opening == Tok::LBracket ? Tok::RBracket
		: Tok::RBrace;
		std::size_t depth = 0;
		for (std::size_t i = open; i < limit && i < m_sig.size(); ++i)
		{
			if (At(i) == opening)
			{
				++depth;
			}
			else if (At(i) == closing && --depth == 0)
			{
				return i;
			}
		}

		return limit;
	}

	// Position of the `>` that closes the `<` at `open` in a type context (`>>`
	// closes two levels), or `limit`.
	std::size_t TyperImpl::MatchAngle(std::size_t open, std::size_t limit) const
	{
		int depth = 0;
		for (std::size_t i = open; i < limit && i < m_sig.size(); ++i)
		{
			switch (At(i))
			{
			case Tok::Lt:
				++depth;
				break;
			case Tok::Gt:
				if (--depth == 0)
				{
					return i;
				}

				break;
			case Tok::Shr:
				depth -= 2;
				if (depth <= 0)
				{
					return depth == 0 ? i : limit;
				}

				break;
			case Tok::LParen:
			case Tok::LBracket:
				i = MatchParen(i, limit);
				break;
			case Tok::Semi:
			case Tok::LBrace:
			case Tok::RBrace:
				return limit;
			default:
				break;
			}
		}

		return limit;
	}

	// ---- literals ---------------------------------------------------------------
	TypeId TyperImpl::NumberType(std::string_view text)
	{
		std::string s;
		for (const char c : text)
		{
			if (c != '\'')
			{
				s += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}
		}

		if (s.empty())
		{
			return TypeTable::Unknown;
		}

		const bool hex    = s.size() > 1 && s[0] == '0' && s[1] == 'x';
		const bool binary = s.size() > 1 && s[0] == '0' && s[1] == 'b';
		const bool is_float = hex ? s.find('p') != std::string::npos
		: !binary && s.find_first_of(".e") != std::string::npos;
		if (is_float)
		{
			const char last = s.back();
			if (last == 'f')
			{
				return m_types.Builtin(BuiltinType::Float);
			}

			if (last == 'l')
			{
				return m_types.Builtin(BuiltinType::LongDouble);
			}

			return std::isdigit(static_cast<unsigned char>(last)) != 0 || last == '.'
			? m_types.Builtin(BuiltinType::Double)
			: TypeTable::Unknown;
		}

		std::size_t i = 0;
		int base      = 10;
		if (hex)
		{
			base = 16;
			i    = 2;
		}
		else if (binary)
		{
			base = 2;
			i    = 2;
		}
		else if (s.size() > 1 && s[0] == '0')
		{
			base = 8;
			i    = 1;
		}

		std::uint64_t value = 0;
		bool overflow       = false;
		bool any            = base == 8;
		for (; i < s.size(); ++i)
		{
			const int digit = DigitValue(s[i]);
			if (digit < 0 || digit >= base)
			{
				break;
			}

			any = true;
			if (value >
				(~0ull - static_cast<std::uint64_t>(digit)) / static_cast<std::uint64_t>(base))
			{
				overflow = true;
			}
			else
			{
				value =
					value * static_cast<std::uint64_t>(base) + static_cast<std::uint64_t>(digit);
			}
		}

		if (!any || overflow)
		{
			return TypeTable::Unknown;
		}

		bool has_unsigned = false;
		std::string longs;
		for (; i < s.size(); ++i)
		{
			if (s[i] == 'u' && !has_unsigned)
			{
				has_unsigned = true;
			}
			else if (s[i] == 'l')
			{
				longs += 'l';
			}
			else
			{
				return TypeTable::Unknown; // user-defined literal, `z`, imaginary...
			}
		}

		// `lul` is not a valid suffix: the `l`s must be adjacent.
		const auto suffix = s.substr(s.size() -(longs.size() +(has_unsigned ? 1 : 0)));
		if (longs.size() > 2 ||(longs.size() == 2 && suffix.find("ll") == std::string::npos))
		{
			return TypeTable::Unknown;
		}

		const bool fits_int  = value <= 0x7FFFFFFFull;
		const bool fits_uint = value <= 0xFFFFFFFFull;
		if (longs.empty())
		{
			if (has_unsigned)
			{
				return fits_uint ? m_types.Builtin(BuiltinType::UInt) : TypeTable::Unknown;
			}

			if (fits_int)
			{
				return m_types.Builtin(BuiltinType::Int);
			}

			// Wider than int: long, unsigned or long long depending on the platform.
			return base != 10 && fits_uint ? m_types.Builtin(BuiltinType::UInt)
			: TypeTable::Unknown;
		}

		if (longs.size() == 1)
		{
			return m_types.Builtin(has_unsigned ? BuiltinType::ULong : BuiltinType::Long);
		}

		return m_types.Builtin(has_unsigned ? BuiltinType::ULongLong : BuiltinType::LongLong);
	}

	TypeId TyperImpl::CharType(std::string_view text)
	{
		if (text.empty())
		{
			return TypeTable::Unknown;
		}

		if (text[0] == '\'')
		{
			return m_types.Builtin(BuiltinType::Char);
		}

		if (text[0] == 'L')
		{
			return m_types.Builtin(BuiltinType::WChar);
		}

		if (text[0] == 'U')
		{
			return m_types.Builtin(BuiltinType::Char32);
		}

		if (text[0] == 'u')
		{
			return m_types.Builtin(
				text.size() > 1 && text[1] == '8' ? BuiltinType::Char8 : BuiltinType::Char16);
		}

		return TypeTable::Unknown;
	}

	// ---- declared types ---------------------------------------------------------
	TypeId TyperImpl::SymbolType(SymbolId symbol)
	{
		if (symbol >= m_symbols.Size())
		{
			return TypeTable::Unknown;
		}

		auto& slot = m_out.m_symbol_type[symbol];
		if (slot != kPending)
		{
			return slot;
		}

		if (m_depth >= kMaxDepth)
		{
			return TypeTable::Unknown;
		}

		// A cycle (alias to itself, `auto x = x;`) reads Unknown.
		slot = TypeTable::Unknown;
		++m_depth;
		const auto result = ComputeSymbolType(symbol);
		--m_depth;
		m_out.m_symbol_type[symbol] = result;
		return result;
	}

	TypeId TyperImpl::ComputeSymbolType(SymbolId symbol)
	{
		switch (m_symbols.kind[symbol])
		{
		case SymbolKind::Variable:
		case SymbolKind::Parameter:
			return VariableType(symbol);
		case SymbolKind::Function:
			return ReturnType(symbol);
		case SymbolKind::TypeAlias:
			return AliasTarget(symbol);
		default:
			return TypeTable::Unknown;
		}
	}

	std::vector<TypeId> TyperImpl::ParseTemplateArguments(
		std::size_t begin,
		std::size_t end,
		ScopeId scope,
		std::uint32_t before,
		std::vector<std::pair<std::uint32_t, std::uint32_t>>* syntax)
	{
		std::vector<TypeId> arguments;
		for (auto first = begin; first < end;)
		{
			auto last           = first;
			std::uint32_t depth = 0;
			for (; last < end; ++last)
			{
				if (At(last) == Tok::Lt)
				{
					++depth;
				}
				else if (At(last) == Tok::Gt && depth != 0)
				{
					--depth;
				}
				else if (At(last) == Tok::Shr || At(last) == Tok::Ellipsis || At(last) == Tok::LParen)
				{
					if (syntax != nullptr)
					{
						syntax->assign(1, {m_sig[begin], m_sig[end]});
					}

					return {TypeTable::Unknown};
				}
				else if (At(last) == Tok::Comma && depth == 0)
				{
					break;
				}
			}

			arguments.push_back(ParseTypeTokens(first, last, scope, before));
			if (syntax != nullptr)
			{
				syntax->emplace_back(m_sig[first], m_sig[last]);
			}

			first = last + 1;
		}

		return arguments;
	}

	TypeId TyperImpl::SubstituteArguments(
		TypeId type,
		TemplateId declaration,
		std::span<const TypeId> arguments)
	{
		if (m_types.Kind(type) == TypeKind::TemplateParameter)
		{
			const auto parameter   = m_types.Arg(type);
			const auto& parameters = m_model.TemplateParameters();
			return parameters.owner[parameter] == declaration && parameters.position[parameter] < arguments.size()
			? arguments[parameters.position[parameter]] : type;
		}

		switch (m_types.Kind(type))
		{
		case TypeKind::Pointer:
			return m_types.Pointer(SubstituteArguments(m_types.Arg(type), declaration, arguments));
		case TypeKind::LRef:
			return m_types.LRef(SubstituteArguments(m_types.Arg(type), declaration, arguments));
		case TypeKind::RRef:
			return m_types.RRef(SubstituteArguments(m_types.Arg(type), declaration, arguments));
		case TypeKind::Const:
			return m_types.Const(SubstituteArguments(m_types.Arg(type), declaration, arguments));
		case TypeKind::Array:
			return m_types.Array(SubstituteArguments(m_types.Arg(type), declaration, arguments),
				m_types.Extent(type));
		case TypeKind::TemplateId:
		{
			const auto instance = m_types.Arg(type);
			const auto primary  = m_out.mInstantiations.primary[instance];
			const auto first    = m_out.mInstantiations.firstArgument[instance];
			const auto count    = m_out.mInstantiations.argumentCount[instance];
			std::vector<TypeId> replaced;
			replaced.reserve(count);
			for (std::uint32_t i = 0; i < count; ++i)
			{
				replaced.push_back(SubstituteArguments(m_out.mTemplateArguments.type[first + i], declaration,
					arguments));
			}

			return Instantiate(primary, std::move(replaced));
		}
		default:
			return type;
		}
	}

	SubstitutionResult TyperImpl::CheckedSubstitute(
		TypeId type,
		TemplateId declaration,
		std::span<const TypeId> arguments,
		SubstitutionContext context,
		std::uint32_t remainingDepth)
	{
		if (remainingDepth == 0)
		{
			return {TypeTable::Unknown, SubstitutionState::Incomplete, SubstitutionReason::DepthLimit};
		}

		const auto kind = m_types.Kind(type);
		if (kind == TypeKind::TemplateParameter)
		{
			const auto parameter   = m_types.Arg(type);
			const auto& parameters = m_model.TemplateParameters();
			if (parameter < parameters.owner.size() && parameters.owner[parameter] == declaration &&
				parameters.position[parameter] < arguments.size())
			{
				if (parameters.pack[parameter])
				{
					return {TypeTable::Unknown, SubstitutionState::Incomplete, SubstitutionReason::Unsupported};
				}

				const auto bound = arguments[parameters.position[parameter]];
				if (m_types.Kind(bound) == TypeKind::TemplateId && !m_types.IsDependent(bound))
				{
					return {bound, SubstitutionState::Success, SubstitutionReason::None};
				}
			}
		}

		if (kind == TypeKind::DecltypeExpression)
		{
			const auto unparenthesized = m_types.Extent(type) != 0;
			auto expression = CheckExpression(m_types.Arg(type), declaration, arguments, context,
				remainingDepth - 1);
			if (expression.result.state != SubstitutionState::Success)
			{
				return expression.result;
			}

			if (!unparenthesized)
			{
				if (expression.category == Category::Unknown)
				{
					return {TypeTable::Unknown, SubstitutionState::Incomplete, SubstitutionReason::Unsupported};
				}

				expression.result.type = expression.category == Category::LValue
				? m_types.LRef(expression.result.type) : m_types.Value(expression.result.type);
			}

			return expression.result;
		}

		if (kind == TypeKind::DependentMember)
		{
			const auto name = m_types.Extent(type);
			const auto receiver = CheckedSubstitute(m_types.Arg(type), declaration, arguments, context,
				remainingDepth - 1);
			if (receiver.state != SubstitutionState::Success)
			{
				return receiver.state == SubstitutionState::Dependent
				? SubstitutionResult {type, receiver.state, receiver.reason}: receiver;
			}

			return SubstituteMember(receiver.type, name, context, remainingDepth - 1);
		}

		if (kind == TypeKind::TemplateId)
		{
			const auto instance   = m_types.Arg(type);
			const auto& instances = m_out.mInstantiations;
			if (instance >= instances.primary.size())
			{
				return {};
			}

			const auto primary = instances.primary[instance];
			const auto first   = instances.firstArgument[instance];
			const auto count   = instances.argumentCount[instance];
			std::vector<TypeId> replaced;
			replaced.reserve(count);
			bool dependent = false;
			for (std::uint32_t i = 0; i < count; ++i)
			{
				const auto result = CheckedSubstitute(m_out.mTemplateArguments.type[first + i], declaration,
					arguments, context, remainingDepth - 1);
				if (result.state != SubstitutionState::Success && result.state != SubstitutionState::Dependent)
				{
					return result;
				}

				dependent = dependent || result.state == SubstitutionState::Dependent;
				replaced.push_back(result.type);
			}

			if (dependent)
			{
				return {type, SubstitutionState::Dependent, SubstitutionReason::None};
			}

			const auto owner = m_model.Templates().owner[primary];
			if (owner < m_symbols.Size() && m_symbols.kind[owner] == SymbolKind::TypeAlias)
			{
				return CheckedSubstitute(SymbolType(owner), primary, replaced, context, remainingDepth - 1);
			}

			const auto concrete = Instantiate(primary, std::move(replaced), {}, context);
			if (m_types.Kind(concrete) != TypeKind::TemplateId)
			{
				return {TypeTable::Unknown, SubstitutionState::Incomplete, SubstitutionReason::Unsupported};
			}

			const auto concreteInstance = m_types.Arg(concrete);
			const auto check            = m_out.mInstantiations.checkState[concreteInstance];
			if (check == SubstitutionState::SubstitutionFailure || check == SubstitutionState::ConstraintUnsatisfied)
			{
				return {TypeTable::Unknown,
					context == SubstitutionContext::Immediate ? SubstitutionState::SubstitutionFailure : SubstitutionState::HardError,
					m_out.mInstantiations.reason[concreteInstance]};
			}

			if (check == SubstitutionState::Incomplete || check == SubstitutionState::Dependent)
			{
				return {TypeTable::Unknown, check, SubstitutionReason::Unsupported};
			}

			return {concrete, SubstitutionState::Success, SubstitutionReason::None};
		}

		if (kind == TypeKind::Pointer || kind == TypeKind::LRef || kind == TypeKind::RRef ||
			kind == TypeKind::Const || kind == TypeKind::Array)
		{
			const auto extent = m_types.Extent(type);
			const auto inner = CheckedSubstitute(m_types.Arg(type), declaration, arguments, context,
				remainingDepth - 1);
			if (inner.state != SubstitutionState::Success && inner.state != SubstitutionState::Dependent)
			{
				return inner;
			}

			return m_types.FormSubstitutedType(kind, inner, extent, context);
		}

		std::vector<TypeBinding> bindings;
		if (declaration < m_model.Templates().owner.size())
		{
			const auto first = m_model.Templates().firstParameter[declaration];
			const auto count = m_model.Templates().parameterCount[declaration];
			bindings.reserve(std::min<std::size_t>(count, arguments.size()));
			for (std::uint32_t i = 0; i < count && i < arguments.size(); ++i)
			{
				bindings.push_back({first + i, arguments[i]});
			}
		}

		return m_types.Substitute(type, bindings, context, remainingDepth);
	}

	SubstitutionResult TyperImpl::SubstituteMember(
		TypeId receiver,
		NameId name,
		SubstitutionContext context,
		std::uint32_t remainingDepth)
	{
		const auto invalid =[&](SubstitutionReason reason)
		{
			return SubstitutionResult {TypeTable::Unknown, context == SubstitutionContext::Immediate
				? SubstitutionState::SubstitutionFailure : SubstitutionState::HardError, reason};
		};
		if (remainingDepth == 0)
		{
			return {TypeTable::Unknown, SubstitutionState::Incomplete, SubstitutionReason::DepthLimit};
		}

		if (m_types.Kind(receiver) == TypeKind::Const)
		{
			receiver = m_types.Arg(receiver);
		}

		SymbolId owner         = kNone;
		TemplateId declaration = kNone;
		std::vector<TypeId> arguments;
		if (m_types.Kind(receiver) == TypeKind::Class)
		{
			owner = m_types.Arg(receiver);
		}
		else if (m_types.Kind(receiver) == TypeKind::TemplateId)
		{
			const auto instance   = m_types.Arg(receiver);
			const auto& instances = m_out.mInstantiations;
			if (instance >= instances.state.size())
			{
				return {};
			}

			if (instances.checkState[instance] == SubstitutionState::HardError || instances.state[instance] == TemplateResolution::Ambiguous)
			{
				return {TypeTable::Unknown, SubstitutionState::HardError, instances.reason[instance]};
			}

			if (instances.checkState[instance] == SubstitutionState::ConstraintUnsatisfied)
			{
				return {TypeTable::Unknown,
					context == SubstitutionContext::Immediate ? SubstitutionState::SubstitutionFailure : SubstitutionState::HardError,
					instances.reason[instance]};
			}

			if (instances.state[instance] != TemplateResolution::Resolved)
			{
				return {};
			}

			declaration      = instances.selected[instance];
			owner            = m_model.Templates().owner[declaration];
			const auto first = instances.firstSubstitution[instance];
			arguments.assign(m_out.mTemplateArguments.type.begin() + first,
				m_out.mTemplateArguments.type.begin() + first + instances.substitutionCount[instance]);
		}
		else if (m_types.Kind(receiver) == TypeKind::Builtin || m_types.Kind(receiver) == TypeKind::Pointer ||
			m_types.Kind(receiver) == TypeKind::LRef || m_types.Kind(receiver) == TypeKind::RRef ||
			m_types.Kind(receiver) == TypeKind::Array)
		{
			return invalid(SubstitutionReason::WrongMemberKind);
		}
		else
		{
			return {TypeTable::Unknown, SubstitutionState::Incomplete, SubstitutionReason::Unsupported};
		}

		if (owner >= m_symbols.Size())
		{
			return {};
		}

		const auto record = m_symbols.decl_node[owner];
		if (record >= m_nodes.size() || m_nodes.Kind(record) != GrammarKind::RecordDefinition)
		{
			return {};
		}

		const auto[begin, end] = NodeSig(record);
		bool definition        = false;
		for (auto pos = begin; pos < end; ++pos)
		{
			definition = definition || At(pos) == Tok::LBrace;
		}

		if (!definition)
		{
			return {};
		}

		if (declaration != kNone)
		{
			if (m_symbols.base_count[owner] != 0)
			{
				return {TypeTable::Unknown, SubstitutionState::Incomplete, SubstitutionReason::Unsupported};
			}

			for (const auto child : m_model.ChildrenOf(record))
			{
				if (m_nodes.Kind(child) == GrammarKind::StaticAssertDeclaration)
				{
					return {TypeTable::Unknown, SubstitutionState::Incomplete, SubstitutionReason::Unsupported};
				}
			}

			for (SymbolId member = 0; member < m_symbols.Size(); ++member)
			{
				if (m_symbols.scope[member] != m_symbols.member_scope[owner])
				{
					continue;
				}

				if (m_symbols.kind[member] == SymbolKind::Variable || m_symbols.kind[member] == SymbolKind::TypeAlias)
				{
					const auto effect = CheckedSubstitute(SymbolType(member), declaration, arguments,
						SubstitutionContext::Instantiation, remainingDepth - 1);
					if (effect.state != SubstitutionState::Success)
					{
						return effect;
					}
				}
				else if (m_symbols.kind[member] == SymbolKind::Function)
				{
					return {TypeTable::Unknown, SubstitutionState::Incomplete, SubstitutionReason::Unsupported};
				}
			}
		}

		const auto memberName = m_model.Names().Find(m_out.m_externals.Text(name));
		const auto member     = m_model.LookupLocal(m_symbols.member_scope[owner], memberName);
		if (member == kNone)
		{
			return m_symbols.base_count[owner] == 0 ? invalid(SubstitutionReason::MissingMember)
			: SubstitutionResult
			{
				TypeTable::Unknown, SubstitutionState::Incomplete, SubstitutionReason::Unsupported
			};
		}

		if (m_symbols.access[member] != AccessLevel::Public || m_symbols.next_same_name[member] != kNone)
		{
			return {TypeTable::Unknown, SubstitutionState::Incomplete, SubstitutionReason::AccessDenied};
		}

		if (m_symbols.kind[member] == SymbolKind::Class)
		{
			return {m_types.Class(member), SubstitutionState::Success, SubstitutionReason::None};
		}

		if (m_symbols.kind[member] == SymbolKind::Enum)
		{
			return {m_types.Enum(member), SubstitutionState::Success, SubstitutionReason::None};
		}

		if (m_symbols.kind[member] != SymbolKind::TypeAlias)
		{
			return invalid(SubstitutionReason::WrongMemberKind);
		}

		return CheckedSubstitute(SymbolType(member), declaration, arguments,
			declaration == kNone ? context : SubstitutionContext::Instantiation, remainingDepth - 1);
	}

	SubstitutionState TyperImpl::MatchTypePattern(
		TypeId pattern,
		TypeId argument,
		TemplateId declaration,
		std::vector<TypeId>& bindings,
		bool ordering,
		std::uint32_t remainingDepth)
	{
		if (remainingDepth == 0 || pattern == TypeTable::Unknown || argument == TypeTable::Unknown)
		{
			return SubstitutionState::Incomplete;
		}

		if (!ordering && m_types.IsDependent(argument))
		{
			return SubstitutionState::Dependent;
		}

		const auto kind = m_types.Kind(pattern);
		if (kind == TypeKind::TemplateParameter)
		{
			const auto parameter   = m_types.Arg(pattern);
			const auto& parameters = m_model.TemplateParameters();
			if (parameter >= parameters.owner.size() || parameters.owner[parameter] != declaration ||
				parameters.pack[parameter])
			{
				return SubstitutionState::Incomplete;
			}

			const auto position = parameters.position[parameter];
			if (position >= bindings.size())
			{
				return SubstitutionState::Incomplete;
			}

			if (bindings[position] != TypeTable::Unknown && bindings[position] != argument)
			{
				return SubstitutionState::DeductionFailure;
			}

			bindings[position] = argument;
			return SubstitutionState::Success;
		}

		if (kind == TypeKind::DependentMember || kind == TypeKind::DecltypeExpression)
		{
			return ordering ? SubstitutionState::Incomplete : SubstitutionState::Success;
		}

		if (kind == TypeKind::TemplateId)
		{
			const auto instance = m_types.Arg(pattern);
			const auto primary  = m_out.mInstantiations.primary[instance];
			const auto owner    = m_model.Templates().owner[primary];
			if (owner < m_symbols.Size() && m_symbols.kind[owner] == SymbolKind::TypeAlias)
			{
				return ordering ? SubstitutionState::Incomplete : SubstitutionState::Success;
			}

			if (m_types.Kind(argument) != TypeKind::TemplateId)
			{
				return SubstitutionState::DeductionFailure;
			}

			const auto actual     = m_types.Arg(argument);
			const auto& instances = m_out.mInstantiations;
			if (instances.primary[actual] != primary || instances.argumentCount[actual] != instances.argumentCount[instance])
			{
				return SubstitutionState::DeductionFailure;
			}

			const auto first       = instances.firstArgument[instance];
			const auto actualFirst = instances.firstArgument[actual];
			const auto count       = instances.argumentCount[instance];
			for (std::uint32_t i = 0; i < count; ++i)
			{
				const auto state = MatchTypePattern(m_out.mTemplateArguments.type[first + i],
					m_out.mTemplateArguments.type[actualFirst + i], declaration, bindings, ordering,
					remainingDepth - 1);
				if (state != SubstitutionState::Success)
				{
					return state;
				}
			}

			return SubstitutionState::Success;
		}

		if (kind == TypeKind::Pointer || kind == TypeKind::LRef || kind == TypeKind::RRef ||
			kind == TypeKind::Const || kind == TypeKind::Array)
		{
			if (m_types.Kind(argument) != kind ||(kind == TypeKind::Array && m_types.Extent(pattern) != m_types.Extent(argument)))
			{
				return SubstitutionState::DeductionFailure;
			}

			return MatchTypePattern(m_types.Arg(pattern), m_types.Arg(argument), declaration, bindings,
				ordering, remainingDepth - 1);
		}

		return pattern == argument ? SubstitutionState::Success : SubstitutionState::DeductionFailure;
	}

	SubstitutionResult TyperImpl::MatchTypePatterns(
		std::span<const TypeId> patterns,
		std::span<const TypeId> arguments,
		TemplateId declaration,
		std::vector<TypeId>& bindings,
		bool ordering)
	{
		const auto& templates = m_model.Templates();
		if (declaration >= templates.owner.size() ||!templates.complete[declaration])
		{
			return {TypeTable::Unknown, SubstitutionState::Incomplete, SubstitutionReason::Unsupported};
		}

		if (patterns.size() != arguments.size())
		{
			return {TypeTable::Unknown, SubstitutionState::DeductionFailure, SubstitutionReason::ArgumentCount};
		}

		const auto count = templates.parameterCount[declaration];
		const auto first = templates.firstParameter[declaration];
		bindings.assign(count, TypeTable::Unknown);
		for (std::uint32_t i = 0; i < count; ++i)
		{
			if (m_model.TemplateParameters().kind[first + i] != TemplateParameterKind::Type || m_model.TemplateParameters().pack[first + i])
			{
				return {TypeTable::Unknown, SubstitutionState::Incomplete, SubstitutionReason::Unsupported};
			}
		}

		for (std::size_t i = 0; i < patterns.size(); ++i)
		{
			const auto state = MatchTypePattern(patterns[i], arguments[i], declaration, bindings, ordering,
				kMaxTemplateDepth);
			if (state != SubstitutionState::Success)
			{
				return {TypeTable::Unknown, state, SubstitutionReason::ConflictingDeduction};
			}
		}

		if (!ordering && std::ranges::find(bindings, TypeTable::Unknown) != bindings.end())
		{
			return {TypeTable::Unknown, SubstitutionState::DeductionFailure,
				SubstitutionReason::ConflictingDeduction};
		}

		if (!ordering)
		{
			for (std::size_t i = 0; i < patterns.size(); ++i)
			{
				const auto result = CheckedSubstitute(patterns[i], declaration, bindings,
					SubstitutionContext::Immediate);
				if (result.state != SubstitutionState::Success)
				{
					return result;
				}

				if (result.type != arguments[i])
				{
					return {TypeTable::Unknown, SubstitutionState::DeductionFailure,
						SubstitutionReason::ConflictingDeduction};
				}
			}
		}

		return {TypeTable::Unknown, SubstitutionState::Success, SubstitutionReason::None};
	}

	TyperImpl::ConstraintOutcome TyperImpl::EvaluateConstraintTokens(
		std::size_t begin,
		std::size_t end,
		TemplateId declaration,
		std::span<const TypeId> bindings,
		SubstitutionContext context,
		std::uint32_t remainingDepth)
	{
		if (remainingDepth == 0)
		{
			return {SubstitutionState::Incomplete, false, SubstitutionReason::DepthLimit};
		}

		while (begin < end && At(begin) == Tok::KwRequires)
		{
			// A bare `requires { ... }` requirement body used as an operand.
			std::size_t brace = begin + 1;
			if (At(brace) == Tok::LParen)
			{
				brace = MatchParen(brace, end);
				if (brace < end)
				{
					++brace;
				}
			}

			if (brace >= end || At(brace) != Tok::LBrace)
			{
				return {SubstitutionState::Incomplete, false, SubstitutionReason::Unsupported};
			}

			const auto close = MatchParen(brace, end);
			if (close >= end)
			{
				return {SubstitutionState::Incomplete, false, SubstitutionReason::Unsupported};
			}

			return EvaluateRequiresBody(brace, close, declaration, bindings, context, remainingDepth - 1);
		}

		// Strip one layer of parentheses covering the whole range.
		if (begin < end && At(begin) == Tok::LParen)
		{
			const auto close = MatchParen(begin, end);
			if (close + 1 == end)
			{
				return EvaluateConstraintTokens(begin + 1, close, declaration, bindings, context,
					remainingDepth - 1);
			}
		}

		// Top-level `||` first (lowest precedence), then `&&`, both short-circuit.
		for (const auto op :
			{
				Tok::PipePipe, Tok::AmpAmp
		})
		{
			for (auto i = begin; i < end;)
			{
				const auto tok = At(i);
				if (tok == Tok::LParen || tok == Tok::LBracket || tok == Tok::LBrace)
				{
					const auto close = MatchParen(i, end);
					i                = close >= end ? end : close + 1;
					continue;
				}

				if (tok == Tok::Lt)
				{
					const auto close = MatchAngle(i, end);
					i                = close >= end ? i + 1 : close + 1;
					continue;
				}

				if (tok == op)
				{
					auto left = EvaluateConstraintTokens(begin, i, declaration, bindings, context, remainingDepth - 1);
					if (left.state != SubstitutionState::Success)
					{
						return left;
					}

					if (op == Tok::PipePipe && left.satisfied)
					{
						return {SubstitutionState::Success, true, SubstitutionReason::None};
					}

					if (op == Tok::AmpAmp && !left.satisfied)
					{
						return {SubstitutionState::Success, false, SubstitutionReason::None};
					}

					return EvaluateConstraintTokens(i + 1, end, declaration, bindings, context, remainingDepth - 1);
				}

				++i;
			}
		}

		if (begin < end && At(begin) == Tok::Bang)
		{
			auto operand = EvaluateConstraintTokens(begin + 1, end, declaration, bindings, context,
				remainingDepth - 1);
			if (operand.state != SubstitutionState::Success)
			{
				return operand;
			}

			operand.satisfied = !operand.satisfied;
			return operand;
		}

		if (end == begin + 1)
		{
			if (At(begin) == Tok::KwTrue)
			{
				return {SubstitutionState::Success, true, SubstitutionReason::None};
			}

			if (At(begin) == Tok::KwFalse)
			{
				return {SubstitutionState::Success, false, SubstitutionReason::None};
			}
		}

		// Concept-id `Name<args...>` covering the whole range.
		if (begin < end && (WordAt(begin) || At(begin) == Tok::ColonColon))
		{
			auto pos = begin +(At(begin) == Tok::ColonColon ? 1 : 0);
			if (pos < end && WordAt(pos))
			{
				++pos;
				if (pos < end && At(pos) == Tok::Lt)
				{
					const auto close = MatchAngle(pos, end);
					if (close + 1 == end)
					{
						return EvaluateConceptId(begin, end, declaration, bindings, context, remainingDepth - 1);
					}
				}
				else if (pos == end)
				{
					// Nullary concept-id or a bare placeholder.
					return EvaluateConceptId(begin, end, declaration, bindings, context, remainingDepth - 1);
				}
			}
		}

		// Atomic expression constraint: must be a constant expression of type bool.
		// Reuse unevaluated-expression checking for validity, then enforce bool.
		for (auto node = std::uint32_t {0}; node < m_nodes.size(); ++node)
		{
			const auto[rangeBegin, rangeEnd] = NodeSig(node);
			if (rangeBegin != begin || rangeEnd != end)
			{
				continue;
			}

			switch (m_nodes.Kind(node))
			{
			case GrammarKind::LiteralExpression:
			case GrammarKind::IdentifierExpression:
			case GrammarKind::ParenthesizedExpression:
			case GrammarKind::UnaryExpression:
			case GrammarKind::BinaryExpression:
			case GrammarKind::CallExpression:
			{
				auto checked = CheckExpression(node, declaration, bindings, context, remainingDepth - 1);
				if (checked.result.state != SubstitutionState::Success)
				{
					if (checked.result.state == SubstitutionState::SubstitutionFailure)
					{
						return {SubstitutionState::Success, false, SubstitutionReason::None};
					}

					return {checked.result.state, false, checked.result.reason};
				}

				if (!m_types.IsBool(checked.result.type))
				{
					// An atomic constraint that is valid but not bool is
					// ill-formed, not a substitution failure.
					return {SubstitutionState::HardError, false, SubstitutionReason::InvalidExpression};
				}

				// A valid bool-valued expression that is not a recognized
				// constant cannot prove satisfaction either way here.
				return {SubstitutionState::Incomplete, false, SubstitutionReason::Unsupported};
			}
			default:
				break;
			}
		}

		return {SubstitutionState::Incomplete, false, SubstitutionReason::Unsupported};
	}

	TyperImpl::ConstraintOutcome TyperImpl::EvaluateRequiresBody(
		std::size_t brace,
		std::size_t close,
		TemplateId declaration,
		std::span<const TypeId> bindings,
		SubstitutionContext context,
		std::uint32_t remainingDepth)
	{
		if (remainingDepth == 0)
		{
			return {SubstitutionState::Incomplete, false, SubstitutionReason::DepthLimit};
		}

		for (auto first = brace + 1; first < close;)
		{
			auto last           = first;
			std::uint32_t depth = 0;
			for (; last < close; ++last)
			{
				const auto tok = At(last);
				if (tok == Tok::LParen || tok == Tok::LBracket || tok == Tok::LBrace || tok == Tok::Lt)
				{
					++depth;
				}
				else if (tok == Tok::RParen || tok == Tok::RBracket || tok == Tok::RBrace || tok == Tok::Gt)
				{
					depth = depth > 0 ? depth - 1 : 0;
				}
				else if (tok == Tok::Semi && depth == 0)
				{
					break;
				}
			}

			if (first != last)
			{
				// Locate the Requirement node covering these tokens when present.
				std::uint32_t requirement = kNone;
				for (std::uint32_t node = 0; node < m_nodes.size(); ++node)
				{
					if (m_nodes.Kind(node) != GrammarKind::Requirement)
					{
						continue;
					}

					const auto[rangeBegin, rangeEnd] = NodeSig(node);
					if (rangeBegin == first && (rangeEnd == last ||(rangeEnd == last + 1 && At(last) == Tok::Semi)))
					{
						requirement = node;
						break;
					}
				}

				ConstraintOutcome outcome {SubstitutionState::Incomplete, false, SubstitutionReason::Unsupported};
				if (requirement != kNone && m_tree.RequirementKindOf(requirement) == RequirementKind::Type)
				{
					outcome = EvaluateTypeRequirement(requirement, declaration, bindings, context, remainingDepth - 1);
				}
				else if (first < last && At(first) == Tok::KwRequires)
				{
					outcome = EvaluateConstraintTokens(first, last, declaration, bindings, context, remainingDepth - 1);
				}
				else if (requirement != kNone)
				{
					bool nested = false;
					for (const auto child : m_model.ChildrenOf(requirement))
					{
						if (m_nodes.Kind(child) == GrammarKind::RequiresExpression)
						{
							const auto[childBegin, childEnd] = NodeSig(child);
							outcome = EvaluateConstraintTokens(childBegin, childEnd, declaration, bindings,
								context, remainingDepth - 1);
							nested = true;
							break;
						}
					}

					if (!nested)
					{
						outcome = {SubstitutionState::Incomplete, false, SubstitutionReason::Unsupported};
					}
				}

				if (outcome.state != SubstitutionState::Success)
				{
					return outcome;
				}

				if (!outcome.satisfied)
				{
					return {SubstitutionState::Success, false, SubstitutionReason::None};
				}
			}

			first = last < close && At(last) == Tok::Semi ? last + 1 : close;
		}

		return {SubstitutionState::Success, true, SubstitutionReason::None};
	}

	TyperImpl::ConstraintOutcome TyperImpl::EvaluateTypeRequirement(
		std::uint32_t requirement,
		TemplateId declaration,
		std::span<const TypeId> bindings,
		SubstitutionContext context,
		std::uint32_t remainingDepth)
	{
		const auto[begin, end] = NodeSig(requirement);
		auto first             = begin +(begin < end && At(begin) == Tok::KwTypename ? 1 : 0);
		auto last              = end;
		if (last > first && At(last - 1) == Tok::Semi)
		{
			--last;
		}

		if (first >= last)
		{
			return {SubstitutionState::Incomplete, false, SubstitutionReason::Unsupported};
		}

		const auto& templates = m_model.Templates();
		const auto scope = declaration < templates.scope.size()
		? templates.scope[declaration] : SemanticModel::TranslationUnitScope;
		const auto type   = ParseTypeTokens(first, last, scope, m_sig[first]);
		const auto result = CheckedSubstitute(type, declaration, bindings, context, remainingDepth);
		if (result.state == SubstitutionState::Success || result.state == SubstitutionState::Dependent)
		{
			return {result.state, result.state == SubstitutionState::Success, SubstitutionReason::None};
		}

		if (result.state == SubstitutionState::SubstitutionFailure)
		{
			return {SubstitutionState::Success, false, SubstitutionReason::None};
		}

		return {result.state, false, result.reason};
	}

	TyperImpl::ConstraintOutcome TyperImpl::EvaluateConceptId(
		std::size_t begin,
		std::size_t end,
		TemplateId declaration,
		std::span<const TypeId> bindings,
		SubstitutionContext context,
		std::uint32_t remainingDepth)
	{
		auto pos = begin +(At(begin) == Tok::ColonColon ? 1 : 0);
		if (pos >= end ||!WordAt(pos))
		{
			return {SubstitutionState::Incomplete, false, SubstitutionReason::Unsupported};
		}

		const auto name       = m_model.Names().Find(TextAt(pos));
		const auto& templates = m_model.Templates();
		const auto scope = declaration < templates.scope.size()
		? templates.scope[declaration] : SemanticModel::TranslationUnitScope;
		const auto symbol = name == kNone ? kNone : m_model.Lookup(scope, name, m_sig[begin]);
		if (symbol == kNone || m_symbols.kind[symbol] != SymbolKind::Concept)
		{
			return {SubstitutionState::Incomplete, false, SubstitutionReason::Unsupported};
		}

		const auto conceptDeclaration = m_model.TemplateOf(symbol);
		if (conceptDeclaration >= templates.owner.size() ||!templates.complete[conceptDeclaration])
		{
			return {SubstitutionState::Incomplete, false, SubstitutionReason::Unsupported};
		}

		std::vector<TypeId> conceptArgs;
		if (pos + 1 < end && At(pos + 1) == Tok::Lt)
		{
			const auto close = MatchAngle(pos + 1, end);
			if (close + 1 != end)
			{
				return {SubstitutionState::Incomplete, false, SubstitutionReason::Unsupported};
			}

			for (auto first = pos + 2; first < close;)
			{
				auto last           = first;
				std::uint32_t depth = 0;
				for (; last < close; ++last)
				{
					const auto tok = At(last);
					if (tok == Tok::Lt)
					{
						++depth;
					}
					else if (tok == Tok::Gt && depth != 0)
					{
						--depth;
					}
					else if (tok == Tok::Comma && depth == 0)
					{
						break;
					}
				}

				const auto parsed = ParseTypeTokens(first, last, scope, m_sig[first]);
				const auto substituted = CheckedSubstitute(parsed, declaration, bindings, context,
					remainingDepth - 1);
				if (substituted.state != SubstitutionState::Success)
				{
					if (substituted.state == SubstitutionState::SubstitutionFailure)
					{
						return {SubstitutionState::Success, false, SubstitutionReason::None};
					}

					return {substituted.state, false, substituted.reason};
				}

				conceptArgs.push_back(substituted.type);
				first = last + 1;
			}
		}

		const auto count = templates.parameterCount[conceptDeclaration];
		if (!conceptArgs.empty() && conceptArgs.size() != count)
		{
			return {SubstitutionState::Incomplete, false, SubstitutionReason::Unsupported};
		}

		if (conceptArgs.empty())
		{
			conceptArgs.assign(count, TypeTable::Unknown);
		}

		const auto conceptNode = templates.node[conceptDeclaration];
		for (auto node = conceptNode; node < m_nodes.SubtreeEnd(conceptNode); ++node)
		{
			if (m_nodes.Kind(node) != GrammarKind::ConceptDefinition)
			{
				continue;
			}

			for (const auto child : m_model.ChildrenOf(node))
			{
				if (m_nodes.Kind(child) == GrammarKind::RequiresExpression)
				{
					const auto[childBegin, childEnd] = NodeSig(child);
					return EvaluateConstraintTokens(childBegin, childEnd, conceptDeclaration, conceptArgs,
						context, remainingDepth - 1);
				}
			}
		}

		return {SubstitutionState::Incomplete, false, SubstitutionReason::Unsupported};
	}

	TyperImpl::ConstraintOutcome TyperImpl::CheckAssociatedConstraints(
		TemplateId declaration,
		std::span<const TypeId> bindings,
		SubstitutionContext context)
	{
		const auto& templates = m_model.Templates();
		if (declaration >= templates.node.size())
		{
			return {SubstitutionState::Incomplete, false, SubstitutionReason::Unsupported};
		}

		const auto root    = templates.node[declaration];
		const auto rootEnd = m_nodes.SubtreeEnd(root);
		bool any           = false;
		for (auto node = root; node < rootEnd; ++node)
		{
			if (m_nodes.Kind(node) != GrammarKind::RequiresClause)
			{
				continue;
			}

			bool nested = false;
			for (auto parent = m_nodes.Parent(node); parent < m_nodes.size() && parent != root;
				parent = m_nodes.Parent(parent))
			{
				if (m_nodes.Kind(parent) == GrammarKind::RequiresClause ||
					m_nodes.Kind(parent) == GrammarKind::RequiresExpression ||
					m_nodes.Kind(parent) == GrammarKind::Requirement)
				{
					nested = true;
					break;
				}
			}

			if (nested)
			{
				continue;
			}

			any                    = true;
			const auto[begin, end] = NodeSig(node);
			if (begin + 1 >= end)
			{
				return {SubstitutionState::Incomplete, false, SubstitutionReason::Unsupported};
			}

			const auto outcome = EvaluateConstraintTokens(begin + 1, end, declaration, bindings, context,
				kMaxTemplateDepth);
			if (outcome.state != SubstitutionState::Success)
			{
				return outcome;
			}

			if (!outcome.satisfied)
			{
				return {SubstitutionState::Success, false, SubstitutionReason::None};
			}
		}

		if (!any)
		{
			return {SubstitutionState::Success, true, SubstitutionReason::None};
		}

		return {SubstitutionState::Success, true, SubstitutionReason::None};
	}

	std::vector<std::string> TyperImpl::ConstraintAtoms(TemplateId declaration)
	{
		std::vector<std::string> atoms;
		const auto& templates = m_model.Templates();
		if (declaration >= templates.node.size())
		{
			return atoms;
		}

		const auto root    = templates.node[declaration];
		const auto rootEnd = m_nodes.SubtreeEnd(root);
		for (auto node = root; node < rootEnd; ++node)
		{
			if (m_nodes.Kind(node) != GrammarKind::RequiresClause)
			{
				continue;
			}

			const auto range = NodeSig(node);
			if (range.first + 1 >= range.second)
			{
				continue;
			}

			// Split the clause body at top-level `&&`; each conjunct is an atom.
			auto first = range.first + 1;
			auto end   = range.second;
			if (first < end && At(first) == Tok::LParen && MatchParen(first, end) + 1 == end)
			{
				++first;
				--end;
			}

			for (auto i = first; i <= end; ++i)
			{
				bool split = i == end;
				if (!split)
				{
					const auto tok = At(i);
					if (tok == Tok::LParen || tok == Tok::LBracket || tok == Tok::LBrace)
					{
						const auto close = MatchParen(i, end);
						i                = close >= end ? end : close;
						continue;
					}

					if (tok == Tok::Lt)
					{
						const auto close = MatchAngle(i, end);
						i                = close >= end ? i : close;
						continue;
					}

					split = tok == Tok::AmpAmp;
				}

				if (split && i > first)
				{
					std::string atom;
					for (auto pos = first; pos < i; ++pos)
					{
						if (!atom.empty())
						{
							atom += ' ';
						}

						atom += std::string(TextAt(pos));
					}

					if (!atom.empty())
					{
						atoms.push_back(std::move(atom));
					}

					first = i + 1;
				}
			}
		}

		std::ranges::sort(atoms);
		atoms.erase(std::ranges::unique(atoms).begin(), atoms.end());
		return atoms;
	}

	TypeId TyperImpl::Instantiate(
		TemplateId declaration,
		std::vector<TypeId> arguments,
		std::vector<std::pair<std::uint32_t, std::uint32_t>> syntax,
		SubstitutionContext context)
	{
		const auto& templates  = m_model.Templates();
		const auto& parameters = m_model.TemplateParameters();
		if (declaration >= templates.owner.size() || mInstantiationDepth >= kMaxTemplateDepth)
		{
			return TypeTable::Unknown;
		}

		++mInstantiationDepth;
		struct DepthGuard
		{
			std::uint32_t& depth;
			~DepthGuard()
			{
				--depth;
			}
		} depthGuard
		{
			mInstantiationDepth
		};
		syntax.resize(arguments.size(), {kNone, kNone});
		const auto firstParameter = templates.firstParameter[declaration];
		const auto count          = templates.parameterCount[declaration];
		bool complete             = templates.complete[declaration] && templates.kind[declaration] == TemplateDeclarationKind::Primary;
		SubstitutionResult formation {TypeTable::Unknown, SubstitutionState::Success,
			SubstitutionReason::None};
		const auto declarationOwner = templates.owner[declaration];
		const bool aliasPack = declarationOwner < m_symbols.Size() && m_symbols.kind[declarationOwner] == SymbolKind::TypeAlias &&
			count != 0 && parameters.pack[firstParameter + count - 1];
		for (std::uint32_t i = 0; i < count; ++i)
		{
			const auto parameter = firstParameter + i;
			complete = complete && parameters.kind[parameter] == TemplateParameterKind::Type &&
				(!parameters.pack[parameter] ||(aliasPack && i + 1 == count));
			if (aliasPack && i + 1 == count)
			{
				break;
			}

			if (i >= arguments.size())
			{
				const auto begin = parameters.defaultBegin[parameter];
				const auto end   = parameters.defaultEnd[parameter];
				const auto defaultType = begin == kNone ? TypeTable::Unknown
				: ParseTypeTokens(Pos(begin), Pos(end), templates.scope[declaration], begin);
				formation = CheckedSubstitute(defaultType, declaration, arguments, SubstitutionContext::Immediate);
				arguments.push_back(formation.type);
				syntax.emplace_back(begin, end);
				if (formation.state != SubstitutionState::Success && formation.state != SubstitutionState::Dependent)
				{
					complete = false;
					break;
				}
			}
		}

		complete       = complete && (aliasPack ? arguments.size() + 1 >= count : arguments.size() == count);
		bool dependent = false;
		for (const auto argument : arguments)
		{
			dependent = dependent || m_types.IsDependent(argument);
			complete  = complete && argument != TypeTable::Unknown;
		}

		if (complete && !dependent)
		{
			const auto constraints = CheckAssociatedConstraints(declaration, arguments, context);
			if (constraints.state == SubstitutionState::Success && !constraints.satisfied)
			{
				formation = {TypeTable::Unknown, SubstitutionState::ConstraintUnsatisfied,
					SubstitutionReason::None};
				complete = false;
			}
			else if (constraints.state == SubstitutionState::HardError)
			{
				formation = {TypeTable::Unknown, SubstitutionState::HardError, constraints.reason};
				complete  = false;
			}
			else if (constraints.state != SubstitutionState::Success)
			{
				complete = false;
			}
		}

		TemplateId selected = declaration;
		std::vector<TypeId> selectedBindings;
		bool ambiguous   = false;
		const auto owner = templates.owner[declaration];
		for (TemplateId candidate = 0; candidate < templates.owner.size(); ++candidate)
		{
			const auto symbol = templates.owner[candidate];
			if (candidate == declaration || symbol == kNone || owner == kNone ||
				m_symbols.name[symbol] != m_symbols.name[owner] || m_symbols.scope[symbol] != m_symbols.scope[owner])
			{
				continue;
			}

			if (templates.kind[candidate] == TemplateDeclarationKind::Partial)
			{
				continue;
			}

			if (templates.kind[candidate] == TemplateDeclarationKind::Primary ||!templates.complete[candidate])
			{
				complete = false;
				continue;
			}

			const auto namePos = Pos(m_symbols.decl_token[symbol]);
			const auto end     = NodeSig(m_symbols.decl_node[symbol]).second;
			const auto close   = MatchAngle(namePos + 1, end);
			if (close >= end)
			{
				complete = false;
				continue;
			}

			const auto pattern = ParseTemplateArguments(namePos + 2, close, templates.scope[candidate],
				m_symbols.decl_token[symbol]);
			if (std::ranges::find(pattern, TypeTable::Unknown) != pattern.end())
			{
				complete = false;
				continue;
			}

			if (!dependent && pattern == arguments)
			{
				if (selected != declaration)
				{
					complete = false;
				}

				selected = candidate;
			}
		}

		if (complete && !dependent && selected == declaration)
		{
			struct PartialMatch
			{
				TemplateId declaration;
				std::vector<TypeId> patterns;
				std::vector<TypeId> bindings;
			};

			constexpr std::size_t kMaxPartialCandidates = 128;
			std::vector<PartialMatch> matches;
			std::size_t visits = 0;
			for (TemplateId candidate = 0; candidate < templates.owner.size(); ++candidate)
			{
				const auto symbol = templates.owner[candidate];
				if (templates.kind[candidate] != TemplateDeclarationKind::Partial || symbol == kNone || owner == kNone ||
					m_symbols.name[symbol] != m_symbols.name[owner] || m_symbols.scope[symbol] != m_symbols.scope[owner])
				{
					continue;
				}

				if (++visits > kMaxPartialCandidates ||!templates.complete[candidate])
				{
					complete = false;
					continue;
				}

				const auto namePos = Pos(m_symbols.decl_token[symbol]);
				const auto end     = NodeSig(m_symbols.decl_node[symbol]).second;
				const auto close   = MatchAngle(namePos + 1, end);
				if (close >= end)
				{
					complete = false;
					continue;
				}

				auto patterns = ParseTemplateArguments(namePos + 2, close, templates.scope[candidate],
					m_symbols.decl_token[symbol]);
				std::vector<TypeId> bindings;
				const auto result = MatchTypePatterns(patterns, arguments, candidate, bindings);
				if (result.state == SubstitutionState::Success)
				{
					const auto partialConstraints = CheckAssociatedConstraints(candidate, bindings, context);
					if (partialConstraints.state == SubstitutionState::Success && partialConstraints.satisfied)
					{
						matches.push_back({candidate, std::move(patterns), std::move(bindings)});
					}
					else if (partialConstraints.state == SubstitutionState::HardError)
					{
						formation = {TypeTable::Unknown, SubstitutionState::HardError, partialConstraints.reason};
						complete  = false;
						break;
					}
					else if (partialConstraints.state == SubstitutionState::Success)
					{
						// Unsatisfied partial constraints: not a match, primary may still match.
					}
					else
					{
						complete = false;
					}
				}
				else if (result.state == SubstitutionState::HardError)
				{
					formation = result;
					complete  = false;
					break;
				}
				else if (result.state != SubstitutionState::DeductionFailure && result.state != SubstitutionState::SubstitutionFailure)
				{
					complete = false;
				}
			}

			if (complete && !matches.empty())
			{
				std::size_t winners = 0;
				for (const auto& match : matches)
				{
					bool best = true;
					for (const auto& other : matches)
					{
						if (&match == &other)
						{
							continue;
						}

						std::vector<TypeId> forwardBindings, reverseBindings;
						const auto forward = MatchTypePatterns(other.patterns, match.patterns, other.declaration,
							forwardBindings, true);
						const auto reverse = MatchTypePatterns(match.patterns, other.patterns, match.declaration,
							reverseBindings, true);
						if (forward.state == SubstitutionState::Incomplete || reverse.state == SubstitutionState::Incomplete)
						{
							complete = false;
						}

						best = best && forward.state == SubstitutionState::Success && reverse.state == SubstitutionState::DeductionFailure;
					}

					if (best)
					{
						selected         = match.declaration;
						selectedBindings = match.bindings;
						++winners;
					}
				}

				ambiguous = complete && winners != 1;
				complete  = complete && !ambiguous;
			}
		}

		auto& instances = m_out.mInstantiations;
		auto& table     = m_out.mTemplateArguments;
		const auto state = ambiguous ? TemplateResolution::Ambiguous : !complete ? TemplateResolution::Incomplete
		: dependent ? TemplateResolution::Dependent : TemplateResolution::Resolved;
		std::uint64_t key = declaration;
		for (const auto argument : arguments)
		{
			key = (key ^ argument) * kTemplateKeyPrime;
		}

		const auto[cacheBegin, cacheEnd] = mInstanceCache.equal_range(key);
		for (auto cached = cacheBegin; complete && cached != cacheEnd; ++cached)
		{
			const auto instance = cached->second;
			if (instances.primary[instance] != declaration || instances.argumentCount[instance] != arguments.size() || instances.state[instance] != state)
			{
				continue;
			}

			const auto first = instances.firstArgument[instance];
			if (std::equal(arguments.begin(), arguments.end(), table.type.begin() + first))
			{
				return m_types.TemplateInstance(instance, dependent);
			}
		}

		const auto instance = static_cast<std::uint32_t>(instances.primary.size());
		instances.primary.push_back(declaration);
		instances.selected.push_back(state == TemplateResolution::Resolved ? selected : kNone);
		instances.firstArgument.push_back(static_cast<std::uint32_t>(table.type.size()));
		instances.argumentCount.push_back(static_cast<std::uint32_t>(arguments.size()));
		instances.firstSubstitution.push_back(instances.firstArgument.back());
		instances.substitutionCount.push_back(instances.argumentCount.back());
		instances.state.push_back(state);
		instances.checkState.push_back(formation.state == SubstitutionState::SubstitutionFailure || formation.state == SubstitutionState::HardError || formation.state == SubstitutionState::ConstraintUnsatisfied
			? formation.state : state == TemplateResolution::Resolved ? SubstitutionState::Success
			: state == TemplateResolution::Dependent ? SubstitutionState::Dependent
			: state == TemplateResolution::Ambiguous ? SubstitutionState::Ambiguous : SubstitutionState::Incomplete);
		instances.reason.push_back(formation.reason);
		instances.firstBase.push_back(kNone);
		instances.baseCount.push_back(0);
		if (complete)
		{
			mInstanceCache.emplace(key, instance);
		}

		for (std::uint32_t i = 0; i < arguments.size(); ++i)
		{
			const auto parameter = i < count ? firstParameter + i : aliasPack ? firstParameter + count - 1 : kNone;
			table.parameter.push_back(parameter);
			table.sourceBegin.push_back(syntax[i].first);
			table.sourceEnd.push_back(syntax[i].second);
			table.type.push_back(arguments[i]);
			table.kind.push_back(parameter == kNone ? TemplateArgumentKind::Unknown
				: parameters.kind[parameter] == TemplateParameterKind::Type ? TemplateArgumentKind::Type
				: parameters.kind[parameter] == TemplateParameterKind::Value ? TemplateArgumentKind::Value : TemplateArgumentKind::Template);
		}

		if (state == TemplateResolution::Resolved && templates.kind[selected] == TemplateDeclarationKind::Partial)
		{
			instances.firstSubstitution[instance] = static_cast<std::uint32_t>(table.type.size());
			instances.substitutionCount[instance] = static_cast<std::uint32_t>(selectedBindings.size());
			for (std::uint32_t i = 0; i < selectedBindings.size(); ++i)
			{
				table.parameter.push_back(templates.firstParameter[selected] + i);
				table.sourceBegin.push_back(kNone);
				table.sourceEnd.push_back(kNone);
				table.type.push_back(selectedBindings[i]);
				table.kind.push_back(TemplateArgumentKind::Type);
			}
		}

		std::vector<TypeId> baseTypes;
		const auto selectedOwner = templates.owner[selected];
		if (complete && selectedOwner != kNone && mInstantiationDepth < kMaxTemplateDepth)
		{
			const auto baseFirst = m_symbols.first_base[selectedOwner];
			const auto baseCount = m_symbols.base_count[selectedOwner];
			const auto end       = NodeSig(m_symbols.decl_node[selectedOwner]).second;
			for (std::uint32_t i = 0; i < baseCount; ++i)
			{
				const auto begin    = Pos(m_model.Bases().token[baseFirst + i]);
				auto last           = begin;
				std::uint32_t depth = 0;
				for (; last < end; ++last)
				{
					if (At(last) == Tok::Lt)
					{
						++depth;
					}
					else if (At(last) == Tok::Gt && depth != 0)
					{
						--depth;
					}
					else if (depth == 0 && (At(last) == Tok::Comma || At(last) == Tok::LBrace))
					{
						break;
					}
				}

				const auto baseType = ParseTypeTokens(begin, last, templates.scope[selected],
					m_model.Bases().token[baseFirst + i]);
				baseTypes.push_back(SubstituteArguments(baseType, selected,
					templates.kind[selected] == TemplateDeclarationKind::Partial ? std::span<const TypeId>(selectedBindings) : std::span<const TypeId>(arguments)));
			}

		}

		instances.firstBase[instance] = static_cast<std::uint32_t>(instances.baseType.size());
		instances.baseCount[instance] = static_cast<std::uint32_t>(baseTypes.size());
		instances.baseType.insert(instances.baseType.end(), baseTypes.begin(), baseTypes.end());
		return m_types.TemplateInstance(instance, dependent ||(!complete && !ambiguous));
	}

	// `[::] a [<...>] [:: b [<...>]]...` at `pos`: a type name. Library (`std::`)
	// names stay External; names declared in this file resolve to symbols.
	TypeId TyperImpl::ParseName(
		std::size_t& pos,
		std::size_t end,
		ScopeId scope,
		std::uint32_t before)
	{
		const bool global = At(pos) == Tok::ColonColon;
		if (global)
		{
			++pos;
		}

		std::string text;
		std::string_view last_component;
		SymbolId current         = kNone;
		bool first               = true;
		bool is_std              = false;
		bool template_args       = false;
		bool dependent           = false;
		bool unresolved          = false;
		TypeId templateType      = TypeTable::Unknown;
		TypeId qualifiedReceiver = TypeTable::Unknown;
		TypeId qualifiedType     = TypeTable::Unknown;
		for (; pos < end && WordAt(pos);)
		{
			const auto spelled = TextAt(pos);
			Append(text, spelled);
			last_component  = spelled;
			const auto name = m_model.Names().Find(spelled);
			SymbolId found  = kNone;
			if (first)
			{
				is_std = spelled == "std";
				if (name != kNone)
				{
					found = global ? m_model.LookupLocal(SemanticModel::TranslationUnitScope, name)
					: m_model.Lookup(scope, name, before);
				}
			}
			else if (current != kNone && name != kNone)
			{
				found = m_model.LookupMember(current, name);
			}

			if (found == kNone)
			{
				unresolved = true;
			}

			current = found;
			if (qualifiedReceiver != TypeTable::Unknown)
			{
				qualifiedType = m_types.DependentMember(qualifiedReceiver, m_out.m_externals.InternCopy(spelled));
			}

			if (found != kNone && m_symbols.kind[found] == SymbolKind::TemplateParameter)
			{
				dependent = true;
			}

			first = false;
			++pos;
			if (pos < end && At(pos) == Tok::Lt)
			{
				const auto close = MatchAngle(pos, end);
				if (close >= end)
				{
					pos = end;
					return TypeTable::Unknown;
				}

				template_args = true;
				for (auto argumentToken = pos + 1; argumentToken < close; ++argumentToken)
				{
					if (!WordAt(argumentToken))
					{
						continue;
					}

					const auto name   = m_model.Names().Find(TextAt(argumentToken));
					const auto symbol = m_model.Lookup(scope, name, m_sig[argumentToken]);
					dependent         = dependent ||(symbol != kNone && m_symbols.kind[symbol] == SymbolKind::TemplateParameter);
				}

				if (At(close) != Tok::Shr && mInstantiationDepth < kMaxTemplateDepth)
				{
					std::vector<std::pair<std::uint32_t, std::uint32_t>> syntax;
					auto arguments = ParseTemplateArguments(pos + 1, close, scope, before, &syntax);
					for (const auto argument : arguments)
					{
						dependent = dependent || m_types.IsDependent(argument);
					}

					if (current != kNone && (m_symbols.kind[current] == SymbolKind::Class ||
						m_symbols.kind[current] == SymbolKind::TypeAlias))
					{
						templateType = Instantiate(m_model.TemplateOf(current), std::move(arguments), std::move(syntax));
					}
				}

				text += '<';
				for (std::size_t i = pos + 1; i < close; ++i)
				{
					Append(text, TextAt(i));
				}

				text += '>';
				pos = close + 1;
				// `>>` closes two levels: the second `>` is consumed with the first.
				if (At(close) == Tok::Shr)
				{
					text.pop_back();
					text += ">>";
				}
			}

			if (pos + 1 < end && At(pos) == Tok::ColonColon && WordAt(pos + 1))
			{
				if (qualifiedType != TypeTable::Unknown)
				{
					qualifiedReceiver = qualifiedType;
				}
				else if (templateType != TypeTable::Unknown)
				{
					qualifiedReceiver = templateType;
				}
				else if (current != kNone && m_symbols.kind[current] == SymbolKind::TemplateParameter)
				{
					const auto parameter = m_model.ParameterOf(current);
					if (parameter != kNone && m_model.TemplateParameters().kind[parameter] == TemplateParameterKind::Type)
					{
						qualifiedReceiver = m_types.Parameter(parameter);
					}
				}

				templateType = TypeTable::Unknown;
				text += "::";
				++pos;
				continue;
			}

			break;
		}

		if (qualifiedType != TypeTable::Unknown)
		{
			return qualifiedType;
		}

		if (dependent)
		{
			if (!template_args && current != kNone && m_symbols.kind[current] == SymbolKind::TemplateParameter)
			{
				const auto parameter = m_model.ParameterOf(current);
				return parameter != kNone && m_model.TemplateParameters().kind[parameter] == TemplateParameterKind::Type
				? m_types.Parameter(parameter) : TypeTable::Unknown;
			}

			return templateType != TypeTable::Unknown ? templateType
			: m_types.DependentName(m_out.m_externals.InternCopy(text), scope);
		}

		if (text.empty())
		{
			return TypeTable::Unknown;
		}

		if (is_std)
		{
			// Well-known aliases the engine can resolve exactly.
			if (text == "std::size_t")
			{
				return m_types.Builtin(BuiltinType::SizeT);
			}

			if (const auto fixed = FixedWidthInteger(text))
			{
				return m_types.Builtin(*fixed);
			}

			return m_types.External(m_out.m_externals.InternCopy(text));
		}

		if (current == kNone || unresolved)
		{
			if (unresolved && text == "size_t")
			{
				return m_types.Builtin(BuiltinType::SizeT);
			}

			if (unresolved)
			{
				if (const auto fixed = FixedWidthInteger(text))
				{
					return m_types.Builtin(*fixed);
				}

				// Names from headers the file includes: the local model never binds
				// them, but the parse-time oracle knows they are types. Like the
				// `std::` names above, they model as external types so diagnostics can
				// spell them instead of reporting `<unknown>`.
				if (const auto header = m_model.Tree().TypeNames())
				{
					if (!last_component.empty() && header->IsType(last_component))
					{
						return m_types.External(m_out.m_externals.InternCopy(text));
					}
				}
			}

			return TypeTable::Unknown;
		}

		switch (m_symbols.kind[current])
		{
		case SymbolKind::Class:
			if (template_args)
			{
				return templateType;
			}

			return (m_symbols.flags[current] & SymbolFlag::Template) != 0
			? TypeTable::Unknown
			: m_types.Class(current);
		case SymbolKind::Enum:
			return m_types.Enum(current);
		case SymbolKind::TypeAlias:
			if (template_args)
			{
				return CheckedSubstitute(templateType, kNone, {}, SubstitutionContext::Instantiation).type;
			}

			return SymbolType(current);
		default:
			return TypeTable::Unknown;
		}
	}

	TyperImpl::Base TyperImpl::ParseBase(
		std::size_t begin,
		std::size_t end,
		ScopeId scope,
		std::uint32_t before)
	{
		Base result;
		result.next     = begin;
		std::size_t pos = begin;
		const auto skip_qualifiers =[&]()
		{
			while (pos < end)
			{
				const Tok tok = At(pos);
				if (tok == Tok::KwConst)
				{
					result.is_const = true;
				}
				else if (!IsIgnoredSpecifier(tok))
				{
					break;
				}

				++pos;
			}
		};

		skip_qualifiers();
		if (pos >= end)
		{
			return result;
		}

		TypeId type     = TypeTable::Unknown;
		const Tok first = At(pos);
		if (IsBuiltinWord(first))
		{
			bool is_unsigned = false;
			bool is_signed   = false;
			bool is_short    = false;
			int longs        = 0;
			std::optional<Tok> core;
			while (pos < end && (IsBuiltinWord(At(pos)) || At(pos) == Tok::KwConst ||
				At(pos) == Tok::KwVolatile))
			{
				switch (At(pos))
				{
				case Tok::KwUnsigned:
					is_unsigned = true;
					break;
				case Tok::KwSigned:
					is_signed = true;
					break;
				case Tok::KwShort:
					is_short = true;
					break;
				case Tok::KwLong:
					++longs;
					break;
				case Tok::KwConst:
					result.is_const = true;
					break;
				case Tok::KwVolatile:
					break;
				default:
					core = At(pos);
					break;
				}

				++pos;
			}

			const Tok kind = core.value_or(Tok::KwInt);
			switch (kind)
			{
			case Tok::KwVoid:
				type = m_types.Builtin(BuiltinType::Void);
				break;
			case Tok::KwBool:
				type = m_types.Builtin(BuiltinType::Bool);
				break;
			case Tok::KwFloat:
				type = m_types.Builtin(BuiltinType::Float);
				break;
			case Tok::KwDouble:
				type =
					m_types.Builtin(longs > 0 ? BuiltinType::LongDouble : BuiltinType::Double);
				break;
			case Tok::KwWchar:
				type = m_types.Builtin(BuiltinType::WChar);
				break;
			case Tok::KwChar8:
				type = m_types.Builtin(BuiltinType::Char8);
				break;
			case Tok::KwChar16:
				type = m_types.Builtin(BuiltinType::Char16);
				break;
			case Tok::KwChar32:
				type = m_types.Builtin(BuiltinType::Char32);
				break;
			case Tok::KwChar:
				type = m_types.Builtin(is_unsigned ? BuiltinType::UChar
					: is_signed ? BuiltinType::SChar
					: BuiltinType::Char);
				break;
			default:
				if (is_short)
				{
					type =
						m_types.Builtin(is_unsigned ? BuiltinType::UShort : BuiltinType::Short);
				}
				else if (longs == 0)
				{
					type = m_types.Builtin(is_unsigned ? BuiltinType::UInt : BuiltinType::Int);
				}
				else if (longs == 1)
				{
					type =
						m_types.Builtin(is_unsigned ? BuiltinType::ULong : BuiltinType::Long);
				}
				else
				{
					type = m_types.Builtin(
						is_unsigned ? BuiltinType::ULongLong : BuiltinType::LongLong);
				}

				break;
			}
		}
		else if (first == Tok::KwAuto)
		{
			result.is_auto = true;
			++pos;
		}
		else if (first == Tok::KwDecltype)
		{
			for (std::uint32_t node = 0; node < m_nodes.size(); ++node)
			{
				if (m_nodes.Kind(node) == GrammarKind::DecltypeSpecifier && m_nodes.FirstToken(node) == m_sig[pos])
				{
					const auto children = m_model.ChildrenOf(node);
					if (children.size() == 1)
					{
						const auto kind = m_nodes.Kind(children.front());
						type = m_types.DecltypeExpression(children.front(), kind == GrammarKind::IdentifierExpression ||
							kind == GrammarKind::MemberExpression);
					}

					break;
				}
			}

			pos = pos + 1 < end && At(pos + 1) == Tok::LParen ? MatchParen(pos + 1, end) + 1 : end;
		}
		else
		{
			bool dependent = false;
			if (first == Tok::KwTypename)
			{
				dependent = true;
				++pos;
			}
			else if (first == Tok::KwStruct || first == Tok::KwClass || first == Tok::KwUnion ||
				first == Tok::KwEnum)
			{
				++pos;
			}

			type = ParseName(pos, end, scope, before);
			(void) dependent;

			// A constrained placeholder (`Concept auto`, `C decltype(auto)`):
			// the constraint does not change deduction, so `auto` deduces
			// from the initializer exactly like plain `auto`.
			if (At(pos) == Tok::KwAuto)
			{
				result.is_auto = true;
				++pos;
			}
			else if (At(pos) == Tok::KwDecltype && pos + 1 < end && At(pos + 1) == Tok::LParen)
			{
				pos = MatchParen(pos + 1, end) + 1;
			}
		}

		skip_qualifiers();
		result.next = std::min(pos, end);
		result.ok   = true;
		result.type = result.is_const && !result.is_auto ? m_types.Const(type) : type;
		return result;
	}

	// `*`, `&`, `&&` and cv-qualifiers applied to `type`, left to right. Anything
	// else (function declarators, packs, names) makes the result Unknown.
	TypeId TyperImpl::ApplyOperators(TypeId type, std::size_t begin, std::size_t end)
	{
		for (std::size_t i = begin; i < end; ++i)
		{
			switch (At(i))
			{
			case Tok::Star:
				type = m_types.Pointer(type);
				break;
			case Tok::Amp:
				type = m_types.LRef(type);
				break;
			case Tok::AmpAmp:
				type = m_types.RRef(type);
				break;
			case Tok::KwConst:
				type = m_types.Const(type);
				break;
			case Tok::KwVolatile:
				break;
			default:
				return TypeTable::Unknown;
			}
		}

		return type;
	}

	TypeId TyperImpl::ParseTypeTokens(
		std::size_t begin,
		std::size_t end,
		ScopeId scope,
		std::uint32_t before)
	{
		const auto base = ParseBase(begin, end, scope, before);
		if (!base.ok || base.is_auto)
		{
			return TypeTable::Unknown;
		}

		return ApplyOperators(base.type, base.next, end);
	}

	// `auto` / `const auto` with the pointer and reference operators written in
	// the declarator, from the type and value category of the initializer
	// (template argument deduction, [dcl.type.auto.deduct]).
	TypeId TyperImpl::DeduceAuto(
		const Base& base,
		std::size_t begin,
		std::size_t end,
		TypeId init,
		Category category)
	{
		if (init == TypeTable::Unknown)
		{
			return TypeTable::Unknown;
		}

		int stars          = 0;
		bool lref          = false;
		bool rref          = false;
		bool pointer_const = false;
		for (std::size_t i = begin; i < end; ++i)
		{
			switch (At(i))
			{
			case Tok::Amp:
				lref = true;
				break;
			case Tok::AmpAmp:
				rref = true;
				break;
			case Tok::Star:
				++stars;
				break;
			case Tok::KwConst:
				pointer_const = pointer_const || stars > 0;
				break;
			case Tok::KwVolatile:
				break;
			default:
				return TypeTable::Unknown;
			}
		}

		if (stars > 1 ||(lref&& rref) ||(stars == 1 && (lref || rref)))
		{
			return TypeTable::Unknown; // `auto**`, `auto*&`: not modelled
		}

		if (stars == 1)
		{
			// `auto* p = e;` needs e to be a pointer: `const auto*` makes the pointee const.
			const auto deduced = m_types.Decay(m_types.Strip(init));
			if (!m_types.IsPointer(deduced))
			{
				return TypeTable::Unknown;
			}

			const auto pointee = m_types.Element(deduced);
			auto result        = m_types.Pointer(base.is_const ? m_types.Const(pointee) : pointee);
			return pointer_const ? m_types.Const(result) : result;
		}

		if (rref)
		{
			if (base.is_const)
			{
				// `const auto&&` is no forwarding reference: it binds rvalues only.
				return category == Category::PRValue ? m_types.RRef(m_types.Const(init))
				: TypeTable::Unknown;
			}

			switch (category)
			{
			case Category::LValue:
				return m_types.LRef(init);
			case Category::PRValue:
				return m_types.RRef(init);
			default:
				return TypeTable::Unknown;
			}
		}

		if (lref)
		{
			return m_types.LRef(
				base.is_const ? m_types.Const(m_types.Value(init)) : m_types.Value(init));
		}

		// `auto x = e;` takes e's type without references, top const or arrays.
		const auto type = m_types.Decay(m_types.Strip(init));
		return base.is_const ? m_types.Const(type) : type;
	}

	// The type declared by `declarator` for the written base type `base`.
	TypeId TyperImpl::DeclaratorType(
		Base base,
		std::uint32_t declarator,
		SymbolKind kind,
		TypeId init_type,
		Category category)
	{
		if (declarator == kNone || FindChild(declarator, GrammarKind::FunctionSuffix) != kNone)
		{
			return TypeTable::Unknown;
		}

		const auto name = FindChild(declarator, GrammarKind::DeclaredName);
		if (name == kNone)
		{
			return TypeTable::Unknown;
		}

		const auto prefix_begin = Pos(m_nodes.FirstToken(declarator));
		const auto prefix_end   = Pos(m_nodes.FirstToken(name));
		TypeId type             = base.type;
		if (base.is_auto)
		{
			type = DeduceAuto(base, prefix_begin, prefix_end, init_type, category);
			if (type == TypeTable::Unknown)
			{
				return TypeTable::Unknown;
			}
		}
		else
		{
			type = ApplyOperators(type, prefix_begin, prefix_end);
		}

		// Array suffixes wrap the element type, the last one innermost.
		std::vector<std::uint32_t> suffixes;
		for (const auto child : m_model.ChildrenOf(declarator))
		{
			if (m_nodes.Kind(child) == GrammarKind::ArraySuffix)
			{
				suffixes.push_back(child);
			}
		}

		for (auto it = suffixes.rbegin(); it != suffixes.rend(); ++it)
		{
			const auto[b, e]     = NodeSig(*it);
			std::uint32_t extent = kNone;
			if (e == b + 3 && At(b) == Tok::LBracket &&
				Tokens()[m_sig[b + 1]].kind == TokenKind::Number)
			{
				const auto text     = TextAt(b + 1);
				std::uint64_t value = 0;
				bool digits         = !text.empty() && text.size() < 10;
				for (const char c : text)
				{
					digits = digits && c >= '0' && c <= '9';
					value  = value * 10 + static_cast<std::uint64_t>(c - '0');
				}

				if (digits)
				{
					extent = static_cast<std::uint32_t>(value);
				}
			}

			if (extent == kNone && kind != SymbolKind::Parameter)
			{
				for (auto pos = b + 1; pos + 1 < e; ++pos)
				{
					if (!WordAt(pos))
					{
						continue;
					}

					const auto symbol = m_model.Lookup(m_model.ScopeOfNode(declarator),
						m_model.Names().Find(TextAt(pos)), m_sig[pos]);
					if (symbol != kNone && m_symbols.kind[symbol] == SymbolKind::TemplateParameter)
					{
						return TypeTable::Unknown;
					}
				}
			}

			type = m_types.Array(type, extent);
		}

		// A parameter declared as an array is a pointer.
		if (kind == SymbolKind::Parameter && m_types.Kind(type) == TypeKind::Array)
		{
			type = m_types.Pointer(m_types.Arg(type));
		}

		return type;
	}

	TypeId TyperImpl::LambdaReturnType(std::uint32_t node)
	{
		const auto[begin, end] = NodeSig(node);
		for (auto position = begin; position < end; ++position)
		{
			if (At(position) == Tok::LParen || At(position) == Tok::LBracket)
			{
				const detail::TokenView view(m_model);
				position = view.Match(position, end);
				continue;
			}

			if (At(position) == Tok::Arrow)
			{
				auto stop = position + 1;
				while (stop < end && At(stop) != Tok::LBrace)
				{
					++stop;
				}

				return ParseTypeTokens(position + 1, stop, m_model.ScopeOfNode(node),
					m_nodes.FirstToken(node));
			}

			if (At(position) == Tok::LBrace)
			{
				break;
			}
		}

		return DeducedReturn(node, false);
	}

	TypeId TyperImpl::ParameterType(std::uint32_t node)
	{
		const auto spec = FindChild(node, GrammarKind::TypeSpecifier);
		if (spec == kNone)
		{
			return TypeTable::Unknown;
		}

		const auto[begin, end] = NodeSig(spec);
		auto base              = ParseBase(begin, end, m_model.ScopeOfNode(node), m_nodes.FirstToken(node));
		if (!base.ok || base.is_auto)
		{
			return TypeTable::Unknown;
		}

		if (base.next < end)
		{
			base.type = ApplyOperators(base.type, base.next, end);
		}

		const auto declarator = FindChild(node, GrammarKind::Declarator);
		if (declarator != kNone && FindChild(declarator, GrammarKind::DeclaredName) == kNone)
		{
			const auto[parameterBegin, parameterEnd] = NodeSig(node);
			auto typeEnd                             = parameterEnd;
			for (auto position = parameterBegin; position < parameterEnd; ++position)
			{
				if (At(position) == Tok::Eq)
				{
					typeEnd = position;
					break;
				}
			}

			return ParseTypeTokens(parameterBegin, typeEnd, m_model.ScopeOfNode(node),
				m_nodes.FirstToken(node));
		}

		return declarator == kNone ? base.type
		: DeclaratorType(base, declarator, SymbolKind::Parameter,
			TypeTable::Unknown, Category::Unknown);
	}

	TypeId TyperImpl::VariableType(SymbolId symbol)
	{
		const auto node = m_symbols.decl_node[symbol];
		if (node >= m_nodes.size())
		{
			return TypeTable::Unknown;
		}

		const auto kind = m_nodes.Kind(node);
		if (kind != GrammarKind::Declaration && kind != GrammarKind::DeclarationStatement &&
			kind != GrammarKind::ParameterDeclaration)
		{
			return TypeTable::Unknown;
		}

		const auto spec = FindChild(node, GrammarKind::TypeSpecifier);
		if (spec == kNone)
		{
			return TypeTable::Unknown;
		}

		std::uint32_t declarator = kNone;
		std::uint32_t init       = kNone;
		for (const auto child : m_model.ChildrenOf(node))
		{
			std::uint32_t candidate = kNone;
			if (m_nodes.Kind(child) == GrammarKind::InitDeclarator)
			{
				candidate = FindChild(child, GrammarKind::Declarator);
			}
			else if (m_nodes.Kind(child) == GrammarKind::Declarator)
			{
				candidate = child;
			}

			if (candidate == kNone)
			{
				continue;
			}

			const auto name = FindChild(candidate, GrammarKind::DeclaredName);
			if (name != kNone && m_nodes.FirstToken(name) == m_symbols.decl_token[symbol])
			{
				declarator = candidate;
				init       = m_nodes.Kind(child) == GrammarKind::InitDeclarator ? child : kNone;
				break;
			}
		}

		if (declarator == kNone)
		{
			return TypeTable::Unknown;
		}

		const auto[spec_begin, spec_end] = NodeSig(spec);
		const auto scope                 = m_model.ScopeOfNode(node);
		auto base                        = ParseBase(spec_begin, spec_end, scope, m_symbols.decl_token[symbol]);
		if (!base.ok)
		{
			return TypeTable::Unknown;
		}

		// Operators the grammar left inside the specifier.
		if (base.next < spec_end)
		{
			if (base.is_auto)
			{
				return TypeTable::Unknown;
			}

			base.type = ApplyOperators(base.type, base.next, spec_end);
		}

		TypeId init_type  = TypeTable::Unknown;
		Category category = Category::Unknown;
		if (base.is_auto)
		{
			std::tie(init_type, category) =
				AutoInitializer(node, init, Pos(m_symbols.decl_token[symbol]));
		}

		return DeclaratorType(base, declarator, m_symbols.kind[symbol], init_type, category);
	}

	// `new` has no dedicated node: the grammar leaves the keyword, the
	// allocated type and the initializer as siblings. Read the allocated
	// type from the tokens so `auto p = new T...` deduces `T*`. Placement
	// arguments, array extents and direct/braced initializers only end the
	// type; anything unresolvable stays Unknown.
	std::pair<TypeId, TyperImpl::Category> TyperImpl::NewExpressionType(
		std::size_t new_pos,
		std::size_t end,
		ScopeId scope)
	{
		const std::pair<TypeId, Category> unknown {TypeTable::Unknown, Category::Unknown};
		if (At(new_pos) != Tok::KwNew)
		{
			return unknown;
		}

		const auto before = m_sig[new_pos];
		std::size_t pos   = new_pos + 1;
		if (At(pos) == Tok::LParen)
		{
			pos = MatchParen(pos, end) + 1; // placement arguments
		}

		if (pos >= end)
		{
			return unknown;
		}

		// The allocated type ends at the initializer or array extent, skipping
		// template argument lists as a unit.
		const auto type_begin = pos;
		auto type_end         = pos;
		while (type_end < end)
		{
			const Tok tok = At(type_end);
			if (tok == Tok::Lt)
			{
				const auto close = MatchAngle(type_end, end);
				if (close >= end)
				{
					return unknown;
				}

				type_end = close + 1;
				continue;
			}

			if (tok == Tok::LParen || tok == Tok::LBrace || tok == Tok::LBracket ||
				tok == Tok::Semi)
			{
				break;
			}

			++type_end;
		}

		if (type_end <= type_begin)
		{
			return unknown;
		}

		const auto allocated = ParseTypeTokens(type_begin, type_end, scope, before);
		if (allocated == TypeTable::Unknown)
		{
			return unknown;
		}

		const auto kind = m_types.Kind(m_types.Strip(allocated));
		if (kind == TypeKind::LRef || kind == TypeKind::RRef || kind == TypeKind::Unknown)
		{
			return unknown;
		}

		return {m_types.Pointer(allocated), Category::PRValue};
	}

	// `= e`, `{e}`, `(e)` after the name, or the range of a range-for.
	std::pair<TypeId, TyperImpl::Category> TyperImpl::AutoInitializer(
		std::uint32_t node,
		std::uint32_t init,
		std::size_t name_pos)
	{
		const std::pair<TypeId, Category> unknown {TypeTable::Unknown, Category::Unknown};
		if (m_nodes.Kind(node) == GrammarKind::ParameterDeclaration)
		{
			return unknown;
		}

		if (At(name_pos + 1) == Tok::Eq && init != kNone)
		{
			// `auto p = new T...`: `new` has no dedicated node, so no child
			// spans the whole initializer. Deduce from the tokens instead.
			const auto first = name_pos + 2;
			if (At(first) == Tok::KwNew)
			{
				const auto allocated = NewExpressionType(
					first, NodeSig(init).second, m_model.ScopeOfNode(node));
				if (allocated.first != TypeTable::Unknown)
				{
					return allocated;
				}
			}

			const auto init_end = NodeSig(init).second;
			for (const auto child : m_model.ChildrenOf(init))
			{
				if (m_nodes.Kind(child) == GrammarKind::Declarator ||
					m_nodes.Kind(child) == GrammarKind::TypeSpecifier)
				{
					continue;
				}

				const auto[cb, ce] = NodeSig(child);
				if (cb == name_pos + 2 && ce == init_end)
				{
					return {NodeType(child), ValueCategory(child)};
				}
			}

			return unknown;
		}

		// `auto x{e};` keeps the initializer out of the tree: only a single literal
		// or variable is typed. Two elements are ill-formed. (`auto x(e);` is bound as
		// a function declaration, so it never gets here.)
		const Tok open = At(name_pos + 1);
		if (open == Tok::LBrace && At(name_pos + 3) == Tok::RBrace)
		{
			return OperandAt(name_pos + 2, m_model.ScopeOfNode(node));
		}

		// `for (auto x : range)`: the declaration ends at the `:`.
		const auto parent = m_nodes.Parent(node);
		if (open == Tok::Colon && m_nodes.Kind(node) == GrammarKind::DeclarationStatement &&
			parent < m_nodes.size() && m_nodes.Kind(parent) == GrammarKind::LoopStatement)
		{
			const auto range_begin = NodeSig(node).second + 1;
			for (const auto child : m_model.ChildrenOf(parent))
			{
				if (m_nodes.Kind(child) == GrammarKind::DeclarationStatement ||
					NodeSig(child).first != range_begin)
				{
					continue;
				}

				// Arrays only: the element of a library container is not modelled.
				auto range = m_types.Value(NodeType(child));
				if (m_types.Kind(range) == TypeKind::Const)
				{
					range = m_types.Arg(range);
				}

				if (!m_types.IsArray(range))
				{
					return unknown;
				}

				return {m_types.Element(range), Category::LValue};
			}
		}

		return unknown;
	}

	std::pair<TypeId, TyperImpl::Category> TyperImpl::OperandAt(std::size_t pos, ScopeId scope)
	{
		if (pos >= m_sig.size())
		{
			return {TypeTable::Unknown, Category::Unknown};
		}

		const auto token = m_sig[pos];
		if (Tokens()[token].kind == TokenKind::Identifier && Tokens()[token].tok == Tok::None)
		{
			// The grammar builds no expression node here, so the name is looked up.
			const auto name   = m_model.Names().Find(Text(token));
			const auto symbol = name == kNone ? kNone : m_model.Lookup(scope, name, token);
			if (symbol == kNone ||(m_symbols.kind[symbol] != SymbolKind::Variable &&
				m_symbols.kind[symbol] != SymbolKind::Parameter))
			{
				return {TypeTable::Unknown, Category::Unknown};
			}

			return {m_types.Value(SymbolType(symbol)), Category::LValue};
		}

		const auto type = LiteralTypeAt(pos);
		const bool string = Tokens()[token].kind == TokenKind::StringLiteral ||
			Tokens()[token].kind == TokenKind::RawStringLiteral;
		return {type,
			type != TypeTable::Unknown && !string ? Category::PRValue : Category::Unknown};
	}

	TyperImpl::Category TyperImpl::ValueCategory(std::uint32_t node)
	{
		if (node >= m_nodes.size() || NodeType(node) == TypeTable::Unknown)
		{
			return Category::Unknown;
		}

		const auto[begin, end] = NodeSig(node);
		const auto kids        = m_model.ChildrenOf(node);
		switch (m_nodes.Kind(node))
		{
		case GrammarKind::LiteralExpression:
			return OperandAt(begin, m_model.ScopeOfNode(node)).second;
		case GrammarKind::IdentifierExpression:
		{
			if (end != begin + 1)
			{
				return Category::Unknown;
			}

			const auto tok = At(begin);
			if (tok == Tok::KwTrue || tok == Tok::KwFalse || tok == Tok::KwNullptr ||
				tok == Tok::KwThis)
			{
				return Category::PRValue;
			}

			return OperandAt(begin, m_model.ScopeOfNode(node)).second;
		}
		case GrammarKind::ParenthesizedExpression:
			return kids.size() == 1 ? ValueCategory(kids[0]) : Category::Unknown;
		case GrammarKind::CallExpression:
			for (const auto& call : m_out.mTemplateCalls)
			{
				if (call.node == node && call.result.state == SubstitutionState::Success)
				{
					return m_types.Kind(call.result.returnType) == TypeKind::LRef
					? Category::LValue : Category::PRValue;
				}
			}

			return Category::Unknown;
		case GrammarKind::CastExpression:
			return Category::PRValue; // `(T&)x` would be an lvalue, but a cast to a reference
			// is not modeled
		case GrammarKind::UnaryExpression:
		{
			if (kids.empty())
			{
				return Category::Unknown;
			}

			if (Pos(m_nodes.FirstToken(kids[0])) != begin + 1)
			{
				return Category::PRValue; // `x++`
			}

			const Tok first = At(begin);
			return first == Tok::Star || first == Tok::PlusPlus || first == Tok::MinusMinus
			? Category::LValue
			: Category::PRValue;
		}
		case GrammarKind::SubscriptExpression:
		{
			if (kids.empty())
			{
				return Category::Unknown;
			}

			const auto base = m_types.Strip(NodeType(kids[0]));
			return m_types.IsArray(base) || m_types.IsPointer(base)
			? Category::LValue
			: Category::Unknown;
		}
		case GrammarKind::BinaryExpression:
		{
			if (kids.size() != 2 ||!SameStart(kids[0], node))
			{
				return Category::Unknown;
			}

			const Tok op = At(NodeSig(kids[0]).second);
			if (op == Tok::Comma)
			{
				return Category::Unknown;
			}

			return IsAssignment(op) ? Category::LValue : Category::PRValue;
		}
		default:
			return Category::Unknown;
		}
	}

	TypeId TyperImpl::DeducedReturn(std::uint32_t function, bool is_const)
	{
		TypeId deduced = TypeTable::Unknown;
		bool first     = true;
		const auto end = m_nodes.SubtreeEnd(function);
		for (std::uint32_t node = function + 1; node < end;)
		{
			const auto kind = m_nodes.Kind(node);
			// A return in a nested function, class or lambda is not ours.
			if (kind == GrammarKind::LambdaExpression || kind == GrammarKind::FunctionDefinition ||
				kind == GrammarKind::RecordDefinition)
			{
				node = std::max<std::uint32_t>(node + 1, m_nodes.SubtreeEnd(node));
				continue;
			}

			if (kind != GrammarKind::ReturnStatement || At(NodeSig(node).first) != Tok::KwReturn)
			{
				++node;
				continue;
			}

			const auto[begin, stop] = NodeSig(node);
			TypeId type             = TypeTable::Unknown;
			if (begin + 1 < stop && At(begin + 1) == Tok::Semi)
			{
				type = m_types.Builtin(BuiltinType::Void);
			}
			else
			{
				for (const auto child : m_model.ChildrenOf(node))
				{
					if (NodeSig(child).first == begin + 1)
					{
						type = m_types.Decay(m_types.Strip(NodeType(child)));
						break;
					}
				}
			}

			// The first return fixes the type (a recursive call reads Unknown until
			// it does); a later one that disagrees makes the program ill-formed.
			if (first)
			{
				if (type == TypeTable::Unknown)
				{
					return TypeTable::Unknown;
				}

				deduced = type;
				first   = false;
			}
			else if (type != TypeTable::Unknown && type != deduced)
			{
				return TypeTable::Unknown;
			}

			node = std::max<std::uint32_t>(node + 1, m_nodes.SubtreeEnd(node));
		}

		if (first)
		{
			deduced = m_types.Builtin(BuiltinType::Void); // no return: falls off the end
		}

		return is_const ? m_types.Const(deduced) : deduced;
	}

	TypeId TyperImpl::ReturnType(SymbolId symbol)
	{
		if ((m_symbols.flags[symbol] & (SymbolFlag::Constructor | SymbolFlag::Destructor)) != 0)
		{
			return TypeTable::Unknown;
		}

		const auto node = m_symbols.decl_node[symbol];
		if (node >= m_nodes.size() ||(m_nodes.Kind(node) != GrammarKind::FunctionDeclaration &&
			m_nodes.Kind(node) != GrammarKind::FunctionDefinition))
		{
			return TypeTable::Unknown;
		}

		const auto spec       = FindChild(node, GrammarKind::TypeSpecifier);
		const auto declarator = FindChild(node, GrammarKind::Declarator);
		if (spec == kNone || declarator == kNone)
		{
			return TypeTable::Unknown;
		}

		const auto[spec_begin, spec_end] = NodeSig(spec);
		const auto scope                 = m_model.ScopeOfNode(node);
		const auto base                  = ParseBase(spec_begin, spec_end, scope, m_symbols.decl_token[symbol]);
		if (!base.ok)
		{
			return TypeTable::Unknown;
		}

		if (base.is_auto)
		{
			auto suffix   = FindChild(declarator, GrammarKind::FunctionSuffix);
			auto trailing = FindChild(declarator, GrammarKind::TrailingReturnType);
			if (trailing == kNone && suffix != kNone)
			{
				trailing = FindChild(suffix, GrammarKind::TrailingReturnType);
			}

			if (trailing == kNone)
			{
				if ((m_symbols.flags[symbol] & SymbolFlag::Template) != 0)
				{
					return TypeTable::Unknown;
				}

				// Deduced from the body: plain `auto` / `const auto` only.
				const auto declared = FindChild(declarator, GrammarKind::DeclaredName);
				if (m_nodes.Kind(node) != GrammarKind::FunctionDefinition || declared == kNone ||
					base.next < spec_end ||
					m_nodes.FirstToken(declared) != m_nodes.FirstToken(declarator))
				{
					return TypeTable::Unknown;
				}

				return DeducedReturn(node, base.is_const);
			}

			std::size_t begin = NodeSig(trailing).first;
			if (At(begin) == Tok::Arrow)
			{
				++begin;
			}

			std::size_t end = begin;
			while (end < m_sig.size() && At(end) != Tok::LBrace && At(end) != Tok::Semi &&
				At(end) != Tok::Eq && At(end) != Tok::KwRequires && At(end) != Tok::KwOverride &&
				At(end) != Tok::KwFinal)
			{
				if (At(end) == Tok::LParen || At(end) == Tok::LBracket)
				{
					end = MatchParen(end, m_sig.size());
				}

				++end;
			}

			return ParseTypeTokens(
				begin, std::min(end, m_sig.size()), scope, m_symbols.decl_token[symbol]);
		}

		const auto name = FindChild(declarator, GrammarKind::DeclaredName);
		if (name == kNone)
		{
			return TypeTable::Unknown;
		}

		const auto prefix_begin = Pos(m_nodes.FirstToken(declarator));
		const auto prefix_end   = Pos(m_nodes.FirstToken(name));
		TypeId type             = base.type;
		if (base.next < spec_end)
		{
			type = ApplyOperators(type, base.next, spec_end);
		}

		// A qualified name or function-pointer declarator leaves other tokens: Unknown.
		return ApplyOperators(type, prefix_begin, prefix_end);
	}

	TypeId TyperImpl::AliasTarget(SymbolId symbol)
	{
		const auto node = m_symbols.decl_node[symbol];
		if (node >= m_nodes.size())
		{
			return TypeTable::Unknown;
		}

		if (m_nodes.Kind(node) == GrammarKind::UsingDeclaration)
		{
			const auto[begin, end] = NodeSig(node);
			const auto name_pos    = Pos(m_symbols.decl_token[symbol]);
			if (name_pos + 2 >= end || At(name_pos + 1) != Tok::Eq)
			{
				return TypeTable::Unknown;
			}

			std::size_t stop = name_pos + 2;
			while (stop < end && At(stop) != Tok::Semi)
			{
				++stop;
			}

			(void) begin;
			return ParseTypeTokens(
				name_pos + 2, stop, m_model.ScopeOfNode(node), m_symbols.decl_token[symbol]);
		}

		// `typedef T Name;` reads like a variable declaration.
		return VariableType(symbol);
	}

	// The return type shared by every overload of `symbol`'s name in its scope.
	TypeId TyperImpl::OverloadReturn(SymbolId symbol)
	{
		TypeId shared = TypeTable::Unknown;
		bool first    = true;
		for (auto candidate = m_model.LookupLocal(m_symbols.scope[symbol], m_symbols.name[symbol]);
			candidate != kNone;
			candidate = m_symbols.next_same_name[candidate])
		{
			if (m_symbols.kind[candidate] != SymbolKind::Function)
			{
				return TypeTable::Unknown;
			}

			if ((m_symbols.flags[candidate] & SymbolFlag::Template) != 0)
			{
				return TypeTable::Unknown;
			}

			const auto type = SymbolType(candidate);
			if (type == TypeTable::Unknown ||(!first && type != shared))
			{
				return TypeTable::Unknown;
			}

			shared = type;
			first  = false;
		}

		return shared;
	}

	// ---- expressions --------------------------------------------------------------
	bool TyperImpl::IsScalar(TypeId type) const
	{
		const auto kind = m_types.Kind(type);
		if (kind == TypeKind::Builtin)
		{
			return !m_types.IsBuiltin(type, BuiltinType::Void);
		}

		return kind == TypeKind::Pointer || kind == TypeKind::Enum;
	}

	ScopeId TyperImpl::EnclosingClass(ScopeId scope) const
	{
		const auto& scopes = m_model.Scopes();
		for (std::size_t steps = 0;
			scope != kNone && scope < scopes.Size() && steps <= scopes.Size(); ++steps)
		{
			if (scopes.kind[scope] == ScopeKind::Class)
			{
				return scope;
			}

			if (scope == SemanticModel::TranslationUnitScope)
			{
				break;
			}

			scope = scopes.parent[scope];
		}

		return kNone;
	}

	TypeId TyperImpl::Promoted(TypeId type)
	{
		type = m_types.Strip(type);
		if (!m_types.IsArithmetic(type))
		{
			return TypeTable::Unknown;
		}

		const auto builtin = static_cast<BuiltinType>(m_types.Arg(type));
		if (m_types.IsFloating(type) || builtin == BuiltinType::SizeT)
		{
			return type;
		}

		const auto promoted = Promote(builtin);
		return promoted ? m_types.Builtin(*promoted) : TypeTable::Unknown;
	}

	// Usual arithmetic conversions. Unknown whenever the result depends on the
	// platform's type widths.
	TypeId TyperImpl::Arithmetic(TypeId a, TypeId b)
	{
		a = m_types.Strip(a);
		b = m_types.Strip(b);
		if (!m_types.IsArithmetic(a) ||!m_types.IsArithmetic(b))
		{
			return TypeTable::Unknown;
		}

		const auto ba = static_cast<BuiltinType>(m_types.Arg(a));
		const auto bb = static_cast<BuiltinType>(m_types.Arg(b));
		for (const auto floating :
			{
				BuiltinType::LongDouble, BuiltinType::Double, BuiltinType::Float
		})
		{
			if (ba == floating || bb == floating)
			{
				return m_types.Builtin(floating);
			}
		}

		if (ba == BuiltinType::SizeT || bb == BuiltinType::SizeT)
		{
			return m_types.Builtin(BuiltinType::SizeT);
		}

		const auto pa = Promote(ba);
		const auto pb = Promote(bb);
		if (!pa ||!pb)
		{
			return TypeTable::Unknown;
		}

		if (*pa == *pb)
		{
			return m_types.Builtin(*pa);
		}

		if (IsUnsignedInt(*pa) == IsUnsignedInt(*pb))
		{
			return m_types.Builtin(Rank(*pa) >= Rank(*pb) ? *pa : *pb);
		}

		const auto unsigned_type = IsUnsignedInt(*pa) ? *pa : *pb;
		const auto signed_type   = IsUnsignedInt(*pa) ? *pb : *pa;
		if (Rank(unsigned_type) >= Rank(signed_type))
		{
			return m_types.Builtin(unsigned_type);
		}

		return signed_type == BuiltinType::LongLong && unsigned_type == BuiltinType::UInt
		? m_types.Builtin(signed_type)
		: TypeTable::Unknown;
	}

	TypeId TyperImpl::NodeType(std::uint32_t node)
	{
		if (node >= m_nodes.size())
		{
			return TypeTable::Unknown;
		}

		auto& slot = m_out.m_node_type[node];
		if (slot != kPending)
		{
			return slot;
		}

		if (m_depth >= kMaxDepth)
		{
			return TypeTable::Unknown;
		}

		slot = TypeTable::Unknown;
		++m_depth;
		const auto result = m_types.Value(ComputeNodeType(node));
		--m_depth;
		m_out.m_node_type[node] = result;
		return result;
	}

	TypeId TyperImpl::ComputeNodeType(std::uint32_t node)
	{
		switch (m_nodes.Kind(node))
		{
		case GrammarKind::LiteralExpression:
			return LiteralType(node);
		case GrammarKind::IdentifierExpression:
			return IdentifierType(node);
		case GrammarKind::ParenthesizedExpression:
		{
			const auto kids = m_model.ChildrenOf(node);
			return kids.size() == 1 ? NodeType(kids[0]) : TypeTable::Unknown;
		}
		case GrammarKind::CastExpression:
		{
			// `(T) operand` is a T: the parser only builds the node when T is certainly a type.
			const auto kids = m_model.ChildrenOf(node);
			if (kids.empty() || m_nodes.Kind(kids[0]) != GrammarKind::TypeSpecifier)
			{
				return TypeTable::Unknown;
			}

			NodeType(kids.back());
			const auto[begin, end] = NodeSig(kids[0]);
			return ParseTypeTokens(begin, end, m_model.ScopeOfNode(node), m_sig[begin]);
		}
		case GrammarKind::UnaryExpression:
			return UnaryType(node);
		case GrammarKind::BinaryExpression:
			return BinaryType(node);
		case GrammarKind::ConditionalExpression:
			return ConditionalType(node);
		case GrammarKind::CallExpression:
			return CallType(node);
		case GrammarKind::SubscriptExpression:
			return SubscriptType(node);
		case GrammarKind::MemberExpression:
			return MemberType(node, false);
		default:
			return TypeTable::Unknown;
		}
	}

	TypeId TyperImpl::LiteralType(std::uint32_t node)
	{
		const auto[begin, end] = NodeSig(node);
		return begin < end ? LiteralTypeAt(begin) : TypeTable::Unknown;
	}

	TypeId TyperImpl::LiteralTypeAt(std::size_t begin)
	{
		const auto& token = Tokens()[m_sig[begin]];
		switch (token.tok)
		{
		case Tok::KwTrue:
		case Tok::KwFalse:
			return m_types.Builtin(BuiltinType::Bool);
		case Tok::KwNullptr:
			return m_types.Builtin(BuiltinType::NullptrT);
		default:
			break;
		}

		switch (token.kind)
		{
		case TokenKind::Number:
			return NumberType(Text(m_sig[begin]));
		case TokenKind::CharacterLiteral:
			return CharType(Text(m_sig[begin]));
		case TokenKind::StringLiteral:
		case TokenKind::RawStringLiteral:
		{
			// Plain narrow literals only: `"..."` and `R"(...)"`.
			const auto text = Text(m_sig[begin]);
			if (!text.empty() &&
				(text[0] == '"' ||(text[0] == 'R' && text.size() > 1 && text[1] == '"')))
			{
				return m_types.Pointer(m_types.Const(m_types.Builtin(BuiltinType::Char)));
			}

			return TypeTable::Unknown;
		}
		default:
			return TypeTable::Unknown;
		}
	}

	TypeId TyperImpl::IdentifierType(std::uint32_t node)
	{
		const auto[begin, end] = NodeSig(node);
		if (end != begin + 1)
		{
			return TypeTable::Unknown;
		}

		const auto token = m_sig[begin];
		// The grammar reads these keywords as identifiers.
		switch (Tokens()[token].tok)
		{
		case Tok::KwTrue:
		case Tok::KwFalse:
			return m_types.Builtin(BuiltinType::Bool);
		case Tok::KwNullptr:
			return m_types.Builtin(BuiltinType::NullptrT);
		default:
			break;
		}

		if (Tokens()[token].tok == Tok::KwThis)
		{
			const auto klass = EnclosingClass(m_model.ScopeOfNode(node));
			if (klass == kNone || m_model.Scopes().owner[klass] == kNone)
			{
				return TypeTable::Unknown;
			}

			const auto owner       = m_model.Scopes().owner[klass];
			const auto declaration = m_model.TemplateOf(owner);
			return m_types.Pointer(declaration != kNone && m_model.Templates().kind[declaration] != TemplateDeclarationKind::Explicit
				? m_types.InjectedClass(owner) : m_types.Class(owner));
		}

		const auto symbol = m_model.ResolveToken(token);
		if (symbol == kNone)
		{
			return TypeTable::Unknown;
		}

		const auto kind = m_symbols.kind[symbol];
		if (kind == SymbolKind::TemplateParameter)
		{
			const auto parameter = m_model.ParameterOf(symbol);
			return parameter != kNone && m_model.TemplateParameters().kind[parameter] == TemplateParameterKind::Type
			? m_types.Parameter(parameter) : TypeTable::Unknown;
		}

		return kind == SymbolKind::Variable || kind == SymbolKind::Parameter
		? SymbolType(symbol)
		: TypeTable::Unknown;
	}

	TypeId TyperImpl::UnaryType(std::uint32_t node)
	{
		const auto kids        = m_model.ChildrenOf(node);
		const auto[begin, end] = NodeSig(node);
		if (kids.empty() || end <= begin)
		{
			return TypeTable::Unknown;
		}

		const Tok first = At(begin);
		if (first == Tok::CaretCaret &&
			m_nodes.Kind(kids[0]) != GrammarKind::ErrorExpression)
		{
			return m_types.External(m_out.m_externals.InternCopy("std::meta::info"));
		}

		const bool prefix =
			(first == Tok::Plus || first == Tok::Minus || first == Tok::Bang ||
			first == Tok::Tilde || first == Tok::Star || first == Tok::Amp ||
			first == Tok::PlusPlus || first == Tok::MinusMinus) &&
			Pos(m_nodes.FirstToken(kids[0])) == begin + 1;
		TypeId operand = TypeTable::Unknown;
		Tok op         = Tok::None;
		if (prefix)
		{
			op      = first;
			operand = NodeType(kids[0]);
		}
		else
		{
			const Tok last = At(end - 1);
			if ((last != Tok::PlusPlus && last != Tok::MinusMinus) ||!SameStart(kids[0], node))
			{
				return TypeTable::Unknown;
			}

			op      = last;
			operand = NodeType(kids[0]);
		}

		const auto stripped = m_types.Strip(operand);
		switch (op)
		{
		case Tok::Bang:
			return IsScalar(stripped) || m_types.IsArray(stripped)
			? m_types.Builtin(BuiltinType::Bool)
			: TypeTable::Unknown;
		case Tok::Plus:
		case Tok::Minus:
			return m_types.IsPointer(stripped) && op == Tok::Plus
			? stripped
			: Promoted(stripped);
		case Tok::Tilde:
			return m_types.IsInteger(stripped) ? Promoted(stripped) : TypeTable::Unknown;
		case Tok::Star:
			if (m_types.IsPointer(stripped) || m_types.IsArray(stripped))
			{
				return m_types.Value(m_types.Element(stripped));
			}

			return TypeTable::Unknown;
		case Tok::Amp:
			if (operand == TypeTable::Unknown || m_types.Kind(stripped) == TypeKind::Class ||
				m_types.Kind(stripped) == TypeKind::External)
			{
				return TypeTable::Unknown; // `operator&` may be overloaded
			}

			return m_types.Pointer(m_types.Value(operand));
		case Tok::PlusPlus:
		case Tok::MinusMinus:
			return m_types.IsArithmetic(stripped) || m_types.IsPointer(stripped)
			? stripped
			: TypeTable::Unknown;
		default:
			return TypeTable::Unknown;
		}
	}

	TypeId TyperImpl::BinaryType(std::uint32_t node)
	{
		const auto kids = m_model.ChildrenOf(node);
		if (kids.size() != 2 ||!SameStart(kids[0], node))
		{
			return TypeTable::Unknown;
		}

		const auto op_pos     = NodeSig(kids[0]).second;
		const Tok op          = At(op_pos);
		const auto left_type  = NodeType(kids[0]);
		const auto right_type = NodeType(kids[1]);
		if (op == Tok::Comma)
		{
			return right_type;
		}

		const auto left    = m_types.Decay(m_types.Strip(left_type));
		const auto right   = m_types.Decay(m_types.Strip(right_type));
		const auto boolean = m_types.Builtin(BuiltinType::Bool);
		if (IsAssignment(op))
		{
			return IsScalar(left) ? m_types.Value(left_type) : TypeTable::Unknown;
		}

		switch (op)
		{
		case Tok::AmpAmp:
		case Tok::PipePipe:
		case Tok::EqEq:
		case Tok::BangEq:
		case Tok::Lt:
		case Tok::Gt:
		case Tok::Le:
		case Tok::Ge:
			return IsScalar(left) && IsScalar(right) ? boolean : TypeTable::Unknown;
		case Tok::Plus:
			if (m_types.IsPointer(left) && m_types.IsInteger(right))
			{
				return left;
			}

			if (m_types.IsInteger(left) && m_types.IsPointer(right))
			{
				return right;
			}

			return Arithmetic(left, right);
		case Tok::Minus:
			if (m_types.IsPointer(left) && m_types.IsInteger(right))
			{
				return left;
			}

			return Arithmetic(left, right);
		case Tok::Star:
		case Tok::Slash:
			return Arithmetic(left, right);
		case Tok::Percent:
			return m_types.IsInteger(left) && m_types.IsInteger(right)
			? Arithmetic(left, right)
			: TypeTable::Unknown;
		case Tok::Shl:
		case Tok::Shr:
			return m_types.IsInteger(left) && m_types.IsInteger(right)
			? Promoted(left)
			: TypeTable::Unknown;
		case Tok::Amp:
		case Tok::Pipe:
		case Tok::Caret:
			return (m_types.IsInteger(left) || m_types.IsBool(left)) &&
				(m_types.IsInteger(right) || m_types.IsBool(right))
			? Arithmetic(left, right)
			: TypeTable::Unknown;
		default:
			return TypeTable::Unknown;
		}
	}

	TypeId TyperImpl::ConditionalType(std::uint32_t node)
	{
		const auto kids = m_model.ChildrenOf(node);
		if (kids.size() != 3 ||!SameStart(kids[0], node))
		{
			return TypeTable::Unknown;
		}

		const auto a = m_types.Value(NodeType(kids[1]));
		const auto b = m_types.Value(NodeType(kids[2]));
		if (a == b)
		{
			return a;
		}

		const auto sa = m_types.Decay(m_types.Strip(a));
		const auto sb = m_types.Decay(m_types.Strip(b));
		if (m_types.IsArithmetic(sa) && m_types.IsArithmetic(sb))
		{
			return Arithmetic(sa, sb);
		}

		if (m_types.IsPointer(sa) && m_types.IsBuiltin(sb, BuiltinType::NullptrT))
		{
			return sa;
		}

		if (m_types.IsPointer(sb) && m_types.IsBuiltin(sa, BuiltinType::NullptrT))
		{
			return sb;
		}

		return TypeTable::Unknown;
	}

	TypeId TyperImpl::SubscriptType(std::uint32_t node)
	{
		const auto kids = m_model.ChildrenOf(node);
		if (kids.size() != 2 ||!SameStart(kids[0], node))
		{
			return TypeTable::Unknown;
		}

		const auto base = m_types.Strip(NodeType(kids[0]));
		if (m_types.IsArray(base) || m_types.IsPointer(base))
		{
			return m_types.Value(m_types.Element(base));
		}

		if (m_types.Kind(base) == TypeKind::Class)
		{
			const auto name = m_model.Names().Find("operator[]");
			const auto member =
				name == kNone ? kNone : m_model.LookupMember(m_types.Arg(base), name);
			if (member != kNone && m_symbols.kind[member] == SymbolKind::Function)
			{
				return OverloadReturn(member);
			}
		}

		return TypeTable::Unknown;
	}

	// Result of `size()`, `empty()`... on a library container.
	TypeId TyperImpl::ExternalCall(TypeId object, std::string_view member)
	{
		const auto head = m_out.ExternalHead(object);
		if (!IsStdContainerHead(head))
		{
			return TypeTable::Unknown;
		}

		if (member == "size" || member == "max_size" ||
			(member == "length" && IsStringHead(head)) ||
			(member == "capacity" && (head == "std::vector" || IsStringHead(head))))
		{
			return m_types.Builtin(BuiltinType::SizeT);
		}

		return member == "empty" ? m_types.Builtin(BuiltinType::Bool) : TypeTable::Unknown;
	}

	// `a.b`, `p->b` and `N::b`: the type of the field, or (as a callee) of the
	// result of calling the function.
	TypeId TyperImpl::MemberType(std::uint32_t node, bool as_call)
	{
		const auto kids = m_model.ChildrenOf(node);
		if (kids.size() != 2 ||!SameStart(kids[0], node))
		{
			return TypeTable::Unknown;
		}

		const auto op_pos                = NodeSig(kids[0]).second;
		const Tok op                     = At(op_pos);
		const auto[name_begin, name_end] = NodeSig(kids[1]);
		if (name_end != name_begin + 1 ||!WordAt(name_begin) || name_begin != op_pos + 1)
		{
			return TypeTable::Unknown;
		}

		SymbolId member = kNone;
		TypeId object   = TypeTable::Unknown;
		if (op == Tok::ColonColon)
		{
			member = m_model.ResolveToken(m_sig[name_begin]);
		}
		else if (op == Tok::Dot || op == Tok::Arrow)
		{
			object = m_types.Strip(NodeType(kids[0]));
			if (op == Tok::Arrow)
			{
				if (!m_types.IsPointer(object))
				{
					return TypeTable::Unknown; // smart pointers and iterators
				}

				object = m_types.Strip(m_types.Element(object));
			}

			if (m_types.Kind(object) == TypeKind::Class)
			{
				const auto name = m_model.Names().Find(TextAt(name_begin));
				member          = name == kNone ? kNone : m_model.LookupMember(m_types.Arg(object), name);
			}
			else if (m_types.Kind(object) == TypeKind::InjectedClass)
			{
				member = m_model.LookupLocal(m_symbols.member_scope[m_types.Arg(object)],
					m_model.Names().Find(TextAt(name_begin)));
			}
			else if (m_types.Kind(object) == TypeKind::TemplateId)
			{
				const auto name    = m_model.Names().Find(TextAt(name_begin));
				const auto binding = m_out.LookupInstantiatedMember(m_types.Arg(object), name);
				if (binding.state == TemplateResolution::Resolved)
				{
					member = binding.symbol;
				}
			}
			else if (m_types.Kind(object) == TypeKind::External && as_call && op == Tok::Dot)
			{
				return ExternalCall(object, TextAt(name_begin));
			}
		}

		if (member == kNone)
		{
			return TypeTable::Unknown;
		}

		const auto kind = m_symbols.kind[member];
		if (kind == SymbolKind::Variable || kind == SymbolKind::Parameter)
		{
			m_out.m_member_symbol[node] = member;
			if (m_types.Kind(object) == TypeKind::TemplateId)
			{
				return m_types.Value(m_out.Substitute(SymbolType(member), m_types.Arg(object)));
			}
		}

		if (as_call)
		{
			if (kind != SymbolKind::Function)
			{
				return TypeTable::Unknown;
			}

			m_out.m_member_symbol[node] = member;
			return OverloadReturn(member);
		}

		return kind == SymbolKind::Variable || kind == SymbolKind::Parameter
		? SymbolType(member)
		: TypeTable::Unknown;
	}

	// `static_cast<T>(...)` and friends: the callee is `kw < T >`.
	TypeId TyperImpl::CastType(std::uint32_t callee)
	{
		const auto[begin, end] = NodeSig(callee);
		if (end < begin + 4 || At(begin + 1) != Tok::Lt || At(end - 1) != Tok::Gt)
		{
			return TypeTable::Unknown;
		}

		return ParseTypeTokens(begin + 2, end - 1, m_model.ScopeOfNode(callee), m_sig[begin]);
	}

	TyperImpl::SubstitutedExpression TyperImpl::CheckExpression(
		std::uint32_t node,
		TemplateId declaration,
		std::span<const TypeId> arguments,
		SubstitutionContext context,
		std::uint32_t remainingDepth)
	{
		const auto invalid =[&]()
		{
			return SubstitutedExpression {
				{
					TypeTable::Unknown, context == SubstitutionContext::Immediate
					? SubstitutionState::SubstitutionFailure : SubstitutionState::HardError,
						SubstitutionReason::InvalidExpression
				}, Category::Unknown};
		};
		if (remainingDepth == 0 || node >= m_nodes.size())
		{
			return {
				{
					TypeTable::Unknown, SubstitutionState::Incomplete, SubstitutionReason::DepthLimit
				}, Category::Unknown};
		}

		const auto[begin, end] = NodeSig(node);
		const auto children    = m_model.ChildrenOf(node);
		const auto kind        = m_nodes.Kind(node);
		if (kind == GrammarKind::IdentifierExpression && end == begin + 1)
		{
			for (auto parent = m_nodes.Parent(node); parent != 0 && parent < m_nodes.size(); parent = m_nodes.Parent(parent))
			{
				if (m_nodes.Kind(parent) != GrammarKind::FunctionDeclaration && m_nodes.Kind(parent) != GrammarKind::FunctionDefinition)
				{
					continue;
				}

				const auto declarator = FindChild(parent, GrammarKind::Declarator);
				const auto suffix = declarator == kNone ? kNone : FindChild(declarator,
					GrammarKind::FunctionSuffix);
				if (suffix == kNone)
				{
					break;
				}

				for (const auto parameter : m_model.ChildrenOf(suffix))
				{
					if (m_nodes.Kind(parameter) != GrammarKind::ParameterDeclaration)
					{
						continue;
					}

					const auto parameterDeclarator = FindChild(parameter, GrammarKind::Declarator);
					const auto name = parameterDeclarator == kNone ? kNone : FindChild(parameterDeclarator,
						GrammarKind::DeclaredName);
					if (name != kNone && m_nodes.FirstToken(name) < m_sig[begin] &&
						Text(m_nodes.FirstToken(name)) == TextAt(begin))
					{
						return {CheckedSubstitute(ParameterType(parameter), declaration, arguments, context,
								remainingDepth - 1), Category::LValue};
					}
				}

				break;
			}

			const auto symbol = m_model.ResolveToken(m_sig[begin]);
			if (symbol == kNone ||(m_symbols.kind[symbol] != SymbolKind::Variable &&
				m_symbols.kind[symbol] != SymbolKind::Parameter))
			{
				return {};
			}

			return {CheckedSubstitute(SymbolType(symbol), declaration, arguments, context, remainingDepth - 1),
				Category::LValue};
		}

		if (kind == GrammarKind::LiteralExpression)
		{
			const auto type = LiteralType(node);
			return {m_types.Substitute(type, {}, context, remainingDepth - 1),
				m_types.Kind(type) == TypeKind::Array ? Category::LValue : Category::PRValue};
		}

		if (kind == GrammarKind::ParenthesizedExpression && children.size() == 1)
		{
			return CheckExpression(children.front(), declaration, arguments, context, remainingDepth - 1);
		}

		if (kind == GrammarKind::UnaryExpression && children.size() == 1)
		{
			auto operand = CheckExpression(children.front(), declaration, arguments, context,
				remainingDepth - 1);
			if (operand.result.state != SubstitutionState::Success)
			{
				return operand;
			}

			const auto type = m_types.Strip(operand.result.type);
			if (m_types.Kind(type) == TypeKind::Class || m_types.Kind(type) == TypeKind::TemplateId ||
				m_types.Kind(type) == TypeKind::External || m_types.Kind(type) == TypeKind::Enum)
			{
				return {
					{
						TypeTable::Unknown, SubstitutionState::Incomplete, SubstitutionReason::Unsupported
					}, Category::Unknown};
			}

			if (At(begin) == Tok::Star)
			{
				if (m_types.Kind(type) != TypeKind::Pointer || m_types.IsBuiltin(m_types.Arg(type),
					BuiltinType::Void))
				{
					return invalid();
				}

				return {
					{
						m_types.Arg(type), SubstitutionState::Success, SubstitutionReason::None
					}, Category::LValue};
			}

			if (At(begin) == Tok::Amp)
			{
				if (operand.category != Category::LValue || m_types.IsBuiltin(type, BuiltinType::Void))
				{
					return invalid();
				}

				return {
					{
						m_types.Pointer(m_types.Value(operand.result.type)), SubstitutionState::Success,
							SubstitutionReason::None
					}, Category::PRValue};
			}

			if (At(begin) == Tok::Plus || At(begin) == Tok::Minus || At(begin) == Tok::Tilde)
			{
				if (At(begin) == Tok::Plus && m_types.Kind(type) == TypeKind::Pointer)
				{
					return {
						{
							type, SubstitutionState::Success, SubstitutionReason::None
						}, Category::PRValue};
				}

				if (!m_types.IsArithmetic(type) ||(At(begin) == Tok::Tilde && !m_types.IsInteger(type) && !m_types.IsBool(type)))
				{
					return invalid();
				}

				return {
					{
						Promoted(type), SubstitutionState::Success, SubstitutionReason::None
					}, Category::PRValue};
			}

			return {};
		}

		if (kind == GrammarKind::BinaryExpression && children.size() == 2)
		{
			const auto left = CheckExpression(children[0], declaration, arguments, context, remainingDepth - 1);
			if (left.result.state != SubstitutionState::Success)
			{
				return left;
			}

			const auto right = CheckExpression(children[1], declaration, arguments, context,
				remainingDepth - 1);
			if (right.result.state != SubstitutionState::Success)
			{
				return right;
			}

			const auto op        = At(NodeSig(children[0]).second);
			const auto leftType  = m_types.Strip(left.result.type);
			const auto rightType = m_types.Strip(right.result.type);
			const auto unmodeled =[&](TypeId type)
			{
				const auto typeKind = m_types.Kind(type);
				return typeKind == TypeKind::Class || typeKind == TypeKind::TemplateId ||
					typeKind == TypeKind::External || typeKind == TypeKind::Enum;
			};
			if (unmodeled(leftType) || unmodeled(rightType))
			{
				return {
					{
						TypeTable::Unknown, SubstitutionState::Incomplete, SubstitutionReason::Unsupported
					}, Category::Unknown};
			}

			if (op == Tok::Comma)
			{
				return right;
			}

			if (op == Tok::Plus || op == Tok::Minus || op == Tok::Star || op == Tok::Slash ||
				op == Tok::Percent || op == Tok::Amp || op == Tok::Pipe || op == Tok::Caret)
			{
				if (m_types.Kind(leftType) == TypeKind::Pointer || m_types.Kind(rightType) == TypeKind::Pointer)
				{
					return {
						{
							TypeTable::Unknown, SubstitutionState::Incomplete, SubstitutionReason::Unsupported
						}, Category::Unknown};
				}

				if (!m_types.IsArithmetic(leftType) ||!m_types.IsArithmetic(rightType))
				{
					return invalid();
				}

				if ((op == Tok::Percent || op == Tok::Amp || op == Tok::Pipe || op == Tok::Caret) &&
					(m_types.IsFloating(leftType) || m_types.IsFloating(rightType)))
				{
					return invalid();
				}

				return {
					{
						Arithmetic(leftType, rightType), SubstitutionState::Success, SubstitutionReason::None
					}, Category::PRValue};
			}

			return {};
		}

		if (kind == GrammarKind::CastExpression && children.size() == 2 &&
			m_nodes.Kind(children.front()) == GrammarKind::TypeSpecifier)
		{
			const auto operand = CheckExpression(children[1], declaration, arguments, context,
				remainingDepth - 1);
			if (operand.result.state != SubstitutionState::Success)
			{
				return operand;
			}

			const auto[typeBegin, typeEnd] = NodeSig(children.front());
			const auto target              = ParseTypeTokens(typeBegin, typeEnd, m_model.ScopeOfNode(node), m_sig[begin]);
			if (m_types.IsBuiltin(target, BuiltinType::Void))
			{
				return {
					{
						target, SubstitutionState::Success, SubstitutionReason::None
					}, Category::PRValue};
			}

			return {};
		}

		if (kind == GrammarKind::CallExpression && !children.empty())
		{
			const auto[calleeBegin, calleeEnd] = NodeSig(children.front());
			if (calleeEnd == calleeBegin + 1 && IsBuiltinWord(At(calleeBegin)) && children.size() == 1)
			{
				const auto type = ParseTypeTokens(calleeBegin, calleeEnd, m_model.ScopeOfNode(node), m_sig[begin]);
				return {
					{
						type, SubstitutionState::Success, SubstitutionReason::None
					}, Category::PRValue};
			}
		}

		return {
			{
				TypeTable::Unknown, SubstitutionState::Incomplete, SubstitutionReason::Unsupported
			}, Category::Unknown};
	}

	SubstitutionState TyperImpl::DeduceType(
		TypeId pattern,
		TypeId argument,
		TemplateId declaration,
		std::vector<TypeId>& arguments,
		std::size_t explicitCount,
		std::uint32_t remainingDepth)
	{
		if (remainingDepth == 0 || pattern == TypeTable::Unknown || argument == TypeTable::Unknown)
		{
			return SubstitutionState::Incomplete;
		}

		if (m_types.IsDependent(argument))
		{
			return SubstitutionState::Dependent;
		}

		const auto kind = m_types.Kind(pattern);
		if (kind == TypeKind::TemplateParameter)
		{
			const auto parameter   = m_types.Arg(pattern);
			const auto& parameters = m_model.TemplateParameters();
			if (parameter >= parameters.owner.size() || parameters.owner[parameter] != declaration)
			{
				return SubstitutionState::Dependent;
			}

			const auto position = parameters.position[parameter];
			if (position >= arguments.size())
			{
				return SubstitutionState::Incomplete;
			}

			if (position < explicitCount)
			{
				return SubstitutionState::Success;
			}

			if (arguments[position] != TypeTable::Unknown && arguments[position] != argument)
			{
				return SubstitutionState::DeductionFailure;
			}

			arguments[position] = argument;
			return SubstitutionState::Success;
		}

		if (kind == TypeKind::DependentMember || kind == TypeKind::DecltypeExpression)
		{
			return SubstitutionState::Success;
		}

		if (kind == TypeKind::Const)
		{
			return DeduceType(m_types.Arg(pattern), m_types.Kind(argument) == TypeKind::Const
				? m_types.Arg(argument) : argument, declaration, arguments, explicitCount, remainingDepth - 1);
		}

		if (kind == TypeKind::Pointer || kind == TypeKind::Array)
		{
			if (m_types.Kind(argument) != kind)
			{
				return m_types.IsDependent(pattern) ? SubstitutionState::DeductionFailure : SubstitutionState::Success;
			}

			return DeduceType(m_types.Arg(pattern), m_types.Arg(argument), declaration, arguments,
				explicitCount, remainingDepth - 1);
		}

		return m_types.IsDependent(pattern) ? SubstitutionState::Incomplete : SubstitutionState::Success;
	}

	TypeId TyperImpl::ResolveTemplateCall(
		std::uint32_t node,
		SymbolId symbol,
		std::span<const TypeId> explicitArguments,
		bool explicitCall)
	{
		constexpr std::uint8_t kExact        = 0;
		constexpr std::uint8_t kPromotion    = 1;
		constexpr std::uint8_t kConversion   = 2;
		constexpr std::uint8_t kEllipsis     = 3;
		constexpr std::size_t kMaxCandidates = 128;
		struct Candidate
		{
			SymbolId symbol = kNone;
			TypeId type     = TypeTable::Unknown;
			std::vector<std::uint8_t> conversions;
			bool isTemplate        = false;
			bool hasReference      = false;
			TemplateId declaration = kNone;
			std::vector<TypeId> patterns;
		};

		const auto children = m_model.ChildrenOf(node);
		const auto publish =[&](TemplateCallResult result, TypeId type)
		{
			result.returnType = type;
			m_out.mTemplateCalls.push_back({node, result});
			return type;
		};
		if (children.empty() ||!m_tree.Diagnostics().empty() || m_tree.Cancelled())
		{
			return publish({}, TypeTable::Unknown);
		}

		for (const auto& directive : m_tree.Directives())
		{
			if (directive.active && (directive.kind == DirectiveKind::Include ||
				directive.kind == DirectiveKind::Define || directive.kind == DirectiveKind::Undef))
			{
				return publish({kNone, SubstitutionState::Incomplete, SubstitutionReason::Unsupported},
					TypeTable::Unknown);
			}
		}

		if (m_tree.TypeNames() != nullptr)
		{
			return publish({kNone, SubstitutionState::Incomplete, SubstitutionReason::Unsupported},
				TypeTable::Unknown);
		}

		for (std::uint32_t part = 0; part < m_nodes.size(); ++part)
		{
			if (m_nodes.Kind(part) == GrammarKind::ImportDeclaration || m_nodes.Kind(part) == GrammarKind::ModuleDeclaration)
			{
				return publish({kNone, SubstitutionState::Incomplete, SubstitutionReason::Unsupported},
					TypeTable::Unknown);
			}

			if (m_nodes.Kind(part) == GrammarKind::UsingDeclaration)
			{
				const auto[begin, end] = NodeSig(part);
				if (end <= begin + 2 || At(begin + 2) != Tok::Eq)
				{
					return publish({kNone, SubstitutionState::Incomplete, SubstitutionReason::Unsupported},
						TypeTable::Unknown);
				}
			}
		}

		std::vector<std::uint32_t> argumentNodes(children.begin() + 1, children.end());
		const auto before                  = m_nodes.FirstToken(children.front());
		bool incomplete                    = false;
		bool dependent                     = false;
		SubstitutionReason rejectionReason = SubstitutionReason::None;
		std::uint32_t rejectionToken       = kNone;
		std::vector<Candidate> viable;
		for (const auto argument : argumentNodes)
		{
			auto type = m_types.Strip(NodeType(argument));
			while (m_types.Kind(type) == TypeKind::Pointer || m_types.Kind(type) == TypeKind::Array)
			{
				type = m_types.Strip(m_types.Arg(type));
			}

			if (m_types.Kind(type) == TypeKind::External || m_types.Kind(type) == TypeKind::TemplateId)
			{
				incomplete = true;
			}
			else if (m_types.Kind(type) == TypeKind::Class)
			{
				const auto owner = m_types.Arg(type);
				incomplete = incomplete || m_symbols.scope[owner] != SemanticModel::TranslationUnitScope ||
					m_symbols.base_count[owner] != 0;
				for (SymbolId member = 0; member < m_symbols.Size(); ++member)
				{
					if (m_symbols.scope[member] == m_symbols.member_scope[owner] &&
						(m_symbols.flags[member] & (SymbolFlag::Friend | SymbolFlag::Constructor)) != 0)
					{
						incomplete = true;
					}
				}
			}
		}

		std::size_t visits = 0;
		for (auto candidate = m_model.LookupLocal(m_symbols.scope[symbol], m_symbols.name[symbol]);
			candidate != kNone; candidate = m_symbols.next_same_name[candidate])
		{
			if (++visits > kMaxCandidates)
			{
				return publish({kNone, SubstitutionState::Incomplete, SubstitutionReason::DepthLimit},
					TypeTable::Unknown);
			}

			if (m_symbols.decl_token[candidate] > before)
			{
				continue;
			}

			if (m_symbols.kind[candidate] != SymbolKind::Function ||
				(m_symbols.flags[candidate] & (SymbolFlag::Qualified | SymbolFlag::Friend | SymbolFlag::Operator | SymbolFlag::Defaulted)) != 0)
			{
				incomplete = true;
				continue;
			}

			const auto declaration = m_model.TemplateOf(candidate);
			const bool isTemplate  = (m_symbols.flags[candidate] & SymbolFlag::Template) != 0;
			if (explicitCall && !isTemplate)
			{
				continue;
			}

			const auto function   = m_symbols.decl_node[candidate];
			const auto declarator = FindChild(function, GrammarKind::Declarator);
			const auto suffix = declarator == kNone ? kNone : FindChild(declarator,
				GrammarKind::FunctionSuffix);
			if (suffix == kNone)
			{
				incomplete = true;
				continue;
			}

			// The exception specification is instantiated separately when
			// required and is not part of the immediate context. A plain
			// `noexcept` is trivially valid; anything parenthesized stays
			// unresolved rather than guessed.
			bool specUnresolved = false;
			for (const auto holder :
				{
					suffix, declarator
			})
			{
				if (holder == kNone)
				{
					continue;
				}

				for (const auto child : m_model.ChildrenOf(holder))
				{
					if (m_nodes.Kind(child) != GrammarKind::NoexceptSpecifier)
					{
						continue;
					}

					const auto[specBegin, specEnd] = NodeSig(child);
					specUnresolved                 = specUnresolved || specEnd > specBegin + 1;
				}
			}

			if (specUnresolved)
			{
				incomplete = true;
				continue;
			}

			bool unsupported                       = false;
			const auto[functionBegin, functionEnd] = NodeSig(function);
			const auto body                        = FindChild(function, GrammarKind::CompoundStatement);
			const auto signatureEnd                = body == kNone ? functionEnd : NodeSig(body).first;
			for (auto position = functionBegin; position < signatureEnd; ++position)
			{
				unsupported = unsupported || At(position) == Tok::KwVolatile || At(position) == Tok::KwThis;
			}

			std::vector<TypeId> arguments(explicitArguments.begin(), explicitArguments.end());
			const auto& templates  = m_model.Templates();
			const auto& parameters = m_model.TemplateParameters();
			std::uint32_t first    = 0;
			std::uint32_t count    = 0;
			if (isTemplate)
			{
				if (declaration >= templates.owner.size() ||!templates.complete[declaration])
				{
					incomplete = true;
					continue;
				}

				first = templates.firstParameter[declaration];
				count = templates.parameterCount[declaration];
				if (arguments.size() > count)
				{
					rejectionReason = SubstitutionReason::ArgumentCount;
					continue;
				}

				arguments.resize(count, TypeTable::Unknown);
				for (std::uint32_t i = 0; i < count; ++i)
				{
					unsupported = unsupported || parameters.pack[first + i] || parameters.kind[first + i] != TemplateParameterKind::Type;
				}
			}

			std::vector<std::uint32_t> parameterNodes;
			std::size_t required = 0;
			for (const auto part : m_model.ChildrenOf(suffix))
			{
				if (m_nodes.Kind(part) != GrammarKind::ParameterDeclaration)
				{
					continue;
				}

				const auto[begin, end] = NodeSig(part);
				if (end == begin + 1 && At(begin) == Tok::KwVoid)
				{
					continue;
				}

				bool hasDefault = false;
				for (auto pos = begin; pos < end; ++pos)
				{
					hasDefault  = hasDefault || At(pos) == Tok::Eq;
					unsupported = unsupported || At(pos) == Tok::Ellipsis;
				}

				parameterNodes.push_back(part);
				required += hasDefault ? 0 : 1;
			}

			const auto[suffixBegin, suffixEnd] = NodeSig(suffix);
			const auto close                   = MatchParen(suffixBegin, suffixEnd);
			const bool variadic                = close > suffixBegin && close < suffixEnd && At(close - 1) == Tok::Ellipsis;
			if (unsupported)
			{
				incomplete = true;
				continue;
			}

			if (argumentNodes.size() < parameterNodes.size())
			{
				incomplete = true;
				continue;
			}

			if (argumentNodes.size() < required ||(!variadic && argumentNodes.size() > parameterNodes.size()))
			{
				rejectionReason = SubstitutionReason::ArgumentCount;
				continue;
			}

			struct Slot
			{
				std::uint32_t token;
				TypeId type;
				bool parameter = false;
			};

			std::vector<Slot> slots;
			const auto spec           = FindChild(function, GrammarKind::TypeSpecifier);
			const auto trailing       = FindChild(suffix, GrammarKind::TrailingReturnType);
			const auto directTrailing = FindChild(declarator, GrammarKind::TrailingReturnType);
			const auto returnNode     = trailing != kNone ? trailing : directTrailing != kNone ? directTrailing : spec;
			const auto returnType     = SymbolType(candidate);
			if (returnNode == kNone)
			{
				incomplete = true;
				continue;
			}

			slots.push_back({m_nodes.FirstToken(returnNode), returnType});
			for (const auto parameter : parameterNodes)
			{
				slots.push_back({m_nodes.FirstToken(parameter), ParameterType(parameter), true});
			}

			std::ranges::sort(slots, {}, &Slot::token);
			const auto checkSignature =[&](std::span<const TypeId> values, bool partial)
			{
				SubstitutionResult result {returnType, SubstitutionState::Success, SubstitutionReason::None};
				for (const auto& slot : slots)
				{
					result = CheckedSubstitute(slot.type, declaration, values, SubstitutionContext::Immediate);
					if (slot.parameter && result.state == SubstitutionState::Success && m_types.IsBuiltin(result.type,
						BuiltinType::Void))
					{
						result = {TypeTable::Unknown, SubstitutionState::SubstitutionFailure,
							SubstitutionReason::VoidParameter};
					}
					else if (!slot.parameter && result.state == SubstitutionState::Success && m_types.Kind(result.type) == TypeKind::Array)
					{
						result = {TypeTable::Unknown, SubstitutionState::SubstitutionFailure,
							SubstitutionReason::InvalidFunctionReturn};
					}

					if (result.state != SubstitutionState::Success && !(partial && result.state == SubstitutionState::Dependent))
					{
						rejectionToken = slot.token;
						return result;
					}
				}

				return result;
			};

			SubstitutionResult result {returnType, SubstitutionState::Success, SubstitutionReason::None};
			if (isTemplate && !explicitArguments.empty())
			{
				auto initial = arguments;
				for (std::uint32_t i = static_cast<std::uint32_t>(explicitArguments.size()); i < count; ++i)
				{
					initial[i] = m_types.Parameter(first + i);
				}

				result = checkSignature(initial, true);
			}

			if (result.state == SubstitutionState::Success || result.state == SubstitutionState::Dependent)
			{
				result = {TypeTable::Unknown, SubstitutionState::Success, SubstitutionReason::None};
				for (std::size_t i = 0; isTemplate && i < std::min(argumentNodes.size(),
					parameterNodes.size()); ++i)
				{
					auto pattern    = ParameterType(parameterNodes[i]);
					auto actual     = NodeType(argumentNodes[i]);
					const auto kind = m_types.Kind(pattern);
					if (kind == TypeKind::LRef || kind == TypeKind::RRef)
					{
						const auto category = ValueCategory(argumentNodes[i]);
						pattern             = m_types.Arg(pattern);
						if (kind == TypeKind::RRef && m_types.Kind(pattern) == TypeKind::TemplateParameter && category == Category::LValue)
						{
							actual = m_types.LRef(actual);
						}
					}
					else
					{
						pattern = m_types.Strip(pattern);
						actual  = m_types.Decay(m_types.Strip(actual));
					}

					result.state = DeduceType(pattern, actual, declaration, arguments, explicitArguments.size());
					if (result.state != SubstitutionState::Success)
					{
						result.reason = result.state == SubstitutionState::DeductionFailure
						? SubstitutionReason::ConflictingDeduction : SubstitutionReason::UnknownType;
						break;
					}
				}
			}

			if (result.state == SubstitutionState::Success)
			{
				for (std::uint32_t i = 0; isTemplate && i < count; ++i)
				{
					if (arguments[i] != TypeTable::Unknown)
					{
						continue;
					}

					const auto begin = parameters.defaultBegin[first + i];
					const auto end   = parameters.defaultEnd[first + i];
					if (begin == kNone)
					{
						result = {TypeTable::Unknown, SubstitutionState::DeductionFailure,
							SubstitutionReason::ConflictingDeduction};
						break;
					}

					const auto defaultType = ParseTypeTokens(Pos(begin), Pos(end), templates.scope[declaration], begin);
					result = CheckedSubstitute(defaultType, declaration, std::span(arguments).first(i),
						SubstitutionContext::Immediate);
					if (result.state != SubstitutionState::Success)
					{
						rejectionToken = begin;
						break;
					}

					arguments[i] = result.type;
				}
			}

			if (result.state == SubstitutionState::Success && isTemplate)
			{
				const auto constraints = CheckAssociatedConstraints(declaration, arguments,
					SubstitutionContext::Immediate);
				if (constraints.state == SubstitutionState::Success && !constraints.satisfied)
				{
					result = {TypeTable::Unknown, SubstitutionState::ConstraintUnsatisfied, SubstitutionReason::None};
				}
				else if (constraints.state == SubstitutionState::HardError)
				{
					result         = {TypeTable::Unknown, SubstitutionState::HardError, constraints.reason};
					rejectionToken = m_nodes.FirstToken(function);
				}
				else if (constraints.state == SubstitutionState::Incomplete)
				{
					result = {TypeTable::Unknown, SubstitutionState::Incomplete, constraints.reason};
				}
				else if (constraints.state == SubstitutionState::Dependent)
				{
					result = {TypeTable::Unknown, SubstitutionState::Dependent, constraints.reason};
				}
			}

			if (result.state == SubstitutionState::Success)
			{
				result = checkSignature(arguments, false);
			}

			if (result.state == SubstitutionState::HardError)
			{
				return publish({kNone, result.state, result.reason, rejectionToken}, TypeTable::Unknown);
			}

			if (result.state == SubstitutionState::Incomplete || result.state == SubstitutionState::Dependent)
			{
				incomplete = incomplete || result.state == SubstitutionState::Incomplete;
				dependent  = dependent || result.state == SubstitutionState::Dependent;
				continue;
			}

			if (result.state == SubstitutionState::ConstraintUnsatisfied || result.state == SubstitutionState::DeductionFailure || result.state == SubstitutionState::SubstitutionFailure)
			{
				rejectionReason = result.reason;
				continue;
			}

			if (result.state != SubstitutionState::Success)
			{
				rejectionReason = result.reason;
				continue;
			}

			Candidate accepted {candidate, TypeTable::Unknown, {}, isTemplate};
			accepted.declaration = declaration;
			accepted.patterns.reserve(parameterNodes.size());
			for (const auto parameter : parameterNodes)
			{
				accepted.patterns.push_back(m_types.Strip(ParameterType(parameter)));
			}

			accepted.type = CheckedSubstitute(returnType, declaration, arguments,
				SubstitutionContext::Immediate).type;
			bool convertible = true;
			for (std::size_t i = 0; i < argumentNodes.size(); ++i)
			{
				if (i >= parameterNodes.size())
				{
					accepted.conversions.push_back(kEllipsis);
					continue;
				}

				const auto parameter = CheckedSubstitute(ParameterType(parameterNodes[i]), declaration,
					arguments, SubstitutionContext::Immediate).type;
				const auto actual     = NodeType(argumentNodes[i]);
				const auto source     = m_types.Strip(actual);
				const auto target     = m_types.Strip(parameter);
				const auto reference  = m_types.Kind(parameter);
				accepted.hasReference = accepted.hasReference || reference == TypeKind::LRef || reference == TypeKind::RRef;
				if (reference == TypeKind::LRef || reference == TypeKind::RRef)
				{
					const auto category = ValueCategory(argumentNodes[i]);
					if (category == Category::Unknown)
					{
						incomplete  = true;
						convertible = false;
						break;
					}

					if ((reference == TypeKind::LRef && !m_types.IsConstQualified(parameter) && category != Category::LValue) ||
						(reference == TypeKind::RRef && category == Category::LValue) ||
						(!m_types.IsConstQualified(parameter) && m_types.IsConstQualified(actual)))
					{
						convertible = false;
						break;
					}
				}

				if (source == target && source != TypeTable::Unknown)
				{
					accepted.conversions.push_back(kExact);
				}
				else if (m_types.IsArithmetic(source) && m_types.IsArithmetic(target))
				{
					const auto promoted = Promote(static_cast<BuiltinType>(m_types.Arg(source)));
					const bool promotion = (promoted && m_types.IsBuiltin(target, *promoted) && source != target) ||
						(m_types.IsBuiltin(source, BuiltinType::Float) && m_types.IsBuiltin(target, BuiltinType::Double));
					accepted.conversions.push_back(promotion ? kPromotion : kConversion);
				}
				else
				{
					incomplete  = true;
					convertible = false;
					break;
				}
			}

			if (convertible)
			{
				viable.push_back(std::move(accepted));
			}
		}

		if (incomplete || dependent)
		{
			return publish({kNone, incomplete ? SubstitutionState::Incomplete : SubstitutionState::Dependent,
					SubstitutionReason::Unsupported}, TypeTable::Unknown);
		}

		if (viable.empty())
		{
			return publish({kNone, SubstitutionState::NoMatch, rejectionReason, rejectionToken},
				TypeTable::Unknown);
		}

		for (const auto& candidate : viable)
		{
			for (const auto& other : viable)
			{
				if (&candidate != &other && (candidate.hasReference || other.hasReference) &&
					candidate.conversions == other.conversions)
				{
					return publish({kNone, SubstitutionState::Incomplete, SubstitutionReason::Unsupported},
						TypeTable::Unknown);
				}
			}
		}

		bool orderingIncomplete = false;
		const auto better =[&](const Candidate& left, const Candidate& right)
		{
			bool strict = false;
			for (std::size_t i = 0; i < left.conversions.size(); ++i)
			{
				if (left.conversions[i] > right.conversions[i])
				{
					return false;
				}

				strict = strict || left.conversions[i] < right.conversions[i];
			}

			if (strict ||(!left.isTemplate && right.isTemplate))
			{
				return true;
			}

			if (left.isTemplate && right.isTemplate && left.conversions == right.conversions)
			{
				const auto leftAtoms  = ConstraintAtoms(left.declaration);
				const auto rightAtoms = ConstraintAtoms(right.declaration);
				if (leftAtoms != rightAtoms)
				{
					bool leftCovers = true;
					for (const auto& atom : rightAtoms)
					{
						leftCovers = leftCovers && std::ranges::find(leftAtoms, atom) != leftAtoms.end();
					}

					bool rightCovers = true;
					for (const auto& atom : leftAtoms)
					{
						rightCovers = rightCovers && std::ranges::find(rightAtoms, atom) != rightAtoms.end();
					}

					if (leftCovers && leftAtoms.size() > rightAtoms.size())
					{
						return true;
					}

					if (rightCovers && rightAtoms.size() > leftAtoms.size())
					{
						return false;
					}

					if (!leftCovers ||!rightCovers)
					{
						orderingIncomplete = true;
						return false;
					}
				}
			}

			if (left.isTemplate && right.isTemplate)
			{
				std::vector<TypeId> forwardBindings, reverseBindings;
				const auto forward = MatchTypePatterns(right.patterns, left.patterns, right.declaration,
					forwardBindings, true);
				const auto reverse = MatchTypePatterns(left.patterns, right.patterns, left.declaration,
					reverseBindings, true);
				orderingIncomplete = orderingIncomplete || forward.state == SubstitutionState::Incomplete || reverse.state == SubstitutionState::Incomplete;
				return forward.state == SubstitutionState::Success && reverse.state == SubstitutionState::DeductionFailure;
			}

			return false;
		};
		for (const auto& candidate : viable)
		{
			bool best = true;
			for (const auto& other : viable)
			{
				if (&candidate != &other && !better(candidate, other))
				{
					best = false;
					break;
				}
			}

			if (best && !orderingIncomplete)
			{
				return publish({candidate.symbol, SubstitutionState::Success, SubstitutionReason::None},
					candidate.type);
			}
		}

		if (orderingIncomplete)
		{
			return publish({kNone, SubstitutionState::Incomplete, SubstitutionReason::Unsupported},
				TypeTable::Unknown);
		}

		return publish({kNone, SubstitutionState::Ambiguous, SubstitutionReason::None}, TypeTable::Unknown);
	}

	TypeId TyperImpl::CallType(std::uint32_t node)
	{
		const auto kids = m_model.ChildrenOf(node);
		if (kids.empty() ||!SameStart(kids[0], node))
		{
			return TypeTable::Unknown;
		}

		const auto callee = kids[0];
		switch (m_nodes.Kind(callee))
		{
		case GrammarKind::IdentifierExpression:
		{
			const auto[begin, end] = NodeSig(callee);
			if (end != begin + 1)
			{
				return TypeTable::Unknown;
			}

			const Tok tok = At(begin);
			if (tok == Tok::KwSizeof || tok == Tok::KwAlignof)
			{
				return m_types.Builtin(BuiltinType::SizeT);
			}

			if (tok == Tok::KwNoexcept)
			{
				return m_types.Builtin(BuiltinType::Bool);
			}

			if (IsBuiltinWord(tok))
			{
				return ParseTypeTokens(begin, end, m_model.ScopeOfNode(callee), m_sig[begin]);
			}

			if (!WordAt(begin))
			{
				return TypeTable::Unknown;
			}

			const auto symbol = m_model.ResolveToken(m_sig[begin]);
			if (symbol == kNone)
			{
				return TypeTable::Unknown;
			}

			switch (m_symbols.kind[symbol])
			{
			case SymbolKind::Function:
				for (auto candidate = m_model.LookupLocal(m_symbols.scope[symbol], m_symbols.name[symbol]);
					candidate != kNone; candidate = m_symbols.next_same_name[candidate])
				{
					if ((m_symbols.flags[candidate] & SymbolFlag::Template) != 0)
					{
						return ResolveTemplateCall(node, symbol, {}, false);
					}
				}

				return OverloadReturn(symbol);
			case SymbolKind::Class:
				return (m_symbols.flags[symbol] & SymbolFlag::Template) != 0
				? TypeTable::Unknown
				: m_types.Class(symbol);
			case SymbolKind::TypeAlias:
				return SymbolType(symbol);
			default:
				return TypeTable::Unknown;
			}
		}
		case GrammarKind::TemplateIdExpression:
		{
			const Tok tok = At(NodeSig(callee).first);
			if (tok == Tok::KwStaticCast || tok == Tok::KwDynamicCast ||
				tok == Tok::KwReinterpretCast || tok == Tok::KwConstCast)
			{
				return CastType(callee);
			}

			const auto[begin, end] = NodeSig(callee);
			if (WordAt(begin) && At(begin + 1) == Tok::Lt && At(end - 1) == Tok::Gt)
			{
				const auto symbol = m_model.ResolveToken(m_sig[begin]);
				if (symbol != kNone && m_symbols.kind[symbol] == SymbolKind::Function)
				{
					const auto arguments = ParseTemplateArguments(begin + 2, end - 1,
						m_model.ScopeOfNode(callee), m_sig[begin]);
					return ResolveTemplateCall(node, symbol, arguments, true);
				}
			}

			const auto templateChildren = m_model.ChildrenOf(callee);
			if (!templateChildren.empty() &&
				m_nodes.Kind(templateChildren[0]) == GrammarKind::MemberExpression)
			{
				const auto memberNode = templateChildren[0];
				(void) MemberType(memberNode, true);
				const auto member       = m_out.m_member_symbol[memberNode];
				const auto declaration  = member == kNone ? kNone : m_model.TemplateOf(member);
				const auto argumentOpen = NodeSig(memberNode).second;
				if (declaration == kNone || m_symbols.next_same_name[member] != kNone ||
					At(argumentOpen) != Tok::Lt || At(end - 1) != Tok::Gt)
				{
					return TypeTable::Unknown;
				}

				const auto arguments = ParseTemplateArguments(argumentOpen + 1, end - 1,
					m_model.ScopeOfNode(callee), m_sig[begin]);
				if (arguments.size() != m_model.Templates().parameterCount[declaration])
				{
					return TypeTable::Unknown;
				}

				return SubstituteArguments(SymbolType(member), declaration, arguments);
			}

			const auto constructed = ParseTypeTokens(begin, end, m_model.ScopeOfNode(callee), m_sig[begin]);
			return m_types.Kind(constructed) == TypeKind::TemplateId ? constructed : TypeTable::Unknown;
		}
		case GrammarKind::MemberExpression:
			return MemberType(callee, true);
		default:
			return TypeTable::Unknown;
		}
	}

	void TyperImpl::BuildDependentReferences()
	{
		auto& references      = m_out.mDependentReferences;
		const auto& instances = m_out.mInstantiations;
		const auto& templates = m_model.Templates();
		for (std::uint32_t node = 0; node < m_nodes.size(); ++node)
		{
			if (m_nodes.Kind(node) != GrammarKind::MemberExpression)
			{
				continue;
			}

			const auto children = m_model.ChildrenOf(node);
			if (children.size() != 2)
			{
				continue;
			}

			const auto token = m_nodes.FirstToken(children[1]);
			auto receiver    = m_types.Strip(NodeType(children[0]));
			if (m_model.ReferenceState(token) != RefState::Dependent && !m_types.IsDependent(receiver))
			{
				continue;
			}

			const auto op = At(NodeSig(children[0]).second);
			if (op == Tok::Arrow)
			{
				receiver = m_types.IsPointer(receiver) ? m_types.Strip(m_types.Element(receiver)) : TypeTable::Unknown;
			}

			const auto name    = m_model.Names().Find(Text(token));
			TemplateId context = kNone;
			for (auto ancestor = node; ancestor < m_nodes.size(); ancestor = m_nodes.Parent(ancestor))
			{
				for (TemplateId id = 0; id < templates.node.size(); ++id)
				{
					if (templates.node[id] == ancestor)
					{
						context = id;
						break;
					}
				}

				if (context != kNone || ancestor == 0)
				{
					break;
				}
			}

			references.node.push_back(node);
			references.token.push_back(token);
			references.definitionScope.push_back(m_model.ScopeOfNode(node));
			references.name.push_back(name);
			references.receiver.push_back(receiver);
			references.firstBinding.push_back(static_cast<std::uint32_t>(references.bindingInstance.size()));
			references.bindingCount.push_back(0);
			references.coverage.push_back(TemplateCoverage::None);
			const auto append =[&](std::uint32_t instance, TemplateMemberBinding binding)
			{
				references.bindingInstance.push_back(instance);
				references.bindingTarget.push_back(binding.symbol);
				references.bindingState.push_back(binding.state);
				++references.bindingCount.back();
			};
			if ((!m_types.IsDependent(receiver) || m_types.Kind(receiver) == TypeKind::InjectedClass) && m_out.MemberSymbol(node) != kNone)
			{
				references.coverage.back() = TemplateCoverage::Invariant;
				append(kNone, {m_out.MemberSymbol(node), TemplateResolution::Resolved});
				continue;
			}

			for (std::uint32_t instance = 0; instance < instances.primary.size(); ++instance)
			{
				if (instances.primary[instance] != context || instances.state[instance] == TemplateResolution::Dependent ||
					(instances.selected[instance] != context && instances.selected[instance] != kNone))
				{
					continue;
				}

				references.coverage.back() = TemplateCoverage::Workspace;
				const auto concrete        = m_types.Strip(m_out.Substitute(receiver, instance));
				TemplateMemberBinding binding;
				if (instances.state[instance] == TemplateResolution::Resolved && m_types.Kind(concrete) == TypeKind::Class)
				{
					binding = m_out.LookupTypeMember(concrete, name, 0);
				}
				else if (instances.state[instance] == TemplateResolution::Resolved && m_types.Kind(concrete) == TypeKind::TemplateId)
				{
					binding = m_out.LookupInstantiatedMember(m_types.Arg(concrete), name);
				}

				append(instance, binding);
			}
		}
	}

	void TyperImpl::Run()
	{
		m_out.m_symbol_type.assign(m_symbols.Size(), kPending);
		m_out.m_node_type.assign(m_nodes.size(), kPending);
		m_out.m_member_symbol.assign(m_nodes.size(), kNone);
		for (SymbolId symbol = 0; symbol < m_symbols.Size(); ++symbol)
		{
			SymbolType(symbol);
		}

		for (std::uint32_t node = 0; node < m_nodes.size(); ++node)
		{
			switch (m_nodes.Kind(node))
			{
			case GrammarKind::ParameterDeclaration:
				m_out.m_node_type[node] = ParameterType(node);
				break;
			case GrammarKind::LambdaExpression:
				m_out.mLambdaNodes.push_back(node);
				m_out.mLambdaReturns.push_back(LambdaReturnType(node));
				m_out.m_node_type[node] = TypeTable::Unknown;
				break;
			case GrammarKind::LiteralExpression:
			case GrammarKind::IdentifierExpression:
			case GrammarKind::ParenthesizedExpression:
			case GrammarKind::CastExpression:
			case GrammarKind::UnaryExpression:
			case GrammarKind::BinaryExpression:
			case GrammarKind::ConditionalExpression:
			case GrammarKind::CallExpression:
			case GrammarKind::SubscriptExpression:
			case GrammarKind::MemberExpression:
				NodeType(node);
				break;
			default:
				m_out.m_node_type[node] = TypeTable::Unknown;
				break;
			}
		}

		const auto instanceCount = m_out.mInstantiations.primary.size();
		for (std::uint32_t instance = 0; instance < instanceCount; ++instance)
		{
			for (std::uint32_t node = 0; node < m_nodes.size(); ++node)
			{
				if (m_nodes.Kind(node) == GrammarKind::ParameterDeclaration)
				{
					m_out.Substitute(m_out.ParameterType(node), instance);
				}
			}
		}

		BuildDependentReferences();
		std::ranges::sort(m_out.mTemplateCalls, {}, &TemplateCallRecord::node);
	}

	TypeModel Typer::Type(const SemanticModel& model)
	{
		const auto nodes = model.Tree().NodesSoA().size();
		TypeModel result(model, std::max<std::size_t>(32 * 1024, nodes * 24));
		TyperImpl(result, model).Run();
		return result;
	}

} // namespace heimdall
