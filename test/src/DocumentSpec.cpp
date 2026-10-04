#include <gtest/gtest.h>

#include "Document.hpp"

using heimdall::lsp::OffsetFromPosition;
using heimdall::lsp::Position;
using heimdall::lsp::ToPosition;

TEST(DocumentSpec, RoundTripsOffsetsThroughPositions)
{
    constexpr std::string_view text = "int a;\nint bb;\n";
    for (std::size_t offset = 0; offset <= text.size(); ++offset)
    {
        EXPECT_EQ(OffsetFromPosition(text, ToPosition(text, offset)), offset) << "offset " << offset;
    }
}

TEST(DocumentSpec, ClampsLineAndCharacterOverflow)
{
    constexpr std::string_view text = "ab\ncde\n";
    // Past end of first line clamps to the newline.
    EXPECT_EQ(OffsetFromPosition(text, { 0, 99 }), 2);
    // Past end of file clamps to EOF.
    EXPECT_EQ(OffsetFromPosition(text, { 99, 0 }), text.size());
    EXPECT_EQ(OffsetFromPosition(text, { 1, 99 }), text.size() - 1);
    // Start of second line.
    EXPECT_EQ(OffsetFromPosition(text, { 1, 0 }), 3);
}

TEST(DocumentSpec, CountsUtf16CodeUnitsLikeToPosition)
{
    // U+00E9 is one UTF-16 unit, U+1F600 is a surrogate pair (two units).
    const std::string text = "a\xC3\xA9\xF0\x9F\x98\x80"
                               "b\n";
    EXPECT_EQ(ToPosition(text, 0).line, 0);
    EXPECT_EQ(ToPosition(text, 0).character, 0);
    EXPECT_EQ(OffsetFromPosition(text, { 0, 0 }), 0);
    EXPECT_EQ(OffsetFromPosition(text, { 0, 1 }), 1);
    EXPECT_EQ(OffsetFromPosition(text, { 0, 2 }), 3);
    EXPECT_EQ(OffsetFromPosition(text, { 0, 4 }), 7);
    EXPECT_EQ(OffsetFromPosition(text, { 1, 0 }), text.size());
}

TEST(DocumentSpec, LineIndexUpdateMatchesFullBuildForRandomEdits)
{
    const char alphabet[] = "ab \n\n\xC3\xA9\xF0\x9F\x98\x80;";
    std::uint32_t state = 777;
    auto next = [&](std::size_t bound) {
        state = state * 1664525u + 1013904223u;
        return static_cast<std::size_t>((state >> 8) % bound);
    };
    std::string text = "int a;\nint bb;\n\nx";
    heimdall::lsp::LineIndex incremental;
    incremental.Build(text);
    for (int step = 0; step < 3000; ++step)
    {
        const std::size_t offset = next(text.size() + 1);
        const std::size_t old_length = next(std::min<std::size_t>(8, text.size() - offset + 1));
        std::string insert;
        for (std::size_t n = next(8); n > 0; --n)
        {
            insert += alphabet[next(sizeof(alphabet) - 1)];
        }
        text.replace(offset, old_length, insert);
        incremental.Update(text, offset, old_length, insert.size());
        heimdall::lsp::LineIndex fresh;
        fresh.Build(text);
        ASSERT_EQ(incremental.LineCount(), fresh.LineCount()) << "step " << step;
        for (std::size_t o = 0; o <= text.size(); ++o)
        {
            const auto position = fresh.ToPosition(o);
            const auto got = incremental.ToPosition(o);
            ASSERT_EQ(got.line, position.line) << "step " << step << " offset " << o;
            ASSERT_EQ(got.character, position.character) << "step " << step << " offset " << o;
        }
        if (text.size() > 120)
        {
            text.erase(0, 60);
            incremental.Build(text);
        }
    }
}

TEST(DocumentSpec, LineIndexRebindSurvivesBufferMove)
{
    std::string text = "a\nb";
    heimdall::lsp::LineIndex index;
    index.Build(text);
    const std::string moved = std::move(text);
    index.Rebind(moved);
    EXPECT_EQ(index.ToPosition(2).line, 1u);
    EXPECT_EQ(index.OffsetFromPosition({ 1, 1 }), 3u);
}
