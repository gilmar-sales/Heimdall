#include <gtest/gtest.h>

#include <Heimdall/ParseTree.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace
{

    std::size_t Count(const heimdall::ParseTree& tree, heimdall::GrammarKind kind)
    {
        std::size_t count = 0;
        for (const auto& node : tree.Nodes())
        {
            count += node.kind == kind;
        }

        return count;
    }

} // namespace

TEST(ParseTreeSpec, ConstructorParametersUseFunctionSuffixWithoutLosingPointerReturnTypes)
{
    using heimdall::GrammarKind;
    const auto tree =
        heimdall::ParseTree::Parse("struct S { S(int a, int b, int c, int d); explicit S(int a, "
                                   "int b, int c, int d, int e) {} };",
                                   {});
    EXPECT_TRUE(tree.Diagnostics().empty());
    EXPECT_EQ(Count(tree, GrammarKind::FunctionSuffix), 2);
    EXPECT_EQ(Count(tree, GrammarKind::ParameterDeclaration), 9);
    const auto pointer =
        heimdall::ParseTree::Parse("struct Widget {}; Widget (*factory)(int, int);", {});
    EXPECT_TRUE(pointer.Diagnostics().empty());
    EXPECT_EQ(Count(pointer, GrammarKind::FunctionSuffix), 1);
    EXPECT_EQ(Count(pointer, GrammarKind::ParameterDeclaration), 2);
    const auto& soa         = pointer.NodesSoA();
    bool        widget_type = false;
    for (std::size_t i = 0; i < soa.size(); ++i)
    {
        if (soa.Kind(i) == GrammarKind::TypeSpecifier &&
            pointer.Text(pointer.Tokens()[soa.FirstToken(i)]) == "Widget")
        {
            widget_type = true;
        }
    }

    EXPECT_TRUE(widget_type);
}

TEST(ParseTreeSpec, SnapshotRetainsAndSharesSourceAndTokensAcrossMoves)
{
    auto source = std::make_shared<const std::string>("int f() { return 2 + 3; }\n");
    auto tokens =
        std::make_shared<const std::vector<heimdall::Token>>(heimdall::Lexer(*source).Lex());
    const auto* data   = tokens->data();
    const auto* text   = source->data();
    auto        parsed = heimdall::ParseTree::ParseSnapshot(source, {}, {}, tokens);
    EXPECT_EQ(parsed.Tokens().data(), data);
    EXPECT_EQ(parsed.Source().data(), text);
    source.reset();
    tokens.reset();
    auto moved = std::move(parsed);
    EXPECT_EQ(moved.Tokens().data(), data);
    EXPECT_EQ(moved.Source(), "int f() { return 2 + 3; }\n");
    EXPECT_TRUE(moved.Diagnostics().empty());
    EXPECT_EQ(moved.Text(moved.Tokens().front()), "int");
}

TEST(ParseTreeSpec, DirectChildRangeMatchesParentRelationshipsForEveryNode)
{
    // Expressions rewrite parents; nested templates, scopes and invalid input
    // exercise the actual ordering invariant rather than a synthetic SoA.
    const std::string_view cases[] = {
        "",
        "int x;",
        "namespace N { struct S { int x; void f(int a); }; int y; }",
        "int f() { int a = 1 + 2 * 3; if (a) { a = f() + 1; } else return 0; return a; }",
        "template<class T> T f(T t) { return t.x[0](1, 2) + T{}; }",
        "auto f = [](int x) { return x ? x + 1 : x * 2; };",
        "void broken( { int a; namespace N { int b; }",
    };
    for (const auto source : cases)
    {
        const auto  tree = heimdall::ParseTree::Parse(source);
        const auto& soa  = tree.NodesSoA();
        for (std::size_t node = 0; node < soa.size(); ++node)
        {
            std::vector<std::size_t> expected;
            for (std::size_t other = node + 1; other < soa.size(); ++other)
            {
                if (soa.Parent(other) == node)
                {
                    expected.push_back(other);
                }
            }

            std::vector<std::size_t> actual;
            for (auto child : tree.DirectChildren(node))
            {
                actual.push_back(child);
            }

            EXPECT_EQ(actual, expected) << source << " node " << node;
            EXPECT_EQ(tree.Children(node), expected);
        }

        EXPECT_TRUE(tree.Children(soa.size()).empty());
        EXPECT_EQ(tree.DirectChildren(soa.size()).begin(), tree.DirectChildren(soa.size()).end());
    }
}

TEST(ParseTreeSpec, ParsesTranslationUnitDefinitionsAndCompoundStatements)
{
    constexpr std::string_view source =
        "namespace demo {\n"
        "struct Item { int value; };\n"
        "int run(int x) { int y = x; if (y) return y; else return 0; }\n"
        "}\n";
    const auto tree = heimdall::ParseTree::Parse(source, heimdall::CppStandard::Cpp23);
    EXPECT_EQ(tree.Standard(), heimdall::CppStandard::Cpp23);
    EXPECT_TRUE(tree.Diagnostics().empty());
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::NamespaceDefinition), 1);
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::RecordDefinition), 1);
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::FunctionDefinition), 1);
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::CompoundStatement), 1);
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::IfStatement), 1);
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::ReturnStatement), 2);
}

