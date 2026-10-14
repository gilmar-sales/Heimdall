#include <Heimdall/RuleEngine.hpp>
#include <Heimdall/SemanticRules.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <string>

namespace
{

    std::vector<heimdall::Diagnostic> Override(const std::string& source)
    {
        const auto tree  = heimdall::ParseTree::Parse(source);
        const auto model = heimdall::Binder::Bind(tree);
        return heimdall::SemanticRules::AnalyzeOverride(model);
    }

    std::vector<heimdall::Diagnostic> AccessControl(const std::string& source)
    {
        const auto tree = heimdall::ParseTree::Parse(source);
        const auto model = heimdall::Binder::Bind(tree);
        const auto types = heimdall::Typer::Type(model);
        return heimdall::SemanticRules::AnalyzeAccessControl(types);
    }

    std::vector<heimdall::Diagnostic> Concepts(const std::string& source)
    {
        const auto tree  = heimdall::ParseTree::Parse(source);
        const auto model = heimdall::Binder::Bind(tree);
        return heimdall::SemanticRules::AnalyzeConcepts(model);
    }

    // Owns every layer; normalizes the body of the named concept.
    struct Normalized
    {
        explicit Normalized(std::string text) :
            source(std::move(text)), tree(heimdall::ParseTree::Parse(source)),
            model(heimdall::Binder::Bind(tree)) {}

        Normalized(const Normalized&) = delete;

        Normalized& operator= (const Normalized&) = delete;

        heimdall::SemanticRules::NormalizedConstraint Body(const std::string& name) const
        {
            const auto& nodes = tree.NodesSoA();
            for (std::uint32_t node = 0; node < nodes.size(); ++node)
            {
                if (nodes.Kind(node) != heimdall::GrammarKind::ConceptDefinition)
                {
                    continue;
                }

                const auto first = nodes.FirstToken(node);
                const auto count = nodes.TokenCount(node);
                for (std::uint32_t token = first; token < first + count; ++token)
                {
                    if (tokensAreWords(token) && tree.Text(tree.Tokens()[token]) == name)
                    {
                        return heimdall::SemanticRules::NormalizeConstraint(model, node);
                    }
                }
            }

            return {};
        }

        heimdall::SymbolId Function(const std::string& name) const
        {
            const auto& symbols = model.Symbols();
            for (heimdall::SymbolId i = 0; i < symbols.Size(); ++i)
            {
                if (symbols.kind[i] == heimdall::SymbolKind::Function &&
                    model.Names().Text(symbols.name[i]) == name)
                {
                    return i;
                }
            }

            return heimdall::kNone;
        }

        std::string source;
        heimdall::ParseTree tree;
        heimdall::SemanticModel model;

    private:
        bool tokensAreWords(std::uint32_t token) const
        {
            return token < tree.Tokens().size() &&
                tree.Tokens()[token].kind == heimdall::TokenKind::Identifier &&
                tree.Tokens()[token].tok == heimdall::Tok::None;
        }
    };

    std::string Fixed(std::string source, const heimdall::Diagnostic& diagnostic)
    {
        source.replace(diagnostic.fix.offset, diagnostic.fix.length, diagnostic.fix.replacement);
        return source;
    }

} // namespace

TEST(AccessControl, ReportsPrivateMemberAccessOutsideTheClass)
{
    const auto diagnostics = AccessControl(
        "class List { int root; public: void Set() { root = 1; } };\n"
        "void f(List list) { list.root = 2; }\n");

    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_EQ(diagnostics[0].code, "cpp/access-control");
    EXPECT_EQ(diagnostics[0].severity, heimdall::Severity::Error);
    EXPECT_NE(diagnostics[0].message.find("private"), std::string::npos);
    EXPECT_EQ(diagnostics[0].line, 2u);
}

TEST(AccessControl, AllowsPublicAndSameClassPrivateAccess)
{
    EXPECT_TRUE(AccessControl(
        "class List { int root; public: int value; void Set() { root = value; } };\n"
        "void f(List list) { list.value = 2; }\n")
        .empty());
}

TEST(AccessControl, AllowsFriendClassesAndFriendMethods)
{
    EXPECT_TRUE(AccessControl(
        "class List { int root; friend class Inspector; };\n"
        "class Inspector { void Read(List& list) { (void)list.root; } };\n")
        .empty());
    EXPECT_TRUE(AccessControl(
        "class Inspector;\n"
        "class List { int root; friend void Inspector::Read(List&); };\n"
        "class Inspector { public: void Read(List& list); };\n"
        "void Inspector::Read(List& list) { (void)list.root; }\n")
        .empty());
}

