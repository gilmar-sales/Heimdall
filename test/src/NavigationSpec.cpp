#include <Heimdall/Navigation.hpp>

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

namespace
{

    std::size_t Find(std::string_view source, std::string_view needle, std::size_t nth = 0)
    {
        std::size_t pos = 0;
        for (std::size_t i = 0;; ++i)
        {
            pos = source.find(needle, pos);
            if (pos == std::string_view::npos || i == nth)
            {
                return pos;
            }

            ++pos;
        }
    }

    std::vector<heimdall::NavTarget> DefinitionOf(std::string_view source, std::string_view needle,
                                                  std::size_t nth = 0)
    {
        const heimdall::ParseTree tree = heimdall::ParseTree::Parse(source);
        return heimdall::Navigation::Definition(tree, Find(source, needle, nth));
    }

    std::vector<heimdall::NavTarget> ImplementationOf(std::string_view source,
                                                      std::string_view needle, std::size_t nth = 0)
    {
        const heimdall::ParseTree tree = heimdall::ParseTree::Parse(source);
        return heimdall::Navigation::Implementation(tree, Find(source, needle, nth));
    }

    std::vector<heimdall::AmbiguousReference> AmbiguitiesIn(std::string_view source)
    {
        const heimdall::ParseTree tree = heimdall::ParseTree::Parse(source);
        return heimdall::Navigation::FindAmbiguities(tree);
    }

} // namespace

TEST(NavigationSpec, CollectsUsingDirectivesWithTheirScope)
{
    constexpr std::string_view source =
        "namespace a { int x; }\n"
        "using namespace a;\n"
        "namespace n { using namespace ::a; void f() { using namespace a; } }\n";
    const heimdall::ParseTree tree       = heimdall::ParseTree::Parse(source);
    const auto                directives = heimdall::Navigation::UsingDirectives(tree);
    ASSERT_EQ(directives.size(), 3u);
    EXPECT_TRUE(directives[0].scope.empty());
    EXPECT_EQ(directives[0].target, (std::vector<std::string> { "a" }));
    EXPECT_EQ(directives[1].scope, (std::vector<std::string> { "n" }));
    EXPECT_EQ(directives[1].target, (std::vector<std::string> { "a" }));
}

TEST(NavigationSpec, DefinitionOfLocalsParametersAndGlobals)
{
    constexpr std::string_view source =
        "int global_value = 1;\n"
        "int f(int param) {\n"
        "    int local = param;\n"
        "    return local + global_value;\n"
        "}\n";
    const auto local = DefinitionOf(source, "local + global_value");
    ASSERT_EQ(local.size(), 1u);
    EXPECT_EQ(local[0].offset, Find(source, "local = param"));
    const auto param = DefinitionOf(source, "param;");
    ASSERT_EQ(param.size(), 1u);
    EXPECT_EQ(param[0].offset, Find(source, "param) {"));
    const auto global = DefinitionOf(source, "global_value;");
    ASSERT_EQ(global.size(), 1u);
    EXPECT_EQ(global[0].offset, Find(source, "global_value = 1"));
}

TEST(NavigationSpec, InnerScopeShadowsOuterDeclaration)
{
    constexpr std::string_view source =
        "int v = 0;\n"
        "void f() {\n"
        "    int v = 1;\n"
        "    { int v = 2; use(v); }\n"
        "    use(v);\n"
        "}\n";
    const auto inner = DefinitionOf(source, "v); }");
    ASSERT_EQ(inner.size(), 1u);
    EXPECT_EQ(inner[0].offset, Find(source, "v = 2"));
    const auto outer = DefinitionOf(source, "v);\n}");
    ASSERT_EQ(outer.size(), 1u);
    EXPECT_EQ(outer[0].offset, Find(source, "v = 1"));
}

TEST(NavigationSpec, QualifiedNamesResolveThroughNamespaces)
{
    constexpr std::string_view source =
        "namespace a { namespace b { int value; void run(); } }\n"
        "void g() { a::b::value = 1; a::b::run(); }\n";
    const auto value = DefinitionOf(source, "value = 1");
    ASSERT_EQ(value.size(), 1u);
    EXPECT_EQ(value[0].offset, Find(source, "value;"));
    const auto run = DefinitionOf(source, "run();", 1);
    ASSERT_EQ(run.size(), 1u);
    EXPECT_EQ(run[0].offset, Find(source, "run();"));
    // The qualifier itself navigates to the namespace.
    const auto ns = DefinitionOf(source, "b::value");
    ASSERT_EQ(ns.size(), 1u);
    EXPECT_EQ(ns[0].offset, Find(source, "b {"));
}

