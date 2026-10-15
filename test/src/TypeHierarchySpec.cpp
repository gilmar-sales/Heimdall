#include <Heimdall/TypeHierarchy.hpp>

#include <gtest/gtest.h>

#include <memory>
#include <string>

namespace
{
    using heimdall::kNone;
    using heimdall::SymbolId;

    struct Hierarchy
    {
        explicit Hierarchy(std::string source) :
            text(std::make_shared<const std::string>(std::move(source))),
            tree(heimdall::ParseTree::Parse(*text)), model(heimdall::Binder::Bind(tree)),
            types(heimdall::Typer::Type(model)) {}

        Hierarchy(const Hierarchy&) = delete;

        Hierarchy& operator= (const Hierarchy&) = delete;

        std::shared_ptr<const std::string> text;
        heimdall::ParseTree tree;
        heimdall::SemanticModel model;
        heimdall::TypeModel types;

        SymbolId Class(std::string_view name) const
        {
            const auto& symbols = model.Symbols();
            for (SymbolId i = 0; i < symbols.Size(); ++i)
            {
                if (symbols.kind[i] == heimdall::SymbolKind::Class &&
                    model.Names().Text(symbols.name[i]) == name)
                {
                    return i;
                }
            }

            return kNone;
        }

        std::uint32_t Token(std::string_view name, std::size_t occurrence = 0) const
        {
            const auto& tokens = tree.Tokens();
            for (std::uint32_t i = 0; i < tokens.size(); ++i)
            {
                if (tokens[i].kind == heimdall::TokenKind::Identifier &&
                    tree.Text(tokens[i]) == name && occurrence--== 0)
                {
                    return i;
                }
            }

            return kNone;
        }

        SymbolId ClassAt(std::string_view name, std::size_t occurrence = 0) const
        {
            return heimdall::TypeHierarchy::FindClassAt(model, types, Token(name, occurrence));
        }

        std::vector<SymbolId> Parents(std::string_view name) const
        {
            return heimdall::TypeHierarchy::Parents(model, Class(name));
        }

        std::vector<SymbolId> Children(std::string_view name) const
        {
            return heimdall::TypeHierarchy::Children(model, Class(name));
        }
    };

    using Ids = std::vector<SymbolId>;
} // namespace

TEST(FindClassAt, ClassNameAliasAndVariableAllNameTheClass)
{
    Hierarchy h(
        "struct Child2 { int c; };\n"
        "using Alias = Child2;\n"
        "int main() { Child2 child2; child2.c = 1; }\n");

    EXPECT_EQ(h.ClassAt("Child2", 0), h.Class("Child2"));
    EXPECT_EQ(h.ClassAt("Alias"), h.Class("Child2"));
    EXPECT_EQ(h.ClassAt("Child2", 2), h.Class("Child2"));
    EXPECT_EQ(h.ClassAt("child2", 0), h.Class("Child2"));
    EXPECT_EQ(h.ClassAt("child2", 1), h.Class("Child2"));
}

TEST(FindClassAt, NonClassVariableIsNotAClass)
{
    Hierarchy h("int* waldo;\n");

    EXPECT_EQ(h.ClassAt("waldo"), kNone);
}

TEST(FindClassAt, MethodNamesItsOwningClass)
{
    Hierarchy h(
        "struct Child2 { void method(); void method(int x); };\n"
        "int main() { Child2 child2; child2.method(5); }\n");

    EXPECT_EQ(h.ClassAt("method", 0), h.Class("Child2"));
    EXPECT_EQ(h.ClassAt("method", 2), h.Class("Child2"));
}

TEST(FindClassAt, FieldDoesNotNameASingleClass)
{
    Hierarchy h(
        "struct Child2 { int field; };\n"
        "int main() { Child2 child2; child2.field = 5; }\n");

    EXPECT_EQ(h.ClassAt("field", 0), kNone);
    EXPECT_EQ(h.ClassAt("field", 1), kNone);
}

