#include <gtest/gtest.h>

#include <Heimdall/Completion.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

// Signature help behaviors ported from clangd's SignatureHelpTest. `^` marks the
// cursor; parameters in expected labels are wrapped in `[[` `]]`.

namespace
{
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
        result.code = std::string(text.substr(0, caret)) + std::string(text.substr(caret + 1));
        result.point = caret;
        return result;
    }

    heimdall::SignatureHelp SignaturesAt(
        std::string_view text,
        const heimdall::ScopeIndex* external = nullptr)
    {
        const Annotated annotated = Annotate(text);
        const heimdall::ParserOptions options;
        return heimdall::CompletionEngine::Signatures(annotated.code, options, annotated.point,
                                                      external);
    }

    // `foo([[int x]], [[int y]]) -> void` back from a signature, to compare labels
    // and parameter ranges in one string.
    std::string Annotated(const heimdall::SignatureInfo& signature)
    {
        std::string out;
        std::size_t at = 0;
        for (const auto& [begin, end] : signature.parameters)
        {
            out += signature.label.substr(at, begin - at);
            out += "[[" + signature.label.substr(begin, end - begin) + "]]";
            at = end;
        }

        return out + signature.label.substr(at);
    }

    std::vector<std::string> Labels(const heimdall::SignatureHelp& help)
    {
        std::vector<std::string> labels;
        for (const auto& signature : help.signatures)
        {
            labels.push_back(Annotated(signature));
        }

        return labels;
    }

} // namespace

TEST(SignatureHelpSpec, ExposesParameterNamesWithoutTypesOrDefaults)
{
    const auto help = SignaturesAt(
        "void Call(unsigned int first, int second, const char* desc = \"\", int = 0);\n"
        "void f() { Call(^); }\n");

    ASSERT_EQ(help.signatures.size(), 1u);
    EXPECT_EQ(help.signatures.front().parameterNames,
              (std::vector<std::string> {"first", "second", "desc", ""}));
}

TEST(SignatureHelpSpec, ListsEveryOverload)
{
    const auto help = SignaturesAt(
        "void foo(int x, int y);\n"
        "void foo(int x, float y);\n"
        "void foo(float x, int y);\n"
        "void foo(float x, float y);\n"
        "void bar(int x, int y = 0);\n"
        "int main() { foo(^); }\n");

    EXPECT_EQ(Labels(help), (std::vector<std::string> {
            "foo([[int x]], [[int y]]) -> void",
            "foo([[int x]], [[float y]]) -> void",
            "foo([[float x]], [[int y]]) -> void",
            "foo([[float x]], [[float y]]) -> void"}));
    EXPECT_EQ(help.active_signature, 0u);
    EXPECT_EQ(help.active_parameter, 0u);
}

TEST(SignatureHelpSpec, RedeclarationsShowTheNamedParameters)
{
    const auto help = SignaturesAt(
        "void sun(int);\n"
        "void sun(int night);\n"
        "void sun(int day);\n"
        "void sun(int);\n"
        "void position() {\n"
        "  sun(^);\n"
        "}\n");

    EXPECT_EQ(Labels(help), (std::vector<std::string> {"sun([[int day]]) -> void"}));
}

TEST(SignatureHelpSpec, DefaultArgumentsStayInTheLabel)
{
    const auto help = SignaturesAt(
        "void bar(int x, int y = 0);\n"
        "void bar(float x = 0, int y = 42);\n"
        "int main() { bar(^\n");

    EXPECT_EQ(Labels(help), (std::vector<std::string> {
            "bar([[int x]], [[int y = 0]]) -> void",
            "bar([[float x = 0]], [[int y = 42]]) -> void"}));
    EXPECT_EQ(help.active_parameter, 0u);
}

TEST(SignatureHelpSpec, ActiveArgumentOfTheOuterCall)
{
    const auto help = SignaturesAt(
        "int baz(int a, int b, int c);\n"
        "int main() { baz(baz(1,2,3), ^); }\n");

    EXPECT_EQ(Labels(help),
              (std::vector<std::string> {"baz([[int a]], [[int b]], [[int c]]) -> int"}));
    EXPECT_EQ(help.active_parameter, 1u);
}

TEST(SignatureHelpSpec, InsideAnArgument)
{
    constexpr std::string_view overloads =
        "void foo(int x);\n"
        "void foo(int x, int y);\n";

    for (const std::string_view call :
        {
            "foo(1+^);", "foo(1^);", "foo(1^0);"
    })
    {
        const auto help = SignaturesAt(std::string(overloads) + "int main() { " +
            std::string(call) + " }\n");

        EXPECT_EQ(Labels(help), (std::vector<std::string> {
                "foo([[int x]]) -> void", "foo([[int x]], [[int y]]) -> void"})) << call;
        EXPECT_EQ(help.active_parameter, 0u) << call;
    }

    const auto nested = SignaturesAt(std::string(overloads) +
        "int bar(int x, int y);\n"
        "int main() { bar(foo(2, 3^)); }\n");

    EXPECT_EQ(Labels(nested), (std::vector<std::string> {"foo([[int x]], [[int y]]) -> void"}));
    EXPECT_EQ(nested.active_parameter, 1u);
}