TEST(ParseTreeSpec, SelectsConditionalBranchesUsingPredefinedCompileMacros)
{
    constexpr std::string_view source =
        "#if FEATURE\n"
        "int enabled() { return 1; }\n"
        "#else\n"
        "int disabled() { return 2; }\n"
        "#endif\n";
    heimdall::ParserOptions options;
    options.standard = heimdall::CppStandard::Cpp23;
    options.predefined_macros.emplace("FEATURE", "1");
    const auto tree = heimdall::ParseTree::Parse(source, options);
    EXPECT_TRUE(tree.Diagnostics().empty());
    EXPECT_EQ(tree.Standard(), heimdall::CppStandard::Cpp23);
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::FunctionDefinition), 1);
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::ReturnStatement), 1);
}

TEST(ParseTreeSpec, RecoversAtSemicolonAndContinuesParsing)
{
    constexpr std::string_view source = "int f() { int broken return 1; return 2; } int g;";
    const auto                 tree   = heimdall::ParseTree::Parse(source);
    EXPECT_FALSE(tree.Diagnostics().empty());
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::Error), 1);
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::ReturnStatement), 2);
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::Declaration), 1);
}

TEST(ParseTreeSpec, ReportsUnclosedFunctionBodyAndKeepsPartialTree)
{
    const auto tree = heimdall::ParseTree::Parse("int f() { return 1;");
    EXPECT_FALSE(tree.Diagnostics().empty());
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::FunctionDefinition), 1);
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::CompoundStatement), 1);
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::ReturnStatement), 1);
}

TEST(ParseTreeSpec, BuildsExpressionNodesForPrecedenceAndPostfixForms)
{
    const auto tree = heimdall::ParseTree::Parse("int f() { return call(value + 2 * other[0]); }");
    EXPECT_TRUE(tree.Diagnostics().empty());
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::ReturnStatement), 1);
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::CallExpression), 1);
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::SubscriptExpression), 1);
    EXPECT_GE(Count(tree, heimdall::GrammarKind::BinaryExpression), 2);
}

TEST(ParseTreeSpec, ParsesParametersAndDeclaratorsWithInitializers)
{
    const auto tree = heimdall::ParseTree::Parse("int sum(int left, int right = 2) { int first = "
                                                 "left, second{right}; return first + second; }");
    EXPECT_TRUE(tree.Diagnostics().empty());
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::ParameterDeclaration), 2);
    EXPECT_GE(Count(tree, heimdall::GrammarKind::InitDeclarator), 2);
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::DeclaredName), 5);
    EXPECT_GE(Count(tree, heimdall::GrammarKind::BinaryExpression), 1);
}

TEST(ParseTreeSpec, KeepsTemplateArgumentCommasInsideParameterAndDeclaratorTypes)
{
    const auto tree = heimdall::ParseTree::Parse(
        "template<class T, class U> struct Pair {};\n"
        "Pair<int, long> combine(Pair<int, long> left, Pair<char, short> right) {\n"
        "  Pair<int, long> first{}, second{}; return first;\n"
        "}");
    EXPECT_TRUE(tree.Diagnostics().empty());
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::ParameterDeclaration), 2);
    EXPECT_GE(Count(tree, heimdall::GrammarKind::InitDeclarator), 2);
}

TEST(ParseTreeSpec, ParsesLambdasAndCommonCompleteStatementForms)
{
    const auto tree = heimdall::ParseTree::Parse(
        "int f(int value) {\n"
        "  auto fn = [value](int x) { return value + x; };\n"
        "  for (int i = 0; i < value; ++i) { if (i) continue; }\n"
        "  do { --value; } while (value);\n"
        "  switch (value) { case 0: break; default: return fn(1); }\n"
        "  try { return fn(value); } catch (...) { return 0; }\n"
        "}");
    EXPECT_TRUE(tree.Diagnostics().empty());
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::LambdaExpression), 1);
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::DoStatement), 1);
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::CaseLabel), 2);
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::TryStatement), 1);
}

TEST(ParseTreeSpec, KeepsPreprocessorDirectiveBodiesOpaque)
{
    constexpr std::string_view source =
        "#include <vector>\n"
        "#define DECLARE(name) int name() { return 1; }\n"
        "int value() {\n"
        "#if FEATURE\n"
        "  return 1;\n"
        "#else\n"
        "  return 2;\n"
        "#endif\n"
        "}\n";
    const auto tree = heimdall::ParseTree::Parse(source);
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::PreprocessorDirective), 5);
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::FunctionDefinition), 1);
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::ReturnStatement), 1);
}

TEST(ParseTreeSpec, BackslashContinuationsStayInsideTheDirective)
{
    constexpr std::string_view source =
        "#define LIST(X) \\\n"
        "    X(A) X(B)\n"
        "enum class Tok : unsigned char\n"
        "{\n"
        "    None = 0,\n"
        "    LIST(A)\n"
        "};\n";
    const auto tree = heimdall::ParseTree::Parse(source);
    EXPECT_TRUE(tree.Diagnostics().empty());
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::PreprocessorDirective), 1);
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::RecordDefinition), 1);
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::Enumerator), 2);
}

