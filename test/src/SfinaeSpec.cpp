#include <algorithm>
#include <array>
#include <memory_resource>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <Heimdall/TypeModel.hpp>
#include <Heimdall/AnalysisFeatures.hpp>
#include <Heimdall/Lexer.hpp>
#include <Heimdall/SemanticRules.hpp>

namespace
{
    struct Typed
    {
        explicit Typed(std::string text) :
            source(std::move(text)), tree(heimdall::ParseTree::Parse(source)),
            model(heimdall::Binder::Bind(tree)), types(heimdall::Typer::Type(model)) {}

        [[nodiscard]] std::string Of(std::string_view name) const
        {
            const auto nameId = model.Names().Find(name);
            for (heimdall::SymbolId symbol = 0; symbol < model.Symbols().Size(); ++symbol)
            {
                if (model.Symbols().name[symbol] == nameId)
                {
                    return types.Spell(types.SymbolType(symbol));
                }
            }

            return "<missing>";
        }

        std::string source;
        heimdall::ParseTree tree;
        heimdall::SemanticModel model;
        heimdall::TypeModel types;
    };
}

TEST(SfinaeSpec, MissingImmediateMemberSelectsEllipsisFallback)
{
    const Typed typed(
R"cpp(
template<class T> typename T::type Probe(T);
long Probe(...);
auto result = Probe(1);
)cpp");

        EXPECT_TRUE(typed.tree.Diagnostics().empty());
    ASSERT_EQ(typed.types.TemplateCalls().size(), 1);
    EXPECT_EQ(typed.types.TemplateCalls().front().result.state, heimdall::SubstitutionState::Success)
    << static_cast<int>(typed.types.TemplateCalls().front().result.reason);
    EXPECT_EQ(typed.Of("result"), "long");
}

TEST(SfinaeSpec, ValidImmediateMemberPreservesTemplateCandidate)
{
    const Typed typed(
R"cpp(
struct Value { using type = int; };
template<class T> typename T::type Probe(T);
long Probe(...);
Value value;
auto result = Probe(value);
)cpp");

        EXPECT_TRUE(typed.tree.Diagnostics().empty());
    EXPECT_EQ(typed.Of("result"), "int");
}

TEST(SfinaeSpec, ReferenceToVoidRejectsExplicitCandidate)
{
    const Typed typed(
R"cpp(
template<class T> T& Probe(int);
template<class T> long Probe(...);
auto result = Probe<void>(1);
)cpp");

        EXPECT_TRUE(typed.tree.Diagnostics().empty());
    EXPECT_EQ(typed.Of("result"), "long");
}

TEST(SfinaeSpec, UnknownHeaderDoesNotEliminateCandidate)
{
    const Typed typed(
R"cpp(
template<class T> typename T::type Probe(T);
long Probe(...);
std::unknown_type value;
auto result = Probe(value);
)cpp");

        EXPECT_EQ(typed.Of("result"), "<unknown>");
}

TEST(SfinaeSpec, DefaultsSubstituteEarlierArguments)
{
    const Typed typed(
R"cpp(
struct Value { using type = short; };
template<class T, class U = typename T::type> U Probe(T);
long Probe(...);
Value value;
auto valid = Probe(value);
auto fallback = Probe(1);
)cpp");

        EXPECT_EQ(typed.Of("valid"), "short");
    EXPECT_EQ(typed.Of("fallback"), "long");
}

TEST(SfinaeSpec, TrailingReturnUsesSameImmediateContext)
{
    const Typed typed(
R"cpp(
template<class T> auto Probe(T) -> typename T::type;
long Probe(...);
auto result = Probe(1);
)cpp");

        EXPECT_EQ(typed.Of("result"), "long");
}

TEST(SfinaeSpec, ClassInstantiationFailureIsNotSwallowedByFallback)
{
    const Typed typed(
R"cpp(
template<class T> struct Helper { using type = typename T::missing; };
template<class T> typename Helper<T>::type Probe(T);
long Probe(...);
auto result = Probe(1);
)cpp");

        ASSERT_EQ(typed.types.TemplateCalls().size(), 1);
    EXPECT_EQ(typed.types.TemplateCalls().front().result.state, heimdall::SubstitutionState::HardError);
    EXPECT_EQ(typed.Of("result"), "<unknown>");
}

TEST(SfinaeSpec, EarlierDefaultFailurePreventsLaterClassInstantiation)
{
    const Typed typed(
R"cpp(
template<class T> struct Helper { using type = typename T::missing; };
template<class T, class U = typename T::missing, class V = typename Helper<T>::type>
int Probe(T);
long Probe(...);
auto result = Probe(1);
)cpp");

        EXPECT_EQ(typed.Of("result"), "long");
    ASSERT_EQ(typed.types.TemplateCalls().size(), 1);
    EXPECT_EQ(typed.types.TemplateCalls().front().result.state, heimdall::SubstitutionState::Success);
}