TEST(NavigationSpec, UsingNamespaceMakesNamesVisible)
{
    constexpr std::string_view source =
        "namespace lib { int answer; void greet(); }\n"
        "using namespace lib;\n"
        "int f() { greet(); return answer; }\n";
    const auto answer = DefinitionOf(source, "answer; }");
    ASSERT_EQ(answer.size(), 1u);
    EXPECT_EQ(answer[0].offset, Find(source, "answer;"));
    const auto greet = DefinitionOf(source, "greet(); return");
    ASSERT_EQ(greet.size(), 1u);
    EXPECT_EQ(greet[0].offset, Find(source, "greet();"));
    // The directive's own target navigates to the namespace.
    const auto ns = DefinitionOf(source, "lib;\nint f");
    ASSERT_EQ(ns.size(), 1u);
    EXPECT_EQ(ns[0].offset, Find(source, "lib {"));
}

TEST(NavigationSpec, UsingDirectiveIsScopedToItsBlock)
{
    constexpr std::string_view source =
        "namespace lib { int answer; }\n"
        "int f() { using namespace lib; return answer; }\n"
        "int g() { return answer; }\n";
    EXPECT_EQ(DefinitionOf(source, "answer; }", 1).size(), 1u);
    EXPECT_TRUE(DefinitionOf(source, "answer; }", 2).empty());
}

TEST(NavigationSpec, TransitiveDirectivesAreFollowed)
{
    constexpr std::string_view source =
        "namespace inner { int deep; }\n"
        "namespace outer { using namespace inner; }\n"
        "using namespace outer;\n"
        "int f() { return deep; }\n";
    const auto deep = DefinitionOf(source, "deep; }");
    ASSERT_EQ(deep.size(), 1u);
    EXPECT_EQ(deep[0].offset, Find(source, "deep;"));
}

TEST(NavigationSpec, ReportsAmbiguousNameFromTwoDirectives)
{
    constexpr std::string_view source =
        "namespace a { int x; }\n"
        "namespace b { int x; }\n"
        "using namespace a;\n"
        "using namespace b;\n"
        "int f() { return x; }\n";
    const auto ambiguities = AmbiguitiesIn(source);
    ASSERT_EQ(ambiguities.size(), 1u);
    EXPECT_EQ(ambiguities[0].name, "x");
    EXPECT_EQ(ambiguities[0].offset, Find(source, "x; }", 2));
    EXPECT_EQ(ambiguities[0].candidates, (std::vector<std::string> { "a::x", "b::x" }));
    // Go-to-definition offers both candidates.
    EXPECT_EQ(DefinitionOf(source, "x; }", 2).size(), 2u);
}

TEST(NavigationSpec, QualifiedOrLocalUseIsNotAmbiguous)
{
    constexpr std::string_view source =
        "namespace a { int x; }\n"
        "namespace b { int x; }\n"
        "using namespace a;\n"
        "using namespace b;\n"
        "int f() { return a::x + b::x; }\n"
        "int g() { int x = 1; return x; }\n";
    EXPECT_TRUE(AmbiguitiesIn(source).empty());
}

TEST(NavigationSpec, OverloadsFromDifferentNamespacesAreNotAmbiguous)
{
    constexpr std::string_view source =
        "namespace a { void show(int); }\n"
        "namespace b { void show(double); }\n"
        "using namespace a;\n"
        "using namespace b;\n"
        "void f() { show(1); }\n";
    EXPECT_TRUE(AmbiguitiesIn(source).empty());
}

TEST(NavigationSpec, DirectiveVersusGlobalDeclaration)
{
    // A directive at global scope competes with a global declaration.
    constexpr std::string_view source =
        "namespace a { int x; }\n"
        "int x;\n"
        "using namespace a;\n"
        "int f() { return x; }\n";
    const auto ambiguities = AmbiguitiesIn(source);
    ASSERT_EQ(ambiguities.size(), 1u);
    EXPECT_EQ(ambiguities[0].candidates, (std::vector<std::string> { "a::x", "x" }));
}

