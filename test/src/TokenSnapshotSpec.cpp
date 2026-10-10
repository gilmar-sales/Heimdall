#include <Heimdall/TokenSnapshot.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>

namespace
{
    void ExpectTokens(const heimdall::TokenSnapshot& snapshot, std::string_view source)
    {
        const auto expected = heimdall::Lexer(source).Lex();
        const auto actual   = snapshot.Materialize();
        ASSERT_EQ(actual->size(), expected.size());
        ASSERT_EQ(snapshot.Size(), expected.size());
        for (std::size_t i = 0; i < expected.size(); ++i)
        {
            const auto token = snapshot.At(i);
            EXPECT_EQ(token.kind, expected[i].kind) << i;
            EXPECT_EQ(token.tok, expected[i].tok) << i;
            EXPECT_EQ(token.offset, expected[i].offset) << i;
            EXPECT_EQ(token.length, expected[i].length) << i;
            EXPECT_EQ((*actual)[i].offset, token.offset);
        }
    }

    TEST(TokenSnapshotSpec, RandomEditsMatchFullLexWithoutChangingPreviousVersions)
    {
        std::string text;
        for (int i = 0; i < 1000; ++i)
        {
            text += "int value = 42; // comment\n";
        }
        auto          source = *heimdall::SourceSnapshot::Create(text);
        auto          tokens = heimdall::TokenSnapshot::Create(heimdall::Lexer(text).Lex());
        std::uint32_t random = 12345;
        const auto    next   = [&random](std::size_t bound)
        {
            random = random * 1664525u + 1013904223u;
            return static_cast<std::size_t>(random >> 8) % bound;
        };
        constexpr std::string_view kAlphabet = "ab_9 \n\t\"'/*(){};.<>=R#x+-eE";

        for (int step = 0; step < 300; ++step)
        {
            const auto  offset  = next(text.size() + 1);
            const auto  removed = next(std::min<std::size_t>(30, text.size() - offset) + 1);
            std::string replacement;
            for (std::size_t i = next(20); i != 0; --i)
            {
                replacement += kAlphabet[next(kAlphabet.size())];
            }
            const auto oldSource = source;
            const auto oldTokens = tokens;
            text.replace(offset, removed, replacement);
            source = *source.Edit(offset, removed, replacement);
            tokens = tokens.Relex(source, { offset, removed, replacement.size() });

            SCOPED_TRACE(step);
            ExpectTokens(tokens, text);
            EXPECT_EQ(*source.Materialize(), text);
            const auto oldText = oldSource.Materialize();
            if (step % 50 == 0)
            {
                ExpectTokens(oldTokens, *oldText);
            }
        }
    }

    TEST(TokenSnapshotSpec, ExtendsTheWindowAcrossAnUnclosedCommentAndDeletionToEmpty)
    {
        const std::string text     = "int value;\n" + std::string(20000, 'a');
        auto              source   = *heimdall::SourceSnapshot::Create(text);
        const auto        previous = heimdall::TokenSnapshot::Create(heimdall::Lexer(text).Lex());

        auto       changed = *source.Edit(0, 0, "/*");
        const auto tokens  = previous.Relex(changed, { 0, 0, 2 });
        ExpectTokens(tokens, "/*" + text);
        auto       empty   = *changed.Edit(0, changed.Size(), "");
        const auto deleted = tokens.Relex(empty, { 0, changed.Size(), 0 });

        ExpectTokens(deleted, "");
    }

    TEST(TokenSnapshotSpec, ALocalEditDoesNotCopyTheWholeTokenBuffer)
    {
        std::string text;
        for (int i = 0; i < 1000; ++i)
        {
            text += "int value = 42;\n";
        }
        auto       source   = *heimdall::SourceSnapshot::Create(text);
        const auto previous = heimdall::TokenSnapshot::Create(heimdall::Lexer(text).Lex());

        auto       changed = *source.Edit(8000, 0, " ");
        const auto tokens  = previous.Relex(changed, { 8000, 0, 1 });

        EXPECT_LT(tokens.CopiedTokens(), 100u);
        EXPECT_EQ(tokens.Materialize(), tokens.Materialize());
        text.insert(8000, " ");
        ExpectTokens(tokens, text);
    }
} // namespace

TEST(TokenSnapshotSpec, DirectivePreservationRejectsMacrosOpaqueTokensAndLineSplices)
{
    const std::string text =
        "#define VALUE 1\nint value = 2;\n// opaque\n#define MACRO \\\n  old\n";
    const auto source = *heimdall::SourceSnapshot::Create(text);
    const auto tokens = heimdall::TokenSnapshot::Create(heimdall::Lexer(text).Lex());
    const auto plain  = text.find("2");

    EXPECT_TRUE(tokens.PreservesDirectives(source, { plain, 1, 1 }, "3"));
    EXPECT_FALSE(tokens.PreservesDirectives(source, { text.find("1"), 1, 1 }, "2"));
    EXPECT_FALSE(tokens.PreservesDirectives(source, { text.find("opaque"), 1, 1 }, "x"));
    EXPECT_FALSE(tokens.PreservesDirectives(source, { plain, 1, 1 }, "#"));
    EXPECT_FALSE(tokens.PreservesDirectives(source, { text.find("old"), 1, 1 }, "x"));
}

TEST(TokenSnapshotSpec, InvalidEditMetadataFallsBackToLosslessFullLex)
{
    const auto before = heimdall::TokenSnapshot::Create(heimdall::Lexer("int old;").Lex());
    const auto source = *heimdall::SourceSnapshot::Create("int changed = 1;");

    const auto tokens = before.Relex(source, { 100, 1, 1 });

    ExpectTokens(tokens, *source.Materialize());
}

TEST(TokenSnapshotSpec, FragmentCompactionPreservesShiftedTokensAndPinnedVersions)
{
    std::string text;
    for (int index = 0; index < 300; ++index)
    {
        text += "int value = 42;\n";
    }
    auto       source       = *heimdall::SourceSnapshot::Create(text);
    auto       tokens       = heimdall::TokenSnapshot::Create(heimdall::Lexer(text).Lex());
    const auto original     = tokens;
    const auto originalText = text;
    bool       compacted    = false;

    for (std::size_t index = 0; index < 300; ++index)
    {
        const auto offset = index * 17 + 4;
        text.insert(offset, " ");
        source    = *source.Edit(offset, 0, " ");
        tokens    = tokens.Relex(source, { offset, 0, 1 });
        compacted = compacted || tokens.CopiedTokens() > 100;
        ExpectTokens(tokens, text);
    }

    EXPECT_TRUE(compacted);
    ExpectTokens(original, originalText);
}