TEST(SfinaeSpec, PrefixReturnFailurePreventsParameterClassInstantiation)
{
    const Typed typed(
R"cpp(
template<class T> struct Helper { using type = typename T::missing; };
template<class T> typename T::missing Probe(typename Helper<T>::type);
template<class T> long Probe(...);
auto result = Probe<int>(1);
)cpp");

        EXPECT_EQ(typed.Of("result"), "long");
}

TEST(SfinaeSpec, TrailingReturnDoesNotProtectEarlierParameterInstantiation)
{
    const Typed typed(
R"cpp(
template<class T> struct Helper { using type = typename T::missing; };
template<class T> auto Probe(typename Helper<T>::type) -> typename T::missing;
template<class T> long Probe(...);
auto result = Probe<int>(1);
)cpp");

        ASSERT_EQ(typed.types.TemplateCalls().size(), 1);
    EXPECT_EQ(typed.types.TemplateCalls().front().result.state, heimdall::SubstitutionState::HardError);
    EXPECT_EQ(typed.Of("result"), "<unknown>");
}

TEST(SfinaeSpec, UnusedAliasArgumentStillParticipatesInSubstitution)
{
    const Typed typed(
R"cpp(
template<class T> using Ignore = int;
template<class T> Ignore<typename T::missing> Probe(T);
long Probe(...);
auto result = Probe(1);
)cpp");

        EXPECT_EQ(typed.Of("result"), "long");
}

TEST(SfinaeSpec, AliasTemplateExpandsWithCanonicalTypeArguments)
{
    const Typed typed(
R"cpp(
template<class T> using Identity = T;
template<class T> Identity<T> Probe(T);
long Probe(...);
auto result = Probe(1);
)cpp");

        EXPECT_EQ(typed.Of("result"), "int");
}

TEST(SfinaeSpec, EmptyLocalClassProvesMissingMember)
{
    const Typed typed(
R"cpp(
struct Empty {};
template<class T> typename T::missing Probe(T);
long Probe(...);
Empty value;
auto result = Probe(value);
)cpp");

        EXPECT_EQ(typed.Of("result"), "long");
}

TEST(SfinaeSpec, ValueMemberIsNotATypeMember)
{
    const Typed typed(
R"cpp(
struct Value { int type; };
template<class T> typename T::type Probe(T);
long Probe(...);
Value value;
auto result = Probe(value);
)cpp");

        EXPECT_EQ(typed.Of("result"), "long");
}

TEST(SfinaeSpec, ForwardDeclarationDoesNotProveMemberAbsence)
{
    const Typed typed(
R"cpp(
struct Forward;
template<class T> typename T::type Probe(T*);
long Probe(...);
Forward* value;
auto result = Probe(value);
)cpp");

        ASSERT_EQ(typed.types.TemplateCalls().size(), 1);
    EXPECT_EQ(typed.types.TemplateCalls().front().result.state,
              heimdall::SubstitutionState::Incomplete);
    EXPECT_EQ(typed.Of("result"), "<unknown>");
}

TEST(SfinaeSpec, ExplicitReferenceTypeDoesNotProvideAClassScope)
{
    const Typed typed(
R"cpp(
struct Value { using type = int; };
template<class T> typename T::type Probe(int);
template<class T> long Probe(...);
auto result = Probe<Value&>(1);
)cpp");

        EXPECT_EQ(typed.Of("result"), "long");
}

TEST(SfinaeSpec, UnknownConstraintsDoNotEnableFallback)
{
    const Typed typed(
R"cpp(
template<class T> requires Unknown<T> int Probe(T);
long Probe(...);
auto result = Probe(1);
)cpp");

        EXPECT_EQ(typed.Of("result"), "<unknown>");
}

TEST(SfinaeSpec, PartialOrderingSelectsThePointerPattern)
{
    const Typed typed(
R"cpp(
template<class T> int Probe(T);
template<class T> long Probe(T*);
int* value;
auto result = Probe(value);
)cpp");

        ASSERT_EQ(typed.types.TemplateCalls().size(), 1);
    EXPECT_EQ(typed.types.TemplateCalls().front().result.state,
              heimdall::SubstitutionState::Success);
    EXPECT_EQ(typed.Of("result"), "long");
}

TEST(SfinaeSpec, ConflictingDeductionDiscardsOnlyThatCandidate)
{
    const Typed typed(
R"cpp(
template<class T> T Probe(T, T);
long Probe(...);
auto result = Probe(1, 2.0);
)cpp");

        EXPECT_EQ(typed.Of("result"), "long");
}

TEST(SfinaeSpec, ExplicitArgumentAllowsConversionInsteadOfRededuction)
{
	const Typed typed(R"cpp(
template<class T> T Probe(T);
auto result = Probe<long>(1);
)cpp");

    EXPECT_EQ(typed.Of("result"), "long");
}