TEST(AccessControl, ResolvesTemplateClassMembersForVisibility)
{
    const auto diagnostics = AccessControl(
        "template<class T> struct Node {};\n"
        "template<class T> class LinkedList { Node<T>* mRoot; public: void Clear() {} };\n"
        "void f() { auto list = LinkedList<int>(); list.mRoot = nullptr; }\n");

    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_EQ(diagnostics[0].message,
        "'Node<int>* LinkedList<int>::mRoot' is private within this context");
}

TEST(AccessControl, ResolvesAutoTemplateTypeFromFactoryForVisibility)
{
    const auto diagnostics = AccessControl(
        "template<class T> struct Node {};\n"
        "template<class T> class LinkedList { Node<T>* mRoot; };\n"
        "LinkedList<int> CreateList() { return {}; }\n"
        "void f() { auto list = CreateList(); list.mRoot = nullptr; }\n");

    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_EQ(diagnostics[0].message,
        "'Node<int>* LinkedList<int>::mRoot' is private within this context");
}

TEST(AccessControl, ChecksProtectedAccessAndNestedClassPrivileges)
{
    EXPECT_EQ(AccessControl(
        "struct Base { protected: int value; };\n"
        "struct Derived : Base { void Set(Derived& other) { other.value = 1; } };\n"
        "void f(Base& base) { base.value = 2; }\n")
        .size(),
        1u);
    EXPECT_TRUE(AccessControl(
        "class Outer { int value; class Inner { void Read(Outer& outer) { (void)outer.value; } }; };\n")
        .empty());
    EXPECT_EQ(AccessControl(
        "class Outer { class Inner { int value; }; void Read(Inner& inner) { (void)inner.value; } };\n")
        .size(),
        1u);
}

TEST(AccessControl, AllowsFriendFreeFunctions)
{
    EXPECT_TRUE(AccessControl(
        "class List { int root; friend void Inspect(List&); };\n"
        "void Inspect(List& list) { (void)list.root; }\n")
        .empty());
}

TEST(ConflictingDeclaration, ReportsRepeatedAutoNameWithPreviousLocation)
{
    const auto tree = heimdall::ParseTree::Parse(
        "template<class T> class LinkedList {};\n"
        "LinkedList<int> CreateList() { return {}; }\n"
        "void f() {\n"
        "    auto list = CreateList();\n"
        "    auto list = LinkedList<int>();\n"
        "}\n");
    const auto model = heimdall::Binder::Bind(tree);
    const auto types = heimdall::Typer::Type(model);
    const auto diagnostics = heimdall::SemanticRules::AnalyzeConflictingDeclarations(types);

    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_EQ(diagnostics[0].code, "cpp/conflicting-declaration");
    EXPECT_EQ(diagnostics[0].severity, heimdall::Severity::Error);
    EXPECT_NE(diagnostics[0].message.find("auto list"), std::string::npos);
    EXPECT_TRUE(diagnostics[0].has_related_information);
    EXPECT_EQ(diagnostics[0].related_message,
        "previous declaration as 'LinkedList<int> list' is here");
    EXPECT_LT(diagnostics[0].related_offset, diagnostics[0].offset);
}

TEST(UndeclaredType, SuggestsTheDeclaredClassNameWithMatchingCase)
{
    const std::string source =
        "template<class TValue> struct Node { TValue value; };\n"
        "template<class TValue> class LinkedList {\n"
        "public:\n"
        "void Push(const TValue& value) {\n"
        "const node = new Node<TValue>{.value = value};\n"
        "}\n"
        "};\n";
    const auto tree = heimdall::ParseTree::Parse(source);
    const auto model = heimdall::Binder::Bind(tree);
    const auto diagnostics = heimdall::SemanticRules::AnalyzeUndeclaredType(model);

    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_EQ(diagnostics[0].code, "cpp/undeclared-type");
    EXPECT_EQ(diagnostics[0].severity, heimdall::Severity::Error);
    EXPECT_EQ(diagnostics[0].offset, source.find("node ="));
    EXPECT_EQ(diagnostics[0].message,
        "'node' does not name a type; did you mean 'Node'? [-Wtemplate-body]");
}

