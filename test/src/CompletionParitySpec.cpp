#include <gtest/gtest.h>

#include <Heimdall/Completion.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

// Behaviors ported from clangd's CodeCompleteTests.cpp that map onto Heimdall's
// lexical/ParseTree completion model (labels, kinds, details, documentation and
// scoping). `^` marks the cursor and is removed before completing.

namespace
{
    using Items = std::vector<heimdall::CompletionItem>;

    struct Annotated
    {
        std::string code;
        std::size_t point = 0;
    };

    Annotated Annotate(std::string_view text)
    {
        Annotated result;
        const std::size_t caret = text.find('^');
        EXPECT_NE(caret, std::string_view::npos) << text;
        result.code = std::string(text.substr(0, caret));
        result.point = caret;
        result.code += text.substr(caret + 1);
        return result;
    }

    Items CompleteAt(std::string_view text, const heimdall::ScopeIndex* external = nullptr)
    {
        const Annotated annotated = Annotate(text);
        const heimdall::ParserOptions options;
        return heimdall::CompletionEngine::Complete(annotated.code, options, annotated.point,
                                                    external);
    }

    const heimdall::CompletionItem* Find(const Items& items, std::string_view label)
    {
        for (const auto& item : items)
        {
            if (item.label == label)
            {
                return &item;
            }
        }

        return nullptr;
    }

    bool Has(const Items& items, std::string_view label)
    {
        return Find(items, label) != nullptr;
    }

    bool Has(const Items& items, std::string_view label, heimdall::CompletionKind kind)
    {
        const auto* item = Find(items, label);
        return item != nullptr && item->kind == kind;
    }

    std::size_t Count(const Items& items, std::string_view label)
    {
        std::size_t count = 0;
        for (const auto& item : items)
        {
            if (item.label == label)
            {
                ++count;
            }
        }

        return count;
    }

    std::vector<std::string> Labels(const Items& items)
    {
        std::vector<std::string> labels;
        labels.reserve(items.size());
        for (const auto& item : items)
        {
            labels.push_back(item.label);
        }

        return labels;
    }

} // namespace

TEST(CompletionParitySpec, MemberFilterKeepsOnlyMatchingMembers)
{
    const auto items = CompleteAt(
        "struct S { int FooBar; int FooBaz; int Qux; };\n"
        "int main() { S().Foo^ }\n");

    EXPECT_TRUE(Has(items, "FooBar"));
    EXPECT_TRUE(Has(items, "FooBaz"));
    EXPECT_FALSE(Has(items, "Qux"));
}

TEST(CompletionParitySpec, FuzzyFilterMatchesSubsequenceAcrossWordBoundaries)
{
    const auto items = CompleteAt(
        "struct S { int FooBar; int FooBaz; int Qux; };\n"
        "int main() { S().Foba^ }\n");

    EXPECT_TRUE(Has(items, "FooBar"));
    EXPECT_TRUE(Has(items, "FooBaz"));
    EXPECT_FALSE(Has(items, "Qux"));
}

TEST(CompletionParitySpec, FilterIgnoresCaseOfTheTypedPrefix)
{
    const auto items = CompleteAt(
        "namespace fake { int BigBang, Babble, Box; }\n"
        "int main() { fake::ba^ }\n");

    EXPECT_TRUE(Has(items, "Babble"));
    EXPECT_TRUE(Has(items, "BigBang"));
    EXPECT_FALSE(Has(items, "Box"));
}

TEST(CompletionParitySpec, FuzzyMatchesRankBelowPrefixMatches)
{
    const auto items = CompleteAt(
        "namespace fake { int BigBang, Babble, Box; }\n"
        "int main() { fake::ba^ }\n");

    const auto labels = Labels(items);
    ASSERT_EQ(labels.size(), 2u);
    EXPECT_EQ(labels[0], "Babble");
    EXPECT_EQ(labels[1], "BigBang");
}

TEST(CompletionParitySpec, MacrosRequireAPrefixMatch)
{
    constexpr std::string_view body =
        "#define MotorCar\n"
        "int Car;\n";

    const auto car = CompleteAt(std::string(body) + "int main() { C^ }");
    const auto motor = CompleteAt(std::string(body) + "int main() { M^ }");

    EXPECT_TRUE(Has(car, "Car"));
    EXPECT_FALSE(Has(car, "MotorCar"));
    EXPECT_TRUE(Has(motor, "MotorCar"));
}

TEST(CompletionParitySpec, AfterDotOffersOnlyMembers)
{
    const auto items = CompleteAt(
        "int global_var;\n"
        "int global_func();\n"
        "#define MACRO X\n"
        "struct GlobalClass {};\n"
        "struct ClassWithMembers {\n"
        "  /// doc for method.\n"
        "  int method();\n"
        "  int field;\n"
        "private:\n"
        "  int private_field;\n"
        "};\n"
        "int test() {\n"
        "  struct LocalClass {};\n"
        "  /// doc for local_var.\n"
        "  int local_var;\n"
        "  ClassWithMembers().^\n"
        "}\n");

    EXPECT_TRUE(Has(items, "method"));
    EXPECT_TRUE(Has(items, "field"));
    EXPECT_FALSE(Has(items, "ClassWithMembers"));
    EXPECT_FALSE(Has(items, "private_field"));
    EXPECT_FALSE(Has(items, "global_var"));
    EXPECT_FALSE(Has(items, "global_func"));
    EXPECT_FALSE(Has(items, "GlobalClass"));
    EXPECT_FALSE(Has(items, "MACRO"));
    EXPECT_FALSE(Has(items, "LocalClass"));
    EXPECT_FALSE(Has(items, "return"));
    const auto* method = Find(items, "method");
    ASSERT_NE(method, nullptr);
    EXPECT_EQ(method->documentation, "doc for method.");
}