TEST(SfinaeSpec, CandidateRequiringReturnDeductionRemainsIncomplete)
{
    const Typed typed(
R"cpp(
template<class T> auto Probe(T value) { return value; }
long Probe(...);
auto result = Probe(1);
)cpp");

        EXPECT_EQ(typed.Of("result"), "<unknown>");
}

TEST(SfinaeSpec, StructuralSubstitutionDistinguishesProtectionFromInstantiation)
{
    std::pmr::monotonic_buffer_resource resource;
    heimdall::TypeTable table(&resource);
    const auto parameter = table.Parameter(0);
    const auto pattern = table.Pointer(parameter);
    const std::array bindings
    {
        heimdall::TypeBinding {0, table.LRef(table.Builtin(heimdall::BuiltinType::Int))}
    };

    const auto immediate = table.Substitute(pattern, bindings);
    const auto effect = table.Substitute(pattern, bindings,
        heimdall::SubstitutionContext::Instantiation);

    EXPECT_EQ(immediate.state, heimdall::SubstitutionState::SubstitutionFailure);
    EXPECT_EQ(immediate.reason, heimdall::SubstitutionReason::PointerToReference);
    EXPECT_EQ(effect.state, heimdall::SubstitutionState::HardError);
    EXPECT_EQ(effect.type, heimdall::TypeTable::Unknown);
}

TEST(SfinaeSpec, StructuralReferenceCollapsingDoesNotBecomeFailure)
{
    std::pmr::monotonic_buffer_resource resource;
    heimdall::TypeTable table(&resource);
    const auto parameter = table.Parameter(0);
    const auto integer = table.Builtin(heimdall::BuiltinType::Int);
    const auto lref = table.LRef(integer);
    const std::array bindings
    {
        heimdall::TypeBinding {0, lref}
    };

    const auto result = table.Substitute(table.RRef(parameter), bindings);

    EXPECT_EQ(result.state, heimdall::SubstitutionState::Success);
    EXPECT_EQ(result.type, lref);
}

TEST(SfinaeSpec, ArraysRejectInvalidElementsAndZeroBounds)
{
    std::pmr::monotonic_buffer_resource resource;
    heimdall::TypeTable table(&resource);
    const auto parameter = table.Parameter(0);
    const auto integer = table.Builtin(heimdall::BuiltinType::Int);
    const std::array voidBinding
    {
        heimdall::TypeBinding {0, table.Builtin(heimdall::BuiltinType::Void)}
    };
    const std::array integerBinding
    {
        heimdall::TypeBinding {0, integer}
    };

    const auto invalidElement = table.Substitute(table.Array(parameter, 1), voidBinding);
    const auto invalidBound = table.Substitute(table.Array(parameter, 0), integerBinding);
    const auto unknownBound = table.Substitute(table.Array(parameter, heimdall::kNone), integerBinding);

    EXPECT_EQ(invalidElement.reason, heimdall::SubstitutionReason::InvalidArrayElement);
    EXPECT_EQ(invalidElement.state, heimdall::SubstitutionState::SubstitutionFailure);
    EXPECT_EQ(invalidBound.reason, heimdall::SubstitutionReason::ZeroArrayBound);
    EXPECT_EQ(unknownBound.state, heimdall::SubstitutionState::Incomplete);
}

TEST(SfinaeSpec, UnknownBindingsAndBudgetsNeverProveFailure)
{
    std::pmr::monotonic_buffer_resource resource;
    heimdall::TypeTable table(&resource);
    const auto parameter = table.Parameter(0);
    const std::array bindings
    {
        heimdall::TypeBinding {0, table.Pointer(heimdall::TypeTable::Unknown)}
    };

    const auto unknown = table.Substitute(parameter, bindings);
    const auto dependent = table.Substitute(parameter, {});
    const auto bounded = table.Substitute(parameter, {}, heimdall::SubstitutionContext::Immediate, 0);

    EXPECT_EQ(unknown.state, heimdall::SubstitutionState::Incomplete);
    EXPECT_EQ(dependent.state, heimdall::SubstitutionState::Dependent);
    EXPECT_EQ(bounded.state, heimdall::SubstitutionState::Incomplete);
    EXPECT_EQ(bounded.reason, heimdall::SubstitutionReason::DepthLimit);
}

TEST(SfinaeSpec, DecltypeArithmeticSubstitutesParameterTypes)
{
    const Typed typed(
R"cpp(
template<class T> auto Probe(T first, T second) -> decltype(first + second);
long Probe(...);
auto result = Probe(1, 2);
)cpp");

        EXPECT_TRUE(typed.tree.Diagnostics().empty());
    EXPECT_EQ(typed.Of("result"), "int");
}

TEST(SfinaeSpec, InvalidDecltypeDereferenceSelectsFallback)
{
    const Typed typed(
R"cpp(
template<class T> auto Probe(T value) -> decltype(*value);
long Probe(...);
auto result = Probe(1);
)cpp");

        EXPECT_TRUE(typed.tree.Diagnostics().empty());
    EXPECT_EQ(typed.Of("result"), "long");
}

