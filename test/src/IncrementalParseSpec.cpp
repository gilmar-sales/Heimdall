#include <gtest/gtest.h>

#include <Heimdall/Lexer.hpp>
#include <Heimdall/ParseTree.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace
{

using heimdall::Lexer;
using heimdall::ParseReuse;
using heimdall::ParserOptions;
using heimdall::ParseTree;

struct Rng
{
    std::uint32_t state;
    std::size_t Next(std::size_t bound)
    {
        state = state * 1664525u + 1013904223u;
        return static_cast<std::size_t>((state >> 8) % bound);
    }
};

const char* const kItems[] = {
    "#include <vector>\n",
    "#define N 3\n",
    "int a = 1;\n",
    "namespace ns {\nint inner() { return 1; }\nstruct S { int x; void f(); };\n}\n",
    "template <typename T>\nT twice(T v) { return v + v; }\n",
    "struct Point\n{\n    int x;\n    int y;\n};\n",
    "int main(int argc, char** argv)\n{\n    int r = 0;\n    for (int i = 0; i < 3; ++i) { r += i; }\n    return r;\n}\n",
    "using Alias = int;\n",
    "enum class E { A, B, C };\n",
    "// comment line\n",
    "/* block */\n",
    "void broken( {\n",
    "class K : public Base { public: K() = default; int v() const { return 1; } };\n",
    "auto lam = [](int q) { return q * 2; };\n",
};

const char* const kSnippets[] = {
    "", ";", "}", "{", "(", ")", "int z;", "\n", " ", "x", "#if 0\n", "#endif\n", "//", "/*", "*/",
    "struct Q {", "};\n", "template <class U> ", "namespace m {", "return 1;", "\"s\"", "int f() { return 0; }\n",
};

std::string Describe(const ParseTree& tree)
{
    std::string out;
    for (const auto& node : tree.Nodes())
    {
        out += std::to_string(static_cast<int>(node.kind)) + ":" + std::to_string(node.first_token) + ":" +
            std::to_string(node.token_count) + ":" + std::to_string(node.parent) + ":" +
            std::to_string(node.subtree_end) + " ";
    }
    out += "|";
    for (const auto& diagnostic : tree.Diagnostics())
    {
        out += std::to_string(diagnostic.offset) + diagnostic.message + ";";
    }
    return out;
}

std::string DescribeItems(const ParseTree& tree)
{
    std::string out;
    for (const auto& item : tree.Items())
    {
        out += "[" + std::to_string(tree.Tokens()[item.first_token].offset) + "," +
            std::to_string(item.token_end - item.first_token) + (item.reusable ? "r" : "x") + "] ";
    }
    return out;
}

bool SameItems(const ParseTree& a, const ParseTree& b)
{
    if (a.Items().size() != b.Items().size())
    {
        return false;
    }
    for (std::size_t i = 0; i < a.Items().size(); ++i)
    {
        const auto& x = a.Items()[i];
        const auto& y = b.Items()[i];
        if (x.first_token != y.first_token || x.token_end != y.token_end || x.sig_count != y.sig_count ||
            x.node_begin != y.node_begin || x.node_end != y.node_end || x.diag_begin != y.diag_begin ||
            x.diag_end != y.diag_end || x.reusable != y.reusable)
        {
            return false;
        }
    }
    return true;
}

} // namespace

TEST(IncrementalParseSpec, ComposeCoversBothEdits)
{
    using Edit = Lexer::TextEdit;
    // Disjoint, overlapping and nested edits all produce a hull whose replacement
    // reproduces the doubly edited text.
    Rng rng{99};
    for (int round = 0; round < 3000; ++round)
    {
        std::string base(40, 'a');
        for (auto& c : base)
        {
            c = static_cast<char>('a' + rng.Next(20));
        }
        const Edit first{rng.Next(base.size() + 1), 0, rng.Next(6)};
        const std::size_t first_old = rng.Next(std::min<std::size_t>(5, base.size() - first.offset + 1));
        const Edit first_edit{first.offset, first_old, first.new_length};
        std::string middle = base;
        middle.replace(first_edit.offset, first_edit.old_length, std::string(first_edit.new_length, '#'));
        const std::size_t second_offset = rng.Next(middle.size() + 1);
        const Edit second_edit{second_offset, rng.Next(std::min<std::size_t>(5, middle.size() - second_offset + 1)),
            rng.Next(6)};
        std::string last = middle;
        last.replace(second_edit.offset, second_edit.old_length, std::string(second_edit.new_length, '@'));

        const Edit hull = Lexer::Compose(first_edit, second_edit);
        ASSERT_LE(hull.offset + hull.old_length, base.size());
        ASSERT_LE(hull.offset + hull.new_length, last.size());
        // Everything outside the hull is untouched.
        EXPECT_EQ(base.substr(0, hull.offset), last.substr(0, hull.offset));
        EXPECT_EQ(base.substr(hull.offset + hull.old_length), last.substr(hull.offset + hull.new_length));
    }
}