TEST(CompletionParitySpec, GlobalScopeOffersVisibleNamesButNotMembers)
{
    const auto items = CompleteAt(
        "int global_var;\n"
        "int global_func();\n"
        "#define MACRO X\n"
        "struct GlobalClass {};\n"
        "struct ClassWithMembers {\n"
        "  /// doc for method.\n"
        "  int method();\n"
        "  int field;\n"
        "};\n"
        "int test() {\n"
        "  struct LocalClass {};\n"
        "  /// doc for local_var.\n"
        "  int local_var;\n"
        "  ^\n"
        "}\n");

    EXPECT_FALSE(Has(items, "method"));
    EXPECT_FALSE(Has(items, "field"));
    EXPECT_TRUE(Has(items, "global_var"));
    EXPECT_TRUE(Has(items, "global_func"));
    EXPECT_TRUE(Has(items, "GlobalClass"));
    EXPECT_TRUE(Has(items, "MACRO"));
    EXPECT_TRUE(Has(items, "local_var"));
    EXPECT_TRUE(Has(items, "LocalClass"));
    const auto* local = Find(items, "local_var");
    ASSERT_NE(local, nullptr);
    EXPECT_EQ(local->documentation, "doc for local_var.");
}

TEST(CompletionParitySpec, MemberAccessInsideTheClassSeesAllAccessLevels)
{
    const auto items = CompleteAt(
        "class Foo {\n"
        "  public: void pub();\n"
        "  protected: void prot();\n"
        "  private: void priv();\n"
        "};\n"
        "void Foo::pub() { this->^ }\n");

    EXPECT_TRUE(Has(items, "pub"));
    EXPECT_TRUE(Has(items, "prot"));
    EXPECT_TRUE(Has(items, "priv"));
}

TEST(CompletionParitySpec, MemberAccessOutsideTheClassHidesNonPublicMembers)
{
    const auto items = CompleteAt(
        "class Foo {\n"
        "  public: void pub();\n"
        "  protected: void prot();\n"
        "  private: void priv();\n"
        "};\n"
        "void test() {\n"
        "  Foo F;\n"
        "  F.^\n"
        "}\n");

    EXPECT_TRUE(Has(items, "pub"));
    EXPECT_FALSE(Has(items, "prot"));
    EXPECT_FALSE(Has(items, "priv"));
}

TEST(CompletionParitySpec, ClassMembersArePrivateByDefault)
{
    const auto items = CompleteAt(
        "class Foo { int hidden; public: int shown; };\n"
        "struct Bar { int open; };\n"
        "void test(Foo f, Bar b) { f.^ }\n");

    EXPECT_TRUE(Has(items, "shown"));
    EXPECT_FALSE(Has(items, "hidden"));
}

TEST(CompletionParitySpec, PrivateMembersOfBasesAreNotInheritedForAccess)
{
    const auto items = CompleteAt(
        "class Base { public: int pub; protected: int prot; private: int priv; };\n"
        "class Derived : public Base {\n"
        "  void f() { this->^ }\n"
        "};\n");

    EXPECT_TRUE(Has(items, "pub"));
    EXPECT_TRUE(Has(items, "prot"));
    EXPECT_FALSE(Has(items, "priv"));
}

TEST(CompletionParitySpec, DerivedMembersHideSameNamedBaseMembers)
{
    const auto items = CompleteAt(
        "struct deque_base {\n"
        "  float size();\n"
        "  double size() const;\n"
        "};\n"
        "struct deque : deque_base {\n"
        "  int size() const;\n"
        "};\n"
        "auto x = deque().^\n");

    ASSERT_EQ(Count(items, "size"), 1u);
    const auto* size = Find(items, "size");
    EXPECT_NE(size->detail.find("int"), std::string::npos) << size->detail;
    EXPECT_EQ(size->detail.find("float"), std::string::npos) << size->detail;
}

TEST(CompletionParitySpec, InjectedClassNameIsNotAMember)
{
    EXPECT_FALSE(Has(CompleteAt("struct X{}; void foo(){ X().^ }"), "X"));
    EXPECT_FALSE(Has(CompleteAt("struct X{ void foo(){ this->^ } };"), "X"));
    EXPECT_TRUE(Has(CompleteAt("struct X{ void foo(){ ^ } };"), "X"));
    EXPECT_TRUE(Has(CompleteAt("struct Y{}; struct X:Y{ void foo(){ ^ } };"), "Y"));
}

TEST(CompletionParitySpec, ClassNameIsVisibleInsideItsOwnMembers)
{
    const auto items = CompleteAt("struct X { void f() { X^ }};");

    EXPECT_TRUE(Has(items, "X", heimdall::CompletionKind::Type));
}