TEST(SfinaeSpec, ValidDecltypeDereferencePreservesLvalueCategory)
{
    const Typed typed(
R"cpp(
template<class T> auto Probe(T value) -> decltype(*value);
long Probe(...);
int* pointer;
auto&& result = Probe(pointer);
)cpp");

        EXPECT_EQ(typed.Of("result"), "int&");
}

TEST(SfinaeSpec, UnparenthesizedAndParenthesizedDecltypeAreDifferent)
{
    const Typed typed(
R"cpp(
template<class T> auto Plain(T value) -> decltype(value);
template<class T> auto Reference(T value) -> decltype((value));
auto&& plain = Plain(1);
auto&& reference = Reference(1);
)cpp");

        EXPECT_EQ(typed.Of("plain"), "int&&");
    EXPECT_EQ(typed.Of("reference"), "int&");
}

TEST(SfinaeSpec, UnmodeledClassOperatorIsNotAnInvalidExpression)
{
    const Typed typed(
R"cpp(
struct Value {};
template<class T> auto Probe(T first, T second) -> decltype(first + second);
long Probe(...);
Value value;
auto result = Probe(value, value);
)cpp");

        EXPECT_EQ(typed.Of("result"), "<unknown>");
    ASSERT_EQ(typed.types.TemplateCalls().size(), 1);
    EXPECT_EQ(typed.types.TemplateCalls().front().result.state,
              heimdall::SubstitutionState::Incomplete);
}

TEST(SfinaeSpec, AliasTemplateIsVisibleOutsideItsParameterScope)
{
    const Typed typed("template<class T> using Identity = T; Identity<int> value;");
    const auto alias = typed.model.LookupLocal(heimdall::SemanticModel::TranslationUnitScope,
        typed.model.Names().Find("Identity"));

    ASSERT_NE(alias, heimdall::kNone);
    EXPECT_NE(typed.model.Symbols().flags[alias] & heimdall::SymbolFlag::Template, 0);
    EXPECT_EQ(typed.Of("value"), "int");
}

TEST(SfinaeSpec, UnnamedDependentParameterKeepsItsEntireTypeSpecifier)
{
    const Typed typed("template<class T> void Probe(typename T::type);");
    const auto& nodes = typed.tree.NodesSoA();
    bool found = false;

    for (std::uint32_t node = 0; node < nodes.size(); ++node)
    {
        if (nodes.Kind(node) == heimdall::GrammarKind::ParameterDeclaration)
        {
            found = true;
            EXPECT_EQ(typed.types.Types().Kind(typed.types.ParameterType(node)),
                      heimdall::TypeKind::DependentMember);
            EXPECT_EQ(typed.types.Spell(typed.types.ParameterType(node)), "T::type");
            for (const auto child : typed.model.ChildrenOf(node))
            {
                EXPECT_NE(nodes.Kind(child), heimdall::GrammarKind::Declarator);
            }
        }
    }

    EXPECT_TRUE(found);
}

TEST(SfinaeSpec, SuccessfulFallbackHasNoSubstitutionDiagnostic)
{
    const Typed typed(
        "template<class T> typename T::missing Probe(T); long Probe(...); auto result = Probe(1);");

    const auto diagnostics = heimdall::SemanticRules::AnalyzeTemplateSubstitution(typed.types);

    EXPECT_TRUE(diagnostics.empty());
}

TEST(SfinaeSpec, HardInstantiationFailureProducesOneDiagnosticWithoutFix)
{
    const Typed typed(
R"cpp(
template<class T> struct Helper { using type = typename T::missing; };
template<class T> typename Helper<T>::type Probe(T);
long Probe(...);
auto result = Probe(1);
)cpp");

        const auto diagnostics = heimdall::SemanticRules::AnalyzeTemplateSubstitution(typed.types);

    ASSERT_EQ(diagnostics.size(), 1);
    EXPECT_EQ(diagnostics.front().rule, heimdall::RuleId::TemplateSubstitution);
    EXPECT_EQ(diagnostics.front().severity, heimdall::Severity::Error);
    EXPECT_FALSE(diagnostics.front().has_fix);
    EXPECT_EQ(typed.source.substr(diagnostics.front().offset, diagnostics.front().length), "Probe");
}

TEST(SfinaeSpec, NoFallbackProducesAFinalNoViableOverloadDiagnostic)
{
    const Typed typed("template<class T> typename T::missing Probe(T); auto result = Probe(1);");

    const auto diagnostics = heimdall::SemanticRules::AnalyzeTemplateSubstitution(typed.types);

    ASSERT_EQ(diagnostics.size(), 1);
    EXPECT_EQ(typed.types.TemplateCalls().front().result.state, heimdall::SubstitutionState::NoMatch);
    EXPECT_EQ(diagnostics.front().code, "cpp/template-substitution");
}

