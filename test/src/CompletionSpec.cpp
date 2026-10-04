#include <gtest/gtest.h>

#include <Heimdall/Completion.hpp>

#include <algorithm>
#include <string_view>
#include <vector>

namespace
{

bool Contains(const std::vector<heimdall::CompletionItem>& items, std::string_view label)
{
    for (const auto& item : items)
    {
        if (item.label == label) return true;
    }
    return false;
}

const heimdall::CompletionItem* Find(const std::vector<heimdall::CompletionItem>& items,
                                     std::string_view label)
{
    for (const auto& item : items)
    {
        if (item.label == label) return &item;
    }
    return nullptr;
}

std::size_t OffsetAfter(std::string_view source, std::string_view needle)
{
    const std::size_t pos = source.find(needle);
    EXPECT_NE(pos, std::string_view::npos) << needle;
    return pos + needle.size();
}

} // namespace

TEST(CompletionSpec, ExtractsIdentifierPrefixBeforeCursor)
{
    EXPECT_EQ(heimdall::CompletionEngine::PrefixAt("int myValue", 11), "myValue");
    EXPECT_EQ(heimdall::CompletionEngine::PrefixAt("int myValue", 6), "my");
    EXPECT_EQ(heimdall::CompletionEngine::PrefixAt("int x", 0), "");
    EXPECT_EQ(heimdall::CompletionEngine::PrefixAt("a+b", 3), "b");
    EXPECT_EQ(heimdall::CompletionEngine::PrefixAt("a", 99), "a");
    EXPECT_EQ(heimdall::CompletionEngine::PrefixAt("", 0), "");
}

TEST(CompletionSpec, SuggestsLocalVariablesAndFunctionsByPrefix)
{
    constexpr std::string_view source =
        "int compute(int value) {\n"
        "  int counter = value;\n"
        "  int count_total = counter;\n"
        "  return count_total + cou;\n"
        "}\n";
    // `find("cou")` hits `counter` first, so anchor on the trailing use.
    const std::size_t pos = source.rfind("cou");
    ASSERT_NE(pos, std::string_view::npos);
    const auto items = heimdall::CompletionEngine::Complete(source, pos + 3);
    EXPECT_TRUE(Contains(items, "counter"));
    EXPECT_TRUE(Contains(items, "count_total"));
    EXPECT_FALSE(Contains(items, "value;"));

    // Function names share the same machinery under a different prefix.
    constexpr std::string_view fn_source = "int compute() {}\nint com";
    const auto fns = heimdall::CompletionEngine::Complete(fn_source, fn_source.size());
    EXPECT_TRUE(Contains(fns, "compute"));
}

TEST(CompletionSpec, SuggestsKeywordsByPrefix)
{
    constexpr std::string_view source = "int f() { ret";
    const auto items = heimdall::CompletionEngine::Complete(source, source.size());
    EXPECT_TRUE(Contains(items, "return"));
    const auto* item = Find(items, "return");
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(item->kind, heimdall::CompletionKind::Keyword);
}

TEST(CompletionSpec, FiltersCandidatesByPrefix)
{
    constexpr std::string_view source = "int alpha = 1;\nint beta = 2;\nint al";
    const auto items = heimdall::CompletionEngine::Complete(source, source.size());
    EXPECT_TRUE(Contains(items, "alpha"));
    EXPECT_FALSE(Contains(items, "beta"));
}

TEST(CompletionSpec, EmptyPrefixReturnsKeywords)
{
    constexpr std::string_view source = "int x = 1;\n";
    const auto items = heimdall::CompletionEngine::Complete(source, source.size());
    EXPECT_TRUE(Contains(items, "return"));
    EXPECT_TRUE(Contains(items, "int"));
}

