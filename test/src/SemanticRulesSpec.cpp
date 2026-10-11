#include <Heimdall/RuleEngine.hpp>
#include <Heimdall/SemanticRules.hpp>

#include <gtest/gtest.h>

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
