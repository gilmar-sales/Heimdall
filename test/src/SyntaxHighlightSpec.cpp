#include <Heimdall/ParseTree.hpp>
#include <Heimdall/SyntaxHighlight.hpp>

#include <gtest/gtest.h>

#include <optional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace
{

    using heimdall::HighlightKind;
    using heimdall::HighlightSpan;

    std::vector<HighlightSpan> Highlight(std::string_view source)
    {
        const heimdall::ParseTree tree = heimdall::ParseTree::Parse(source);
        return heimdall::SyntaxHighlight::Extract(tree);
    }

    std::optional<HighlightSpan> SpanOf(
        std::string_view source,
        const std::vector<HighlightSpan>& spans,
        std::string_view text,
        std::size_t nth = 0)
    {
        for (const HighlightSpan& span : spans)
        {
            if (source.substr(span.offset, span.length) == text && nth--== 0)
            {
                return span;
            }
        }

        return std::nullopt;
    }

    bool IsKind(
        std::string_view source,
        const std::vector<HighlightSpan>& spans,
        std::string_view text,
        HighlightKind kind,
        std::size_t nth = 0)
    {
        const auto span = SpanOf(source, spans, text, nth);
        return span && span->kind == kind;
    }

} // namespace

TEST(SyntaxHighlightSpec, EmptySourceHasNoSpans)
{
    EXPECT_TRUE(Highlight("").empty());
}

TEST(SyntaxHighlightSpec, ClassifiesHeaderTypesInMemberTemplateArguments)
{
    struct HeaderTypes final : heimdall::TypeNameOracle
    {
        bool IsType(std::string_view name) const noexcept override
        {
            return name == "ComponentManager";
        }

        std::uint64_t Fingerprint() const noexcept override
        {
            return 1;
        }
    };

    constexpr std::string_view source =
        "void F() { services.AddSingleton<ComponentManager>(); }";
    heimdall::ParserOptions options;
    options.type_names = std::make_shared<HeaderTypes>();
    const auto tree = heimdall::ParseTree::Parse(source, options);

    const auto spans = heimdall::SyntaxHighlight::Extract(tree);

    EXPECT_TRUE(IsKind(source, spans, "ComponentManager", HighlightKind::Type));
    EXPECT_TRUE(IsKind(source, spans, "AddSingleton", HighlightKind::Method));
}

TEST(SyntaxHighlightSpec, ClassifiesLexicalCategories)
{
    constexpr std::string_view source = "int x = 42; // note\nconst char* s = \"hi\";\n";

    const auto spans = Highlight(source);

    EXPECT_TRUE(IsKind(source, spans, "int", HighlightKind::Keyword));
    EXPECT_TRUE(IsKind(source, spans, "42", HighlightKind::Number));
    EXPECT_TRUE(IsKind(source, spans, "// note", HighlightKind::Comment));
    EXPECT_TRUE(IsKind(source, spans, "\"hi\"", HighlightKind::String));
    EXPECT_TRUE(IsKind(source, spans, "=", HighlightKind::Operator));
    EXPECT_FALSE(SpanOf(source, spans, ";"));
}

TEST(SyntaxHighlightSpec, HighlightsDoxygenCommandsWithoutChangingCommentText)
{
    constexpr std::string_view source = "/** @brief Summary\n * \\param value details */\n// @brief ordinary\n";

    const auto spans = Highlight(source);

    EXPECT_TRUE(IsKind(source, spans, "@brief", HighlightKind::Doxygen));
    EXPECT_TRUE(IsKind(source, spans, "\\param", HighlightKind::Doxygen));
    EXPECT_TRUE(IsKind(source, spans, "// @brief ordinary", HighlightKind::Comment));
    EXPECT_TRUE(std::ranges::any_of(spans,[&](const HighlightSpan& span)
        {
            return span.kind == HighlightKind::Comment &&
            source.substr(span.offset, span.length).find("Summary") != std::string_view::npos;
        }));
}

TEST(SyntaxHighlightSpec, MarksDeclarationsAndTheirUses)
{
    constexpr std::string_view source = "namespace app\n{\nclass Widget\n{\npublic:\n    int mCount;\n"
        "    void Run();\n};\n}\nvoid Use(app::Widget& w)\n{\n"
        "    w.Run();\n    w.mCount = 1;\n}\n";

    const auto spans = Highlight(source);

    const auto widget = SpanOf(source, spans, "Widget");
    ASSERT_TRUE(widget);
    EXPECT_EQ(widget->kind, HighlightKind::Class);
    EXPECT_EQ(widget->modifiers, heimdall::kHighlightDeclaration);
    EXPECT_TRUE(IsKind(source, spans, "app", HighlightKind::Namespace));
    EXPECT_TRUE(IsKind(source, spans, "Widget", HighlightKind::Class, 1));
    EXPECT_EQ(SpanOf(source, spans, "Widget", 1) -> modifiers, 0);
    EXPECT_TRUE(IsKind(source, spans, "mCount", HighlightKind::Property, 0));
    EXPECT_TRUE(IsKind(source, spans, "Run", HighlightKind::Method, 1));
    EXPECT_TRUE(IsKind(source, spans, "mCount", HighlightKind::Property, 1));
}