TEST(CompletionSpec, SuppressesCompletionInsideCommentsAndStrings)
{
    {
        constexpr std::string_view source = "// hello wor";
        EXPECT_TRUE(heimdall::CompletionEngine::Complete(source, source.size()).empty());
    }
    {
        constexpr std::string_view source = "/* hello wor";
        EXPECT_TRUE(heimdall::CompletionEngine::Complete(source, source.size()).empty());
    }
    {
        constexpr std::string_view source = "const char* s = \"hello wor";
        EXPECT_TRUE(heimdall::CompletionEngine::Complete(source, source.size()).empty());
    }
}

TEST(CompletionSpec, SuppressesMemberAccessUntilMembersAreModeled)
{
    {
        constexpr std::string_view source = " Khalifa; Khalifa.";
        const auto items = heimdall::CompletionEngine::Complete(source, source.size());
        EXPECT_TRUE(items.empty());
    }
    {
        constexpr std::string_view source = "ptr->";
        const auto items = heimdall::CompletionEngine::Complete(source, source.size());
        EXPECT_TRUE(items.empty());
    }
    {
        // Even with a partial member name, no guesses: `value.mem`.
        constexpr std::string_view source = "value.mem";
        const auto items = heimdall::CompletionEngine::Complete(source, source.size());
        EXPECT_TRUE(items.empty());
    }
}

TEST(CompletionSpec, SuggestsPreprocessorDirectivesAfterHash)
{
    constexpr std::string_view source = "#inc";
    const auto items = heimdall::CompletionEngine::Complete(source, source.size());
    EXPECT_TRUE(Contains(items, "include"));
    const auto* item = Find(items, "include");
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(item->kind, heimdall::CompletionKind::Directive);
}

TEST(CompletionSpec, SuggestsDefinesAndPredefinedMacros)
{
    constexpr std::string_view source = "#define MY_FEATURE 1\nint x = MY_FEA";
    heimdall::ParserOptions options;
    options.predefined_macros.emplace("PLATFORM_FLAG", "1");
    const auto items = heimdall::CompletionEngine::Complete(source, options, source.size());
    EXPECT_TRUE(Contains(items, "MY_FEATURE"));
    // Predefined macro does not match this prefix, but is returned for empty prefixes.
    const auto all = heimdall::CompletionEngine::Complete("#define MY_FEATURE 1\n", options, 0);
    EXPECT_TRUE(Contains(all, "PLATFORM_FLAG"));
    const auto* macro = Find(items, "MY_FEATURE");
    ASSERT_NE(macro, nullptr);
    EXPECT_EQ(macro->kind, heimdall::CompletionKind::Macro);
}

TEST(CompletionSpec, ClassifiesDeclaredFunctionsAndTypes)
{
    constexpr std::string_view source =
        "struct Widget { int value; };\n"
        "int compute(Widget w) { return w.value; }\n"
        "int comp";
    const auto items = heimdall::CompletionEngine::Complete(source, source.size());
    EXPECT_TRUE(Contains(items, "compute"));
    const auto* function = Find(items, "compute");
    ASSERT_NE(function, nullptr);
    EXPECT_EQ(function->kind, heimdall::CompletionKind::Function);

    constexpr std::string_view type_source = "struct Widget { int v; };\nWid";
    const auto types = heimdall::CompletionEngine::Complete(type_source, type_source.size());
    EXPECT_TRUE(Contains(types, "Widget"));
    const auto* type = Find(types, "Widget");
    ASSERT_NE(type, nullptr);
    EXPECT_EQ(type->kind, heimdall::CompletionKind::Type);
}

TEST(CompletionSpec, CompletesOnBrokenCodeWithoutCrashing)
{
    constexpr std::string_view source = "int f() { int broken return con";
    const auto items = heimdall::CompletionEngine::Complete(source, source.size());
    // `con` matches `continue`/`const`/`constexpr` even though the missing
    // ';' leaves a parse diagnostic behind.
    EXPECT_TRUE(Contains(items, "continue"));
    EXPECT_TRUE(Contains(items, "const"));
}