TEST(ParseTreeSpec, UnclosedParenDoesNotSwallowOuterScopeCloser)
{
    constexpr std::string_view source =
        "namespace ns {\n"
        "void broken(\n"
        "}\n"
        "void later() {}\n";
    const auto tree = heimdall::ParseTree::Parse(source);
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::NamespaceDefinition), 1);
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::FunctionDefinition), 1);
    bool saw_unclosed = false;
    for (const auto& diagnostic : tree.Diagnostics())
    {
        if (diagnostic.message == "unclosed delimiter")
        {
            saw_unclosed = true;
        }
    }

    EXPECT_TRUE(saw_unclosed);
}

TEST(ParseTreeSpec, MaintainsValidParentLinksAndContainedTokenRanges)
{
    constexpr std::string_view source =
        "template<class T, class U> struct Pair { T first; U second; };\n"
        "int f(Pair<int, long> p) { return p.first + p.second * 2; }\n";
    const auto tree = heimdall::ParseTree::Parse(source);
    ASSERT_FALSE(tree.Nodes().empty());
    EXPECT_EQ(tree.Nodes()[heimdall::ParseTree::RootNode].kind,
              heimdall::GrammarKind::TranslationUnit);
    for (std::size_t i = 1; i < tree.Nodes().size(); ++i)
    {
        const auto& node = tree.Nodes()[i];
        ASSERT_LT(node.parent, tree.Nodes().size());
        const auto& parent = tree.Nodes()[node.parent];
        EXPECT_LE(parent.first_token, node.first_token);
        EXPECT_LE(node.first_token + node.token_count, parent.first_token + parent.token_count);
    }
}

TEST(ParseTreeSpec, ParsesNestedTemplateIdsAsPostfixExpressionsNotComparisons)
{
    const auto tree = heimdall::ParseTree::Parse(
        "int f() { return choose<std::pair<int, long>, std::vector<char>>(make<int>(), value); }");
    EXPECT_TRUE(tree.Diagnostics().empty());
    EXPECT_GE(Count(tree, heimdall::GrammarKind::TemplateIdExpression), 3);
    EXPECT_GE(Count(tree, heimdall::GrammarKind::TemplateArgument), 5);
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::CallExpression), 2);
}

TEST(ParseTreeSpec, ParsesFunctionDeclarationsAndClassMemberPrototypes)
{
    const auto tree = heimdall::ParseTree::Parse(
        "int transform(const Widget& input, int scale = 1);\n"
        "struct Widget {\n"
        "  Widget();\n"
        "  int value() const;\n"
        "  int data;\n"
        "};\n");
    EXPECT_TRUE(tree.Diagnostics().empty());
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::FunctionDeclaration), 3);
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::ParameterDeclaration), 2);
    // Includes the constructor's name, which must remain a declarator rather
    // than being consumed as a return-type specifier.
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::DeclaredName), 6);
}

TEST(ParseTreeSpec, ParsesModuleUnitsConceptsAndRequiresClauses)
{
    const auto tree = heimdall::ParseTree::Parse(
        "export module sample.core;\n"
        "import std;\n"
        "export import :detail;\n"
        "template<class T> concept HasValue = requires(T value) { value.get(); };\n"
        "template<class T> void use(T value) requires HasValue<T> { }\n");
    EXPECT_TRUE(tree.Diagnostics().empty());
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::ModuleDeclaration), 1);
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::ImportDeclaration), 2);
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::ConceptDefinition), 1);
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::RequiresExpression), 1);
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::Requirement), 1);
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::RequiresClause), 1);
}

TEST(ParseTreeSpec, ParsesComplexDeclaratorsWithPointersArraysAndQualifiers)
{
    const auto tree = heimdall::ParseTree::Parse(
        "int (*fp)(int, char);\n"
        "int values[3][4];\n"
        "const char* const* argv;\n"
        "int* const p = nullptr;\n"
        "int& ref = values[0][0];\n"
        "std::vector<int>::iterator it;\n"
        "int Widget::*mp;\n");
    EXPECT_TRUE(tree.Diagnostics().empty());
    EXPECT_GE(Count(tree, heimdall::GrammarKind::DeclaredName), 7);
    EXPECT_GE(Count(tree, heimdall::GrammarKind::PointerOperator), 4);
    EXPECT_GE(Count(tree, heimdall::GrammarKind::ArraySuffix), 2);
    EXPECT_GE(Count(tree, heimdall::GrammarKind::FunctionSuffix), 1);
    EXPECT_GE(Count(tree, heimdall::GrammarKind::NestedNameSpecifier), 1);
}

TEST(ParseTreeSpec, ParsesFunctionSuffixesWithTrailingReturnNoexceptAndAttributes)
{
    const auto tree = heimdall::ParseTree::Parse(
        "[[nodiscard]] auto compute(int x) -> int;\n"
        "void stable() noexcept;\n"
        "void guarded() noexcept(true);\n"
        "struct Flags { int x : 3; unsigned y : 4; };\n");
    EXPECT_TRUE(tree.Diagnostics().empty());
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::FunctionDeclaration), 3);
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::TrailingReturnType), 1);
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::NoexceptSpecifier), 2);
    EXPECT_GE(Count(tree, heimdall::GrammarKind::AttributeSpecifier), 1);
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::BitfieldSuffix), 2);
}