TEST(ModernizeOverride, ReportsAnOverrideWithoutTheKeyword)
{
    const std::string source      = "struct Base { virtual void f(int x); };\n"
                                    "struct Derived : Base { void f(int y); };\n";
    const auto        diagnostics = Override(source);
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_EQ(diagnostics[0].code, "cpp/modernize-override");
    EXPECT_EQ(diagnostics[0].rule, heimdall::RuleId::ModernizeOverride);
    EXPECT_EQ(diagnostics[0].line, 2u);
    EXPECT_EQ(source.substr(diagnostics[0].offset, diagnostics[0].length), "f");
    EXPECT_NE(diagnostics[0].message.find("Base"), std::string::npos);
    EXPECT_TRUE(diagnostics[0].has_fix);
    EXPECT_FALSE(diagnostics[0].fix_is_safe);
    EXPECT_EQ(Fixed(source, diagnostics[0]),
              "struct Base { virtual void f(int x); };\n"
              "struct Derived : Base { void f(int y) override; };\n");
}

TEST(ModernizeOverride, InsertsAfterConstNoexceptAndTrailingReturn)
{
    const std::string source =
        "struct Base { virtual int a() const; virtual void b() noexcept; virtual auto c() -> int; "
        "};\n"
        "struct D : Base {\n"
        "    int a() const;\n"
        "    void b() noexcept;\n"
        "    auto c() -> int;\n"
        "};\n";
    const auto diagnostics = Override(source);
    ASSERT_EQ(diagnostics.size(), 3u);
    EXPECT_EQ(Fixed(source, diagnostics[0]).find("int a() const override;") != std::string::npos,
              true);
    EXPECT_EQ(
        Fixed(source, diagnostics[1]).find("void b() noexcept override;") != std::string::npos,
        true);
    EXPECT_EQ(Fixed(source, diagnostics[2]).find("auto c() -> int override;") != std::string::npos,
              true);
}

TEST(ModernizeOverride, InsertsBeforePureSpecifierAndBodies)
{
    const std::string source =
        "struct Base { virtual void a() = 0; virtual void b(); };\n"
        "struct D : Base {\n"
        "    void a() = 0;\n"
        "    void b() { }\n"
        "};\n";
    const auto diagnostics = Override(source);
    ASSERT_EQ(diagnostics.size(), 2u);
    EXPECT_NE(Fixed(source, diagnostics[0]).find("void a() override = 0;"), std::string::npos);
    EXPECT_NE(Fixed(source, diagnostics[1]).find("void b() override { }"), std::string::npos);
}

TEST(ModernizeOverride, VirtualKeywordInTheDerivedClassStillCounts)
{
    const auto diagnostics = Override("struct Base { virtual void f(); };\n"
                                      "struct D : Base { virtual void f(); };\n");
    EXPECT_EQ(diagnostics.size(), 1u);
}

TEST(ModernizeOverride, SilentWhenOverrideOrFinalIsWritten)
{
    EXPECT_TRUE(Override("struct Base { virtual void f(); virtual void g(); };\n"
                         "struct D : Base { void f() override; void g() final; };\n")
                    .empty());
    EXPECT_TRUE(Override("struct Base { virtual void f(); };\n"
                         "struct D : Base { void f() final override; };\n")
                    .empty());
}

TEST(ModernizeOverride, SilentWhenTheBaseFunctionIsNotVirtual)
{
    EXPECT_TRUE(Override("struct Base { void f(); };\n"
                         "struct D : Base { void f(); };\n")
                    .empty());
}

TEST(ModernizeOverride, SilentWhenTheSignatureDiffers)
{
    EXPECT_TRUE(
        Override(
            "struct Base { virtual void f(int); virtual void g() const; virtual void h(int&); };\n"
            "struct D : Base { void f(long); void g(); void h(int); void f(int, int); };\n")
            .empty());
}

TEST(ModernizeOverride, SilentWhenTheBaseIsUnknown)
{
    EXPECT_TRUE(Override("struct D : Missing { void f(); };\n").empty());
    EXPECT_TRUE(Override("template <class T> struct Base { virtual void f(); };\n"
                         "struct D : Base<int> { void f(); };\n")
                    .empty());
}

TEST(ModernizeOverride, StillReportsWhenAnotherBaseIsUnknown)
{
    const auto diagnostics = Override("struct Known { virtual void f(); };\n"
                                      "struct D : Missing, Known { void f(); };\n");
    EXPECT_EQ(diagnostics.size(), 1u);
}

TEST(ModernizeOverride, FindsVirtualFunctionsInGrandparents)
{
    const auto diagnostics =
        Override("struct A { virtual void f(); };\n"
                 "struct B : A {};\n"
                 "struct C : B { void f(); };\n");
    EXPECT_EQ(diagnostics.size(), 1u);
}

