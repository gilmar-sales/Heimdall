#include <Heimdall/ParseTree.hpp>
#include <Heimdall/SyntaxHighlight.hpp>

#include <gtest/gtest.h>

#include <optional>
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
}
