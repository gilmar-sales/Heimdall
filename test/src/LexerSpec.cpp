#include <gtest/gtest.h>

#include <Heimdall/Lexer.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace
{

std::string Reconstruct(std::string_view source, const std::vector<heimdall::Token>& tokens)
{
    std::string out;
    for (const auto& token : tokens)
    {
        out.append(source.substr(token.offset, token.length));
    }
    return out;
}

} // namespace

TEST(LexerSpec, RoundTripsEmptyAndOrdinarySourceByteExactly)
{
    for (const std::string_view source : { "", "int x = 42;\r\n", "//comment\n/* block */\t" })
    {
        const heimdall::Lexer lexer(source);
        const auto tokens = lexer.Lex();
        EXPECT_EQ(Reconstruct(source, tokens), source);
        std::size_t next = 0;
        for (const auto& token : tokens)
        {
            EXPECT_EQ(token.offset, next);
            EXPECT_GT(token.length, 0);
            next += token.length;
        }
        EXPECT_EQ(next, source.size());
    }
}

TEST(LexerSpec, RecognizesTriviaCommentsAndLiteralForms)
{
    constexpr std::string_view source = "  name // line\n/* block */ \"text\" 'c' R\"tag(raw)tag\" 123 0xAB";
    const heimdall::Lexer lexer(source);
    const auto tokens = lexer.Lex();
    EXPECT_EQ(Reconstruct(source, tokens), source);

    bool whitespace = false;
    bool identifier = false;
    bool line_comment = false;
    bool block_comment = false;
    bool string_literal = false;
    bool character_literal = false;
    bool raw_string = false;
    bool number = false;
    for (const auto& token : tokens)
    {
        whitespace |= token.kind == heimdall::TokenKind::Whitespace;
        identifier |= token.kind == heimdall::TokenKind::Identifier;
        line_comment |= token.kind == heimdall::TokenKind::LineComment;
        block_comment |= token.kind == heimdall::TokenKind::BlockComment;
        string_literal |= token.kind == heimdall::TokenKind::StringLiteral;
        character_literal |= token.kind == heimdall::TokenKind::CharacterLiteral;
        raw_string |= token.kind == heimdall::TokenKind::RawStringLiteral;
        number |= token.kind == heimdall::TokenKind::Number;
        EXPECT_EQ(lexer.Text(token), source.substr(token.offset, token.length));
    }
    EXPECT_TRUE(whitespace);
    EXPECT_TRUE(identifier);
    EXPECT_TRUE(line_comment);
    EXPECT_TRUE(block_comment);
    EXPECT_TRUE(string_literal);
    EXPECT_TRUE(character_literal);
    EXPECT_TRUE(raw_string);
    EXPECT_TRUE(number);
}

TEST(LexerSpec, UnterminatedStringStopsAtNewline)
{
    // While the user types `foo("`, the rest of the file must not become one
    // string literal: an unterminated quote ends at the line break (except a
    // `\` + newline continuation, which stays inside the literal).
    constexpr std::string_view source = "foo(\"bar\nint x = 1;\n";
    const heimdall::Lexer lexer(source);
    const auto tokens = lexer.Lex();
    EXPECT_EQ(Reconstruct(source, tokens), source);
    bool saw_string = false;
    bool saw_identifier_after = false;
    for (const auto& token : tokens)
    {
        if (token.kind == heimdall::TokenKind::StringLiteral)
        {
            saw_string = true;
            EXPECT_EQ(lexer.Text(token), "\"bar");
        }
        if (saw_string && token.kind == heimdall::TokenKind::Identifier &&
            lexer.Text(token) == "int")
        {
            saw_identifier_after = true;
        }
    }
    EXPECT_TRUE(saw_string);
    EXPECT_TRUE(saw_identifier_after);
}

TEST(LexerSpec, UnterminatedConstructsConsumeToEndWithoutLosingBytes)
{
    for (const std::string_view source : { "\"unterminated", "/* unterminated", "R\"x(raw" })
    {
        const heimdall::Lexer lexer(source);
        const auto tokens = lexer.Lex();
        EXPECT_EQ(Reconstruct(source, tokens), source);
        ASSERT_FALSE(tokens.empty());
        EXPECT_EQ(tokens.back().offset + tokens.back().length, source.size());
    }
}

TEST(LexerSpec, UsesMaximalMunchForMultiCharacterPunctuators)
{
    constexpr std::string_view source = "a::b->c == d && e <=> f ... g <<= 1";
    const heimdall::Lexer lexer(source);
    const auto tokens = lexer.Lex();
    std::vector<std::string_view> punctuators;
    for (const auto& token : tokens)
    {
        if (token.kind == heimdall::TokenKind::Punctuation) punctuators.push_back(lexer.Text(token));
    }
    EXPECT_EQ(punctuators, (std::vector<std::string_view> { "::", "->", "==", "&&", "<=>", "...", "<<=" }));
    EXPECT_EQ(Reconstruct(source, tokens), source);
}

TEST(LexerSpec, RoundTripsEveryBenchmarkCorpusFile)
{
    const auto corpus = std::filesystem::path(HEIMDALL_SOURCE_DIR) / "bench" / "corpus";
    ASSERT_TRUE(std::filesystem::exists(corpus));
    for (const auto& entry : std::filesystem::directory_iterator(corpus))
    {
        if (!entry.is_regular_file())
        {
            continue;
        }
        std::ifstream file(entry.path(), std::ios::binary);
        const std::string source(std::istreambuf_iterator<char>(file), {});
        const heimdall::Lexer lexer(source);
        EXPECT_EQ(Reconstruct(source, lexer.Lex()), source) << entry.path().string();
    }
}