TEST(CompletionParitySpec, KindsOfDeclarations)
{
    const auto items = CompleteAt(
        "int variable;\n"
        "struct Struct {};\n"
        "int function();\n"
        "#define MACRO 10\n"
        "int X = ^\n");

    EXPECT_TRUE(Has(items, "function", heimdall::CompletionKind::Function));
    EXPECT_TRUE(Has(items, "variable", heimdall::CompletionKind::Variable));
    EXPECT_TRUE(Has(items, "int", heimdall::CompletionKind::Type));
    EXPECT_TRUE(Has(items, "Struct", heimdall::CompletionKind::Type));
    EXPECT_TRUE(Has(items, "MACRO", heimdall::CompletionKind::Macro));
}

TEST(CompletionParitySpec, NamespaceKeywordCompletes)
{
    EXPECT_TRUE(Has(CompleteAt("nam^"), "namespace", heimdall::CompletionKind::Keyword));
}

TEST(CompletionParitySpec, AnonymousUnionMembersAreFields)
{
    const auto items = CompleteAt(
        "struct X {\n"
        "  union {\n"
        "    void *a;\n"
        "  };\n"
        "};\n"
        "auto u = X().^\n");

    EXPECT_TRUE(Has(items, "a", heimdall::CompletionKind::Variable));
}

TEST(CompletionParitySpec, TemplatesHaveTheirEntityKind)
{
    const auto items = CompleteAt(
        "template <class T> struct complete_class {};\n"
        "template <class T> void complete_function();\n"
        "template <class T> using complete_type_alias = int;\n"
        "template <class T> int complete_variable = 10;\n"
        "auto x = complete_^\n");

    EXPECT_TRUE(Has(items, "complete_class", heimdall::CompletionKind::Type));
    EXPECT_TRUE(Has(items, "complete_function", heimdall::CompletionKind::Function));
    EXPECT_TRUE(Has(items, "complete_type_alias", heimdall::CompletionKind::Type));
    EXPECT_TRUE(Has(items, "complete_variable", heimdall::CompletionKind::Variable));
}

TEST(CompletionParitySpec, UnscopedEnumeratorsAreVisibleUnqualified)
{
    const auto items = CompleteAt(
        "enum Color {\n"
        "  Red\n"
        "};\n"
        "Color u = ^\n");

    EXPECT_TRUE(Has(items, "Red"));
}

TEST(CompletionParitySpec, ScopedEnumeratorsNeedTheirQualifier)
{
    const auto items = CompleteAt(
        "enum class Color { Red };\n"
        "Color u = R^\n");

    EXPECT_FALSE(Has(items, "Red"));
}

TEST(CompletionParitySpec, NoDuplicates)
{
    const auto items = CompleteAt(
        "class Adapter {\n"
        "};\n"
        "void f() {\n"
        "  Adapter^\n"
        "}\n");

    EXPECT_EQ(Count(items, "Adapter"), 1u);
}

TEST(CompletionParitySpec, GlobalQualifierListsGlobalsAndFunctions)
{
    const auto items = CompleteAt(
        "struct XYZ {};\n"
        "void f() { ::^ }\n");

    EXPECT_TRUE(Has(items, "XYZ", heimdall::CompletionKind::Type));
    EXPECT_TRUE(Has(items, "f", heimdall::CompletionKind::Function));
}

TEST(CompletionParitySpec, FullyQualifiedNamespaceMembers)
{
    const auto items = CompleteAt(
        "namespace ns { void bar(); struct XYZ {}; }\n"
        "void f() { ::ns::^ }\n");

    EXPECT_TRUE(Has(items, "XYZ", heimdall::CompletionKind::Type));
    EXPECT_TRUE(Has(items, "bar", heimdall::CompletionKind::Function));
}

TEST(CompletionParitySpec, InlineNamespaceMembersAreVisibleThroughTheParent)
{
    const auto items = CompleteAt(
        "namespace a { inline namespace b { struct Foo {}; } }\n"
        "void f() { a::Fo^ }\n");

    EXPECT_TRUE(Has(items, "Foo", heimdall::CompletionKind::Type));
}

TEST(CompletionParitySpec, UsingDirectiveInNamespaceExposesMembersThroughIt)
{
    const auto items = CompleteAt(
        "namespace ns1 { int fromOne; }\n"
        "namespace ns2 { int fromTwo; }\n"
        "namespace foo {\n"
        "  using namespace ns1;\n"
        "  int own;\n"
        "}\n"
        "void f() { foo::^ }\n");

    EXPECT_TRUE(Has(items, "own"));
    EXPECT_TRUE(Has(items, "fromOne"));
    EXPECT_FALSE(Has(items, "fromTwo"));
}

TEST(CompletionParitySpec, UsingDirectiveExposesMembersUnqualified)
{
    const auto items = CompleteAt(
        "namespace lib { int vector_size; }\n"
        "using namespace lib;\n"
        "void f() { vec^ }\n");

    EXPECT_TRUE(Has(items, "vector_size"));
}

TEST(CompletionParitySpec, UsingDeclarationExposesTheNamedMember)
{
    const auto items = CompleteAt(
        "void foo(int);\n"
        "namespace std2 {\n"
        "  using ::foo;\n"
        "}\n"
        "void bar() { std2::^; }\n");

    EXPECT_TRUE(Has(items, "foo"));
}