TEST(ParseTreeSpec, ParsesLeadingRequiresClausesBeforeDeclarationsAndDefinitions)
{
    const auto tree = heimdall::ParseTree::Parse(
        "template<class T> requires HasValue<T> void use(T value);\n"
        "template<class T> requires Sortable<T> void sort(T& value) { }\n"
        "template<class T> void check(T value) requires Checkable<T> && Printable<T>;\n");
    EXPECT_TRUE(tree.Diagnostics().empty());
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::TemplateDeclaration), 3);
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::RequiresClause), 3);
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::FunctionDeclaration), 2);
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::FunctionDefinition), 1);
}

TEST(ParseTreeSpec, ParsesEnumDefinitionsWithCommaSeparatedEnumerators)
{
    const auto tree = heimdall::ParseTree::Parse(
        "namespace fs = std::filesystem;\n"
        "namespace\n"
        "{\n"
        "enum class Command\n"
        "{\n"
        "    Lint,\n"
        "    Check,\n"
        "    Format = 4,\n"
        "    Parse = Lint | Check\n"
        "};\n"
        "}\n"
        "enum Old { A = 1 << 2, B, C };\n"
        "enum class Mode : unsigned char { Off, On };\n");
    EXPECT_TRUE(tree.Diagnostics().empty());
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::RecordDefinition), 3);
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::Enumerator), 9);
    // 9 enumerators plus the `fs` namespace-alias name.
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::DeclaredName), 10);
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::NamespaceDefinition), 1);
}

TEST(ParseTreeSpec, ReportsMissingSemicolonBetweenStructMembers)
{
    constexpr std::string_view source =
        "struct Options\n"
        "{\n"
        "    int standard = 1\n"
        "    int compile_commands\n"
        "    int inputs;\n"
        "};\n";
    const auto tree = heimdall::ParseTree::Parse(source);
    ASSERT_EQ(tree.Diagnostics().size(), 2);
    EXPECT_EQ(tree.Diagnostics()[0].message, "expected ';' before 'int'");
    EXPECT_EQ(tree.Diagnostics()[0].offset, source.find("int compile_commands"));
    EXPECT_EQ(tree.Diagnostics()[1].message, "expected ';' before 'int'");
    EXPECT_EQ(tree.Diagnostics()[1].offset, source.find("int inputs"));
    // Shrunk declaration nodes must still be contained in their parents.
    for (std::size_t i = 1; i < tree.Nodes().size(); ++i)
    {
        const auto& node   = tree.Nodes()[i];
        const auto& parent = tree.Nodes()[node.parent];
        EXPECT_LE(parent.first_token, node.first_token);
        EXPECT_LE(node.first_token + node.token_count, parent.first_token + parent.token_count);
    }
}

TEST(ParseTreeSpec, ReportsMissingSemicolonBetweenLocalDeclarations)
{
    constexpr std::string_view source =
        "int f()\n"
        "{\n"
        "    int first = 1\n"
        "    int second = first;\n"
        "    return second;\n"
        "}\n";
    const auto tree = heimdall::ParseTree::Parse(source);
    ASSERT_EQ(tree.Diagnostics().size(), 1);
    EXPECT_EQ(tree.Diagnostics()[0].message, "expected ';' before 'int'");
    EXPECT_EQ(tree.Diagnostics()[0].offset, source.find("int second"));
}

TEST(ParseTreeSpec, ReportsMissingSemicolonBetweenExpressionStatements)
{
    constexpr std::string_view source =
        "void g()\n"
        "{\n"
        "    run()\n"
        "    stop();\n"
        "}\n";
    const auto tree = heimdall::ParseTree::Parse(source);
    ASSERT_EQ(tree.Diagnostics().size(), 1);
    EXPECT_EQ(tree.Diagnostics()[0].message, "expected ';' before 'stop'");
    EXPECT_EQ(tree.Diagnostics()[0].offset, source.find("stop"));
}

TEST(ParseTreeSpec, AcceptsLocalStructDefinitionWithDirectInitDeclarator)
{
    constexpr std::string_view source =
        "void run(Token job_stop)\n"
        "{\n"
        "    struct Scope\n"
        "    {\n"
        "        RequestContext context;\n"
        "        Scope(std::stop_token stop)\n"
        "        {\n"
        "            context.stop = std::move(stop);\n"
        "            t_context = &context;\n"
        "        }\n"
        "        ~Scope()\n"
        "        {\n"
        "            t_context = nullptr;\n"
        "        }\n"
        "    } scope(job_stop);\n"
        "}\n";
    const auto tree = heimdall::ParseTree::Parse(source);
    EXPECT_TRUE(tree.Diagnostics().empty());
}