TEST(SfinaeSpec, OpenIncludeEnvironmentDoesNotProveNoMatch)
{
    const Typed typed(
        "#include \"unknown.hpp\"\ntemplate<class T> typename T::missing Probe(T); auto result = Probe(1);");

    const auto diagnostics = heimdall::SemanticRules::AnalyzeTemplateSubstitution(typed.types);

    EXPECT_TRUE(diagnostics.empty());
    ASSERT_EQ(typed.types.TemplateCalls().size(), 1);
    EXPECT_EQ(typed.types.TemplateCalls().front().result.state,
              heimdall::SubstitutionState::Incomplete);
}

TEST(SfinaeSpec, WorkspaceRebuildsResultsWithoutChangingOldSnapshot)
{
    heimdall::Workspace workspace;
    const auto source = std::make_shared<const std::string>(
        "template<class T> typename T::missing Probe(T); auto result = Probe(1);");
    const auto id = workspace.Open("sfinae.cpp", source, 1);
    ASSERT_TRUE(id);
    const auto old = workspace.Snapshot();
    const auto oldTypes = old.Types(*id);
    ASSERT_EQ(oldTypes->TemplateCalls().size(), 1);
    EXPECT_EQ(oldTypes->TemplateCalls().front().result.state, heimdall::SubstitutionState::NoMatch);

    ASSERT_TRUE(workspace.Update(*id, std::make_shared<const std::string>(
        "template<class T> typename T::missing Probe(T); long Probe(...); auto result = Probe(1);"), 2));
    const auto current = workspace.Snapshot();
    const auto currentTypes = current.Types(*id);

    ASSERT_EQ(currentTypes->TemplateCalls().size(), 1);
    EXPECT_EQ(currentTypes->TemplateCalls().front().result.state, heimdall::SubstitutionState::Success);
    EXPECT_NE(currentTypes, oldTypes);
    EXPECT_EQ(current.Types(*id), currentTypes);
    EXPECT_EQ(oldTypes->TemplateCalls().front().result.state, heimdall::SubstitutionState::NoMatch);
    EXPECT_TRUE(heimdall::SemanticRules::AnalyzeTemplateSubstitution(*currentTypes).empty());
}

TEST(SfinaeSpec, DiagnosticDispatchHonorsRuleDisableAndSeverity)
{
    heimdall::Workspace workspace;
    const auto id = workspace.Open("sfinae.cpp", std::make_shared<const std::string>(
        "template<class T> typename T::missing Probe(T); auto result = Probe(1);"));
    ASSERT_TRUE(id);
    heimdall::AnalysisContext context(workspace.Snapshot(), *id);
    heimdall::RuleOptions options;
    for (const auto& rule : heimdall::RuleCatalog())
    {
        options.overrides.push_back({std::string(rule.code), false, rule.default_severity});
    }

    EXPECT_TRUE(heimdall::AnalysisFeatures::Diagnostics(context, heimdall::RuleEngine(options),
                                                        true).empty());
    options.overrides.push_back({"cpp/template-substitution", true, heimdall::Severity::Warning});
    const auto diagnostics = heimdall::AnalysisFeatures::Diagnostics(context,
        heimdall::RuleEngine(options), true);

    ASSERT_EQ(diagnostics.size(), 1);
    EXPECT_EQ(diagnostics.front().severity, heimdall::Severity::Warning);
}

TEST(SfinaeSpec, PrototypeParameterInDecltypeShadowsOuterVariable)
{
    const Typed typed(
        "long value; template<class T> auto Probe(T value) -> decltype(value); auto result = Probe(1);");

    EXPECT_EQ(typed.Of("result"), "int");
}

TEST(SfinaeSpec, VoidParameterListIsNotARequiredArgument)
{
    const Typed typed("template<class T> int Probe(void); auto result = Probe<int>();");

    EXPECT_EQ(typed.Of("result"), "int");
    EXPECT_TRUE(heimdall::SemanticRules::AnalyzeTemplateSubstitution(typed.types).empty());
}

TEST(SfinaeSpec, SubstitutingVoidForAParameterTypeDiscardsCandidate)
{
    const Typed typed(
        "template<class T> int Probe(T); template<class T> long Probe(...); auto result = Probe<void>(1);");

    EXPECT_EQ(typed.Of("result"), "long");
}

TEST(SfinaeSpec, FloatingPromotionOutranksFloatingConversion)
{
    const Typed typed(
        "template<class T> int Probe(double); template<class T> long Probe(long double); auto result = Probe<int>(1.0f);");

    EXPECT_EQ(typed.Of("result"), "int");
}

TEST(SfinaeSpec, UnmodeledReferenceRankingDoesNotSelectAPresumedWinner)
{
    const Typed typed(
        "template<class T> int Probe(T&&); long Probe(const int&); auto result = Probe(1);");

    EXPECT_EQ(typed.Of("result"), "<unknown>");
    EXPECT_TRUE(heimdall::SemanticRules::AnalyzeTemplateSubstitution(typed.types).empty());
}