TEST(ModernizeOverride, DeepHierarchiesAreNotCutOff)
{
    std::string source = "struct C0 { virtual void f(); };\n";
    for (int i = 1; i < 300; ++i)
    {
        source +=
            "struct C" + std::to_string(i) + " : C" + std::to_string(i - 1) + " { void f(); };\n";
    }

    EXPECT_EQ(Override(source).size(), 299u);
}

TEST(ModernizeOverride, DiamondsAndCyclesTerminate)
{
    EXPECT_EQ(Override("struct A { virtual void f(); };\n"
                       "struct B : A {};\n"
                       "struct C : A {};\n"
                       "struct D : B, C { void f(); };\n")
                  .size(),
              1u);
    EXPECT_TRUE(Override("struct A : B { void f(); };\n"
                         "struct B : A { void f(); };\n")
                    .empty());
}

TEST(ModernizeOverride, ImplicitlyVirtualIntermediateOverridesAreFound)
{
    // B::f is virtual only because it overrides A::f; C::f overrides both.
    const auto diagnostics =
        Override("struct A { virtual void f(); };\n"
                 "struct B : A { void f() override; };\n"
                 "struct C : B { void f(); };\n");
    EXPECT_EQ(diagnostics.size(), 1u);
}

TEST(ModernizeOverride, IgnoresStaticConstructorsAndDestructors)
{
    EXPECT_TRUE(Override("struct Base { virtual ~Base(); static void s(); Base(); };\n"
                         "struct D : Base { ~D(); static void s(); D(); };\n")
                    .empty());
}

TEST(ModernizeOverride, IgnoresOutOfClassDefinitions)
{
    EXPECT_TRUE(Override("struct Base { virtual void f(); };\n"
                         "struct D : Base { void f() override; };\n"
                         "void D::f() {}\n")
                    .empty());
}

TEST(ModernizeOverride, HandlesQualifiedAndNamespacedBases)
{
    const auto diagnostics =
        Override("namespace lib { struct Base { virtual void f(); }; }\n"
                 "namespace app { struct D : public lib::Base { void f(); }; }\n");
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_EQ(diagnostics[0].line, 2u);
}

TEST(ModernizeOverride, HandlesOperatorsAndOverloads)
{
    const std::string source =
        "struct Base { virtual bool operator==(const Base&) const; virtual void f(int); virtual "
        "void f(double); };\n"
        "struct D : Base { bool operator==(const Base&) const; void f(double); };\n";
    const auto diagnostics = Override(source);
    ASSERT_EQ(diagnostics.size(), 2u);
    EXPECT_NE(Fixed(source, diagnostics[0]).find("operator==(const Base&) const override;"),
              std::string::npos);
    EXPECT_NE(Fixed(source, diagnostics[1]).find("void f(double) override;"), std::string::npos);
}

TEST(ModernizeOverride, ParameterNamesAndDefaultsDoNotMatter)
{
    EXPECT_EQ(Override("struct Base { virtual void f(int a, int b = 2); };\n"
                       "struct D : Base { void f(int x, int y); };\n")
                  .size(),
              1u);
}

TEST(ModernizeOverride, ToleratesBrokenInput)
{
    for (const char* source : { "struct B { virtual void f(); }; struct D : B { void f(",
                                "struct B { virtual void f(); }; struct D : B { void f() ",
                                "struct D : B { void f", "" })
    {
        EXPECT_NO_FATAL_FAILURE(Override(source)) << source;
    }
}

TEST(ModernizeOverride, BatchFixLeavesTheSourceAlone)
{
    // The match is textual, so the edit is a quick fix only (never --fix).
    const std::string source      = "struct Base { virtual void f(); };\n"
                                    "struct D : Base { void f(); };\n";
    const auto        diagnostics = Override(source);
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_EQ(heimdall::RuleEngine::ApplyFixes(source, diagnostics), source);
}

TEST(ModernizeOverride, CanBeDisabledThroughRuleOverrides)
{
    const std::string     source = "struct Base { virtual void f(); };\n"
                                   "struct D : Base { void f(); };\n";
    const auto            tree   = heimdall::ParseTree::Parse(source);
    const auto            model  = heimdall::Binder::Bind(tree);
    heimdall::RuleOptions options;
    options.overrides.push_back({ "cpp/modernize-override", false, heimdall::Severity::Warning });
    EXPECT_TRUE(heimdall::RuleEngine(options)
                    .ApplyPolicy(heimdall::SemanticRules::AnalyzeOverride(model), tree)
                    .empty());

    options.overrides.back() = { "cpp/modernize-override", true, heimdall::Severity::Error };
    const auto escalated     = heimdall::RuleEngine(options).ApplyPolicy(
        heimdall::SemanticRules::AnalyzeOverride(model),
        tree);
    ASSERT_EQ(escalated.size(), 1u);
    EXPECT_EQ(escalated[0].severity, heimdall::Severity::Error);
}