TEST(CompletionParitySpec, UsingDeclarationCompletesNamespaceMembers)
{
    const auto items = CompleteAt(
        "namespace ns {\n"
        "  int func(int a, int b);\n"
        "}\n"
        "using ns::^;\n");

    ASSERT_EQ(items.size(), 1u);
    EXPECT_EQ(items[0].label, "func");
    EXPECT_EQ(items[0].kind, heimdall::CompletionKind::Function);
}

TEST(CompletionParitySpec, DocumentationFromPlainDoxygenAndBlockComments)
{
    const auto items = CompleteAt(
        "// Non-doxygen comment.\n"
        "int foo();\n"
        "/// Doxygen comment.\n"
        "/// \\param int a\n"
        "int bar(int a);\n"
        "/* Multi-line\n"
        "   block comment\n"
        "*/\n"
        "int baz();\n"
        "int x = ^\n");

    const auto* foo = Find(items, "foo");
    const auto* bar = Find(items, "bar");
    const auto* baz = Find(items, "baz");
    ASSERT_NE(foo, nullptr);
    ASSERT_NE(bar, nullptr);
    ASSERT_NE(baz, nullptr);
    EXPECT_EQ(foo->documentation, "Non-doxygen comment.");
    EXPECT_NE(bar->documentation.find("Doxygen comment."), std::string::npos);
    EXPECT_NE(bar->documentation.find("Parameters"), std::string::npos);
    EXPECT_NE(baz->documentation.find("Multi-line"), std::string::npos);
    EXPECT_NE(baz->documentation.find("block comment"), std::string::npos);
}

TEST(CompletionParitySpec, RulerAndDetachedCommentsAreNotDocumentation)
{
    const auto items = CompleteAt(
        "// ------------------\n"
        "int comments_foo();\n"
        "// A comment and a decl are separated by newlines.\n"
        "\n"
        "int comments_bar();\n"
        "// this comment should be in the results.\n"
        "int comments_baz();\n"
        "int a = comments^;\n");

    const auto* foo = Find(items, "comments_foo");
    const auto* bar = Find(items, "comments_bar");
    const auto* baz = Find(items, "comments_baz");
    ASSERT_NE(foo, nullptr);
    ASSERT_NE(bar, nullptr);
    ASSERT_NE(baz, nullptr);
    EXPECT_EQ(foo->documentation, "");
    EXPECT_EQ(bar->documentation, "");
    EXPECT_EQ(baz->documentation, "this comment should be in the results.");
}

TEST(CompletionParitySpec, UnknownQualifierOffersNothing)
{
    const auto items = CompleteAt(
        "namespace clang { int v; }\n"
        "void f() { clangd::^\n");

    EXPECT_TRUE(items.empty());
}

TEST(CompletionParitySpec, NamespaceLabelHasNoTrailingColons)
{
    const auto items = CompleteAt(
        "namespace clang { }\n"
        "void f() {\n"
        "  clan^\n"
        "}\n");

    EXPECT_TRUE(Has(items, "clang", heimdall::CompletionKind::Namespace));
    EXPECT_FALSE(Has(items, "clang::"));
}

TEST(CompletionParitySpec, QualifiedCompletionInsideMacroArguments)
{
    const auto items = CompleteAt(
        "void f(const char *, int x);\n"
        "#define F(x) f(#x, x)\n"
        "namespace ns {\n"
        "int X;\n"
        "int Y;\n"
        "}  // namespace ns\n"
        "int f(int input_num) {\n"
        "  F(ns::^)\n"
        "}\n");

    EXPECT_EQ(Labels(items), (std::vector<std::string> {"X", "Y"}));
}

TEST(CompletionParitySpec, UnqualifiedCompletionInsideMacroArgumentsInNamespace)
{
    const auto items = CompleteAt(
        "void f(const char *, int x);\n"
        "#define F(x) f(#x, x)\n"
        "namespace ns {\n"
        "int X;\n"
        "int f(int input_num) {\n"
        "  F(^)\n"
        "}\n"
        "}  // namespace ns\n");

    EXPECT_TRUE(Has(items, "X"));
}

TEST(CompletionParitySpec, NoCompletionInsideExcludedPreprocessorBranch)
{
    const auto items = CompleteAt(
        "int bar(int param_in_bar) {\n"
        "}\n"
        "int foo(int param_in_foo) {\n"
        "#if 0\n"
        "  par^\n"
        "#endif\n"
        "}\n");

    EXPECT_TRUE(items.empty());
}

TEST(CompletionParitySpec, DirectiveCompletesMidWord)
{
    const auto items = CompleteAt(
        "#ifnd^ef FOO_H_\n"
        "#define BAR_H_\n"
        "int foo() {}\n"
        "#endif\n");

    EXPECT_EQ(Labels(items), (std::vector<std::string> {"ifndef"}));
}

TEST(CompletionParitySpec, DetailKeepsDefaultArguments)
{
    constexpr std::string_view context =
        "int X(int A = 0);\n"
        "int Y(int A, int B = 0);\n";

    const auto x = CompleteAt(std::string(context) + "int y = X^");
    const auto y = CompleteAt(std::string(context) + "int y = Y^");

    const auto* xItem = Find(x, "X");
    const auto* yItem = Find(y, "Y");
    ASSERT_NE(xItem, nullptr);
    ASSERT_NE(yItem, nullptr);
    EXPECT_NE(xItem->detail.find("(int A = 0)"), std::string::npos) << xItem->detail;
    EXPECT_NE(yItem->detail.find("(int A, int B = 0)"), std::string::npos) << yItem->detail;
}

