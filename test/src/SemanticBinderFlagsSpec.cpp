#include <Heimdall/SemanticModel.hpp>

#include <gtest/gtest.h>

#include <memory>
#include <string>

namespace
{

    struct Bound
    {
        explicit Bound(std::string source) :
            text(std::make_shared<const std::string>(std::move(source))),
            tree(heimdall::ParseTree::Parse(*text)), model(heimdall::Binder::Bind(tree))
        {
        }

        Bound(const Bound&) = delete;

        Bound& operator=(const Bound&) = delete;

        std::shared_ptr<const std::string> text;
        heimdall::ParseTree                tree;
        heimdall::SemanticModel            model;

        heimdall::SymbolId Find(std::string_view name) const
        {
            const auto& symbols = model.Symbols();
            for (heimdall::SymbolId i = 0; i < symbols.Size(); ++i)
            {
                if (model.Names().Text(symbols.name[i]) == name)
                {
                    return i;
                }
            }

            return heimdall::kNone;
        }

        bool Has(std::string_view name, std::uint32_t flag) const
        {
            const auto symbol = Find(name);
            return symbol != heimdall::kNone && (model.Symbols().flags[symbol] & flag) != 0;
        }

        // Index of the first token spelled `text`.
        std::uint32_t Token(std::string_view spelled) const
        {
            const auto& tokens = tree.Tokens();
            for (std::uint32_t i = 0; i < tokens.size(); ++i)
            {
                if (tree.Text(tokens[i]) == spelled)
                {
                    return i;
                }
            }

            return heimdall::kNone;
        }
    };

} // namespace

TEST(BinderPointers, DeclaratorsBuiltFromStarsArePointers)
{
    Bound bound("int* a; int** b; const char* c; int d; int* e[2]; int& f = d;\n");
    EXPECT_TRUE(bound.Has("a", heimdall::SymbolFlag::Pointer));
    EXPECT_TRUE(bound.Has("b", heimdall::SymbolFlag::Pointer));
    EXPECT_TRUE(bound.Has("c", heimdall::SymbolFlag::Pointer));
    EXPECT_FALSE(bound.Has("d", heimdall::SymbolFlag::Pointer));
    EXPECT_FALSE(bound.Has("e", heimdall::SymbolFlag::Pointer));
    EXPECT_FALSE(bound.Has("f", heimdall::SymbolFlag::Pointer));
}

TEST(BinderPointers, ParametersAreFlaggedToo)
{
    Bound bound("void f(int* p, int q, int*& r) {}\n");
    EXPECT_TRUE(bound.Has("p", heimdall::SymbolFlag::Pointer));
    EXPECT_FALSE(bound.Has("q", heimdall::SymbolFlag::Pointer));
    EXPECT_FALSE(bound.Has("r", heimdall::SymbolFlag::Pointer));
}

TEST(BinderPointers, OnlyTheStarredDeclaratorOfAListIsAPointer)
{
    Bound bound("void f() { int *a = 0, b = 0, *c = 0; }\n");
    EXPECT_TRUE(bound.Has("a", heimdall::SymbolFlag::Pointer));
    EXPECT_NE(bound.Find("b"), heimdall::kNone);
    EXPECT_FALSE(bound.Has("b", heimdall::SymbolFlag::Pointer));
    // The grammar does not give the third declarator a Declarator node: it is
    // bound by name but not known to be a pointer.
    EXPECT_NE(bound.Find("c"), heimdall::kNone);
}

TEST(BinderPointers, TypedefAndAutoPointersAreNotKnown)
{
    Bound bound("typedef int* Handle; void f(Handle h, auto a) {}\n");
    EXPECT_FALSE(bound.Has("h", heimdall::SymbolFlag::Pointer));
    EXPECT_FALSE(bound.Has("a", heimdall::SymbolFlag::Pointer));
}