TEST(ModernizeOverride, CanBeSuppressedByComment)
{
    const std::string source =
        "struct Base { virtual void f(); virtual void g(); };\n"
        "struct D : Base {\n"
        "    void f(); // heimdall-disable-line cpp/modernize-override\n"
        "    // heimdall-disable-next-line cpp/modernize-override\n"
        "    void g();\n"
        "};\n";
    const auto tree  = heimdall::ParseTree::Parse(source);
    const auto model = heimdall::Binder::Bind(tree);
    const auto raw   = heimdall::SemanticRules::AnalyzeOverride(model);
    ASSERT_EQ(raw.size(), 2u);
    EXPECT_TRUE(heimdall::RuleEngine().ApplyPolicy(raw, tree).empty());
}

TEST(ModernizeOverride, IsRegisteredInTheRuleCatalog)
{
    const auto* info = heimdall::FindRuleByCode("cpp/modernize-override");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->id, heimdall::RuleId::ModernizeOverride);
    EXPECT_EQ(info->layer, "semantic");
    EXPECT_TRUE(heimdall::IsKnownRuleCode("cpp/modernize-override"));
}

TEST(ConceptRules, AreRegisteredInTheRuleCatalog)
{
    const std::pair<std::string_view, heimdall::RuleId> entries[] = {
        {"cpp/concept-recursive", heimdall::RuleId::ConceptRecursion},
        {"cpp/concept-constrained", heimdall::RuleId::ConceptConstrained},
        {"cpp/concept-placement", heimdall::RuleId::ConceptPlacement},
        {"cpp/concept-specialization", heimdall::RuleId::ConceptSpecialization},
        {"cpp/requires-form", heimdall::RuleId::RequiresForm},
        {"cpp/requires-parameter", heimdall::RuleId::RequiresParameter},
        {"cpp/concept-arity", heimdall::RuleId::ConceptArity},
        {"cpp/requires-unknown-type", heimdall::RuleId::RequiresValidity},
        {"cpp/concept-redeclaration", heimdall::RuleId::ConceptRedeclaration},
    };

    for (const auto&[code, id] : entries)
    {
        const auto* info = heimdall::FindRuleByCode(code);
        ASSERT_NE(info, nullptr) << code;
        EXPECT_EQ(info->id, id);
        EXPECT_EQ(info->layer, "semantic");
        EXPECT_TRUE(heimdall::IsKnownRuleCode(code));
    }
}

TEST(ConceptRules, ReportsRecursiveConcepts)
{
    const auto diagnostics = Concepts("template <typename T> concept V = V<T*>;\n");

    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_EQ(diagnostics[0].code, "cpp/concept-recursive");
    EXPECT_EQ(diagnostics[0].severity, heimdall::Severity::Error);
    EXPECT_NE(diagnostics[0].message.find("V"), std::string::npos);
}

TEST(ConceptRules, AllowsOtherConceptsAndShadowedNames)
{
    EXPECT_TRUE(Concepts("template <typename T> concept C1 = true;\n"
        "template <typename T> concept C2 = C1<T>;\n")
        .empty());
    // A same-named local parameter shadows the concept: not recursion.
    EXPECT_TRUE(Concepts("template <typename T> concept C = requires (T C) { C; };\n").empty());
}

TEST(ConceptRules, ReportsConstrainedConceptDefinitions)
{
    const auto constrained = Concepts("template <typename T> concept C1 = true;\n"
        "template <C1 T> concept E1 = true;\n"
        "template <class T> requires C1<T> concept E2 = true;\n");

    ASSERT_EQ(constrained.size(), 2u);
    for (const auto& diagnostic : constrained)
    {
        EXPECT_EQ(diagnostic.code, "cpp/concept-constrained");
        EXPECT_EQ(diagnostic.severity, heimdall::Severity::Error);
    }

    EXPECT_TRUE(Concepts("template <typename T> concept C = true;\n").empty());
}