TEST(NavigationSpec, DirectiveInsideNamespaceNominatesAtCommonAncestor)
{
    // The nominated namespace appears in the nearest enclosing namespace that
    // holds both the directive and the target: here the global scope, where the
    // global `x` competes with `lib::x`.
    constexpr std::string_view source =
        "int x;\n"
        "namespace lib { int x; }\n"
        "namespace n {\n"
        "    using namespace lib;\n"
        "    int f() { return x; }\n"
        "}\n";
    EXPECT_EQ(AmbiguitiesIn(source).size(), 1u);
}

TEST(NavigationSpec, MemberAccessFindsRecordMembers)
{
    constexpr std::string_view source =
        "struct S { int field; void method(); };\n"
        "void f(S s) { s.field = 1; s.method(); }\n";
    const auto field = DefinitionOf(source, "field = 1");
    ASSERT_EQ(field.size(), 1u);
    EXPECT_EQ(field[0].offset, Find(source, "field;"));
    const auto method = DefinitionOf(source, "method();", 1);
    ASSERT_EQ(method.size(), 1u);
    EXPECT_EQ(method[0].offset, Find(source, "method();"));
}

TEST(NavigationSpec, DefinitionPrefersFunctionBodyOverDeclaration)
{
    constexpr std::string_view source =
        "namespace a { void run(int); }\n"
        "void a::run(int value) {}\n"
        "void g() { a::run(1); }\n";
    const auto run = DefinitionOf(source, "run(1)");
    ASSERT_EQ(run.size(), 1u);
    EXPECT_TRUE(run[0].is_definition);
    EXPECT_EQ(run[0].offset, Find(source, "run(int value)"));
}

TEST(NavigationSpec, OutOfLineMemberBodiesSeeClassMembers)
{
    constexpr std::string_view source =
        "namespace a { struct S { int count; void bump(); }; }\n"
        "void a::S::bump() { count = count + 1; }\n";
    const auto count = DefinitionOf(source, "count + 1");
    ASSERT_EQ(count.size(), 1u);
    EXPECT_EQ(count[0].offset, Find(source, "count;"));
}

TEST(NavigationSpec, ImplementationFindsOverridersAndOutOfLineBodies)
{
    constexpr std::string_view source =
        "struct Base { virtual void run() = 0; void plain(); };\n"
        "struct Left : Base { void run() override {} };\n"
        "struct Right : public Base { void run() override {} };\n"
        "void Base::plain() {}\n";
    const auto overriders = ImplementationOf(source, "run() = 0");
    ASSERT_EQ(overriders.size(), 2u);
    EXPECT_EQ(overriders[0].offset, Find(source, "run() override"));
    EXPECT_EQ(overriders[1].offset, Find(source, "run() override", 1));
    const auto plain = ImplementationOf(source, "plain();");
    ASSERT_EQ(plain.size(), 1u);
    EXPECT_EQ(plain[0].offset, Find(source, "plain() {}"));
}

TEST(NavigationSpec, ImplementationOfInterfaceTypeListsDerivedTypes)
{
    constexpr std::string_view source =
        "struct Shape { virtual void draw() = 0; };\n"
        "struct Circle : Shape { void draw() override {} };\n"
        "struct Square : Shape { void draw() override {} };\n"
        "void render(Shape &shape) {}\n";
    const auto derived = ImplementationOf(source, "Shape &shape");
    ASSERT_EQ(derived.size(), 2u);
    EXPECT_EQ(derived[0].name, "Circle");
    EXPECT_EQ(derived[1].name, "Square");
}

TEST(NavigationSpec, InheritedMembersResolveThroughBaseClasses)
{
    constexpr std::string_view source =
        "struct Base { int shared; };\n"
        "struct Derived : Base { int use() { return shared; } };\n";
    const auto shared = DefinitionOf(source, "shared; }");
    ASSERT_EQ(shared.size(), 1u);
    EXPECT_EQ(shared[0].offset, Find(source, "shared;"));
}

TEST(NavigationSpec, FindDefinitionsMatchesScopeNameAndArity)
{
    constexpr std::string_view source = "namespace a { void run(int) {} void run(int, int) {} }\n";
    const heimdall::ParseTree  tree   = heimdall::ParseTree::Parse(source);
    const auto one = heimdall::Navigation::FindDefinitions(tree, { "a" }, "run", true, 1);
    ASSERT_EQ(one.size(), 1u);
    EXPECT_EQ(one[0].offset, Find(source, "run(int)"));
    const auto two = heimdall::Navigation::FindDefinitions(tree, { "a" }, "run", true, 2);
    ASSERT_EQ(two.size(), 1u);
    EXPECT_EQ(two[0].offset, Find(source, "run(int, int)"));
    EXPECT_TRUE(heimdall::Navigation::FindDefinitions(tree, { "b" }, "run", true, 1).empty());
}