TEST(TypeParents, SimpleInheritance)
{
    Hierarchy h(
        "struct Parent { int a; };\n"
        "struct Child1 : Parent { int b; };\n"
        "struct Child2 : Child1 { int c; };\n");

    EXPECT_EQ(h.Parents("Parent"), Ids {});
    EXPECT_EQ(h.Parents("Child1"), Ids {h.Class("Parent")});
    EXPECT_EQ(h.Parents("Child2"), Ids {h.Class("Child1")});
}

TEST(TypeParents, MultipleInheritanceKeepsDeclarationOrder)
{
    Hierarchy h(
        "struct Parent1 {};\n"
        "struct Parent2 {};\n"
        "struct Parent3 : Parent2 {};\n"
        "struct Child : Parent1, Parent3 {};\n");

    EXPECT_EQ(h.Parents("Child"), (Ids {h.Class("Parent1"), h.Class("Parent3")}));
    EXPECT_EQ(h.Parents("Parent3"), Ids {h.Class("Parent2")});
}

TEST(TypeParents, ClassTemplateHasItsBase)
{
    Hierarchy h("struct Parent {};\n"
        "template <typename T> struct Child : Parent {};\n");

    EXPECT_EQ(h.Parents("Child"), Ids {h.Class("Parent")});
}

TEST(TypeParents, DependentBaseFallsBackToPrimaryTemplate)
{
    Hierarchy h(
        "template <typename T> struct Parent {};\n"
        "template <typename T> struct Child1 : Parent<T> {};\n"
        "template <typename T> struct Child2 : Parent<T>::Type {};\n"
        "template <typename T> struct Child3 : T {};\n");

    EXPECT_EQ(h.Parents("Child1"), Ids {h.Class("Parent")});
    EXPECT_EQ(h.Parents("Child3"), Ids {});
}

TEST(TypeParents, IncompleteClassHasNoParents)
{
    Hierarchy h("class Incomplete;\n");

    EXPECT_EQ(h.Parents("Incomplete"), Ids {});
}

TEST(TypeParents, SelfAndCyclicBasesTerminate)
{
    Hierarchy h(
        "template <int N> struct S : S<N + 1> {};\n"
        "struct A : B {};\n"
        "struct B : A {};\n");

    EXPECT_EQ(h.Parents("S"), Ids {});
    EXPECT_EQ(h.Children("A"), Ids {h.Class("B")});
    EXPECT_EQ(h.Children("B"), Ids {h.Class("A")});
}

TEST(TypeChildren, DirectDerivedClassesOnly)
{
    Hierarchy h(
        "struct Parent {};\n"
        "struct Child1a : Parent {};\n"
        "struct Child1b : Parent {};\n"
        "struct Child2 : Child1a {};\n");

    EXPECT_EQ(h.Children("Parent"), (Ids {h.Class("Child1a"), h.Class("Child1b")}));
    EXPECT_EQ(h.Children("Child1a"), Ids {h.Class("Child2")});
    EXPECT_EQ(h.Children("Child2"), Ids {});
}

TEST(TypeChildren, MultipleInheritanceAppearsUnderEveryParent)
{
    Hierarchy h(
        "struct Parent1 {};\n"
        "struct Parent2 {};\n"
        "struct Parent3 : Parent2 {};\n"
        "struct Child : Parent1, Parent3 {};\n");

    EXPECT_EQ(h.Children("Parent1"), Ids {h.Class("Child")});
    EXPECT_EQ(h.Children("Parent2"), Ids {h.Class("Parent3")});
    EXPECT_EQ(h.Children("Parent3"), Ids {h.Class("Child")});
}

TEST(TypeChildren, TemplateDerivedFromPrimaryTemplate)
{
    Hierarchy h(
        "template <typename T> struct Parent {};\n"
        "template <typename T> struct Child : Parent<T> {};\n");

    EXPECT_EQ(h.Children("Parent"), Ids {h.Class("Child")});
}