TEST(ParseTreeSpec, AcceptsLocalStructWithBaseClauseAndDeclarator)
{
    constexpr std::string_view source =
        "void run()\n"
        "{\n"
        "    struct Local final : Base { int x; } value{};\n"
        "}\n";
    const auto tree = heimdall::ParseTree::Parse(source);
    EXPECT_TRUE(tree.Diagnostics().empty());
}

TEST(ParseTreeSpec, ReportsMissingCommaBetweenParameters)
{
    constexpr std::string_view source = "int sum(int left int right);\n";
    const auto                 tree   = heimdall::ParseTree::Parse(source);
    ASSERT_EQ(tree.Diagnostics().size(), 1);
    EXPECT_EQ(tree.Diagnostics()[0].message, "expected ',' before 'int'");
    EXPECT_EQ(tree.Diagnostics()[0].offset, source.find("int right"));
}

TEST(ParseTreeSpec, ReportsMissingSemicolonAfterFunctionDeclaration)
{
    constexpr std::string_view source =
        "void first()\n"
        "int second();\n";
    const auto tree = heimdall::ParseTree::Parse(source);
    ASSERT_EQ(tree.Diagnostics().size(), 1);
    EXPECT_EQ(tree.Diagnostics()[0].message, "expected ';' before 'int'");
    EXPECT_EQ(tree.Diagnostics()[0].offset, source.find("int second"));
}

TEST(ParseTreeSpec, ReportsMissingCommaBetweenEnumerators)
{
    constexpr std::string_view source =
        "enum class Command\n"
        "{\n"
        "    Lint,\n"
        "    Check\n"
        "    Format,\n"
        "    Parse\n"
        "};\n";
    const auto tree = heimdall::ParseTree::Parse(source);
    ASSERT_EQ(tree.Diagnostics().size(), 1);
    EXPECT_EQ(tree.Diagnostics()[0].message, "expected ',' before 'Format'");
    EXPECT_EQ(tree.Diagnostics()[0].offset, source.find("Format"));
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::Enumerator), 4);
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::DeclaredName), 4);
}

TEST(ParseTreeSpec, ParsesCastsNewExpressionsAndLiteralFormsWithoutMissingSemicolonNoise)
{
    const auto tree = heimdall::ParseTree::Parse(
        "struct Scale\n"
        "{\n"
        "    int ratio = (int)1.5;\n"
        "    int* blob = (int*)0;\n"
        "    void* token = (void*)blob;\n"
        "    unsigned count = 16u;\n"
        "    const char* text = \"ab\" \"cd\";\n"
        "    int* slots = new int[2];\n"
        "    void paint() const;\n"
        "    void reset() noexcept;\n"
        "};\n"
        "int f()\n"
        "{\n"
        "    int ratio = (int)1.5;\n"
        "    delete[] ratio;\n"
        "    return ratio;\n"
        "}\n");
    EXPECT_TRUE(tree.Diagnostics().empty());
}

TEST(ParseTreeSpec, ParsesUsingInsideBodiesAsUsingDeclarationNotAVariable)
{
    constexpr std::string_view source =
        "void f() { using namespace a; using a::b; using T = int; T value = 1; }\n";
    const auto tree = heimdall::ParseTree::Parse(source);
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::UsingDeclaration), 3);
    // Only `value` is a declared name: neither `namespace` nor `b` is.
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::DeclaredName), 2); // `f` and `value`
}

TEST(ParseTreeSpec, AccessSpecifierIsALabelNotADeclaration)
{
    // `private:` used to parse as a declaration whose bit-field width swallowed
    // the next member, hiding it from navigation and completion.
    constexpr std::string_view source =
        "class A {\n"
        "public:\n"
        "    void run();\n"
        "private:\n"
        "    static int helper(int a) noexcept;\n"
        "protected:\n"
        "    int value;\n"
        "};\n";
    const auto tree = heimdall::ParseTree::Parse(source);
    EXPECT_TRUE(tree.Diagnostics().empty());
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::AccessSpecifier), 3);
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::BitfieldSuffix), 0);
    // Every member is its own declaration with its own declared name.
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::FunctionDeclaration), 2);
    std::size_t names = 0;
    for (const auto& node : tree.Nodes())
    {
        if (node.kind != heimdall::GrammarKind::DeclaredName)
        {
            continue;
        }

        const auto text = tree.Text(tree.Tokens()[node.first_token]);
        names += text == "run" || text == "helper" || text == "value";
    }

    EXPECT_EQ(names, 3u);
}

TEST(ParseTreeSpec, ScopeQualifierAndBitfieldAreNotAccessSpecifiers)
{
    constexpr std::string_view source =
        "struct S {\n"
        "    unsigned flag : 1;\n"
        "    int x = ns::value;\n"
        "};\n";
    const auto tree = heimdall::ParseTree::Parse(source);
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::AccessSpecifier), 0);
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::BitfieldSuffix), 1);
}