TEST(BinderPointers, FunctionsRememberTheirPointerReturnType)
{
    Bound bound("int* a() { return 0; }\n"
                "const char* b();\n"
                "auto c() -> char* { return 0; }\n"
                "int d() { return 0; }\n"
                "int& e();\n"
                "auto f() -> int& { return g; }\n"
                "std::vector<int*> h();\n");
    EXPECT_TRUE(bound.Has("a", heimdall::SymbolFlag::ReturnsPointer));
    EXPECT_TRUE(bound.Has("b", heimdall::SymbolFlag::ReturnsPointer));
    EXPECT_TRUE(bound.Has("c", heimdall::SymbolFlag::ReturnsPointer));
    EXPECT_FALSE(bound.Has("d", heimdall::SymbolFlag::ReturnsPointer));
    EXPECT_FALSE(bound.Has("e", heimdall::SymbolFlag::ReturnsPointer));
    EXPECT_FALSE(bound.Has("f", heimdall::SymbolFlag::ReturnsPointer));
    EXPECT_FALSE(bound.Has("h", heimdall::SymbolFlag::ReturnsPointer));
}

TEST(BinderCode, InactiveBranchesAndDirectivesAreNotCode)
{
    Bound bound("#define M 1\n"
                "int live;\n"
                "#if 0\n"
                "int dead;\n"
                "#endif\n");
    EXPECT_TRUE(bound.model.IsCode(bound.Token("live")));
    EXPECT_FALSE(bound.model.IsCode(bound.Token("dead")));
    EXPECT_FALSE(bound.model.IsCode(bound.Token("M")));
    EXPECT_NE(bound.Find("live"), heimdall::kNone);
    EXPECT_EQ(bound.Find("dead"), heimdall::kNone);
}

TEST(BinderCode, SignificantTokensAreCodeOnlyAndAscending)
{
    Bound       bound("// comment\n"
                      "int a; /* block */ int b;\n"
                      "#if 0\n"
                      "int c;\n"
                      "#endif\n");
    const auto& significant = bound.model.Significant();
    ASSERT_FALSE(significant.empty());
    for (std::size_t i = 0; i < significant.size(); ++i)
    {
        const auto& token = bound.tree.Tokens()[significant[i]];
        EXPECT_NE(token.kind, heimdall::TokenKind::Whitespace);
        EXPECT_NE(token.kind, heimdall::TokenKind::LineComment);
        EXPECT_NE(token.kind, heimdall::TokenKind::BlockComment);
        EXPECT_TRUE(bound.model.IsCode(significant[i]));
        if (i > 0)
        {
            EXPECT_LT(significant[i - 1], significant[i]);
        }
    }

    for (const auto token : significant)
    {
        EXPECT_NE(bound.tree.Text(bound.tree.Tokens()[token]), "c");
    }
}

TEST(BinderNodes, ChildrenOfListsEveryNodeUnderItsParent)
{
    Bound       bound("namespace n { int a; int b; }\nint c;\n");
    const auto& nodes = bound.tree.Nodes();
    std::size_t total = 0;
    for (std::uint32_t node = 0; node < nodes.size(); ++node)
    {
        for (const auto child : bound.model.ChildrenOf(node))
        {
            EXPECT_EQ(nodes[child].parent, node);
            ++total;
        }
    }

    // Every node except the root has exactly one parent.
    EXPECT_EQ(total, nodes.size() - 1);
    EXPECT_TRUE(bound.model.ChildrenOf(static_cast<std::uint32_t>(nodes.size())).empty());
}

TEST(BinderNodes, ScopeOfNodeFollowsTheEnclosingDefinition)
{
    Bound       bound("namespace n { struct S { int m; }; }\nint g;\n");
    const auto& nodes  = bound.tree.Nodes();
    const auto& scopes = bound.model.Scopes();
    const auto  m      = bound.Find("m");
    const auto  g      = bound.Find("g");
    ASSERT_NE(m, heimdall::kNone);
    ASSERT_NE(g, heimdall::kNone);
    const auto m_scope = bound.model.ScopeOfNode(bound.model.Symbols().decl_node[m]);
    EXPECT_EQ(scopes.kind[m_scope], heimdall::ScopeKind::Class);
    EXPECT_EQ(m_scope, bound.model.Symbols().scope[m]);
    EXPECT_EQ(bound.model.ScopeOfNode(bound.model.Symbols().decl_node[g]),
              heimdall::SemanticModel::TranslationUnitScope);
    EXPECT_EQ(bound.model.ScopeOfNode(0), heimdall::SemanticModel::TranslationUnitScope);
    EXPECT_EQ(bound.model.ScopeOfNode(static_cast<std::uint32_t>(nodes.size() + 5)),
              heimdall::SemanticModel::TranslationUnitScope);
}