TEST(NavigationSpec, ExternalIndexSuppliesHeaderDeclarationsWithFiles)
{
    constexpr std::string_view header = "namespace lib { int answer; void greet(); }\n";
    heimdall::ScopeIndex       index  = heimdall::CompletionEngine::IndexScopes(header, {});
    for (auto& scope : index)
    {
        for (auto& member : scope.members)
        {
            member.file = 7;
        }
    }

    constexpr std::string_view source = "using namespace lib;\nint f() { return answer; }\n";
    const heimdall::ParseTree  tree   = heimdall::ParseTree::Parse(source);
    const auto                 targets =
        heimdall::Navigation::Definition(tree, Find(source, "answer; }") + 1, &index);
    ASSERT_EQ(targets.size(), 1u);
    EXPECT_EQ(targets[0].file, 7);
    EXPECT_EQ(targets[0].offset, Find(header, "answer"));
    EXPECT_EQ(targets[0].scope, (std::vector<std::string> { "lib" }));
}

TEST(NavigationSpec, AmbiguityAcrossHeaderNamespaces)
{
    heimdall::ScopeIndex index = heimdall::CompletionEngine::IndexScopes(
        "namespace one { int size; }\nnamespace two { int size; }\n", {});
    constexpr std::string_view source =
        "using namespace one;\nusing namespace two;\nint f() { return size; }\n";
    const heimdall::ParseTree tree        = heimdall::ParseTree::Parse(source);
    const auto                ambiguities = heimdall::Navigation::FindAmbiguities(tree, &index);
    ASSERT_EQ(ambiguities.size(), 1u);
    EXPECT_EQ(ambiguities[0].candidates, (std::vector<std::string> { "one::size", "two::size" }));
}

TEST(NavigationSpec, DefinitionSiteJumpsBackToItsDeclaration)
{
    constexpr std::string_view source =
        "namespace lib { struct Widget { void run(int); void run(); }; }\n"
        "void lib::Widget::run(int times) {}\n"
        "void lib::Widget::run() {}\n";
    const auto one = DefinitionOf(source, "run(int times)");
    ASSERT_EQ(one.size(), 1u);
    EXPECT_FALSE(one[0].is_definition);
    EXPECT_EQ(one[0].offset, Find(source, "run(int);"));
    const auto none = DefinitionOf(source, "run() {}");
    ASSERT_EQ(none.size(), 1u);
    EXPECT_EQ(none[0].offset, Find(source, "run();"));
    // And the declaration goes forward to the body of the same overload.
    const auto body = DefinitionOf(source, "run(int);");
    ASSERT_EQ(body.size(), 1u);
    EXPECT_EQ(body[0].offset, Find(source, "run(int times)"));
}

TEST(NavigationSpec, ImplementationOfDeclarationExcludesTheDeclaration)
{
    constexpr std::string_view source =
        "struct Widget { void run(); };\n"
        "void Widget::run() {}\n";
    const auto targets = ImplementationOf(source, "run();");
    ASSERT_EQ(targets.size(), 1u);
    EXPECT_EQ(targets[0].offset, Find(source, "run() {}"));
}

