#include <Heimdall/TypeModel.hpp>

namespace heimdall
{
	namespace
	{
		[[nodiscard]] SubstitutionResult Invalid(
			SubstitutionContext context,
			SubstitutionReason reason)
		{
			return {TypeTable::Unknown, context == SubstitutionContext::Immediate
				? SubstitutionState::SubstitutionFailure : SubstitutionState::HardError, reason};
		}
	}

	SubstitutionResult TypeTable::Substitute(
		TypeId type,
		std::span<const TypeBinding> bindings,
		SubstitutionContext context,
		std::uint32_t remainingDepth)
	{
		if (remainingDepth == 0)
		{
			return {Unknown, SubstitutionState::Incomplete, SubstitutionReason::DepthLimit};
		}

		const auto kind = Kind(type);
		if (kind == TypeKind::Unknown)
		{
			return {};
		}

		if (kind == TypeKind::TemplateParameter)
		{
			for (const auto& binding : bindings)
			{
				if (binding.parameter == Arg(type))
				{
					if (binding.type == Unknown || Kind(binding.type) == TypeKind::Unknown)
					{
						return {};
					}

					if (IsDependent(binding.type))
					{
						return {binding.type, SubstitutionState::Dependent, SubstitutionReason::None};
					}

					return Substitute(binding.type, {}, context, remainingDepth - 1);
				}
			}

			return {type, SubstitutionState::Dependent, SubstitutionReason::None};
		}

		if (kind == TypeKind::DependentName || kind == TypeKind::DependentMember || kind == TypeKind::DecltypeExpression || kind == TypeKind::TemplateId ||
			kind == TypeKind::InjectedClass)
		{
			return {Unknown, SubstitutionState::Incomplete, SubstitutionReason::Unsupported};
		}

		if (kind != TypeKind::Pointer && kind != TypeKind::LRef && kind != TypeKind::RRef &&
			kind != TypeKind::Const && kind != TypeKind::Array)
		{
			return {type, SubstitutionState::Success, SubstitutionReason::None};
		}

		const auto inner = Substitute(Arg(type), bindings, context, remainingDepth - 1);
		return FormSubstitutedType(kind, inner, Extent(type), context);
	}

	SubstitutionResult TypeTable::FormSubstitutedType(
		TypeKind kind,
		SubstitutionResult inner,
		std::uint32_t extent,
		SubstitutionContext context)
	{
		if (inner.state == SubstitutionState::Success && Kind(inner.type) == TypeKind::Unknown)
		{
			return {};
		}

		if (inner.state != SubstitutionState::Success && inner.state != SubstitutionState::Dependent)
		{
			return inner;
		}

		const auto innerKind = Kind(inner.type);
		TypeId result        = Unknown;
		switch (kind)
		{
		case TypeKind::Pointer:
			if (innerKind == TypeKind::LRef || innerKind == TypeKind::RRef)
			{
				return Invalid(context, SubstitutionReason::PointerToReference);
			}

			result = Pointer(inner.type);
			break;
		case TypeKind::LRef:
		case TypeKind::RRef:
			if (IsBuiltin(Strip(inner.type), BuiltinType::Void))
			{
				return Invalid(context, SubstitutionReason::ReferenceToVoid);
			}

			result = kind == TypeKind::LRef ? LRef(inner.type) : RRef(inner.type);
			break;
		case TypeKind::Const:
			result = Const(inner.type);
			break;
		case TypeKind::Array:
			if (innerKind == TypeKind::LRef || innerKind == TypeKind::RRef ||
				IsBuiltin(Strip(inner.type), BuiltinType::Void))
			{
				return Invalid(context, SubstitutionReason::InvalidArrayElement);
			}

			if (extent == 0)
			{
				return Invalid(context, SubstitutionReason::ZeroArrayBound);
			}

			if (extent == kNone)
			{
				return {Unknown, SubstitutionState::Incomplete, SubstitutionReason::UnknownArrayBound};
			}

			result = Array(inner.type, extent);
			break;
		default:
			return {Unknown, SubstitutionState::Incomplete, SubstitutionReason::Unsupported};
		}

		return {result, inner.state, SubstitutionReason::None};
	}
}