TEST(ConceptRules, ReportsConceptsOutsideNamespaceScope)
{
    const auto diagnostics =
        Concepts("struct S { template <typename T> concept C = true; };\n");

    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_EQ(diagnostics[0].code, "cpp/concept-placement");
    EXPECT_EQ(diagnostics[0].severity, heimdall::Severity::Error);
    EXPECT_TRUE(Concepts("namespace N { template <typename T> concept C = true; }\n").empty());
}

TEST(ConceptRules, ReportsConceptSpecializations)
{
    const auto diagnostics = Concepts("template <> concept C<int> = true;\n"
        "template <typename T> concept D<T*> = true;\n");

    ASSERT_EQ(diagnostics.size(), 2u);
    for (const auto& diagnostic : diagnostics)
    {
        EXPECT_EQ(diagnostic.code, "cpp/concept-specialization");
        EXPECT_EQ(diagnostic.severity, heimdall::Severity::Error);
    }
}

TEST(ConceptRules, ReportsUnparenthesizedTemplateCallsInRequiresClauses)
{
    const auto diagnostics = Concepts("template <typename T> bool is_purrable();\n"
        "template <typename T> void g(T t) requires is_purrable<T>() {}\n");

    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_EQ(diagnostics[0].code, "cpp/requires-form");
    EXPECT_EQ(diagnostics[0].severity, heimdall::Severity::Error);
    EXPECT_TRUE(Concepts("template <typename T> bool is_purrable();\n"
        "template <typename T> void g(T t) requires (is_purrable<T>()) {}\n")
        .empty());
    EXPECT_TRUE(Concepts("template <typename T> concept P = true;\n"
        "template <typename T> concept Q = true;\n"
        "template <typename T> void g(T t) requires P<T> && Q<T> {}\n")
        .empty());
}

TEST(ConceptRules, ReportsInvalidRequiresParameters)
{
    const auto defaulted = Concepts(
        "template <typename T> concept C1 = requires (T t = 0) { t; };\n");

    ASSERT_EQ(defaulted.size(), 1u);
    EXPECT_EQ(defaulted[0].code, "cpp/requires-parameter");
    EXPECT_EQ(defaulted[0].severity, heimdall::Severity::Error);
    EXPECT_NE(defaulted[0].message.find("default"), std::string::npos);

    const auto variadic = Concepts(
        "template <typename T> concept C2 = requires (T t, ...) { t; };\n");

    ASSERT_EQ(variadic.size(), 1u);
    EXPECT_EQ(variadic[0].code, "cpp/requires-parameter");
    EXPECT_NE(variadic[0].message.find("ellipsis"), std::string::npos);

    EXPECT_TRUE(Concepts(
        "template <typename T, typename U> concept C = requires (T a, U&& u) { a + u; };\n")
        .empty());
}

TEST(ConceptRules, ChecksConceptArityByPosition)
{
    // Full arity in constraint expressions: 1-param takes exactly 1.
    const auto full = Concepts("template <typename T> concept One = true;\n"
        "template <typename A, typename B> concept Two = true;\n"
        "void f() requires Two<int> {}\n");

    ASSERT_EQ(full.size(), 1u);
    EXPECT_EQ(full[0].code, "cpp/concept-arity");
    EXPECT_EQ(full[0].severity, heimdall::Severity::Error);
    EXPECT_TRUE(Concepts("template <typename T> concept One = true;\n"
        "void f() requires One<int> {}\n")
        .empty());

    // Type-constraints take one less: the deduced type is implicit.
    const auto constrained = Concepts("template <typename T> concept One = true;\n"
        "template <One<int> T> void f(T t) {}\n");

    ASSERT_EQ(constrained.size(), 1u);
    EXPECT_EQ(constrained[0].code, "cpp/concept-arity");
    EXPECT_TRUE(Concepts("template <typename T> concept One = true;\n"
        "template <One T> void f(T t) {}\n")
        .empty());
    EXPECT_TRUE(Concepts("template <typename A, typename B> concept Two = true;\n"
        "template <Two<int> T> void f(T t) {}\n")
        .empty());

    // Compound return-type requirements are type-constraints too.
    const auto compound = Concepts("template <typename T> concept One = true;\n"
        "template <typename T> concept C = requires (T a) { { a } -> One<T>; };\n");

    ASSERT_EQ(compound.size(), 1u);
    EXPECT_EQ(compound[0].code, "cpp/concept-arity");
    EXPECT_TRUE(Concepts("template <typename A, typename B> concept Two = true;\n"
        "template <typename T> concept C = requires (T a) { { a } -> Two<T>; };\n")
        .empty());

    // Placeholders behave like constrained parameters.
    EXPECT_TRUE(Concepts("template <typename T> concept One = true;\n"
        "One auto x = 1;\n")
        .empty());
}