TEST(CompletionSpec, ClassifiesLocalsAndParametersAsVariables)
{
    constexpr std::string_view source =
        "int compute(int myParam) {\n"
        "  int myLocal = myPa;\n"
        "  return myL;\n"
        "}\n";
    const std::size_t param_pos = source.find("= myPa") + 5;
    const auto params = heimdall::CompletionEngine::Complete(source, param_pos);
    const auto* param = Find(params, "myParam");
    ASSERT_NE(param, nullptr) << "expected myParam among completions";
    EXPECT_EQ(param->kind, heimdall::CompletionKind::Variable);
    const std::size_t local_pos = source.rfind("myL") + 3;
    const auto locals = heimdall::CompletionEngine::Complete(source, local_pos);
    const auto* local = Find(locals, "myLocal");
    ASSERT_NE(local, nullptr) << "expected myLocal among completions";
    EXPECT_EQ(local->kind, heimdall::CompletionKind::Variable);
}

TEST(CompletionSpec, HidesLocalsFromOtherFunctions)
{
    constexpr std::string_view source =
        "void first() {\n"
        "  int alpha_local = 1;\n"
        "  consume(alpha_local);\n"
        "}\n"
        "void second() {\n"
        "  int alpha_second = 2;\n"
        "  int x = alpha_;\n"
        "}\n";
    const std::size_t pos = source.rfind("alpha_") + 6;
    const auto items = heimdall::CompletionEngine::Complete(source, pos);
    EXPECT_TRUE(Contains(items, "alpha_second"));
    EXPECT_FALSE(Contains(items, "alpha_local"));
}

TEST(CompletionSpec, HidesDeclarationsAfterCursor)
{
    constexpr std::string_view source =
        "void f() {\n"
        "  int x = late_;\n"
        "  int late_value = 1;\n"
        "}\n";
    const std::size_t pos = source.find("late_") + 5;
    const auto items = heimdall::CompletionEngine::Complete(source, pos);
    EXPECT_FALSE(Contains(items, "late_value"));
}

TEST(CompletionSpec, SuggestsParametersOnlyInsideTheirFunction)
{
    constexpr std::string_view source =
        "int sum(int first_arg, int second_arg) {\n"
        "  return first_;\n"
        "}\n"
        "int other() {\n"
        "  return first_;\n"
        "}\n";
    const std::size_t inside = source.find("return first_") + 13;
    EXPECT_TRUE(Contains(heimdall::CompletionEngine::Complete(source, inside), "first_arg"));
    const std::size_t outside = source.rfind("return first_") + 13;
    const auto outer = heimdall::CompletionEngine::Complete(source, outside);
    EXPECT_FALSE(Contains(outer, "first_arg"));
    EXPECT_FALSE(Contains(outer, "second_arg"));
}

TEST(CompletionSpec, ResolvesNamespaceMembers)
{
    constexpr std::string_view source =
        "int global_item = 1;\n"
        "namespace tools {\n"
        "int tool_item = 2;\n"
        "int tool_other = 3;\n"
        "}\n"
        "int x = tools::tool_i;\n";
    const std::size_t pos = source.rfind("tool_i") + 6;
    const auto items = heimdall::CompletionEngine::Complete(source, pos);
    EXPECT_TRUE(Contains(items, "tool_item"));
    EXPECT_FALSE(Contains(items, "tool_other"));
    EXPECT_FALSE(Contains(items, "global_item"));
}

TEST(CompletionSpec, ResolvesNestedNamespaces)
{
    constexpr std::string_view source =
        "namespace outer {\n"
        "namespace inner {\n"
        "int deep_item = 1;\n"
        "}\n"
        "int shallow_item = 2;\n"
        "}\n"
        "int a = outer::inner::deep_;\n"
        "int b = outer::shallow_;\n";
    const std::size_t deep = source.find("deep_;") + 5;
    const auto deep_items = heimdall::CompletionEngine::Complete(source, deep);
    EXPECT_TRUE(Contains(deep_items, "deep_item"));

    const std::size_t shallow = source.find("shallow_;") + 8;
    const auto shallow_items = heimdall::CompletionEngine::Complete(source, shallow);
    EXPECT_TRUE(Contains(shallow_items, "shallow_item"));
    EXPECT_FALSE(Contains(shallow_items, "deep_item"));
}

