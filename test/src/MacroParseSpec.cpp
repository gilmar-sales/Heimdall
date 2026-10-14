#include <Heimdall/MacroResolver.hpp>
#include <Heimdall/MacroTable.hpp>
#include <Heimdall/ParseTree.hpp>
#include <Heimdall/SymbolOutline.hpp>

#include <gtest/gtest.h>

#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace
{

    using heimdall::GrammarKind;

    // Resolves the same table at every offset, which is all these sources need.
    class TableResolver final : public heimdall::MacroResolver
    {
    public:
        explicit TableResolver(std::initializer_list<std::string_view> definitions)
        {
            for (const std::string_view definition : definitions)
            {
                auto parsed = heimdall::MacroTable::ParseDefinition(definition);
                EXPECT_TRUE(parsed.has_value()) << definition;
                if (parsed)
                {
                    mTable.Define(std::move(*parsed));
                }
            }
        }

        const heimdall::MacroDef* Find(std::string_view name, std::size_t) const noexcept override
        {
            return mTable.Find(name);
        }

        std::uint64_t Fingerprint() const noexcept override
        {
            return mTable.Size();
        }

        std::optional<heimdall::MacroTable> TableAt(std::size_t) const override
        {
            return mTable;
        }

    private:
        heimdall::MacroTable mTable;
    };

    heimdall::ParseTree Parse(std::string_view source,
                              std::initializer_list<std::string_view> macros)
    {
        heimdall::ParserOptions options;
        options.macro_resolver = std::make_shared<TableResolver>(macros);
        return heimdall::ParseTree::Parse(source, options);
    }

    std::size_t Count(const heimdall::ParseTree& tree, GrammarKind kind)
    {
        std::size_t count = 0;
        for (const auto& node : tree.Nodes())
        {
            count += node.kind == kind;
        }

        return count;
    }

    std::string Describe(const heimdall::ParseTree& tree)
    {
        std::string text;
        for (const auto& diagnostic : tree.Diagnostics())
        {
            text += diagnostic.message + "; ";
        }

        return text;
    }

} // namespace

TEST(MacroParseSpec, WholeStatementMacrosPreserveFollowingStatements)
{
    const auto tree = Parse("void Test() { CHECK_OP(+, 1, 2) CHECK_OP(/, 3, 1) int after; }",
        {"CHECK_OP(op, x, y) static_assert((x op y) == (x op y));"});

    EXPECT_TRUE(tree.Diagnostics().empty()) << Describe(tree);
    EXPECT_EQ(Count(tree, GrammarKind::MacroInvocation), 2);
    EXPECT_EQ(Count(tree, GrammarKind::DeclarationStatement), 1);
}

TEST(MacroParseSpec, AcceptsVariadicStatementMacroWithInternalSemicolon)
{
    const auto tree = Parse(
        "void Test() { FORWARD(value = 1; other = 2;) after = 3; }",
        {"FORWARD(...) __VA_ARGS__"});

    EXPECT_TRUE(tree.Diagnostics().empty()) << Describe(tree);
    EXPECT_EQ(Count(tree, GrammarKind::MacroInvocation), 1);
    EXPECT_EQ(Count(tree, GrammarKind::ExpressionStatement), 3);
}

TEST(MacroParseSpec, ForwardedStatementStillRequiresSemicolon)
{
    const auto tree = Parse("void Test() { FORWARD(value = 1) after = 3; }",
        {"FORWARD(...) __VA_ARGS__"});

    EXPECT_FALSE(tree.Diagnostics().empty());
}

TEST(MacroParseSpec, EmptyConditionalMacroPreservesFollowingStatement)
{
    const auto tree = Parse("void Test() { CONDITIONAL(value = 1;) after = 3; }",
        {"CONDITIONAL(...)"});

    EXPECT_TRUE(tree.Diagnostics().empty()) << Describe(tree);
    EXPECT_EQ(Count(tree, GrammarKind::ExpressionStatement), 1);
}

TEST(MacroParseSpec, NestedStatementMacrosConsumeWholeDefinitions)
{
    // As in Assimp's IMPL_STRUCT: one call expands (through nested macros)
    // to whole function definitions with no trailing `;` of its own.
    const auto tree = Parse("namespace Api { GEN(Widget) GEN(Gadget) struct After {}; }",
        {"GEN(ty) MAKE(ty) FREE(ty)", "MAKE(ty) void make##ty() {}",
            "FREE(ty) void free##ty() {}"});

    EXPECT_TRUE(tree.Diagnostics().empty()) << Describe(tree);
    EXPECT_EQ(Count(tree, GrammarKind::MacroInvocation), 2);
    EXPECT_EQ(Count(tree, GrammarKind::RecordDefinition), 1);
}