TEST(ConceptRules, ReportsUnknownTypesInTypeRequirementsOutsideTemplates)
{
    const auto diagnostics =
        Concepts("static_assert(requires { typename Nonesuch; });\n");

    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_EQ(diagnostics[0].code, "cpp/requires-unknown-type");
    EXPECT_EQ(diagnostics[0].severity, heimdall::Severity::Error);
    EXPECT_TRUE(Concepts("struct S {};\nstatic_assert(requires { typename S; });\n").empty());
    EXPECT_TRUE(Concepts(
        "template <typename T> concept C = requires { typename Nonesuch; };\n")
        .empty());
    EXPECT_TRUE(Concepts("static_assert(requires { typename S<int>; });\n").empty());
}

TEST(ConceptRules, WarnsOnRedeclarationsWithDifferentConstraintSpellings)
{
    const auto diagnostics = Concepts("template <typename T> concept I = true;\n"
        "template <typename T> concept D = true;\n"
        "template <I T> void f(T t) requires D<T> {}\n"
        "template <typename T> requires I<T> && D<T> void f(T t) {}\n");

    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_EQ(diagnostics[0].code, "cpp/concept-redeclaration");
    EXPECT_EQ(diagnostics[0].severity, heimdall::Severity::Warning);

    EXPECT_TRUE(Concepts("template <typename T> concept I = true;\n"
        "template <typename T> concept D = true;\n"
        "template <I T> void g(T t) requires D<T> {}\n"
        "template <I T> void g(T t) requires D<T> {}\n")
        .empty());
}

TEST(ConceptRules, AcceptsTheReferenceHashableExample)
{
    EXPECT_TRUE(Concepts("#include <cstddef>\n"
        "template <typename T> struct hash {};\n"
        "template <typename T, typename U> concept convertible_to = true;\n"
        "template <typename T> concept Hashable = requires (T a)\n"
        "{\n"
        "    { hash<T>{}(a) } -> convertible_to<std::size_t>;\n"
        "};\n"
        "template <Hashable T> void f(T value) {}\n")
        .empty());
}

TEST(ConceptRules, RunsInsideTheFullPipeline)
{
    const auto tree  = heimdall::ParseTree::Parse(
        "template <typename T> concept V = V<T*>;\n");
    const auto model = heimdall::Binder::Bind(tree);
    const auto types = heimdall::Typer::Type(model);
    const auto diagnostics = heimdall::SemanticRules::Analyze(model, types);

    EXPECT_TRUE(std::any_of(diagnostics.begin(), diagnostics.end(),
        [](const heimdall::Diagnostic& diagnostic)
        {
            return diagnostic.code == "cpp/concept-recursive";
        }));
}

TEST(ConceptNormalization, ExpandsConjunctionsWithProvenance)
{
    Normalized normalized("template <typename T> constexpr bool is_meowable = true;\n"
        "template <typename T> constexpr bool is_cat = true;\n"
        "template <typename T> concept Meowable = is_meowable<T>;\n"
        "template <typename T> concept GoodMeowableCat = Meowable<T> && is_cat<T>;\n");
    const auto good = normalized.Body("GoodMeowableCat");

    ASSERT_EQ(good.kind, heimdall::SemanticRules::NormalizedConstraint::Kind::All);
    ASSERT_EQ(good.operands.size(), 2u);
    EXPECT_EQ(good.operands[0].atom.provenance, "concept:Meowable");
    EXPECT_NE(good.operands[0].atom.expression.find("is_meowable"), std::string::npos);
    // Written inline in GoodMeowableCat itself, not inherited from Meowable.
    EXPECT_EQ(good.operands[1].atom.provenance, "concept:GoodMeowableCat");
}