TEST(SignatureHelpSpec, EmptyParameterListIsStillShown)
{
    const auto help = SignaturesAt(
        "struct A {int x;};\n"
        "struct B {B(A);};\n"
        "void f();\n"
        "int main() {\n"
        "  B b({1});\n"
        "  f(^);\n"
        "}\n");

    EXPECT_EQ(Labels(help), (std::vector<std::string> {"f() -> void"}));
}

TEST(SignatureHelpSpec, ConstructorsForParenthesizedInitialization)
{
    constexpr std::string_view type =
        "struct S {\n"
        "  S(int);\n"
        "  S(const S &) = delete;\n"
        "};\n";

    for (const std::string_view init :
        {
            "S s(^);", "auto s = S(^);", "auto s = new S(^);"
    })
    {
        const auto help = SignaturesAt(std::string(type) + std::string(init));

        EXPECT_EQ(Labels(help), (std::vector<std::string> {"S([[int]])"})) << init;
    }
}

TEST(SignatureHelpSpec, ConstructorsForBracedInitialization)
{
    constexpr std::string_view type =
        "struct S {\n"
        "  S(int);\n"
        "  S(const S &) = delete;\n"
        "};\n";

    for (const std::string_view init :
        {
            "S s{^};", "S s = {^};", "auto s = S{^};"
    })
    {
        const auto help = SignaturesAt(std::string(type) + std::string(init));

        EXPECT_EQ(Labels(help), (std::vector<std::string> {"S{[[int]]}"})) << init;
    }
}

TEST(SignatureHelpSpec, AggregateInitializationListsTheFields)
{
    constexpr std::string_view type =
        "struct S {\n"
        "  int a, b, c, d;\n"
        "};\n";
    const std::string aggregate = "S{[[int a]], [[int b]], [[int c]], [[int d]]}";

    const auto empty = SignaturesAt(std::string(type) + "S s{^}");
    const auto second = SignaturesAt(std::string(type) + "S s{1,^}");
    const auto designated = SignaturesAt(std::string(type) + "S s{.c=3,^}");

    EXPECT_EQ(Labels(empty), (std::vector<std::string> {aggregate}));
    EXPECT_EQ(Labels(second), (std::vector<std::string> {aggregate}));
    EXPECT_EQ(second.active_parameter, 1u);
    EXPECT_EQ(Labels(designated), (std::vector<std::string> {aggregate}));
    EXPECT_EQ(designated.active_parameter, 3u);
}

TEST(SignatureHelpSpec, MemberInitializerShowsTheMemberTypeConstructors)
{
    const auto help = SignaturesAt(
        "struct A { A(int); };\n"
        "struct B {\n"
        "  B() : a_elem(^) {}\n"
        "  A a_elem;\n"
        "};\n");

    EXPECT_EQ(Labels(help), (std::vector<std::string> {"A([[int]])"}));
}

TEST(SignatureHelpSpec, MemberFunctionsThroughTheReceiver)
{
    const auto help = SignaturesAt(
        "struct Foo {\n"
        "  /// Member doc\n"
        "  int foo();\n"
        "  int foo(double d);\n"
        "};\n"
        "void test() {\n"
        "  Foo f;\n"
        "  f.foo(^);\n"
        "}\n");

    EXPECT_EQ(Labels(help),
              (std::vector<std::string> {"foo() -> int", "foo([[double d]]) -> int"}));
    EXPECT_EQ(help.signatures.front().documentation, "Member doc");
}

TEST(SignatureHelpSpec, InheritedMemberFunctions)
{
    const auto help = SignaturesAt(
        "struct Base { void run(int times); };\n"
        "struct Derived : Base {};\n"
        "void test(Derived* d) { d->run(^); }\n");

    EXPECT_EQ(Labels(help), (std::vector<std::string> {"run([[int times]]) -> void"}));
}

TEST(SignatureHelpSpec, QualifiedFunctions)
{
    const auto help = SignaturesAt(
        "namespace ns { int twice(int value); }\n"
        "int x = ns::twice(^);\n");

    EXPECT_EQ(Labels(help), (std::vector<std::string> {"twice([[int value]]) -> int"}));
}