TEST(CompletionSpec, UnknownQualifierOffersNothing)
{
    constexpr std::string_view source =
        "namespace tools {\n"
        "int tool_item = 1;\n"
        "}\n"
        "int x = nope::tool_;\n";
    EXPECT_TRUE(heimdall::CompletionEngine::Complete(source, source.size() - 2).empty());
}

TEST(CompletionSpec, ResolvesScopedEnumMembers)
{
    constexpr std::string_view source =
        "enum class Color { Red, Green };\n"
        "int compute() { return 0; }\n"
        "Color c = Color::R;\n";
    const std::size_t pos = source.find("Color::R") + 8;
    const auto items = heimdall::CompletionEngine::Complete(source, pos);
    EXPECT_TRUE(Contains(items, "Red"));
    EXPECT_FALSE(Contains(items, "Green"));
    EXPECT_FALSE(Contains(items, "compute"));

    constexpr std::string_view all_source =
        "enum class Color { Red, Green };\n"
        "int compute() { return 0; }\n"
        "Color c2 = Color::;\n";
    const std::size_t all_pos = all_source.find("Color::") + 7;
    const auto all = heimdall::CompletionEngine::Complete(all_source, all_pos);
    EXPECT_TRUE(Contains(all, "Red"));
    EXPECT_TRUE(Contains(all, "Green"));
    EXPECT_FALSE(Contains(all, "compute"));
}

TEST(CompletionSpec, GlobalQualifierListsGlobalsWithoutLocalsOrKeywords)
{
    constexpr std::string_view source =
        "int gvalue = 1;\n"
        "void f() {\n"
        "  int gotham = 2;\n"
        "  consume(gotham);\n"
        "}\n"
        "int y = ::g;\n";
    const std::size_t pos = source.rfind("::g") + 3;
    const auto items = heimdall::CompletionEngine::Complete(source, pos);
    EXPECT_TRUE(Contains(items, "gvalue"));
    EXPECT_FALSE(Contains(items, "gotham"));
    EXPECT_FALSE(Contains(items, "goto"));
}

TEST(CompletionSpec, IndexScopesListsScopesAndMembers)
{
    constexpr std::string_view source =
        "#define FEATURE_FLAG 1\n"
        "int global_fn() { return 0; }\n"
        "namespace tools {\n"
        "struct Widget { int value; };\n"
        "int tool_fn();\n"
        "namespace inner {\n"
        "int deep_fn();\n"
        "}\n"
        "}\n"
        "enum class Color { Red, Green };\n";
    const heimdall::ScopeIndex index =
        heimdall::CompletionEngine::IndexScopes(source, heimdall::ParserOptions {});

    auto find_scope = [&](std::vector<std::string> path) -> const heimdall::IndexedScope* {
        for (const auto& scope : index)
        {
            if (scope.path == path) return &scope;
        }
        return nullptr;
    };
    const auto* root = find_scope({});
    ASSERT_NE(root, nullptr);
    EXPECT_TRUE(Contains(root->members, "global_fn"));
    EXPECT_TRUE(Contains(root->members, "FEATURE_FLAG"));
    EXPECT_TRUE(Contains(root->members, "tools"));

    const auto* tools = find_scope({ "tools" });
    ASSERT_NE(tools, nullptr);
    EXPECT_TRUE(Contains(tools->members, "Widget"));
    EXPECT_TRUE(Contains(tools->members, "tool_fn"));
    EXPECT_TRUE(Contains(tools->members, "inner"));
    EXPECT_FALSE(Contains(tools->members, "deep_fn"));

    const auto* inner = find_scope({ "tools", "inner" });
    ASSERT_NE(inner, nullptr);
    EXPECT_TRUE(Contains(inner->members, "deep_fn"));
}