TEST(ParseTreeSpec, UnnamedConstQualifiedPointerParametersAreNotMissingCommas)
{
    // `const A::B *` without a parameter name used to parse `const` as the
    // declared name and report "expected ',' before 'A'".
    for (const std::string_view source : {
             "void f(int, const A::B *);\n",
             "void f(ParseTree &, const A &, std::stop_token, const B::C *);\n",
             "void f(const A::B &, volatile ns::T *, const C * const *);\n",
             "class P { friend void detail::ParseWithGrammar(ParseTree &, const PreprocessorResult "
             "&,\n"
             "    std::stop_token, const Preprocessor::MacroMap *); };\n",
         })
    {
        const auto tree = heimdall::ParseTree::Parse(source);
        EXPECT_TRUE(tree.Diagnostics().empty()) << source << tree.Diagnostics().front().message;
        for (const auto& node : tree.Nodes())
        {
            if (node.kind != heimdall::GrammarKind::DeclaredName)
            {
                continue;
            }

            EXPECT_NE(tree.Text(tree.Tokens()[node.first_token]), "const") << source;
        }
    }
}

TEST(ParseTreeSpec, SoaColumnsMatchTheAosCompatibilityView)
{
    const auto tree = heimdall::ParseTree::Parse(
        "#include <vector>\nnamespace n { struct S { int v; }; int f(int a) { return a; } }\n");
    const auto& soa = tree.NodesSoA();
    const auto& aos = tree.Nodes();
    ASSERT_EQ(soa.size(), aos.size());
    ASSERT_FALSE(soa.empty());
    ASSERT_EQ(soa.kind.size(), soa.parent.size());
    ASSERT_EQ(soa.first_token.size(), soa.subtree_end.size());
    for (std::size_t i = 0; i < soa.size(); ++i)
    {
        EXPECT_EQ(soa.Kind(i), aos[i].kind);
        EXPECT_EQ(soa.FirstToken(i), aos[i].first_token);
        EXPECT_EQ(soa.TokenCount(i), aos[i].token_count);
        EXPECT_EQ(soa.Parent(i), aos[i].parent);
        EXPECT_EQ(soa.SubtreeEnd(i), aos[i].subtree_end);
        const auto view = soa[i];
        EXPECT_EQ(view.GetKind(), aos[i].kind);
        EXPECT_EQ(view.GetFirstToken(), aos[i].first_token);
        EXPECT_EQ(view.GetTokenCount(), aos[i].token_count);
        EXPECT_EQ(view.GetParent(), aos[i].parent);
        EXPECT_EQ(view.GetSubtreeEnd(), aos[i].subtree_end);
    }
}

TEST(ParseTreeSpec, AuxiliaryTokenIndicesMatchTokenKinds)
{
    const auto tree = heimdall::ParseTree::Parse(
        "#define A 1\n#include <vector>\nint x = A; // note\n#if 0\n#endif\n");
    const auto& tokens = tree.Tokens();
    ASSERT_EQ(tree.TokenKindMask().size(), tokens.size());

    std::vector<std::uint32_t> identifiers;
    for (std::size_t i = 0; i < tokens.size(); ++i)
    {
        const auto kind = tokens[i].kind;
        const bool trivia =
            kind == heimdall::TokenKind::Whitespace || kind == heimdall::TokenKind::LineComment ||
            kind == heimdall::TokenKind::BlockComment;
        EXPECT_EQ(tree.TokenKindMask()[i], trivia ? 1 : 0) << "token " << i;
        if (kind == heimdall::TokenKind::Identifier)
        {
            identifiers.push_back(static_cast<std::uint32_t>(i));
        }
    }

    EXPECT_EQ(tree.IdentifierTokens(), identifiers);

    // Every directive token lies inside some directive; every token inside a
    // directive is listed, in ascending order.
    std::vector<std::uint32_t> expected;
    for (const auto& directive : tree.Directives())
    {
        for (std::size_t i = 0; i < tokens.size(); ++i)
        {
            if (tokens[i].offset >= directive.offset &&
                tokens[i].offset + tokens[i].length <= directive.offset + directive.length)
            {
                expected.push_back(static_cast<std::uint32_t>(i));
            }
        }
    }

    EXPECT_FALSE(expected.empty());
    EXPECT_EQ(tree.DirectiveTokens(), expected);
}

TEST(ParseTreeSpec, ParsesLanguageLinkageSpecifications)
{
    const auto tree = heimdall::ParseTree::Parse(
        "extern \"C\" {\n"
        "    void foo();\n"
        "    int bar();\n"
        "}\n"
        "extern \"C++\" {\n"
        "    void baz();\n"
        "}\n");
    EXPECT_TRUE(tree.Diagnostics().empty());
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::LanguageLinkageSpec), 2);
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::FunctionDeclaration), 3);
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::DeclaredName), 3);
}

TEST(ParseTreeSpec, ParsesLanguageLinkageWithNestedDeclarations)
{
    const auto tree = heimdall::ParseTree::Parse(
        "extern \"C\" {\n"
        "    struct S { int x; };\n"
        "    int value = 42;\n"
        "    void func(int param);\n"
        "}\n");
    EXPECT_TRUE(tree.Diagnostics().empty());
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::LanguageLinkageSpec), 1);
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::RecordDefinition), 1);
    // struct S {} is both a RecordDefinition and a Declaration; int value is a Declaration
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::Declaration), 2);
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::FunctionDeclaration), 1);
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::DeclaredName), 4); // S, value, func, param
}