TEST(CompletionParitySpec, DetailUsesTheLastNamedRedeclaration)
{
    const auto items = CompleteAt(
        "void sun(int);\n"
        "void sun(int night);\n"
        "void sun(int day);\n"
        "void sun(int);\n"
        "void position() {\n"
        "  sun^;\n"
        "}\n");

    const auto* sun = Find(items, "sun");
    ASSERT_NE(sun, nullptr);
    EXPECT_NE(sun->detail.find("int day"), std::string::npos) << sun->detail;
}

TEST(CompletionParitySpec, TemplateTemplateParameterIsVisibleInTheBody)
{
    const auto items = CompleteAt(
        "template <template <class> class TT> int foo() {\n"
        "  int a = ^\n"
        "}\n");

    EXPECT_TRUE(Has(items, "TT"));
}

TEST(CompletionParitySpec, MemberOfNonDependentFieldInsideTemplate)
{
    const auto items = CompleteAt(
        "struct Plain { int xxx; };\n"
        "template <typename T> class Templ { public: Plain ppp; };\n"
        "template <typename T> void foo(Templ<T> &t) {\n"
        "  t.ppp.^\n"
        "}\n");

    EXPECT_TRUE(Has(items, "xxx"));
}

TEST(CompletionParitySpec, IncompleteLambdaKeepsTheEnclosingVariable)
{
    EXPECT_TRUE(Has(CompleteAt("auto&& x = []{^"), "x"));
}

TEST(CompletionParitySpec, StructuredBindingNamesAreVariables)
{
    const auto items = CompleteAt(
        "struct S {\n"
        "  int x;\n"
        "  float y;\n"
        "};\n"
        "void function() {\n"
        "  const auto &[xxx, yyy] = S{};\n"
        "  yyy^\n"
        "}\n");

    EXPECT_TRUE(Has(items, "yyy", heimdall::CompletionKind::Variable));
}

TEST(CompletionParitySpec, RangeForStructuredBindingWithUnresolvedRange)
{
    const auto items = CompleteAt(
        "int main() {\n"
        "  for (auto [loopVar] : y ) {\n"
        "    int z = loopV^;\n"
        "  }\n"
        "}\n");

    EXPECT_TRUE(Has(items, "loopVar", heimdall::CompletionKind::Variable));
}

TEST(CompletionParitySpec, LambdaVariableIsAVariable)
{
    const auto items = CompleteAt(
        "void function() {\n"
        "  auto Lambda = [](int a, const double &b) {return 1.f;};\n"
        "  Lam^\n"
        "}\n");

    EXPECT_EQ(Labels(items), (std::vector<std::string> {"Lambda"}));
    EXPECT_EQ(items[0].kind, heimdall::CompletionKind::Variable);
}

TEST(CompletionParitySpec, ConstructorInitializerOutOfLineOffersFields)
{
    const auto items = CompleteAt(
        "namespace ns {\n"
        "  class X { public: X(); int x_; };\n"
        "  X::X() : x_^(0) {}\n"
        "}\n");

    EXPECT_TRUE(Has(items, "x_", heimdall::CompletionKind::Variable));
}

TEST(CompletionParitySpec, ConstructorInitializerInClassOffersLaterFields)
{
    const auto items = CompleteAt(
        "namespace ns {\n"
        "  struct X {\n"
        "    X() : x^\n"
        "    int xyz_;\n"
        "  };\n"
        "}\n");

    EXPECT_TRUE(Has(items, "xyz_"));
}

TEST(CompletionParitySpec, ConstructorInitializerArgumentOffersGlobals)
{
    const auto items = CompleteAt(
        "int foo();\n"
        "namespace ns {\n"
        "  struct X {\n"
        "    X() : xyz_(fo^\n"
        "    int xyz_;\n"
        "  };\n"
        "}\n");

    EXPECT_TRUE(Has(items, "foo", heimdall::CompletionKind::Function));
}

TEST(CompletionParitySpec, OffsetofDesignatorOffersOnlyFields)
{
    const auto items = CompleteAt(
        "struct S { int field; int other; void fieldFn(); };\n"
        "int x = __builtin_offsetof(S, fiel^d);\n");

    EXPECT_EQ(Labels(items), (std::vector<std::string> {"field"}));
}

TEST(CompletionParitySpec, OffsetofDesignatorWalksNestedMembersAndBases)
{
    const auto nested = CompleteAt(
        "struct Inner { int field; void fieldFn(); };\n"
        "struct Outer { Inner inner; };\n"
        "int x = __builtin_offsetof(Outer, inner.fiel^d);\n");
    const auto inherited = CompleteAt(
        "struct Base { int field; void fieldFn(); };\n"
        "struct Derived : Base {};\n"
        "int x = offsetof(Derived, fiel^d);\n");

    EXPECT_EQ(Labels(nested), (std::vector<std::string> {"field"}));
    EXPECT_EQ(Labels(inherited), (std::vector<std::string> {"field"}));
}