TEST(SfinaeSpec, SubstitutionBudgetDoesNotPublishNoMatch)
{
    constexpr std::uint32_t kCandidateCount = 129;
    std::string source;
    for (std::uint32_t i = 0; i < kCandidateCount; ++i)
    {
        source += "template<class T> typename T::Member" + std::to_string(i) + " Probe(T);\n";
    }

    const Typed typed(source + "auto result = Probe(1);\n");

    ASSERT_EQ(typed.types.TemplateCalls().size(), 1);
    EXPECT_EQ(typed.types.TemplateCalls().front().result.state,
              heimdall::SubstitutionState::Incomplete);
    EXPECT_EQ(typed.types.TemplateCalls().front().result.reason,
              heimdall::SubstitutionReason::DepthLimit);
    EXPECT_TRUE(heimdall::SemanticRules::AnalyzeTemplateSubstitution(typed.types).empty());
}

TEST(SfinaeSpec, EveryPrefixOfADependentSignatureRemainsBounded)
{
    constexpr std::size_t kDiagnosticLimit = 64;
    const std::string source = "template<class T> using Identity = T; template<class T> auto Probe(T value) -> decltype(*value); long Probe(...); auto result = Probe(1);";

    for (std::size_t prefix = 0; prefix <= source.size(); ++prefix)
    {
        SCOPED_TRACE(prefix);
        const Typed typed(source.substr(0, prefix));
        EXPECT_LE(typed.tree.Diagnostics().size(), kDiagnosticLimit);
        for (const auto& call : typed.types.TemplateCalls())
        {
            EXPECT_LT(call.node, typed.tree.NodesSoA().size());
        }
    }
}

TEST(SfinaeSpec, TriviaAndParameterRenamePreserveSubstitutionOutcome)
{
    const Typed original(
        "template<class T> auto Probe(T value) -> decltype(*value); long Probe(...); auto result = Probe(1);");
    const Typed renamed(
        "template < class U > auto Probe ( U argument ) -> decltype ( *argument );\nlong Probe ( ... );\nauto result = Probe ( 1 );");

    EXPECT_EQ(original.Of("result"), "long");
    EXPECT_EQ(renamed.Of("result"), original.Of("result"));
    ASSERT_EQ(original.types.TemplateCalls().size(), 1);
    ASSERT_EQ(renamed.types.TemplateCalls().size(), 1);
    EXPECT_EQ(original.types.TemplateCalls().front().result.state,
              renamed.types.TemplateCalls().front().result.state);
}

TEST(SfinaeSpec, MacroAndUsingEnvironmentsDoNotCloseTheOverloadSet)
{
    const Typed macro(
        "#define T int\ntemplate<class U> typename U::missing Probe(U); auto result = Probe(1);");
    const Typed directive(
        "namespace Extra {} using namespace Extra; template<class T> typename T::missing Probe(T); auto result = Probe(1);");

    EXPECT_TRUE(heimdall::SemanticRules::AnalyzeTemplateSubstitution(macro.types).empty());
    EXPECT_TRUE(heimdall::SemanticRules::AnalyzeTemplateSubstitution(directive.types).empty());
}

TEST(SfinaeSpec, TemplateClosersAndShiftOperatorsKeepTheirPhysicalTokens)
{
    const std::string source = "Alias<Alias<T>> value; Trait<(N >> 1)> trait; template<class T> auto Probe(T t) -> decltype(*t);";
    const auto tokens = heimdall::Lexer(source).Lex();
    std::string reconstructed;
    std::uint32_t shifts = 0;

    for (const auto& token : tokens)
    {
        reconstructed.append(source.substr(token.offset, token.length));
        shifts += token.tok == heimdall::Tok::Shr ? 1 : 0;
    }

    EXPECT_EQ(reconstructed, source);
    EXPECT_EQ(shifts, 2);
}

TEST(SfinaeSpec, ExplicitSubstitutionDefersUndeducedDefaultsUntilNeeded)
{
    const Typed typed("template<class T, class U = T> U Probe(); auto result = Probe<int>();");

    EXPECT_EQ(typed.Of("result"), "int");
}

TEST(SfinaeSpec, ArrayReturnTypeDiscardsOnlyTheInvalidCandidate)
{
    const Typed typed(
        "typedef int Array[3]; template<class T> T Probe(int); template<class T> long Probe(...); auto result = Probe<Array>(1);");

    EXPECT_EQ(typed.Of("result"), "long");
}

TEST(SfinaeSpec, AliasReferenceCannotBecomeAPointerPointee)
{
    const Typed typed(
        "using Reference = int&; template<class T> T* Probe(int); template<class T> long Probe(...); auto result = Probe<Reference>(1);");

    EXPECT_EQ(typed.Of("result"), "long");
}