TEST(SyntaxHighlightSpec, ClassifiesMemberAccessWithoutDeclarations)
{
    constexpr std::string_view source = "void F()\n{\n    obj.Call(1);\n    obj->value = 2;\n}\n";

    const auto spans = Highlight(source);

    EXPECT_TRUE(IsKind(source, spans, "Call", HighlightKind::Method));
    EXPECT_TRUE(IsKind(source, spans, "value", HighlightKind::Property));
}

TEST(SyntaxHighlightSpec, DoesNotTreatVariableDeclarationAsCall)
{
    constexpr std::string_view source = "void F()\n{\n    Widget w(3);\n    Make(w);\n}\n";

    const auto spans = Highlight(source);

    EXPECT_FALSE(SpanOf(source, spans, "w"));
    EXPECT_TRUE(IsKind(source, spans, "Make", HighlightKind::Function));
}

TEST(SyntaxHighlightSpec, ClassifiesTemplateMethodCallsWithoutDeclarations)
{
    constexpr std::string_view source =
        "void F() {\n"
        "services.AddSingleton<FreyrOptions>(mFreyrOptionsBuilder.Build());\n"
        "const auto systemManager = serviceProvider.GetService<SystemManager>();\n"
        "services->AddSingleton<std::vector<FreyrOptions>>();\n"
        "services.template AddTransient</* type */ Query> ();\n"
        "Make<std::vector<Query>>();\n"
        "services.value < limit;\n"
        "services.Callback<Query>;\n"
        "}\n";

    const auto spans = Highlight(source);

    EXPECT_TRUE(IsKind(source, spans, "AddSingleton", HighlightKind::Method));
    EXPECT_TRUE(IsKind(source, spans, "AddSingleton", HighlightKind::Method, 1));
    EXPECT_TRUE(IsKind(source, spans, "Build", HighlightKind::Method));
    EXPECT_TRUE(IsKind(source, spans, "GetService", HighlightKind::Method));
    EXPECT_TRUE(IsKind(source, spans, "AddTransient", HighlightKind::Method));
    EXPECT_TRUE(IsKind(source, spans, "Make", HighlightKind::Function));
    EXPECT_TRUE(IsKind(source, spans, "value", HighlightKind::Property));
    EXPECT_TRUE(IsKind(source, spans, "Callback", HighlightKind::Property));
}

TEST(SyntaxHighlightSpec, LeavesAmbiguousNamesUnclassified)
{
    constexpr std::string_view source = "class Thing {};\nint Thing2;\nstruct Thing {};\nThing t;\n";

    const auto spans = Highlight(source);

    EXPECT_FALSE(SpanOf(source, spans, "Thing", 3));
}

TEST(SyntaxHighlightSpec, ClassifiesPreprocessorDirectivesAndMacros)
{
    constexpr std::string_view source = "#include <vector>\n#define LIMIT 8\nint a[LIMIT];\n";

    const auto spans = Highlight(source);

    EXPECT_TRUE(IsKind(source, spans, "#include", HighlightKind::Macro));
    EXPECT_TRUE(IsKind(source, spans, "<vector>", HighlightKind::String));
    EXPECT_TRUE(IsKind(source, spans, "#define", HighlightKind::Macro));
    const auto definition = SpanOf(source, spans, "LIMIT", 0);
    ASSERT_TRUE(definition);
    EXPECT_EQ(definition->modifiers, heimdall::kHighlightDeclaration);
    EXPECT_TRUE(IsKind(source, spans, "LIMIT", HighlightKind::Macro, 1));
}

TEST(SyntaxHighlightSpec, SpansAreOrderedAndDoNotOverlap)
{
    constexpr std::string_view source = "#include <a>\nstruct S { int v; };\n/* c\nd */ S s{};\n";

    const auto spans = Highlight(source);

    std::size_t end = 0;
    for (const HighlightSpan& span : spans)
    {
        EXPECT_GE(span.offset, end);
        end = span.offset + span.length;
    }
}