TEST(ConceptNormalization, DistinguishesCopiedFromInheritedAtoms)
{
    Normalized normalized("template <typename T> constexpr bool is_meowable = true;\n"
        "template <typename T> constexpr bool is_cat = true;\n"
        "template <typename T> concept Meowable = is_meowable<T>;\n"
        "template <typename T> concept BadMeowableCat = is_meowable<T> && is_cat<T>;\n"
        "template <typename T> concept GoodMeowableCat = Meowable<T> && is_cat<T>;\n");
    const auto bad  = normalized.Body("BadMeowableCat");
    const auto good = normalized.Body("GoodMeowableCat");
    const auto meow = normalized.Body("Meowable");

    ASSERT_EQ(bad.operands.size(), 2u);
    ASSERT_EQ(good.operands.size(), 2u);
    // Same spelling, different provenance: not identical.
    EXPECT_NE(bad.operands[0].atom.provenance, good.operands[0].atom.provenance);
    EXPECT_EQ(bad.operands[0].atom.expression, good.operands[0].atom.expression);
    // Good inherits through Meowable, so it subsumes Meowable; Bad merely copies.
    EXPECT_TRUE(heimdall::SemanticRules::ConstraintSubsumes(good, meow));
    EXPECT_FALSE(heimdall::SemanticRules::ConstraintSubsumes(meow, good));
    EXPECT_FALSE(heimdall::SemanticRules::ConstraintSubsumes(bad, meow));
}

TEST(ConceptNormalization, SubsumptionOrdersDecrementableAndRevIterator)
{
    Normalized normalized(
        "template <typename T> concept Decrementable = requires (T t) { --t; };\n"
        "template <typename T> concept RevIterator = Decrementable<T> && requires (T t) { *t; };\n");
    const auto dec = normalized.Body("Decrementable");
    const auto rev = normalized.Body("RevIterator");

    EXPECT_EQ(dec.kind, heimdall::SemanticRules::NormalizedConstraint::Kind::Atom);
    EXPECT_EQ(rev.kind, heimdall::SemanticRules::NormalizedConstraint::Kind::All);
    EXPECT_TRUE(heimdall::SemanticRules::ConstraintSubsumes(rev, dec));
    EXPECT_FALSE(heimdall::SemanticRules::ConstraintSubsumes(dec, rev));
    EXPECT_TRUE(heimdall::SemanticRules::ConstraintSubsumes(dec, dec));
}

TEST(ConceptNormalization, NormalizesDisjunctionsAndParentheses)
{
    Normalized normalized("template <typename T> concept A = true;\n"
        "template <typename T> concept B = true;\n"
        "template <typename T> concept C = (A<T> || B<T>) && A<T>;\n");
    const auto body = normalized.Body("C");

    ASSERT_EQ(body.kind, heimdall::SemanticRules::NormalizedConstraint::Kind::All);
    ASSERT_EQ(body.operands.size(), 2u);
    EXPECT_EQ(body.operands[0].kind, heimdall::SemanticRules::NormalizedConstraint::Kind::Any);
    EXPECT_EQ(body.operands[0].operands.size(), 2u);
}

TEST(ConceptNormalization, GathersAssociatedConstraintsInCanonicalOrder)
{
    Normalized normalized("template <typename T> concept C1 = true;\n"
        "template <typename T> concept D = true;\n"
        "template <C1 T> void f(T t) requires D<T> {}\n"
        "template <typename T> void g(T t) {}\n"
        "void h(C1 auto x) {}\n");
    const auto f = normalized.Function("f");
    ASSERT_NE(f, heimdall::kNone);
    const auto associated =
        heimdall::SemanticRules::NormalizeAssociatedConstraints(normalized.model, f);

    ASSERT_EQ(associated.kind, heimdall::SemanticRules::NormalizedConstraint::Kind::All);
    ASSERT_EQ(associated.operands.size(), 2u);
    EXPECT_EQ(associated.operands[0].atom.provenance, "template-head");
    EXPECT_NE(associated.operands[0].atom.expression.find("C1"), std::string::npos);
    EXPECT_EQ(associated.operands[1].kind,
        heimdall::SemanticRules::NormalizedConstraint::Kind::Atom);
    EXPECT_EQ(associated.operands[1].atom.provenance, "concept:D");

    const auto g = normalized.Function("g");
    ASSERT_NE(g, heimdall::kNone);
    const auto unconstrained =
        heimdall::SemanticRules::NormalizeAssociatedConstraints(normalized.model, g);
    EXPECT_TRUE(unconstrained.operands.empty());
    // Anything subsumes the unconstrained; it subsumes nothing constrained.
    EXPECT_TRUE(heimdall::SemanticRules::ConstraintSubsumes(associated, unconstrained));
    EXPECT_FALSE(heimdall::SemanticRules::ConstraintSubsumes(unconstrained, associated));

    const auto h = normalized.Function("h");
    ASSERT_NE(h, heimdall::kNone);
    const auto abbreviated =
        heimdall::SemanticRules::NormalizeAssociatedConstraints(normalized.model, h);
    ASSERT_EQ(abbreviated.operands.size(), 1u);
    EXPECT_EQ(abbreviated.operands[0].atom.provenance, "abbreviated");
}