TEST(CompletionParitySpec, NoCompletionsWhileNamingANewVariable)
{
    const auto items = CompleteAt(
        "int naber;\n"
        "void f() { int n^ }\n");

    EXPECT_TRUE(items.empty());
}

TEST(CompletionParitySpec, OutOfLineMemberDefinitionOffersPrivateMembers)
{
    const auto items = CompleteAt(
        "class Foo {\n"
        "    int func(int a, int b);\n"
        "};\n"
        "int Foo::func^\n");

    const auto* func = Find(items, "func");
    ASSERT_NE(func, nullptr);
    EXPECT_EQ(func->kind, heimdall::CompletionKind::Function);
    EXPECT_NE(func->detail.find("(int a, int b)"), std::string::npos) << func->detail;
}

TEST(CompletionParitySpec, MacroFromTheMainFile)
{
    const auto items = CompleteAt(
        "#define CLANGD_PREAMBLE_MAIN x\n"
        "int x = 0;\n"
        "#define CLANGD_MAIN x\n"
        "void f() { CLANGD_^ }\n");

    EXPECT_EQ(Labels(items),
              (std::vector<std::string> {"CLANGD_MAIN", "CLANGD_PREAMBLE_MAIN"}));
}

TEST(CompletionParitySpec, MacroLabelAfterFunctionLikeMacroUse)
{
    const auto items = CompleteAt(
        "#define ECHO(X) X\n"
        "#define ECHO2(X) ECHO(X)\n"
        "int finish_preamble = EC^HO(2);\n");

    EXPECT_EQ(Labels(items), (std::vector<std::string> {"ECHO", "ECHO2"}));
}

TEST(CompletionParitySpec, UnderscoreMacrosNeedAnExactPrefix)
{
    const auto plain = CompleteAt(
        "#define gl_foo() 42\n"
        "#define _gl_foo() 42\n"
        "#define glfbar() 42\n"
        "int gl_frob();\n"
        "int _gl_frob();\n"
        "int main() { int y = glf^; }\n");
    const auto underscore = CompleteAt(
        "#define gl_foo() 42\n"
        "#define _gl_foo() 42\n"
        "#define glfbar() 42\n"
        "int gl_frob();\n"
        "int _gl_frob();\n"
        "int main() { int y = _gl^; }\n");

    EXPECT_TRUE(Has(plain, "gl_frob"));
    EXPECT_TRUE(Has(plain, "glfbar"));
    EXPECT_FALSE(Has(plain, "gl_foo"));
    EXPECT_FALSE(Has(plain, "_gl_foo"));
    EXPECT_TRUE(Has(underscore, "_gl_frob"));
    EXPECT_TRUE(Has(underscore, "_gl_foo"));
}

TEST(CompletionParitySpec, ExplicitObjectMemberFunctionHasNoImplicitMembers)
{
    const auto items = CompleteAt(
        "struct A {\n"
        "  int member {};\n"
        "  int memberFnA(int a);\n"
        "  void foo(this A& self) {\n"
        "    mem^;\n"
        "  }\n"
        "};\n");

    EXPECT_FALSE(Has(items, "member"));
    EXPECT_FALSE(Has(items, "memberFnA"));
}

TEST(CompletionParitySpec, ExplicitObjectParameterOffersMembers)
{
    const auto items = CompleteAt(
        "struct A {\n"
        "  int member {};\n"
        "  int memberFnA(int a);\n"
        "  void foo(this A& self) {\n"
        "    self.mem^;\n"
        "  }\n"
        "};\n");

    EXPECT_TRUE(Has(items, "member"));
    EXPECT_TRUE(Has(items, "memberFnA"));
}

TEST(CompletionParitySpec, ImplicitMembersAreVisibleInsideMemberFunctions)
{
    const auto items = CompleteAt(
        "struct A {\n"
        "  int member {};\n"
        "  int memberFnA(int a);\n"
        "  void foo() {\n"
        "    mem^;\n"
        "  }\n"
        "};\n");

    EXPECT_TRUE(Has(items, "member"));
    EXPECT_TRUE(Has(items, "memberFnA"));
}

TEST(CompletionParitySpec, ImplicitMembersAreVisibleInOutOfLineDefinitions)
{
    const auto items = CompleteAt(
        "struct A {\n"
        "  int member {};\n"
        "  void foo();\n"
        "};\n"
        "void A::foo() { mem^; }\n");

    EXPECT_TRUE(Has(items, "member"));
}

TEST(CompletionParitySpec, ConstObjectOffersOnlyConstMemberFunctions)
{
    const auto items = CompleteAt(
        "struct S {\n"
        "  void foo1(int a);\n"
        "  void foo2(int a) const;\n"
        "  int field;\n"
        "};\n"
        "void test(const S s) {\n"
        "  s.^;\n"
        "}\n");

    EXPECT_FALSE(Has(items, "foo1"));
    EXPECT_TRUE(Has(items, "foo2"));
    EXPECT_TRUE(Has(items, "field"));
}

TEST(CompletionParitySpec, ConstMemberFunctionThisOffersOnlyConstMemberFunctions)
{
    const auto items = CompleteAt(
        "struct S {\n"
        "  void foo1(int a);\n"
        "  void foo2(int a) const;\n"
        "};\n"
        "void S::foo2(int a) const {\n"
        "  this->^;\n"
        "}\n");

    EXPECT_FALSE(Has(items, "foo1"));
    EXPECT_TRUE(Has(items, "foo2"));
}