// ---- names: types, values, casts -----------------------------------------------------

namespace
{

    class FakeTypeNames final : public heimdall::TypeNameOracle
    {
      public:
        FakeTypeNames(std::initializer_list<std::string_view> names,
                      std::uint64_t fingerprint = 1) : m_names(names), m_fingerprint(fingerprint)
        {
        }

        bool IsType(std::string_view name) const noexcept override
        {
            return std::find(m_names.begin(), m_names.end(), name) != m_names.end();
        }

        std::uint64_t Fingerprint() const noexcept override { return m_fingerprint; }

      private:
        std::vector<std::string_view> m_names;
        std::uint64_t                 m_fingerprint;
    };

    heimdall::ParseTree ParseWith(std::string_view                                source,
                                  std::shared_ptr<const heimdall::TypeNameOracle> names = {})
    {
        heimdall::ParserOptions options;
        options.type_names = std::move(names);
        return heimdall::ParseTree::Parse(source, options);
    }

} // namespace

TEST(ParseTreeSpec, ProductOfVariablesIsNotADeclaration)
{
    const auto tree = ParseWith("void f(int a, int b) { a * b; }\n");
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::ExpressionStatement), 1);
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::DeclarationStatement), 0);

    const auto locals = ParseWith("void f() { int a = 1; int b = 2; a * b; a & b; }\n");
    EXPECT_EQ(Count(locals, heimdall::GrammarKind::DeclarationStatement), 2);
    EXPECT_EQ(Count(locals, heimdall::GrammarKind::ExpressionStatement), 2);
}

TEST(ParseTreeSpec, UnknownOrTypeNamesKeepTheDeclarationReading)
{
    // Nothing is known about `Foo`: the shape-based reading is unchanged.
    EXPECT_EQ(
        Count(ParseWith("void f() { Foo * p; }\n"), heimdall::GrammarKind::DeclarationStatement),
        1);
    EXPECT_EQ(Count(ParseWith("struct Foo {};\nvoid f() { Foo * p; Foo & r = *p; }\n"),
                    heimdall::GrammarKind::DeclarationStatement),
              2);
}

TEST(ParseTreeSpec, ABlockScopedValueShadowsAType)
{
    const std::string_view source =
        "struct T {};\n"
        "void f(int x) {\n"
        "    T * a;\n"                // declaration: T is the struct
        "    { int T = 2; T * x; }\n" // product: the local T hides the struct
        "    T * b;\n"                // declaration again after the block
        "}\n";
    const auto tree = ParseWith(source);
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::DeclarationStatement),
              3); // `T * a`, `int T`, `T * b`
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::ExpressionStatement), 1);
}

TEST(ParseTreeSpec, ForLoopVariablesAreVisibleInTheBodyOnly)
{
    const auto tree = ParseWith("void f() { for (int i = 0; i < 3; ++i) { i * i; } }\n");
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::ExpressionStatement), 1 + 0);
}

TEST(ParseTreeSpec, ParsesCastsOnlyWhenTheTypeIsKnown)
{
    const std::string_view source =
        "struct Foo { int v; };\n"
        "void f(void *p, double d) {\n"
        "    auto a = (Foo*)p;\n"
        "    auto b = (int)d;\n"
        "    auto c = (unsigned long)d + 1;\n"
        "    auto e = (Unknown*)p;\n"
        "    auto g = (d);\n"
        "}\n";
    const auto tree = ParseWith(source);
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::CastExpression), 3);
    // The unknown one keeps the old reading: a parenthesized expression.
    EXPECT_GE(Count(tree, heimdall::GrammarKind::ParenthesizedExpression), 2);
}

TEST(ParseTreeSpec, CastsAreNotMistakenForParenthesizedProducts)
{
    // `(x) * y` with a variable `x` is a product, even if `x` shares a name with a type elsewhere.
    const auto tree =
        ParseWith("struct x {};\nvoid f() { int x = 1; int y = 2; int z = (x) * y; }\n");
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::CastExpression), 0);
}

TEST(ParseTreeSpec, HeaderTypeNamesComeFromTheOracle)
{
    const std::string_view source = "void f(void *p) { auto w = (Widget*)p; Widget * q; }\n";
    EXPECT_EQ(Count(ParseWith(source), heimdall::GrammarKind::CastExpression), 0);

    const auto known = ParseWith(
        source,
        std::make_shared<FakeTypeNames>(std::initializer_list<std::string_view> { "Widget" }));
    EXPECT_EQ(Count(known, heimdall::GrammarKind::CastExpression), 1);
    // The cast holds its type and its operand.
    for (std::size_t n = 0; n < known.Nodes().size(); ++n)
    {
        if (known.Nodes()[n].kind == heimdall::GrammarKind::CastExpression)
        {
            const auto children = known.Children(n);
            ASSERT_EQ(children.size(), 2u);
            EXPECT_EQ(known.Nodes()[children[0]].kind, heimdall::GrammarKind::TypeSpecifier);
            EXPECT_EQ(known.Nodes()[children[1]].kind, heimdall::GrammarKind::IdentifierExpression);
        }
    }
}