TEST(IncrementalParseSpec, ReusedParseEqualsFullParseForRandomEdits)
{
    Rng rng{2024};
    ParserOptions options;
    std::size_t reused_total = 0;
    for (int round = 0; round < 60; ++round)
    {
        std::string text;
        for (std::size_t n = 8 + rng.Next(10); n > 0; --n)
        {
            text += kItems[rng.Next(std::size(kItems))];
        }

        auto previous = std::make_shared<ParseTree>(ParseTree::Parse(text, options));
        auto previous_text = std::make_shared<std::string>(text);
        previous->HoldSource(previous_text);
        for (int step = 0; step < 40; ++step)
        {
            const std::size_t offset = rng.Next(text.size() + 1);
            const std::size_t old_length = rng.Next(std::min<std::size_t>(12, text.size() - offset + 1));
            const std::string insert = kSnippets[rng.Next(std::size(kSnippets))];
            std::string edited = text;
            edited.replace(offset, old_length, insert);

            const ParseReuse reuse{previous.get(), offset, old_length, insert.size()};
            auto reused = std::make_shared<ParseTree>(ParseTree::Parse(edited, options, {}, nullptr, &reuse));
            const ParseTree full = ParseTree::Parse(edited, options);
            ASSERT_EQ(Describe(*reused), Describe(full))
                << "round " << round << " step " << step << " offset " << offset << " old " << old_length
                << " insert '" << insert << "'\n--- old text ---\n" << text << "\n--- text ---\n" << edited
                << "\nOLD ITEMS: " << DescribeItems(*previous) << "\nFULL ITEMS: " << DescribeItems(full);
            ASSERT_TRUE(SameItems(*reused, full)) << "items differ at round " << round << " step " << step;
            reused_total += reused->ReusedItems();

            auto owned = std::make_shared<std::string>(edited);
            reused->HoldSource(owned);
            // Re-point the view at the owned copy so `previous` outlives this scope's string.
            previous = std::make_shared<ParseTree>(ParseTree::Parse(*owned, options, {}, nullptr, &reuse));
            previous->HoldSource(owned);
            text = edited;
            if (text.size() > 4000)
            {
                break;
            }
        }
    }
    // The point of the feature: most items survive an edit.
    EXPECT_GT(reused_total, 1000u);
}

TEST(IncrementalParseSpec, EditInsideOneFunctionReparsesOnlyThatItem)
{
    std::string text;
    for (int n = 0; n < 50; ++n)
    {
        text += "int f" + std::to_string(n) + "(int a) { return a + " + std::to_string(n) + "; }\n";
    }
    ParserOptions options;
    auto owned = std::make_shared<std::string>(text);
    ParseTree previous = ParseTree::Parse(*owned, options);
    previous.HoldSource(owned);

    const std::size_t offset = text.find("a + 25") + 4;
    std::string edited = text;
    edited.replace(offset, 2, "100");
    const ParseReuse reuse{&previous, offset, 2, 3};
    const ParseTree incremental = ParseTree::Parse(edited, options, {}, nullptr, &reuse);
    EXPECT_EQ(Describe(incremental), Describe(ParseTree::Parse(edited, options)));
    // The edited item and its predecessor (parsers may peek one token ahead) are re-parsed.
    EXPECT_GE(incremental.ReusedItems(), 47u);
    EXPECT_LE(incremental.ReusedItems(), 49u);
}

TEST(IncrementalParseSpec, ReusedParseEqualsFullParseForWellFormedEdits)
{
    // Edits that swap, add and drop whole items and tweak identifiers inside
    // bodies: the realistic typing pattern, where most items must be reused.
    Rng rng{31337};
    ParserOptions options;
    std::size_t reused_total = 0;
    std::size_t item_total = 0;
    for (int round = 0; round < 40; ++round)
    {
        std::string text;
        for (std::size_t n = 20 + rng.Next(20); n > 0; --n)
        {
            text += kItems[rng.Next(std::size(kItems) - 4)]; // skip the deliberately broken ones
        }
        auto owned = std::make_shared<std::string>(text);
        auto previous = std::make_shared<ParseTree>(ParseTree::Parse(*owned, options));
        previous->HoldSource(owned);
        for (int step = 0; step < 30; ++step)
        {
            std::string edited = *owned;
            std::size_t offset = 0;
            std::size_t old_length = 0;
            std::string insert;
            switch (rng.Next(3))
            {
            case 0:
                offset = edited.find('\n', rng.Next(edited.size())) ;
                offset = offset == std::string::npos ? edited.size() : offset + 1;
                insert = kItems[rng.Next(std::size(kItems) - 4)];
                break;
            case 1:
                offset = rng.Next(edited.size());
                old_length = std::min<std::size_t>(1 + rng.Next(8), edited.size() - offset);
                break;
            default:
                offset = rng.Next(edited.size());
                insert = "q";
                break;
            }
            edited.replace(offset, old_length, insert);
            const ParseReuse reuse{previous.get(), offset, old_length, insert.size()};
            auto next_owned = std::make_shared<std::string>(edited);
            auto reused = std::make_shared<ParseTree>(ParseTree::Parse(*next_owned, options, {}, nullptr, &reuse));
            reused->HoldSource(next_owned);
            const ParseTree full = ParseTree::Parse(edited, options);
            ASSERT_EQ(Describe(*reused), Describe(full))
                << "round " << round << " step " << step << " offset " << offset << " old " << old_length
                << " insert '" << insert << "'\n--- old text ---\n" << *owned << "\n--- text ---\n" << edited;
            ASSERT_TRUE(SameItems(*reused, full));
            reused_total += reused->ReusedItems();
            item_total += static_cast<std::size_t>(std::count_if(full.Items().begin(), full.Items().end(),
                [](const heimdall::TopLevelItem& item) { return item.reusable; }));
            owned = next_owned;
            previous = reused;
            if (owned->size() > 6000)
            {
                break;
            }
        }
    }
    EXPECT_GT(reused_total * 100, item_total * 80) << reused_total << " of " << item_total;
}