TEST(CompletionParitySpec, ConceptsCompleteAsTypes)
{
    const auto items = CompleteAt(
        "template<class T>\n"
        "concept Small = sizeof(T) <= 8;\n"
        "template<Sma^ U>\n"
        "int foo();\n");

    EXPECT_TRUE(Has(items, "Small", heimdall::CompletionKind::Type));
}

TEST(CompletionParitySpec, NoCrashOnPartialInputs)
{
    EXPECT_NO_FATAL_FAILURE(CompleteAt(
        "int main() {\n"
        "  auto w = [&](auto &&f) { return f(f); };\n"
        "  return n * ^(f)(n - 1);\n"
        "}\n"));
    EXPECT_NO_FATAL_FAILURE(CompleteAt("auto f = []() {\n  1.0_^\n};\n"));
    EXPECT_NO_FATAL_FAILURE(CompleteAt(
        "struct FooBarBaz {};\n"
        "void test() {\n"
        "  if (FooBarBaz * x^) {}\n"
        "}\n"));
    EXPECT_NO_FATAL_FAILURE(CompleteAt(
        "template <typename = int> struct Foo {};\n"
        "auto a = [x(3)](Foo<^>){};\n"));
    EXPECT_NO_FATAL_FAILURE(CompleteAt(
        "struct Base {\n"
        "  virtual ~Base() = 0;\n"
        "  virtual operator int() = 0;\n"
        "  virtual Base& operator+(Base&) = 0;\n"
        "};\n"
        "struct Derived : Base {\n"
        "  ^\n"
        "};\n"));
    EXPECT_NO_FATAL_FAILURE(CompleteAt("#pragma ^ // no new line"));
    EXPECT_NO_FATAL_FAILURE(CompleteAt("#include \"./^\"\n"));
}

TEST(CompletionParitySpec, QualifiedTypeInParameterListAfterBacktracking)
{
    const auto items = CompleteAt(
        "namespace ns {\n"
        "struct FooBarBaz {};\n"
        "} // namespace ns\n"
        "int foo(ns::FooBar^\n");

    EXPECT_EQ(Labels(items), (std::vector<std::string> {"FooBarBaz"}));
}

TEST(CompletionParitySpec, IfConditionWithQualifiedPrefix)
{
    const auto items = CompleteAt(
        "namespace ns { int NotRecovered() { return 0; } }\n"
        "void f() {\n"
        "  if (auto x = ns::NotRecover^)\n"
        "}\n");

    EXPECT_EQ(Labels(items), (std::vector<std::string> {"NotRecovered"}));
}

TEST(CompletionParitySpec, RecoveredCallResultOffersMembers)
{
    const auto items = CompleteAt(
        "struct S { int member; };\n"
        "S overloaded(int);\n"
        "void foo() {\n"
        "  overloaded().^\n"
        "}\n");

    EXPECT_EQ(Labels(items), (std::vector<std::string> {"member"}));
}

TEST(CompletionParitySpec, EnumeratorsOfHeaderEnumsThroughTheIndex)
{
    const heimdall::ParserOptions options;
    const heimdall::ScopeIndex index = heimdall::CompletionEngine::IndexScopes(
        "namespace ns {\n"
        "  enum Unscoped { Clangd1 };\n"
        "  class C {\n"
        "  public:\n"
        "    enum Unscoped { Clangd2 };\n"
        "  };\n"
        "  enum class Scoped { Clangd3 };\n"
        "}\n",
        options);

    const auto unqualified = CompleteAt("void bar() { ns::Clangd^ }", &index);
    const auto nested = CompleteAt("void bar() { ns::C::Clangd^ }", &index);
    const auto scoped = CompleteAt("void bar() { ns::Scoped::Clangd^ }", &index);

    EXPECT_EQ(Labels(unqualified), (std::vector<std::string> {"Clangd1"}));
    EXPECT_EQ(Labels(nested), (std::vector<std::string> {"Clangd2"}));
    EXPECT_EQ(Labels(scoped), (std::vector<std::string> {"Clangd3"}));
}

TEST(CompletionParitySpec, CommentParameterNameHint)
{
    constexpr std::string_view code =
        "void fun(int foo, int bar);\n"
        "void overloaded(int param_int);\n"
        "void overloaded(int param_int, int param_other);\n"
        "void overloaded(char param_char);\n"
        "int main() {\n";

    EXPECT_EQ(Labels(CompleteAt(std::string(code) + "fun(/*^")),
              (std::vector<std::string> {"foo=*/"}));
    EXPECT_EQ(Labels(CompleteAt(std::string(code) + "fun(1, /*^")),
              (std::vector<std::string> {"bar=*/"}));
    EXPECT_TRUE(CompleteAt(std::string(code) + "/*^").empty());
    EXPECT_EQ(Labels(CompleteAt(std::string(code) + "overloaded(/*^")),
              (std::vector<std::string> {"param_char=*/", "param_int=*/"}));
    EXPECT_EQ(Labels(CompleteAt(std::string(code) + "fun(/*  ^")),
              (std::vector<std::string> {"foo=*/"}));
    EXPECT_EQ(Labels(CompleteAt(std::string(code) + "fun(/* f^")),
              (std::vector<std::string> {"foo=*/"}));
    EXPECT_TRUE(CompleteAt(std::string(code) + "fun(/* x^").empty());
    EXPECT_TRUE(CompleteAt(std::string(code) + "fun(/* f ^").empty());
}

