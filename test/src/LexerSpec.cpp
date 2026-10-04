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

namespace
{

bool SameTokens(const std::vector<heimdall::Token>& a, const std::vector<heimdall::Token>& b)
{
    if (a.size() != b.size())
    {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        if (a[i].kind != b[i].kind || a[i].offset != b[i].offset || a[i].length != b[i].length)
        {
            return false;
        }
    }
    return true;
}

} // namespace

TEST(LexerSpec, RelexMatchesFullLexForTargetedEdits)
{
    const std::string base = "#include <a>\nint main() {\n  /* c */ auto s = R\"x(raw\n)x\"; // t\n  return 0x1F + 1.5e+3;\n}\n";
    struct Case { std::size_t offset; std::size_t old_length; std::string insert; };
    const Case cases[] = {
        { 0, 0, "" }, { 0, 0, "x" }, { base.size(), 0, "int y;" }, { 0, base.size(), "" },
        { base.find("/*") + 2, 0, "*/ int q; /*" },   // changes comment boundaries
        { base.find("R\"x(") + 3, 0, "(" },           // changes raw string delimiter
        { base.find(")x\""), 1, "" },                 // breaks raw string terminator
        { base.find("main"), 4, "m" },
        { base.find("0x1F") + 1, 1, "" },             // hex number becomes decimal
        { base.find("//"), 0, "/" },
        { base.find("\n  return"), 1, " " },          // joins a line comment into code
    };
    for (const auto& c : cases)
    {
        std::string edited = base;
        edited.replace(c.offset, c.old_length, c.insert);
        auto tokens = heimdall::Lexer(base).Lex();
        heimdall::Lexer(edited).Relex(tokens, { c.offset, c.old_length, c.insert.size() });
        EXPECT_TRUE(SameTokens(tokens, heimdall::Lexer(edited).Lex()))
            << "offset " << c.offset << " old " << c.old_length << " insert '" << c.insert << "'";
    }
}

TEST(LexerSpec, RelexMatchesFullLexForRandomEditSequences)
{
    const char alphabet[] = "ab_9 \n\t\"'/*(){};.<>=R\#x+-eE";
    std::uint32_t state = 12345;
    auto next = [&](std::size_t bound) {
        state = state * 1664525u + 1013904223u;
        return static_cast<std::size_t>((state >> 8) % bound);
    };
    std::string text = "int main() { auto s = R\"(a)\"; /* c */ return 1; } // end\n";
    auto tokens = heimdall::Lexer(text).Lex();
    for (int step = 0; step < 4000; ++step)
    {
        const std::size_t offset = next(text.size() + 1);
        const std::size_t old_length = next(std::min<std::size_t>(6, text.size() - offset + 1));
        std::string insert;
        for (std::size_t n = next(6); n > 0; --n)
        {
            insert += alphabet[next(sizeof(alphabet) - 1)];
        }
        text.replace(offset, old_length, insert);
        heimdall::Lexer(text).Relex(tokens, { offset, old_length, insert.size() });
        ASSERT_TRUE(SameTokens(tokens, heimdall::Lexer(text).Lex()))
            << "step " << step << " offset " << offset << " old " << old_length << " text:\n" << text;
        if (text.size() > 400)
        {
            text.erase(0, 200);
            tokens = heimdall::Lexer(text).Lex();
        }
    }
}

TEST(LexerSpec, ClassifiesKeywordsAndPunctuatorsOnce)
{
    const std::string_view source = "template<class T> struct S { T v; auto f() const -> T; };\n"
                                    "int x = a::b >>= 1; // template\n"
                                    "\"(\" Template classy 42 R\"x(()x\"";
    const heimdall::Lexer lexer(source);
    const auto tokens = lexer.Lex();

    for (const auto& token : tokens)
    {
        const auto text = lexer.Text(token);
        const bool classifiable = token.kind == heimdall::TokenKind::Identifier ||
            token.kind == heimdall::TokenKind::Punctuation;
        // Single source of truth: the lexer's tag is exactly the table lookup.
        EXPECT_EQ(token.tok, classifiable ? heimdall::LookupTok(text) : heimdall::Tok::None) << text;
    }

    const auto find = [&](std::string_view text)
    {
        for (const auto& token : tokens)
        {
            if (lexer.Text(token) == text) return token.tok;
        }
        return heimdall::Tok::None;
    };
    EXPECT_EQ(find("template"), heimdall::Tok::KwTemplate);
    EXPECT_EQ(find("struct"), heimdall::Tok::KwStruct);
    EXPECT_EQ(find("->"), heimdall::Tok::Arrow);
    EXPECT_EQ(find("::"), heimdall::Tok::ColonColon);
    EXPECT_EQ(find(">>="), heimdall::Tok::ShrEq);
    EXPECT_EQ(find("T"), heimdall::Tok::None);
    EXPECT_EQ(find("Template"), heimdall::Tok::None);
    EXPECT_EQ(find("classy"), heimdall::Tok::None);
    EXPECT_EQ(find("\"(\""), heimdall::Tok::None);
    EXPECT_EQ(find("// template"), heimdall::Tok::None);
}

TEST(LexerSpec, RelexKeepsTokClassification)
{
    const std::string before = "int a = 1;\nfor (;;) {}\n";
    const std::string after = "int a = 1;\nwhile (;;) {}\n";
    heimdall::Lexer old_lexer(before);
    auto tokens = old_lexer.Lex();
    const heimdall::Lexer new_lexer(after);
    new_lexer.Relex(tokens, {11, 3, 5});
    const auto fresh = new_lexer.Lex();
    ASSERT_EQ(tokens.size(), fresh.size());
    for (std::size_t i = 0; i < fresh.size(); ++i)
    {
        EXPECT_EQ(tokens[i].tok, fresh[i].tok) << i;
        EXPECT_EQ(tokens[i].offset, fresh[i].offset) << i;
    }
}

TEST(LexerSpec, TokenStaysTwelveBytes)
{
    EXPECT_EQ(sizeof(heimdall::Token), 12u);
}