TEST(SyntaxHighlightSpec, KindNamesFollowTheLegendOrder)
{
    EXPECT_EQ(heimdall::HighlightKindName(HighlightKind::Keyword), "keyword");
    EXPECT_EQ(heimdall::HighlightKindName(HighlightKind::EnumMember), "enumMember");
    EXPECT_EQ(heimdall::HighlightKindName(HighlightKind::Variable), "variable");
    EXPECT_EQ(heimdall::HighlightKindName(HighlightKind::Doxygen), "doxygen");
}

TEST(SyntaxHighlightSpec, ResolvesMacrosDefinedOutsideTheFile)
{
    constexpr std::string_view source = "int a = EXTERNAL_LIMIT;\nint b = Local(1);\n";
    const auto external = heimdall::MacroTable::ParseDefinition("EXTERNAL_LIMIT 8");
    ASSERT_TRUE(external);
    const heimdall::ParseTree tree = heimdall::ParseTree::Parse(source);
    heimdall::HighlightContext context;
    context.findMacro = [&](std::string_view name, std::size_t)->const heimdall::MacroDef *
    {
        return name == "EXTERNAL_LIMIT" ? & *external : nullptr;
    };

    const auto spans = heimdall::SyntaxHighlight::Extract(tree, context);

    EXPECT_TRUE(IsKind(source, spans, "EXTERNAL_LIMIT", HighlightKind::Macro));
    EXPECT_TRUE(IsKind(source, spans, "Local", HighlightKind::Function));
}

TEST(SyntaxHighlightSpec, MarksInactiveGroupsWithFullCoverage)
{
    constexpr std::string_view source = "int a;\n#if 0\nint dead = Call(1);\n#endif\nint c;\n";
    const heimdall::ParseTree tree = heimdall::ParseTree::Parse(source);
    const std::size_t deadBegin = source.find("int dead");
    const std::size_t deadEnd = source.find("#endif");
    const std::vector<heimdall::ActiveSourceRange> active = {
        {
            0, deadBegin
        }, {deadEnd, source.size() - deadEnd}
    };
    heimdall::HighlightContext context;
    context.activeRanges = active;

    const auto spans = heimdall::SyntaxHighlight::Extract(tree, context);

    const auto dead = SpanOf(source, spans, "int", 1);
    ASSERT_TRUE(dead);
    EXPECT_EQ(dead->modifiers & heimdall::kHighlightInactive, heimdall::kHighlightInactive);
    const auto call = SpanOf(source, spans, "Call");
    ASSERT_TRUE(call);
    EXPECT_EQ(call->modifiers & heimdall::kHighlightInactive, heimdall::kHighlightInactive);
    const auto deadVar = SpanOf(source, spans, "dead");
    ASSERT_TRUE(deadVar);
    EXPECT_EQ(deadVar->modifiers & heimdall::kHighlightInactive, heimdall::kHighlightInactive);
    const auto live = SpanOf(source, spans, "int", 0);
    ASSERT_TRUE(live);
    EXPECT_EQ(live->modifiers & heimdall::kHighlightInactive, 0);
    EXPECT_EQ(SpanOf(source, spans, "#if")->modifiers & heimdall::kHighlightInactive, 0);
}

TEST(SyntaxHighlightSpec, HighlightsModuleAndImportOnlyAsContextualKeywords)
{
    const std::string source =
        "export module app.core;\n"
        "import std;\n"
        "import :detail;\n"
        "import <vector>;\n";

    const auto spans = Highlight(source);

    EXPECT_TRUE(IsKind(source, spans, "export", HighlightKind::Keyword));
    EXPECT_TRUE(IsKind(source, spans, "module", HighlightKind::Keyword));
    EXPECT_TRUE(IsKind(source, spans, "import", HighlightKind::Keyword, 0));
    EXPECT_TRUE(IsKind(source, spans, "import", HighlightKind::Keyword, 1));
    EXPECT_TRUE(IsKind(source, spans, "import", HighlightKind::Keyword, 2));
}

TEST(SyntaxHighlightSpec, DoesNotHighlightModuleOrImportUsedAsNames)
{
    const std::string source =
        "int module = 1;\n"
        "int f() { int import = module; return import + module; }\n";

    const auto spans = Highlight(source);

    for (std::size_t nth = 0; nth < 4; ++nth)
    {
        EXPECT_FALSE(IsKind(source, spans, "module", HighlightKind::Keyword, nth)) << nth;
    }

    for (std::size_t nth = 0; nth < 2; ++nth)
    {
        EXPECT_FALSE(IsKind(source, spans, "import", HighlightKind::Keyword, nth)) << nth;
    }
}