TEST(NavigationSpec, EnumDefinitionPointsAtTheNameInSourceAndHeaders)
{
    constexpr std::string_view source =
        "namespace n {\n"
        "    enum class Mode { Fast, Slow };\n"
        "    enum Plain { One };\n"
        "}\n"
        "n::Mode a = n::Mode::Fast;\n"
        "n::Plain b = n::One;\n";
    const auto mode = DefinitionOf(source, "Mode a");
    ASSERT_EQ(mode.size(), 1u);
    EXPECT_EQ(mode[0].offset, Find(source, "Mode {"));
    const auto qualifier = DefinitionOf(source, "Mode::Fast");
    ASSERT_EQ(qualifier.size(), 1u);
    EXPECT_EQ(qualifier[0].offset, Find(source, "Mode {"));
    const auto plain = DefinitionOf(source, "Plain b");
    ASSERT_EQ(plain.size(), 1u);
    EXPECT_EQ(plain[0].offset, Find(source, "Plain {"));
    const auto enumerator = DefinitionOf(source, "Fast;");
    ASSERT_EQ(enumerator.size(), 1u);
    EXPECT_EQ(enumerator[0].offset, Find(source, "Fast,"));

    // Same through the header index: one result, aimed at the name token.
    constexpr std::string_view header = "namespace n {\n    enum class Mode { Fast, Slow };\n}\n";
    heimdall::ScopeIndex       index  = heimdall::CompletionEngine::IndexScopes(header, {});
    constexpr std::string_view user   = "using namespace n;\nMode value;\nauto x = Mode::Fast;\n";
    const heimdall::ParseTree  tree   = heimdall::ParseTree::Parse(user);
    for (const std::size_t at : { Find(user, "Mode value"), Find(user, "Mode::Fast") })
    {
        const auto external = heimdall::Navigation::Definition(tree, at, &index);
        ASSERT_EQ(external.size(), 1u);
        EXPECT_EQ(external[0].offset, Find(header, "Mode {"));
        EXPECT_EQ(external[0].length, 4u);
    }
}

TEST(NavigationSpec, ResolvesMembersDeclaredRightAfterAnAccessSpecifier)
{
    constexpr std::string_view source =
        "class LineIndex\n"
        "{\n"
        "public:\n"
        "    int ToPosition(int offset) const;\n"
        "private:\n"
        "    static int Utf16Width(int text, int stop) noexcept;\n"
        "    int m_text;\n"
        "};\n"
        "int LineIndex::Utf16Width(int text, int stop) noexcept { return text + stop; }\n"
        "int LineIndex::ToPosition(int offset) const { return Utf16Width(offset, 1) + m_text; }\n"
        "struct S { private: int hidden; void helper(); public: void run() { helper(); hidden = 1; "
        "} };\n"
        "void S::helper() {}\n";

    // Call site -> out-of-line definition.
    const auto call = DefinitionOf(source, "Utf16Width(offset");
    ASSERT_EQ(call.size(), 1u);
    EXPECT_EQ(call[0].offset, Find(source, "Utf16Width(int text, int stop) noexcept {"));
    EXPECT_TRUE(call[0].is_definition);

    // In-class declaration -> its definition; definition -> its declaration.
    const auto from_declaration = DefinitionOf(source, "Utf16Width(int text, int stop) noexcept;");
    ASSERT_EQ(from_declaration.size(), 1u);
    EXPECT_EQ(from_declaration[0].offset,
              Find(source, "Utf16Width(int text, int stop) noexcept {"));
    const auto from_definition = DefinitionOf(source, "Utf16Width(int text, int stop) noexcept {");
    ASSERT_EQ(from_definition.size(), 1u);
    EXPECT_EQ(from_definition[0].offset, Find(source, "Utf16Width(int text, int stop) noexcept;"));

    // Same line as the specifier: `private: int hidden;`.
    const auto hidden = DefinitionOf(source, "hidden = 1");
    ASSERT_EQ(hidden.size(), 1u);
    EXPECT_EQ(hidden[0].offset, Find(source, "hidden;"));
    const auto helper = DefinitionOf(source, "helper();");
    ASSERT_EQ(helper.size(), 1u);
    EXPECT_EQ(helper[0].offset, Find(source, "helper() {}"));
}

TEST(NavigationSpec, ResolvesPrivateMemberDeclaredInAHeaderIndex)
{
    constexpr std::string_view header =
        "class LineIndex\n"
        "{\n"
        "public:\n"
        "    void Build(int text);\n"
        "private:\n"
        "    static int Utf16Width(int text, int stop) noexcept;\n"
        "};\n";
    heimdall::ScopeIndex       index = heimdall::CompletionEngine::IndexScopes(header, {});
    constexpr std::string_view source =
        "int LineIndex::Utf16Width(int text, int stop) noexcept { return text + stop; }\n";
    const heimdall::ParseTree tree = heimdall::ParseTree::Parse(source);
    const auto targets = heimdall::Navigation::Definition(tree, Find(source, "Utf16Width"), &index);
    ASSERT_EQ(targets.size(), 1u);
    EXPECT_EQ(targets[0].offset, Find(header, "Utf16Width"));
}
