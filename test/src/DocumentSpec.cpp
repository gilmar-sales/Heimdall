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