TEST(SignatureHelpSpec, VariadicParameterAbsorbsTrailingArguments)
{
    constexpr std::string_view header =
        "void fun(int x, ...) {}\n"
        "void test() {";
    const std::string expected = "fun([[int x]], [[...]]) -> void";

    const auto first = SignaturesAt(std::string(header) + "fun(^);}");
    const auto second = SignaturesAt(std::string(header) + "fun(1, ^);}");
    const auto third = SignaturesAt(std::string(header) + "fun(1, 2, ^);}");

    EXPECT_EQ(Labels(first), (std::vector<std::string> {expected}));
    EXPECT_EQ(first.active_parameter, 0u);
    EXPECT_EQ(Labels(second), (std::vector<std::string> {expected}));
    EXPECT_EQ(second.active_parameter, 1u);
    EXPECT_EQ(Labels(third), (std::vector<std::string> {expected}));
    EXPECT_EQ(third.active_parameter, 1u);
}

TEST(SignatureHelpSpec, VariadicTemplateParameterPack)
{
    const auto help = SignaturesAt(
        "template<typename T, typename ...Args>\n"
        "void fun(T t, Args ...args) {}\n"
        "void test() { fun(1, 2, ^);}\n");

    ASSERT_EQ(help.signatures.size(), 1u);
    EXPECT_EQ(help.signatures.front().parameters.size(), 2u);
    EXPECT_EQ(help.active_parameter, 1u);
}

TEST(SignatureHelpSpec, ExplicitTemplateArgumentsBeforeTheCall)
{
    const auto help = SignaturesAt(
        "template <class T>\n"
        "void foo(T, T, T);\n"
        "int main() {\n"
        "  foo<int>(^);\n"
        "}\n");

    EXPECT_EQ(Labels(help), (std::vector<std::string> {"foo([[T]], [[T]], [[T]]) -> void"}));
}

TEST(SignatureHelpSpec, ExplicitObjectParameterIsNotAnArgument)
{
    const auto help = SignaturesAt(
        "struct A {\n"
        "  void foo(this auto&& self, int arg);\n"
        "};\n"
        "int main() {\n"
        "  A a {};\n"
        "  a.foo(^);\n"
        "}\n");

    EXPECT_EQ(Labels(help), (std::vector<std::string> {"foo([[int arg]]) -> void"}));
}

TEST(SignatureHelpSpec, OpeningParenthesisOfTheCall)
{
    for (const std::string_view test :
        {
            "int foo(int a, int b, int c);\n"
            "int main() { foo(foo $p( foo(10, 10, 10), ^ )); }\n",
            "struct Foo { Foo(int a, int b, int c); };\n"
            "int main() { Foo $p( 10, ^ ); }\n",
            "struct Foo { Foo(int a, int b, int c); };\n"
            "int main() { new Foo $p( 10, ^ ); }\n",
            "struct Foo { int foo(int, int); };\n"
            "template <typename T> void bar(T t) { Foo f; f.foo$p(t, ^t); }\n"
    })
    {
        std::string text(test);
        const std::size_t marker = text.find("$p");
        text.erase(marker, 2);

        const auto help = SignaturesAt(text);

        EXPECT_EQ(help.call_open, marker) << test;
        EXPECT_FALSE(help.signatures.empty()) << test;
    }
}

TEST(SignatureHelpSpec, HeaderFunctionsThroughTheIndex)
{
    const heimdall::ParserOptions options;
    const heimdall::ScopeIndex index = heimdall::CompletionEngine::IndexScopes(
        "namespace lib {\n"
        "  /// Joins two parts.\n"
        "  int join(int left, int right);\n"
        "}\n"
        "int area(int width, int height);\n",
        options);

    const auto qualified = SignaturesAt("int x = lib::join(1, ^);", &index);
    const auto global = SignaturesAt("int y = area(^);", &index);

    EXPECT_EQ(Labels(qualified),
              (std::vector<std::string> {"join([[int left]], [[int right]]) -> int"}));
    EXPECT_EQ(qualified.active_parameter, 1u);
    EXPECT_EQ(qualified.signatures.front().documentation, "Joins two parts.");
    EXPECT_EQ(Labels(global),
              (std::vector<std::string> {"area([[int width]], [[int height]]) -> int"}));
}

TEST(SignatureHelpSpec, IncompleteRecursiveCall)
{
    const auto help = SignaturesAt("auto x() { x(^");

    EXPECT_EQ(Labels(help), (std::vector<std::string> {"x() -> auto"}));
}

TEST(SignatureHelpSpec, NothingOutsideACall)
{
    EXPECT_TRUE(SignaturesAt("int foo(int);\nint main() { foo(1); ^ }\n").signatures.empty());
    EXPECT_TRUE(SignaturesAt("int foo(int);\nint main() ^{ }\n").signatures.empty());
    EXPECT_TRUE(SignaturesAt("int foo(int);\nint main() { foo(\"a^\"); }\n").signatures.empty());
    EXPECT_TRUE(SignaturesAt("int foo(int);\nint main() { int a[3]; a[^]; }\n").signatures.empty());
    EXPECT_TRUE(SignaturesAt("#ifdef IS_HEADER\n#else\n#^endif\n").signatures.empty());
    EXPECT_TRUE(SignaturesAt("struct S {\n  int field;^\n};\n").signatures.empty());
}