TEST(SfinaeSpec, PointerArgumentStillRequiresAssociatedNamespaceLookup)
{
    const Typed typed(
        "namespace Extra { struct Value { using type = int; }; } template<class T> typename T::type Probe(T*); long Probe(...); Extra::Value* value; auto result = Probe(value);");

    EXPECT_EQ(typed.Of("result"), "<unknown>");
    EXPECT_TRUE(heimdall::SemanticRules::AnalyzeTemplateSubstitution(typed.types).empty());
}

TEST(SfinaeSpec, ImportedDeclarationsDoNotCloseTheOverloadSet)
{
    const Typed typed(
        "import Extra; template<class T> typename T::missing Probe(T); auto result = Probe(1);");

    EXPECT_TRUE(heimdall::SemanticRules::AnalyzeTemplateSubstitution(typed.types).empty());
}

TEST(SfinaeSpec, PartialSpecializationBindsItsOwnParameters)
{
    const Typed typed(
R"cpp(
template<class T> struct Trait { using type = long; };
template<class T> struct Trait<T*> { using type = T; };
template<class T> typename Trait<T>::type Probe(T);
int* pointer;
auto result = Probe(pointer);
)cpp");

        EXPECT_EQ(typed.Of("result"), "int");
}

TEST(SfinaeSpec, FailedPartialMatchSelectsPrimaryTemplate)
{
    const Typed typed(
R"cpp(
template<class T> struct Trait { using type = long; };
template<class T> struct Trait<T*> { using type = T; };
template<class T> typename Trait<T>::type Probe(T);
auto result = Probe(1);
)cpp");

        EXPECT_EQ(typed.Of("result"), "long");
}

TEST(SfinaeSpec, PartialSpecializationsUseBidirectionalOrdering)
{
    const Typed typed(
R"cpp(
template<class T> struct Trait { using type = char; };
template<class T> struct Trait<T*> { using type = long; };
template<class T> struct Trait<const T*> { using type = int; };
template<class T> typename Trait<T>::type Probe(T);
const int* pointer;
auto result = Probe(pointer);
)cpp");

        EXPECT_EQ(typed.Of("result"), "int");
}

TEST(SfinaeSpec, AmbiguousPartialSpecializationsRemainAmbiguous)
{
    const Typed typed(
R"cpp(
template<class T, class U> struct Trait { using type = char; };
template<class T, class U> struct Trait<T*, U> { using type = int; };
template<class T, class U> struct Trait<T, U*> { using type = long; };
template<class T> typename Trait<T, T>::type Probe(T);
int* pointer;
auto result = Probe(pointer);
)cpp");

        EXPECT_EQ(typed.Of("result"), "<unknown>");
    ASSERT_EQ(typed.types.TemplateCalls().size(), 1);
    EXPECT_EQ(typed.types.TemplateCalls().front().result.state, heimdall::SubstitutionState::HardError);
    EXPECT_NE(std::ranges::find(typed.types.Instantiations().state,
                                heimdall::TemplateResolution::Ambiguous),
        typed.types.Instantiations().state.end());
}

TEST(SfinaeSpec, VoidAliasPartialSpecializationRejectsMissingType)
{
    const Typed typed(
R"cpp(
template<class...> using Void = void;
template<class T, class = void> struct Trait { using type = long; };
template<class T> struct Trait<T, Void<typename T::type> > { using type = typename T::type; };
template<class T> typename Trait<T>::type Probe(T);
struct Value { using type = int; };
Value value;
auto valid = Probe(value);
auto fallback = Probe(1);
)cpp");

        EXPECT_EQ(typed.Of("valid"), "int");
    EXPECT_EQ(typed.Of("fallback"), "long");
}

TEST(SfinaeSpec, PlainNoexceptDoesNotAffectSelection)
{
    const Typed typed(
R"cpp(
template<class T> int Probe(T) noexcept;
long Probe(...);
auto result = Probe(1);
)cpp");

        EXPECT_EQ(typed.Of("result"), "int");
    EXPECT_TRUE(heimdall::SemanticRules::AnalyzeTemplateSubstitution(typed.types).empty());
}

TEST(SfinaeSpec, DependentNoexceptSpecifierRemainsIncomplete)
{
    const Typed typed(
R"cpp(
template<class T> int Probe(T) noexcept(sizeof(T) > 0);
long Probe(...);
auto result = Probe(1);
)cpp");

        ASSERT_EQ(typed.types.TemplateCalls().size(), 1);
    EXPECT_EQ(typed.types.TemplateCalls().front().result.state,
              heimdall::SubstitutionState::Incomplete);
    EXPECT_EQ(typed.Of("result"), "<unknown>");
    EXPECT_TRUE(heimdall::SemanticRules::AnalyzeTemplateSubstitution(typed.types).empty());
}

TEST(SfinaeSpec, NestedAliasWithSpacedClosersSubstitutes)
{
    const Typed typed(
R"cpp(
template<class T> using Identity = T;
template<class T> Identity<Identity<T> > Probe(T);
long Probe(...);
auto result = Probe(1);
)cpp");

        EXPECT_EQ(typed.Of("result"), "int");
}

