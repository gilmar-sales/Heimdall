#include <Heimdall/TypeModel.hpp>

namespace heimdall
{
	TemplateArgumentTable::TemplateArgumentTable(std::pmr::memory_resource* resource) :
		kind(resource), type(resource), parameter(resource), sourceBegin(resource), sourceEnd(resource) {}

	InstantiationTable::InstantiationTable(std::pmr::memory_resource* resource) :
		primary(resource), selected(resource), firstArgument(resource), argumentCount(resource),
		state(resource),
		firstBase(resource), baseCount(resource), baseType(resource) {}

	DependentReferenceTable::DependentReferenceTable(std::pmr::memory_resource* resource) :
		node(resource), token(resource), definitionScope(resource), name(resource), receiver(resource),
		firstBinding(resource), bindingCount(resource), coverage(resource), bindingInstance(resource),
		bindingTarget(resource), bindingState(resource) {}

	TypeId TypeModel::SubstitutedType(TypeId type, std::uint32_t instance) const noexcept
	{
		if (instance >= mInstantiations.primary.size() || mInstantiations.state[instance] != TemplateResolution::Resolved)
		{
			return TypeTable::Unknown;
		}

		if (m_types.Kind(type) == TypeKind::TemplateParameter)
		{
			const auto first = mInstantiations.firstArgument[instance];
			for (std::uint32_t i = 0; i < mInstantiations.argumentCount[instance]; ++i)
			{
				if (mTemplateArguments.parameter[first + i] == m_types.Arg(type))
				{
					return mTemplateArguments.type[first + i];
				}
			}

			return TypeTable::Unknown;
		}

		if (!m_types.IsDependent(type))
		{
			return type;
		}

		const auto kind = m_types.Kind(type);
		if (kind != TypeKind::Pointer && kind != TypeKind::LRef && kind != TypeKind::RRef && kind != TypeKind::Const && kind != TypeKind::Array)
		{
			return TypeTable::Unknown;
		}

		const auto inner = SubstitutedType(m_types.Arg(type), instance);
		if (inner == TypeTable::Unknown)
		{
			return TypeTable::Unknown;
		}

		for (TypeId candidate = 1; candidate < m_types.Size(); ++candidate)
		{
			if (m_types.Kind(candidate) == kind && m_types.Arg(candidate) == inner && m_types.Extent(candidate) == m_types.Extent(type))
			{
				return candidate;
			}
		}

		return TypeTable::Unknown;
	}

	TypeId TypeModel::Substitute(TypeId type, std::uint32_t instance)
	{
		if (instance >= mInstantiations.primary.size())
		{
			return TypeTable::Unknown;
		}

		if (m_types.Kind(type) == TypeKind::TemplateParameter)
		{
			const auto first = mInstantiations.firstArgument[instance];
			for (std::uint32_t i = 0; i < mInstantiations.argumentCount[instance]; ++i)
			{
				if (mTemplateArguments.parameter[first + i] == m_types.Arg(type))
				{
					return mTemplateArguments.type[first + i];
				}
			}

			return type;
		}

		switch (m_types.Kind(type))
		{
		case TypeKind::Pointer:
			return m_types.Pointer(Substitute(m_types.Arg(type), instance));
		case TypeKind::LRef:
			return m_types.LRef(Substitute(m_types.Arg(type), instance));
		case TypeKind::RRef:
			return m_types.RRef(Substitute(m_types.Arg(type), instance));
		case TypeKind::Const:
			return m_types.Const(Substitute(m_types.Arg(type), instance));
		case TypeKind::Array:
			return m_types.Array(Substitute(m_types.Arg(type), instance), m_types.Extent(type));
		default:
			return type;
		}
	}

	TemplateMemberBinding TypeModel::LookupInstantiatedMember(std::uint32_t instance, NameId name) const
	{
		if (instance >= mInstantiations.primary.size())
		{
			return {};
		}

		const auto state = mInstantiations.state[instance];
		if (state != TemplateResolution::Resolved)
		{
			return {kNone, state};
		}

		const auto declaration = mInstantiations.selected[instance];
		if (declaration >= m_model->Templates().owner.size())
		{
			return {};
		}

		const auto owner = m_model->Templates().owner[declaration];
		if (owner >= m_model->Symbols().Size())
		{
			return {};
		}

		const auto& symbols = m_model->Symbols();
		const auto symbol   = m_model->LookupLocal(symbols.member_scope[owner], name);
		if (symbol == kNone)
		{
			TemplateMemberBinding result;
			bool incomplete = false;
			for (std::uint32_t i = 0; i < mInstantiations.baseCount[instance]; ++i)
			{
				const auto binding = LookupTypeMember(mInstantiations.baseType[mInstantiations.firstBase[instance] + i],
					name, 1);
				if (binding.state == TemplateResolution::Ambiguous)
				{
					return binding;
				}

				incomplete = incomplete || binding.state != TemplateResolution::Resolved;
				if (binding.symbol != kNone)
				{
					if (result.symbol != kNone)
					{
						return {kNone, TemplateResolution::Ambiguous};
					}

					result = binding;
				}
			}

			return incomplete ? TemplateMemberBinding {}: result;
		}

		if (symbols.next_same_name[symbol] != kNone || symbols.kind[symbol] == SymbolKind::Function)
		{
			return {kNone, TemplateResolution::Ambiguous};
		}

		return symbols.kind[symbol] == SymbolKind::Variable
		? TemplateMemberBinding {symbol, TemplateResolution::Resolved}: TemplateMemberBinding {};
	}