TEST(MacroParseSpec, ForwardingMacroRelyingOnOuterSemicolonStaysAnExpression)
{
    // `FORWARD(lock.Check(1, 2));` expands without its own `;`: the outer one
    // terminates it, so the invocation parses as a plain call statement.
    const auto tree = Parse("struct Lock { void Check(int, int); };\n"
        "void Test(Lock& lock) { FORWARD(lock.Check(1, 2)); lock.Check(3, 4); }",
        {"FORWARD(...) __VA_ARGS__"});

    EXPECT_TRUE(tree.Diagnostics().empty()) << Describe(tree);
    EXPECT_EQ(Count(tree, GrammarKind::MacroInvocation), 1);
    EXPECT_EQ(Count(tree, GrammarKind::ExpressionStatement), 1);
}

TEST(MacroParseSpec, EmptyMacroAtNamespaceScopeIsDroppedAsDecoration)
{
    // An empty body counts as a decoration: the invocation leaves the token
    // stream before parsing, so the following definition stands on its own.
    const auto tree = Parse("namespace Api { DISABLED(Widget) struct After {}; }",
        {"DISABLED(...)"});

    EXPECT_TRUE(tree.Diagnostics().empty()) << Describe(tree);
    EXPECT_EQ(Count(tree, GrammarKind::MacroInvocation), 0);
    EXPECT_EQ(Count(tree, GrammarKind::RecordDefinition), 1);
}

TEST(MacroParseSpec, ForwardingWarningMacrosPreserveNamespaceScope)
{
    const auto tree = Parse("SUPPRESS_WARNINGS namespace Engine { struct Body {}; void Update() {} }",
        {"IF_ENABLED(x) x", "WARNING(x) _Pragma(#x)",
            "SUPPRESS_WARNINGS IF_ENABLED(WARNING(GCC diagnostic push))"});

    EXPECT_TRUE(tree.Diagnostics().empty()) << Describe(tree);
    EXPECT_EQ(Count(tree, GrammarKind::NamespaceDefinition), 1);
    EXPECT_EQ(Count(tree, GrammarKind::RecordDefinition), 1);
    const auto& soa = tree.NodesSoA();
    for (std::size_t i = 0; i < soa.size(); ++i)
    {
        if (soa.Kind(i) == GrammarKind::FunctionDefinition)
        {
            EXPECT_EQ(soa.Kind(soa.Parent(i)), GrammarKind::NamespaceDefinition);
        }
    }
}

TEST(MacroParseSpec, ExpressionMacroDoesNotHideAMissingSemicolon)
{
    const auto tree = Parse("void Test() { VALUE(1) int after; }", {"VALUE(x) (x)"});

    EXPECT_FALSE(tree.Diagnostics().empty());
    EXPECT_EQ(Count(tree, GrammarKind::MacroInvocation), 0);
}

TEST(MacroParseSpec, ForwardedExpressionsAreNotWarningDecorations)
{
    const auto tree = Parse("int Test() { return VALUE; }",
        {"FORWARD(x) x", "VALUE FORWARD(1)"});

    EXPECT_TRUE(tree.Diagnostics().empty()) << Describe(tree);
    for (std::size_t i = 0; i < tree.Tokens().size(); ++i)
    {
        if (tree.Text(tree.Tokens()[i]) == "VALUE")
        {
            EXPECT_FALSE(tree.IsDecorationToken(i));
        }
    }
}

TEST(MacroParseSpec, GlobalScopeQualifiedNamesParseAsStatementsExpressionsAndTemplateArguments)
{
    constexpr std::string_view source =
        "void f()\n{\n    ::std::vector< ::std::string> parsed;\n"
        "    ::std::string::size_type pos = 0;\n    while (::testing::AlwaysTrue()) { }\n"
        "    auto p = static_cast< ::std::string*>(nullptr);\n    out << ::std::endl;\n"
        "    ::new (buffer) Widget(1);\n    ::std::stringstream ss;\n    int x = ::y;\n"
        "    auto d = static_cast<char>(::tolower(c));\n    return ::g(1);\n}\n";

    const auto tree = heimdall::ParseTree::Parse(source);

    EXPECT_TRUE(tree.Diagnostics().empty()) << Describe(tree);
}

TEST(MacroParseSpec, FunctionLikeDecorationMacroWithArgumentsIsDropped)
{
    constexpr std::string_view source = "GTEST_DISABLE_WARNINGS_POP_()\n"
        "class Widget\n{\npublic:\n    void Run() GTEST_LOCK_EXCLUDED_(mutex);\n"
        "    int Count() const;\n};\n";

    const auto tree = Parse(source, {"GTEST_DISABLE_WARNINGS_POP_()",
            "GTEST_LOCK_EXCLUDED_(locks)"});

    EXPECT_TRUE(tree.Diagnostics().empty()) << Describe(tree);
    EXPECT_EQ(Count(tree, GrammarKind::FunctionDeclaration), 2);
}

TEST(MacroParseSpec, DecorationThroughNestedMacrosIsRecognized)
{
    constexpr std::string_view source = "class Box\n{\n    GUARDED_BY(mutex) int value;\n};\n";

    const auto tree = Parse(source, {"ANNOTATE(x)", "GUARDED_BY(x) ANNOTATE(guarded_by(x))"});

    EXPECT_TRUE(tree.Diagnostics().empty()) << Describe(tree);
}