TEST(CompletionSpec, ExternalIndexFeedsQualifiedLookup)
{
    const heimdall::ScopeIndex external = {
        { { "std" }, heimdall::CompletionKind::Namespace,
          { { "vector", heimdall::CompletionKind::Type, "type" } } },
        { {}, heimdall::CompletionKind::Keyword, { { "printf", heimdall::CompletionKind::Function, "function" } } },
    };
    constexpr std::string_view use = "int x = std::vec;\n";
    const auto qualified =
        heimdall::CompletionEngine::Complete(use, heimdall::ParserOptions {}, use.size() - 2, &external);
    EXPECT_TRUE(Contains(qualified, "vector"));
    EXPECT_FALSE(Contains(qualified, "printf"));

    // Namespaced header members stay qualified-only: unqualified `vec` must
    // not offer `std::vector`...
    constexpr std::string_view plain_use = "int y = vec;\n";
    const auto plain = heimdall::CompletionEngine::Complete(plain_use, heimdall::ParserOptions {},
                                                           plain_use.size() - 2, &external);
    EXPECT_FALSE(Contains(plain, "vector"));
    // ...while header globals are visible unqualified.
    constexpr std::string_view global_use = "int y = print;\n";
    const auto globals = heimdall::CompletionEngine::Complete(global_use, heimdall::ParserOptions {},
                                                             global_use.size() - 2, &external);
    EXPECT_TRUE(Contains(globals, "printf"));
}

TEST(CompletionSpec, ClassifiesTypeAliasesAsTypes)
{
    constexpr std::string_view source =
        "template<class T> struct Basic {};\n"
        "typedef Basic<char> narrow;\n"
        "using wide = Basic<wchar_t>;\n"
        "int nar;\n";
    // Cursor past `;` (empty prefix) so both aliases match.
    const auto items = heimdall::CompletionEngine::Complete(source, source.size() - 1);
    const auto* narrow = Find(items, "narrow");
    ASSERT_NE(narrow, nullptr);
    EXPECT_EQ(narrow->kind, heimdall::CompletionKind::Type);
    const auto* wide = Find(items, "wide");
    ASSERT_NE(wide, nullptr);
    EXPECT_EQ(wide->kind, heimdall::CompletionKind::Type);
}

TEST(CompletionSpec, FunctionDetailShowsSignatureAndDocs)
{
    constexpr std::string_view source =
        "/// Adds two numbers.\n"
        "/// Returns their sum.\n"
        "int add(int left, int right) { return left + right; }\n"
        "int x = ad;\n";
    const auto items = heimdall::CompletionEngine::Complete(source, source.size() - 2);
    const auto* item = Find(items, "add");
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(item->detail, "int add(int left, int right)");
    EXPECT_EQ(item->documentation, "Adds two numbers.\nReturns their sum.");
}

TEST(CompletionSpec, VariableDetailShowsTypeAndDocs)
{
    constexpr std::string_view source =
        "int f() {\n"
        "  /// The running total.\n"
        "  long total = 0;\n"
        "  return tot;\n"
        "}\n";
    const std::size_t pos = source.rfind("tot") + 3;
    const auto items = heimdall::CompletionEngine::Complete(source, pos);
    const auto* item = Find(items, "total");
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(item->detail, "long");
    EXPECT_EQ(item->documentation, "The running total.");
}

TEST(CompletionSpec, BlankLineBreaksDocAttachment)
{
    constexpr std::string_view source =
        "/// Stale comment.\n"
        "\n"
        "int fresh = 1;\n"
        "int y = fre;\n";
    const auto items = heimdall::CompletionEngine::Complete(source, source.size() - 2);
    const auto* item = Find(items, "fresh");
    ASSERT_NE(item, nullptr);
    EXPECT_TRUE(item->documentation.empty());
}

TEST(CompletionSpec, MacroDetailShowsValueAndDocs)
{
    constexpr std::string_view source =
        "/// Maximum buffer size.\n"
        "#define LIMIT 1024\n"
        "int x = LIM;\n";
    const auto items = heimdall::CompletionEngine::Complete(source, source.size() - 2);
    const auto* item = Find(items, "LIMIT");
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(item->detail, "1024");
    EXPECT_EQ(item->documentation, "Maximum buffer size.");
}