TEST(SfinaeSpec, UnspacedNestedClosersRemainIncompleteWithoutGuessing)
{
    const Typed typed(
        "template<class T> using Identity = T; template<class T> Identity<Identity<T>> Probe(T); long Probe(...); auto result = Probe(1);");

    EXPECT_EQ(typed.Of("result"), "<unknown>");
    EXPECT_TRUE(heimdall::SemanticRules::AnalyzeTemplateSubstitution(typed.types).empty());
}

TEST(SfinaeSpec, PointerFunctionPatternOutranksUnconstrainedTypePattern)
{
    const Typed typed(
R"cpp(
template<class T> int Probe(T);
template<class T> long Probe(T*);
int* pointer;
auto result = Probe(pointer);
)cpp");

        EXPECT_EQ(typed.Of("result"), "long");
}

TEST(SfinaeSpec, FalseAssociatedConstraintPreventsSignatureInstantiation)
{
    const Typed typed(
R"cpp(
template<class T> struct Helper { using type = typename T::missing; };
template<class T> requires false typename Helper<T>::type Probe(T);
long Probe(...);
auto result = Probe(1);
)cpp");

        EXPECT_EQ(typed.Of("result"), "long");
    EXPECT_TRUE(heimdall::SemanticRules::AnalyzeTemplateSubstitution(typed.types).empty());
}

TEST(SfinaeSpec, TrueAssociatedConstraintPreservesCandidate)
{
    const Typed typed(
R"cpp(
template<class T> requires true int Probe(T);
long Probe(...);
auto result = Probe(1);
)cpp");

        EXPECT_EQ(typed.Of("result"), "int");
}

TEST(SfinaeSpec, TypeRequirementSatisfactionUsesSubstitutedReceiver)
{
    const Typed typed(
R"cpp(
template<class T> concept HasType = requires { typename T::type; };
template<class T> requires HasType<T> int Probe(T);
long Probe(...);
struct Value { using type = int; };
Value value;
auto valid = Probe(value);
auto fallback = Probe(1);
)cpp");

        EXPECT_EQ(typed.Of("valid"), "int");
    EXPECT_EQ(typed.Of("fallback"), "long");
}

TEST(SfinaeSpec, ConstraintConjunctionStopsBeforeHardInstantiation)
{
    const Typed typed(
R"cpp(
template<class T> struct Helper { using type = typename T::missing; };
template<class T> requires (false && requires { typename Helper<T>::type; }) int Probe(T);
long Probe(...);
auto result = Probe(1);
)cpp");

        EXPECT_EQ(typed.Of("result"), "long");
}

TEST(SfinaeSpec, ConstraintDisjunctionStopsAfterSatisfiedLeftOperand)
{
    const Typed typed(
R"cpp(
template<class T> struct Helper { using type = typename T::missing; };
template<class T> requires (true || requires { typename Helper<T>::type; }) int Probe(T);
long Probe(...);
auto result = Probe(1);
)cpp");

        EXPECT_EQ(typed.Of("result"), "int");
}

TEST(SfinaeSpec, FailedRequirementDoesNotHideRequiredClassInstantiationError)
{
    const Typed typed(
R"cpp(
template<class T> struct Helper { using type = typename T::missing; };
template<class T> requires requires { typename Helper<T>::type; } int Probe(T);
long Probe(...);
auto result = Probe(1);
)cpp");

        ASSERT_EQ(typed.types.TemplateCalls().size(), 1);
    EXPECT_EQ(typed.types.TemplateCalls().front().result.state, heimdall::SubstitutionState::HardError);
}

TEST(SfinaeSpec, ValidNonBooleanAtomicConstraintIsAHardError)
{
    const Typed typed(
R"cpp(
template<class T> requires (1) int Probe(T);
long Probe(...);
auto result = Probe(1);
)cpp");

        ASSERT_EQ(typed.types.TemplateCalls().size(), 1);
    EXPECT_EQ(typed.types.TemplateCalls().front().result.state, heimdall::SubstitutionState::HardError);
}

TEST(SfinaeSpec, ConstrainedClassFailureIsImmediateSubstitutionFailure)
{
    const Typed typed(
R"cpp(
template<class T> requires false struct Helper { using type = int; };
template<class T> typename Helper<T>::type Probe(T);
long Probe(...);
auto result = Probe(1);
)cpp");

        EXPECT_EQ(typed.Of("result"), "long");
}

TEST(SfinaeSpec, ConceptIdentityParticipatesInConstraintSubsumption)
{
    const Typed typed(
R"cpp(
template<class T> concept HasType = requires { typename T::type; };
template<class T> requires HasType<T> long Probe(T);
template<class T> requires (HasType<T> && true) int Probe(T);
struct Value { using type = int; };
Value value;
auto result = Probe(value);
)cpp");

        EXPECT_EQ(typed.Of("result"), "int");
}