TEST(MacroParseSpec, ParenthesizedExpressionMacrosAreNotDecoration)
{
    constexpr std::string_view source = "int f(long v)\n{\n    return v == LIMIT || err == 0;\n}\n";

    const auto tree = Parse(source, {"LIMIT (-9223372036854775807L - 1)", "err (*error_slot())"});

    EXPECT_TRUE(tree.Diagnostics().empty()) << Describe(tree);
    EXPECT_EQ(Count(tree, GrammarKind::MacroInvocation), 0);
}

TEST(MacroParseSpec, TestStyleMacroTakesTheFollowingBlockAsItsBody)
{
    constexpr std::string_view source = "TEST(Suite, Case)\n{\n    int value = 1;\n    use(value);\n}\n"
        "int after;\n";

    const auto tree = Parse(source, {"TEST(a, b) void Test_##a##_##b()"});

    EXPECT_TRUE(tree.Diagnostics().empty()) << Describe(tree);
    EXPECT_EQ(Count(tree, GrammarKind::MacroInvocation), 1);
    EXPECT_EQ(Count(tree, GrammarKind::CompoundStatement), 1);
    EXPECT_EQ(Count(tree, GrammarKind::DeclarationStatement), 1);
}

TEST(MacroParseSpec, DeclarationMacrosAreInvocationsWithTheirSemicolon)
{
    constexpr std::string_view source =
        "DECLARE_FLAG(verbose);\nclass Widget\n{\npublic:\n    FRIEND_TEST(WidgetTest, Run);\n"
        "    DECLARE_ID(Widget)\n    int value;\n};\n";

    const auto tree = Parse(source, {"DECLARE_FLAG(name) extern bool name;",
            "FRIEND_TEST(a, b) friend class a##_##b##_Test",
            "DECLARE_ID(T) static int Id();"});

    EXPECT_TRUE(tree.Diagnostics().empty()) << Describe(tree);
    EXPECT_EQ(Count(tree, GrammarKind::MacroInvocation), 3);
}

TEST(MacroParseSpec, RegistrationMacroChainsUpToTheSemicolon)
{
    constexpr std::string_view source = "BENCHMARK(Fill)->Arg(1)->Arg(2);\nint after;\n";

    const auto tree = Parse(source, {"BENCHMARK(fn) static auto reg = Register(fn)"});

    EXPECT_TRUE(tree.Diagnostics().empty()) << Describe(tree);
    EXPECT_EQ(Count(tree, GrammarKind::MacroInvocation), 1);
}

TEST(MacroParseSpec, MacroInsideFunctionBodyTakesItsBlock)
{
    constexpr std::string_view source =
        "void f()\n{\n    FOR_EACH(item, items)\n    {\n        use(item);\n    }\n    done();\n}\n";

    const auto tree = Parse(source, {"FOR_EACH(x, c) for (auto& x : c)"});

    EXPECT_TRUE(tree.Diagnostics().empty()) << Describe(tree);
    EXPECT_EQ(Count(tree, GrammarKind::MacroInvocation), 1);
}

TEST(MacroParseSpec, WithoutAResolverMacroNamesBehaveAsBefore)
{
    constexpr std::string_view source = "int after;\n";

    const auto tree = heimdall::ParseTree::Parse(source);

    EXPECT_EQ(Count(tree, GrammarKind::MacroInvocation), 0);
    EXPECT_TRUE(tree.Diagnostics().empty());
}

TEST(MacroParseSpec, UnknownNamesAreNotInvocations)
{
    constexpr std::string_view source = "Widget Make(int x);\nvoid f()\n{\n    Make(1);\n}\n";

    const auto tree = Parse(source, {"OTHER(x) x"});

    EXPECT_EQ(Count(tree, GrammarKind::MacroInvocation), 0);
    EXPECT_TRUE(tree.Diagnostics().empty()) << Describe(tree);
}

TEST(MacroParseSpec, TestStyleMacrosAreListedInTheOutlineUnderTheirSpelling)
{
    constexpr std::string_view source =
        "struct Fixture\n{\n    FRIEND_TEST(Suite, Case);\n};\n"
        "TEST(Suite, Case)\n{\n    int value = 1;\n}\nint after;\n";
    const auto tree = Parse(source, {"TEST(a, b) void Test_##a##_##b()",
            "FRIEND_TEST(a, b) friend class a##_##b##_Test"});

    const auto symbols = heimdall::SymbolOutline::Extract(tree);

    std::vector<std::string> names;
    for (const auto& symbol : symbols)
    {
        names.push_back(symbol.name);
    }

    EXPECT_EQ(names, (std::vector<std::string> {"Fixture", "TEST(Suite, Case)", "after"}));
    EXPECT_EQ(symbols[1].kind, heimdall::OutlineKind::Function);
    EXPECT_EQ(source.substr(symbols[1].nameOffset, symbols[1].nameLength), "TEST");
}