TEST(CompletionSpec, NamespaceDetailAndDocs)
{
    constexpr std::string_view source =
        "/// Helpful tools.\n"
        "namespace tools {\n"
        "int run();\n"
        "}\n"
        "int x = tool;\n";
    const auto items = heimdall::CompletionEngine::Complete(source, source.size() - 2);
    const auto* item = Find(items, "tools");
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(item->kind, heimdall::CompletionKind::Namespace);
    EXPECT_EQ(item->detail, "namespace tools");
    EXPECT_EQ(item->documentation, "Helpful tools.");
}

TEST(CompletionSpec, StructTagDetailAndDocs)
{
    constexpr std::string_view source =
        "/// A small widget.\n"
        "struct Widget { int value; };\n"
        "Wid x;\n";
    const auto items = heimdall::CompletionEngine::Complete(source, source.size() - 5);
    const auto* item = Find(items, "Widget");
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(item->detail, "struct Widget");
    EXPECT_EQ(item->documentation, "A small widget.");
}

TEST(CompletionSpec, EnumMemberDetailShowsScope)
{
    constexpr std::string_view source =
        "enum class Color { Red, Green };\n"
        "Color c = Color::R;\n";
    const std::size_t pos = source.find("Color::R") + 8;
    const auto items = heimdall::CompletionEngine::Complete(source, pos);
    const auto* item = Find(items, "Red");
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(item->detail, "Color::Red");
}

TEST(CompletionSpec, HoverReturnsSignatureAndDocs)
{
    constexpr std::string_view source =
        "/// Adds two numbers.\n"
        "int add(int left, int right) { return left + right; }\n"
        "int x = add(1, 2);\n";
    // Mid-identifier counts as hovering the symbol.
    const std::size_t pos = source.rfind("add") + 1;
    const auto hovered = heimdall::CompletionEngine::Hover(source, heimdall::ParserOptions {}, pos);
    ASSERT_TRUE(hovered.has_value());
    EXPECT_EQ(hovered->label, "add");
    EXPECT_EQ(hovered->detail, "int add(int left, int right)");
    EXPECT_EQ(hovered->documentation, "Adds two numbers.");
}

TEST(CompletionSpec, HoverReturnsNullOffSymbol)
{
    constexpr std::string_view source = "int value = 1;\n";
    EXPECT_FALSE(heimdall::CompletionEngine::Hover(source, heimdall::ParserOptions {}, 3).has_value());
    EXPECT_FALSE(heimdall::CompletionEngine::Hover(source, heimdall::ParserOptions {}, 0).has_value());
    // Keywords carry no useful popup.
    constexpr std::string_view keyword = "int f() { return 0; }\n";
    EXPECT_FALSE(
        heimdall::CompletionEngine::Hover(keyword, heimdall::ParserOptions {}, 12).has_value());
}

TEST(CompletionSpec, TemplateTagDocsAttachAboveTemplateHead)
{
    constexpr std::string_view source =
        "/// A generic box.\n"
        "template<class T>\n"
        "struct Box { T value; };\n"
        "Box x;\n";
    const auto items = heimdall::CompletionEngine::Complete(source, source.size() - 5);
    const auto* item = Find(items, "Box");
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(item->detail, "struct Box");
    EXPECT_EQ(item->documentation, "A generic box.");
}

TEST(CompletionSpec, ResultsAreDeduplicatedAndSorted)
{
    constexpr std::string_view source = "int alpha = 1;\nint alpha = 2;\nint alp";
    const auto items = heimdall::CompletionEngine::Complete(source, source.size());
    std::size_t count = 0;
    for (const auto& item : items) count += item.label == "alpha";
    EXPECT_EQ(count, 1);
    EXPECT_TRUE(std::is_sorted(items.begin(), items.end(), [](const auto& left, const auto& right) {
        return left.label < right.label;
    }));
}