	TemplateMemberBinding TypeModel::LookupTypeMember(TypeId type, NameId name,
		std::uint32_t depth) const
	{
		constexpr std::uint32_t kMaxLookupDepth = 64;
		if (depth >= kMaxLookupDepth || m_types.IsDependent(type))
		{
			return {};
		}

		SymbolId owner         = kNone;
		std::uint32_t instance = kNone;
		if (m_types.Kind(type) == TypeKind::Class)
		{
			owner = m_types.Arg(type);
		}
		else if (m_types.Kind(type) == TypeKind::TemplateId)
		{
			instance = m_types.Arg(type);
			if (instance >= mInstantiations.state.size() || mInstantiations.state[instance] != TemplateResolution::Resolved)
			{
				return {};
			}

			owner = m_model->Templates().owner[mInstantiations.selected[instance]];
		}

		return LookupMemberView(owner, instance, name, depth);
	}

	TemplateMemberBinding TypeModel::LookupMemberView(
		SymbolId owner,
		std::uint32_t instance,
		NameId name,
		std::uint32_t depth) const
	{
		constexpr std::uint32_t kMaxLookupDepth = 64;
		const auto& symbols                     = m_model->Symbols();
		if (owner >= symbols.Size() || depth >= kMaxLookupDepth)
		{
			return {};
		}

		const auto symbol = m_model->LookupLocal(symbols.member_scope[owner], name);
		if (symbol != kNone)
		{
			return symbols.kind[symbol] == SymbolKind::Variable && symbols.next_same_name[symbol] == kNone
			? TemplateMemberBinding {symbol, TemplateResolution::Resolved}
			: TemplateMemberBinding
			{
				kNone, TemplateResolution::Ambiguous
			};
		}

		TemplateMemberBinding result {kNone, TemplateResolution::Resolved};
		bool incomplete  = false;
		const auto count = instance == kNone ? symbols.base_count[owner] : mInstantiations.baseCount[instance];
		for (std::uint32_t i = 0; i < count; ++i)
		{
			TemplateMemberBinding binding;
			if (instance != kNone)
			{
				binding = LookupTypeMember(mInstantiations.baseType[mInstantiations.firstBase[instance] + i], name,
					depth + 1);
			}
			else
			{
				const auto base = m_model->Bases().target[symbols.first_base[owner] + i];
				if (base != kNone)
				{
					binding = LookupMemberView(base, kNone, name, depth + 1);
				}
			}

			if (binding.state == TemplateResolution::Ambiguous)
			{
				return binding;
			}

			incomplete = incomplete || binding.state != TemplateResolution::Resolved;
			if (binding.symbol != kNone)
			{
				if (result.symbol != kNone)
				{
					return {kNone, TemplateResolution::Ambiguous};
				}

				result = binding;
			}
		}

		return incomplete ? TemplateMemberBinding {}: result;
	}

	TemplateMemberBinding TypeModel::DependentBinding(std::uint32_t token) const noexcept
	{
		const auto& references = mDependentReferences;
		for (std::uint32_t row = 0; row < references.token.size(); ++row)
		{
			if (references.token[row] != token)
			{
				continue;
			}

			SymbolId target = kNone;
			bool incomplete = references.bindingCount[row] == 0;
			for (std::uint32_t i = 0; i < references.bindingCount[row]; ++i)
			{
				const auto binding = references.firstBinding[row] + i;
				if (references.bindingState[binding] == TemplateResolution::Ambiguous)
				{
					return {kNone, TemplateResolution::Ambiguous};
				}

				if (references.bindingState[binding] != TemplateResolution::Resolved)
				{
					incomplete = true;
					continue;
				}

				const auto symbol = references.bindingTarget[binding];
				if (symbol == kNone)
				{
					incomplete = true;
					continue;
				}

				if (target != kNone && target != symbol)
				{
					return {kNone, TemplateResolution::Ambiguous};
				}

				target = symbol;
			}

			if (incomplete || references.coverage[row] != TemplateCoverage::Invariant)
			{
				return {};
			}

			return {target, TemplateResolution::Resolved};
		}

		return {};
	}
}
