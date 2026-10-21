#include "Document.hpp"
#include "SemanticTokens.hpp"

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

    std::string Encode(
        std::string_view text,
        const std::vector<HighlightSpan>& spans,
        std::optional<heimdall::lsp::ByteRange> range = std::nullopt)
    {
        heimdall::lsp::LineIndex lines;
        lines.Build(text);
        std::string out;
        heimdall::lsp::AppendSemanticTokens(spans, text, lines, range, out);
        return out;
    }

} // namespace

TEST(SemanticTokensSpec, EncodesDeltasBetweenTokens)
{
    constexpr std::string_view text = "int a;\nint b;\n";
    const std::vector<HighlightSpan> spans = {
        {
            0, 3, HighlightKind::Keyword, 0
        },
        {7, 3, HighlightKind::Keyword, 0}};

    EXPECT_EQ(Encode(text, spans), "{\"data\":[0,0,3,0,0,1,0,3,0,0]}");
}

TEST(SemanticTokensSpec, SplitsMultilineTokensPerLine)
{
    constexpr std::string_view text = "/* ab\ncd */ x";
    const std::vector<HighlightSpan> spans = {
        {
            0, 11, HighlightKind::Comment, 0
    }};

    EXPECT_EQ(Encode(text, spans), "{\"data\":[0,0,5,3,0,1,0,5,3,0]}");
}

TEST(SemanticTokensSpec, MeasuresColumnsInUtf16Units)
{
    constexpr std::string_view text = "\"\xC3\xA9\" x";
    const std::vector<HighlightSpan> spans = {
        {
            0, 4, HighlightKind::String, 0
        },
        {5, 1, HighlightKind::Variable, 1}};

    EXPECT_EQ(Encode(text, spans), "{\"data\":[0,0,3,2,0,0,4,1,15,1]}");
}

TEST(SemanticTokensSpec, RangeKeepsOnlyOverlappingSpans)
{
    constexpr std::string_view text = "int a;\nint b;\n";
    const std::vector<HighlightSpan> spans = {
        {
            0, 3, HighlightKind::Keyword, 0
        },
        {7, 3, HighlightKind::Keyword, 0}};

    EXPECT_EQ(Encode(text, spans, heimdall::lsp::ByteRange {7, 14}),
              "{\"data\":[1,0,3,0,0]}");
}

TEST(SemanticTokensSpec, LegendListsEveryKindAndTheDeclarationModifier)
{
    const std::string legend = heimdall::lsp::SemanticTokensLegend();

    EXPECT_NE(legend.find("\"keyword\""), std::string::npos);
    EXPECT_NE(legend.find("\"variable\""), std::string::npos);
    EXPECT_NE(legend.find("\"tokenModifiers\":[\"declaration\",\"inactive\",\"definition\",\"readonly\",\"static\""),
              std::string::npos);
    EXPECT_NE(legend.find("\"functionScope\""), std::string::npos);
    EXPECT_NE(legend.find("\"typeParameter\""), std::string::npos);
    EXPECT_NE(legend.find("\"concept\""), std::string::npos);
}

TEST(SemanticTokensSpec, EncodesEveryAdvertisedModifierBit)
{
    constexpr std::string_view text = "value";
    const std::vector<HighlightSpan> spans = {
        {
            0, 5, HighlightKind::Variable,
                heimdall::kHighlightDeclaration | heimdall::kHighlightInactive |
                heimdall::HighlightMask(heimdall::HighlightModifier::Definition) | heimdall::HighlightMask(heimdall::HighlightModifier::Readonly) |
                heimdall::HighlightMask(heimdall::HighlightModifier::Static) | heimdall::HighlightMask(heimdall::HighlightModifier::GlobalScope) |
                heimdall::HighlightMask(heimdall::HighlightModifier::ClassScope) | heimdall::HighlightMask(heimdall::HighlightModifier::FunctionScope)
    }};

    EXPECT_EQ(Encode(text, spans), "{\"data\":[0,0,5,15,255]}");
}