TEST(CompletionParitySpec, SuggestsOverridesOfInheritedVirtuals)
{
    const auto items = CompleteAt(
        "class A {\n"
        " public:\n"
        "  virtual void vfunc(bool param);\n"
        "  virtual void vfunc(bool param, int p);\n"
        "  void func(bool param);\n"
        "};\n"
        "class B : public A {\n"
        "  virtual void ttt(bool param) const;\n"
        "  void vfunc(bool param, int p) override;\n"
        "};\n"
        "class C : public B {\n"
        " public:\n"
        "  void vfunc(bool param) override;\n"
        "  ^\n"
        "};\n");

    EXPECT_TRUE(Has(items, "void vfunc(bool param, int p) override"));
    EXPECT_TRUE(Has(items, "void ttt(bool param) const override"));
    EXPECT_FALSE(Has(items, "void vfunc(bool param) override"));
    EXPECT_FALSE(Has(items, "void func(bool param) override"));
}

TEST(CompletionParitySpec, DeprecatedDeclarationsAreFlagged)
{
    const auto items = CompleteAt(
        "void TestClangd();\n"
        "[[deprecated]] void TestClangc();\n"
        "int main() { TestClang^ }\n");

    const auto* plain = Find(items, "TestClangd");
    const auto* deprecated = Find(items, "TestClangc");
    ASSERT_NE(plain, nullptr);
    ASSERT_NE(deprecated, nullptr);
    EXPECT_FALSE(plain->deprecated);
    EXPECT_TRUE(deprecated->deprecated);
}

TEST(CompletionParitySpec, EmptyCommentLineKeepsBothParagraphs)
{
    const auto items = CompleteAt(
        "// First paragraph.\n"
        "//\n"
        "// Second paragraph.\n"
        "int documented();\n"
        "int x = docu^\n");

    const auto* documented = Find(items, "documented");
    ASSERT_NE(documented, nullptr);
    EXPECT_NE(documented->documentation.find("First paragraph."), std::string::npos);
    EXPECT_NE(documented->documentation.find("Second paragraph."), std::string::npos);
}

TEST(CompletionParitySpec, ElseBranchOfDisabledConditionalCompletes)
{
    const auto items = CompleteAt(
        "int param_value;\n"
        "int foo() {\n"
        "#if 0\n"
        "  int hidden;\n"
        "#else\n"
        "  return param_^\n"
        "#endif\n"
        "}\n");

    EXPECT_TRUE(Has(items, "param_value"));
}

TEST(CompletionParitySpec, ReturnTypeAtNamespaceScopeStillCompletesTypes)
{
    const auto items = CompleteAt(
        "struct Widget { void Run(); };\n"
        "void Wid^\n");

    EXPECT_TRUE(Has(items, "Widget", heimdall::CompletionKind::Type));
}

TEST(CompletionParitySpec, HoverStillFindsPrivateMembers)
{
    const std::string source =
        "class Foo { int hidden; public: int shown; };\n"
        "void test(Foo f) { f.hidden; }\n";
    const heimdall::ParserOptions options;

    const auto hover =
        heimdall::CompletionEngine::Hover(source, options, source.rfind("hidden") + 2);

    ASSERT_TRUE(hover.has_value());
    EXPECT_EQ(hover->label, "hidden");
}

TEST(CompletionParitySpec, PointerToConstOffersOnlyConstMemberFunctions)
{
    const auto items = CompleteAt(
        "struct S {\n"
        "  void mutate();\n"
        "  void read() const;\n"
        "  static void make();\n"
        "};\n"
        "void test(const S* s) {\n"
        "  s->^;\n"
        "}\n");

    EXPECT_FALSE(Has(items, "mutate"));
    EXPECT_TRUE(Has(items, "read"));
    EXPECT_TRUE(Has(items, "make"));
}

TEST(CompletionParitySpec, UnscopedEnumeratorsAreQualifiedByTheirEnclosingNamespace)
{
    const auto items = CompleteAt(
        "namespace ns { enum Mode { Fast, Slow }; }\n"
        "auto m = ns::Fa^\n");

    EXPECT_EQ(Labels(items), (std::vector<std::string> {"Fast"}));
}

TEST(CompletionParitySpec, UsingDirectivesAreFollowedTransitively)
{
    const auto items = CompleteAt(
        "namespace inner { int deep; }\n"
        "namespace middle { using namespace inner; }\n"
        "namespace outer { using namespace middle; }\n"
        "int x = outer::de^\n");

    EXPECT_TRUE(Has(items, "deep"));
}

TEST(CompletionParitySpec, InheritedProtectedMembersAreVisibleInDerivedMembers)
{
    const auto items = CompleteAt(
        "class Base { protected: int prot; private: int priv; };\n"
        "class Derived : public Base {\n"
        "  void f();\n"
        "};\n"
        "void Derived::f() { pr^ }\n");

    EXPECT_TRUE(Has(items, "prot"));
    EXPECT_FALSE(Has(items, "priv"));
}