TEST(ParseTreeSpec, AVariableOfTheFileHidesAHeaderType)
{
    const auto names =
        std::make_shared<FakeTypeNames>(std::initializer_list<std::string_view> { "count" });
    const auto tree =
        ParseWith("void f(int count, int n) { auto a = (count) - n; count * n; }\n", names);
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::CastExpression), 0);
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::DeclarationStatement), 1); // only `auto a`
}

TEST(ParseTreeSpec, ReusedItemsAreReparsedWhenTheKnownNamesChange)
{
    const std::string   first    = "struct A {};\nvoid f(void *p) { auto x = (A*)p; }\n";
    auto                owned    = std::make_shared<std::string>(first);
    heimdall::ParseTree previous = heimdall::ParseTree::Parse(*owned, {});
    previous.HoldSource(owned);
    ASSERT_EQ(Count(previous, heimdall::GrammarKind::CastExpression), 1);

    // `A` stops being a type: the unchanged second item must not be copied as is.
    std::string edited = first;
    edited.replace(edited.find("A {}"), 1, "B");
    const heimdall::ParseReuse reuse { &previous, edited.find("B {}"), 1, 1 };
    const auto incremental = heimdall::ParseTree::Parse(edited, {}, {}, nullptr, &reuse);
    EXPECT_EQ(Count(incremental, heimdall::GrammarKind::CastExpression), 0);
    EXPECT_EQ(Count(incremental, heimdall::GrammarKind::CastExpression),
              Count(heimdall::ParseTree::Parse(edited, {}), heimdall::GrammarKind::CastExpression));

    // An edit that leaves the names alone still reuses the items.
    std::string touched = first;
    touched.replace(touched.find("auto x"), 4, "auto");
    touched.insert(touched.find("void f"), "int unrelated;\n");
    const heimdall::ParseReuse reuse_unrelated { &previous, touched.find("int unrelated"), 0, 15 };
    const auto kept = heimdall::ParseTree::Parse(touched, {}, {}, nullptr, &reuse_unrelated);
    EXPECT_EQ(Count(kept, heimdall::GrammarKind::CastExpression), 1);
}

TEST(ParseTreeSpec, ConstructorDefinitionsDoNotHideTheirClassName)
{
    // `Widget::Widget` names a function, but `Widget w(1);` still declares a Widget.
    const auto tree = ParseWith("struct Widget { Widget(int); };\n"
                                "Widget::Widget(int) {}\n"
                                "void use() { Widget w(1); Widget * p = nullptr; }\n");
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::DeclarationStatement), 2);
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::ExpressionStatement), 0);
}

TEST(ParseTreeSpec, AlignasSpecifiesDeclarationsAtEveryScope)
{
    constexpr std::string_view source =
        "struct S {\n"
        "    alignas(alignof(std::max_align_t)) std::array<std::byte, 16> m_scratch_buffer;\n"
        "    [[maybe_unused]] alignas(4) int b;\n"
        "};\n"
        "alignas(16) static int g;\n"
        "alignas(S) char storage[sizeof(S)];\n"
        "void f() {\n"
        "    alignas(32) float v[8];\n"
        "    alignas(alignof(long)) int local = alignof(int) + 1;\n"
        "}\n";
    const auto tree = heimdall::ParseTree::Parse(source, heimdall::CppStandard::Cpp20);
    EXPECT_TRUE(tree.Diagnostics().empty());
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::DeclarationStatement), 2);
    EXPECT_GE(Count(tree, heimdall::GrammarKind::AttributeSpecifier), 5);
}

TEST(ParseTreeSpec, AlignasOnRecordKeepsItsName)
{
    constexpr std::string_view source = "struct alignas(16) A { int x; };\nA a;\n";
    const auto tree = heimdall::ParseTree::Parse(source, heimdall::CppStandard::Cpp20);
    EXPECT_TRUE(tree.Diagnostics().empty());
    EXPECT_EQ(Count(tree, heimdall::GrammarKind::RecordDefinition), 1);
}

TEST(ParseTreeSpec, FileLocalDecorationMacroDoesNotBreakDeclarations)
{
    constexpr std::string_view source =
        "#define STBIDEF extern\n"
        "#include <stdio.h>\n"
        "STBIDEF int stbi_is_16_bit_from_file(FILE *f);\n"
        "STBIDEF void stbi_hdr_to_ldr_gamma(float gamma);\n";

    const auto tree = heimdall::ParseTree::Parse(source);
    EXPECT_TRUE(tree.Diagnostics().empty());
}

TEST(ParseTreeSpec, IfConstexprAndIfConstevalParseWithoutDiagnostics)
{
    const auto tree = heimdall::ParseTree::Parse(
        "int f()\n{\n"
        "    if constexpr (sizeof(void*) == 8)\n    {\n        return 1;\n    }\n    else\n    {\n "
        "       return 2;\n    }\n"
        "}\n"
        "int g()\n{\n"
        "    if consteval { return 1; }\n"
        "    if !consteval { return 2; }\n"
        "    if constexpr (sizeof(int) == 4) return 3;\n"
        "    return 0;\n"
        "}\n");
    EXPECT_TRUE(tree.Diagnostics().empty());
}
