#include <gtest/gtest.h>

#include <Heimdall/Completion.hpp>
#include <Heimdall/TypeLayout.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <string_view>
#include <vector>

namespace
{

    bool Contains(const std::vector<heimdall::CompletionItem>& items, std::string_view label)
    {
        for (const auto& item : items)
        {
            if (item.label == label)
            {
                return true;
            }
        }

        return false;
    }

    const heimdall::CompletionItem* Find(const std::vector<heimdall::CompletionItem>& items,
                                         std::string_view                             label)
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
    const auto                 items  = heimdall::CompletionEngine::Complete(source, source.size());
    EXPECT_TRUE(Contains(items, "return"));
    const auto* item = Find(items, "return");
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(item->kind, heimdall::CompletionKind::Keyword);
}

TEST(CompletionSpec, FiltersCandidatesByPrefix)
{
    constexpr std::string_view source = "int alpha = 1;\nint beta = 2;\nint al";
    const auto                 items  = heimdall::CompletionEngine::Complete(source, source.size());
    EXPECT_TRUE(Contains(items, "alpha"));
    EXPECT_FALSE(Contains(items, "beta"));
}

TEST(CompletionSpec, EmptyPrefixReturnsKeywords)
{
    constexpr std::string_view source = "int x = 1;\n";
    const auto                 items  = heimdall::CompletionEngine::Complete(source, source.size());
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

TEST(CompletionSpec, SuppressesMemberAccessOnReceiversItCannotType)
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
    const auto                 items  = heimdall::CompletionEngine::Complete(source, source.size());
    EXPECT_TRUE(Contains(items, "include"));
    const auto* item = Find(items, "include");
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(item->kind, heimdall::CompletionKind::Directive);
}

TEST(CompletionSpec, SuggestsDefinesAndPredefinedMacros)
{
    constexpr std::string_view source = "#define MY_FEATURE 1\nint x = MY_FEA";
    heimdall::ParserOptions    options;
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
    const auto                 items  = heimdall::CompletionEngine::Complete(source, source.size());
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
    const auto        params    = heimdall::CompletionEngine::Complete(source, param_pos);
    const auto*       param     = Find(params, "myParam");
    ASSERT_NE(param, nullptr) << "expected myParam among completions";
    EXPECT_EQ(param->kind, heimdall::CompletionKind::Variable);
    const std::size_t local_pos = source.rfind("myL") + 3;
    const auto        locals    = heimdall::CompletionEngine::Complete(source, local_pos);
    const auto*       local     = Find(locals, "myLocal");
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
    const std::size_t pos   = source.rfind("alpha_") + 6;
    const auto        items = heimdall::CompletionEngine::Complete(source, pos);
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
    const std::size_t pos   = source.find("late_") + 5;
    const auto        items = heimdall::CompletionEngine::Complete(source, pos);
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
    const auto        outer   = heimdall::CompletionEngine::Complete(source, outside);
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
    const std::size_t pos   = source.rfind("tool_i") + 6;
    const auto        items = heimdall::CompletionEngine::Complete(source, pos);
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
    const std::size_t deep       = source.find("deep_;") + 5;
    const auto        deep_items = heimdall::CompletionEngine::Complete(source, deep);
    EXPECT_TRUE(Contains(deep_items, "deep_item"));

    const std::size_t shallow       = source.find("shallow_;") + 8;
    const auto        shallow_items = heimdall::CompletionEngine::Complete(source, shallow);
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
    const std::size_t pos   = source.find("Color::R") + 8;
    const auto        items = heimdall::CompletionEngine::Complete(source, pos);
    EXPECT_TRUE(Contains(items, "Red"));
    EXPECT_FALSE(Contains(items, "Green"));
    EXPECT_FALSE(Contains(items, "compute"));

    constexpr std::string_view all_source =
        "enum class Color { Red, Green };\n"
        "int compute() { return 0; }\n"
        "Color c2 = Color::;\n";
    const std::size_t all_pos = all_source.find("Color::") + 7;
    const auto        all     = heimdall::CompletionEngine::Complete(all_source, all_pos);
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
    const std::size_t pos   = source.rfind("::g") + 3;
    const auto        items = heimdall::CompletionEngine::Complete(source, pos);
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
            if (scope.path == path)
            {
                return &scope;
            }
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
        { { "std" },
          heimdall::CompletionKind::Namespace,
          { { "vector", heimdall::CompletionKind::Type, "type" } } },
        { {},
          heimdall::CompletionKind::Keyword,
          { { "printf", heimdall::CompletionKind::Function, "function" } } },
    };
    constexpr std::string_view use       = "int x = std::vec;\n";
    const auto                 qualified = heimdall::CompletionEngine::Complete(
        use, heimdall::ParserOptions {}, use.size() - 2, &external);
    EXPECT_TRUE(Contains(qualified, "vector"));
    EXPECT_FALSE(Contains(qualified, "printf"));

    // Namespaced header members stay qualified-only: unqualified `vec` must
    // not offer `std::vector`...
    constexpr std::string_view plain_use = "int y = vec;\n";
    const auto                 plain     = heimdall::CompletionEngine::Complete(
        plain_use, heimdall::ParserOptions {}, plain_use.size() - 2, &external);
    EXPECT_FALSE(Contains(plain, "vector"));
    // ...while header globals are visible unqualified.
    constexpr std::string_view global_use = "int y = print;\n";
    const auto                 globals    = heimdall::CompletionEngine::Complete(
        global_use, heimdall::ParserOptions {}, global_use.size() - 2, &external);
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
    const auto  items  = heimdall::CompletionEngine::Complete(source, source.size() - 1);
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
    const auto  items = heimdall::CompletionEngine::Complete(source, source.size() - 2);
    const auto* item  = Find(items, "add");
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
    const std::size_t pos   = source.rfind("tot") + 3;
    const auto        items = heimdall::CompletionEngine::Complete(source, pos);
    const auto*       item  = Find(items, "total");
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
    const auto  items = heimdall::CompletionEngine::Complete(source, source.size() - 2);
    const auto* item  = Find(items, "fresh");
    ASSERT_NE(item, nullptr);
    EXPECT_TRUE(item->documentation.empty());
}

TEST(CompletionSpec, MacroDetailShowsValueAndDocs)
{
    constexpr std::string_view source =
        "/// Maximum buffer size.\n"
        "#define LIMIT 1024\n"
        "int x = LIM;\n";
    const auto  items = heimdall::CompletionEngine::Complete(source, source.size() - 2);
    const auto* item  = Find(items, "LIMIT");
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
    const auto  items = heimdall::CompletionEngine::Complete(source, source.size() - 2);
    const auto* item  = Find(items, "tools");
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(item->kind, heimdall::CompletionKind::Namespace);
    EXPECT_EQ(item->detail, "namespace tools");
    EXPECT_EQ(item->documentation, "Helpful tools.");
}

TEST(CompletionSpec, CompoundHeaderNamespaceKeepsNamespaceKind)
{
    const auto index = heimdall::CompletionEngine::IndexScopes(
        "namespace heimdall::cli { struct Options {}; int Run(); }", {});
    constexpr std::string_view source = "heimdall::cli::Options options;";
    for (const auto& scope : index)
    {
        if (scope.path == std::vector<std::string> { "heimdall", "cli" })
        {
            EXPECT_EQ(scope.kind, heimdall::CompletionKind::Namespace);
        }
    }

    const auto hover =
        heimdall::CompletionEngine::Hover(source, {}, source.find("cli") + 1, &index);
    ASSERT_TRUE(hover.has_value());
    EXPECT_EQ(hover->kind, heimdall::CompletionKind::Namespace);
    EXPECT_EQ(hover->detail, "namespace heimdall::cli");
}

TEST(CompletionSpec, StructTagDetailAndDocs)
{
    constexpr std::string_view source =
        "/// A small widget.\n"
        "struct Widget { int value; };\n"
        "Wid x;\n";
    const auto  items = heimdall::CompletionEngine::Complete(source, source.size() - 5);
    const auto* item  = Find(items, "Widget");
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(item->detail, "struct Widget");
    EXPECT_EQ(item->documentation, "A small widget.");
}

TEST(CompletionSpec, EnumMemberDetailShowsScope)
{
    constexpr std::string_view source =
        "enum class Color { Red, Green };\n"
        "Color c = Color::R;\n";
    const std::size_t pos   = source.find("Color::R") + 8;
    const auto        items = heimdall::CompletionEngine::Complete(source, pos);
    const auto*       item  = Find(items, "Red");
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
    EXPECT_FALSE(
        heimdall::CompletionEngine::Hover(source, heimdall::ParserOptions {}, 10).has_value());
    EXPECT_FALSE(
        heimdall::CompletionEngine::Hover(source, heimdall::ParserOptions {}, 11).has_value());
    // Control-flow keywords carry no useful popup.
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
    const auto  items = heimdall::CompletionEngine::Complete(source, source.size() - 5);
    const auto* item  = Find(items, "Box");
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(item->detail, "struct Box");
    EXPECT_EQ(item->documentation, "A generic box.");
}

TEST(CompletionSpec, ResultsAreDeduplicatedAndSorted)
{
    constexpr std::string_view source = "int alpha = 1;\nint alpha = 2;\nint alp";
    const auto                 items  = heimdall::CompletionEngine::Complete(source, source.size());
    std::size_t                count  = 0;
    for (const auto& item : items)
    {
        count += item.label == "alpha";
    }

    EXPECT_EQ(count, 1);
    EXPECT_TRUE(std::is_sorted(items.begin(), items.end(), [](const auto& left, const auto& right) {
        return left.label < right.label;
    }));
}

namespace
{

    // `|` marks the cursor. Returns the labels offered there.
    std::vector<std::string> MemberLabels(std::string                 source,
                                          const heimdall::ScopeIndex* external = nullptr)
    {
        const std::size_t cursor = source.find('|');
        source.erase(cursor, 1);
        std::vector<std::string> labels;
        for (const auto& item : heimdall::CompletionEngine::Complete(source, {}, cursor, external))
        {
            labels.push_back(item.label);
        }

        return labels;
    }

    bool Has(const std::vector<std::string>& labels, std::string_view label)
    {
        return std::find(labels.begin(), labels.end(), label) != labels.end();
    }

    constexpr std::string_view kStdLike =
        "namespace std {\n"
        "template<class T> class vector { public: void push_back(const T&); void clear(); T& "
        "front(); unsigned size() const; };\n"
        "class basic_string { public: basic_string(); void resize(int); void clear(); };\n"
        "typedef basic_string string;\n"
        "template<class T> class unique_ptr { public: T* get(); void reset(); };\n"
        "}\n";

} // namespace

TEST(CompletionSpec, MemberAccessListsMembersOfLocalAndParameterTypes)
{
    const auto dot =
        MemberLabels("struct P { int x; void run(); private: int y; void hid(); P(); ~P(); };\n"
                     "void f() { P p; p.| }\n");
    EXPECT_TRUE(Has(dot, "x"));
    EXPECT_TRUE(Has(dot, "run"));
    EXPECT_TRUE(Has(dot, "y"));
    EXPECT_TRUE(Has(dot, "hid"));
    EXPECT_FALSE(Has(dot, "P")); // constructors are not members you can name

    const auto arrow = MemberLabels("struct P { int x; void run(); };\nvoid f(P* p) { p->r| }\n");
    EXPECT_EQ(arrow, std::vector<std::string> { "run" });
    const auto reference = MemberLabels("struct P { int x; };\nvoid f(const P& r) { r.| }\n");
    EXPECT_TRUE(Has(reference, "x"));
    EXPECT_TRUE(MemberLabels("struct P { int x; };\nvoid f(const P& r, int z) { z.| }\n").empty());
}

TEST(CompletionSpec, MemberAccessIncludesInheritedMembers)
{
    const auto labels =
        MemberLabels("struct B { int bx; void bm(); };\nstruct M : public B { int mx; };\n"
                     "struct D : M { int dx; };\nvoid f() { D d; d.| }\n");
    EXPECT_TRUE(Has(labels, "dx"));
    EXPECT_TRUE(Has(labels, "mx"));
    EXPECT_TRUE(Has(labels, "bx"));
    EXPECT_TRUE(Has(labels, "bm"));
}

TEST(CompletionSpec, MemberAccessFollowsChainsCallsAndSubscripts)
{
    const std::string types = "struct I { int deep; };\nstruct O { I in; I get(); I* ptr(); };\n";
    EXPECT_TRUE(Has(MemberLabels(types + "void f() { O o; o.in.| }\n"), "deep"));
    EXPECT_TRUE(Has(MemberLabels(types + "void f() { O o; o.get().| }\n"), "deep"));
    EXPECT_TRUE(Has(MemberLabels(types + "void f() { O o; o.ptr()->| }\n"), "deep"));
    EXPECT_TRUE(Has(MemberLabels(types + "void f(O* o) { o->in.| }\n"), "deep"));
}

TEST(CompletionSpec, MemberAccessResolvesAliasesNamespacesAndAuto)
{
    EXPECT_TRUE(
        Has(MemberLabels("struct P { int x; };\nusing Q = P;\nvoid f() { Q q; q.| }\n"), "x"));
    EXPECT_TRUE(
        Has(MemberLabels("struct P { int x; };\ntypedef P R;\nvoid f() { R r; r.| }\n"), "x"));
    EXPECT_TRUE(
        Has(MemberLabels("namespace n { struct P { int x; }; }\nvoid f() { n::P a; a.| }\n"), "x"));
    EXPECT_TRUE(Has(MemberLabels("namespace n { struct P { int x; }; }\nusing namespace n;\n"
                                 "void f() { P a; a.| }\n"),
                    "x"));
    EXPECT_TRUE(
        Has(MemberLabels(
                "namespace n { struct P { int x; }; }\nnamespace n { void f() { P a; a.| } }\n"),
            "x"));
    EXPECT_TRUE(Has(MemberLabels("struct P { int x; };\nvoid f() { auto a = P(); a.| }\n"), "x"));
    EXPECT_TRUE(
        Has(MemberLabels("struct P { int x; };\nvoid f() { auto a = new P(); a->| }\n"), "x"));
    EXPECT_TRUE(Has(MemberLabels("struct P { int x; };\nP g;\nvoid f() { g.| }\n"), "x"));
}

TEST(CompletionSpec, MemberAccessOnThisAndImplicitFields)
{
    const auto labels = MemberLabels("struct I { int deep; };\n"
                                     "struct P { int x; I in; void m(); };\n"
                                     "void P::m() { this->| }\n");
    EXPECT_TRUE(Has(labels, "x"));
    EXPECT_TRUE(Has(labels, "m"));
    EXPECT_TRUE(
        Has(MemberLabels("struct I { int deep; };\nstruct P { I in; void m() { in.| } };\n"),
            "deep"));
    EXPECT_TRUE(
        Has(MemberLabels(
                "struct I { int deep; };\nstruct P { I in; void m(); };\nvoid P::m() { in.| }\n"),
            "deep"));
}

TEST(CompletionSpec, MemberAccessUsesTheHeaderIndexForStandardLikeTypes)
{
    const heimdall::ScopeIndex index = heimdall::CompletionEngine::IndexScopes(kStdLike, {});

    // `std::string` is a typedef of another record: resolved through the alias.
    const auto text = MemberLabels("void f() { std::string body; body.| }\n", &index);
    EXPECT_TRUE(Has(text, "resize"));
    EXPECT_TRUE(Has(text, "clear"));
    EXPECT_EQ(MemberLabels("void f() { std::string s; s.cl| }\n", &index),
              std::vector<std::string> { "clear" });
    EXPECT_TRUE(
        Has(MemberLabels("using namespace std;\nvoid f() { string s; s.| }\n", &index), "resize"));
    EXPECT_TRUE(Has(MemberLabels("void f() { auto s = std::string(); s.| }\n", &index), "resize"));

    const auto vector = MemberLabels("void f() { std::vector<int> v; v.| }\n", &index);
    EXPECT_TRUE(Has(vector, "push_back"));
    EXPECT_TRUE(Has(vector, "size"));

    // Member of a header type that is itself a header type.
    EXPECT_TRUE(
        Has(MemberLabels("struct P { std::string name; };\nvoid f(P p) { p.name.| }\n", &index),
            "resize"));
}

TEST(CompletionSpec, MemberAccessThroughSmartPointersAndContainers)
{
    const heimdall::ScopeIndex index = heimdall::CompletionEngine::IndexScopes(kStdLike, {});
    const std::string          p     = "struct P { int x; };\n";
    const auto arrow = MemberLabels(p + "void f() { std::unique_ptr<P> u; u->| }\n", &index);
    EXPECT_TRUE(Has(arrow, "x"));
    EXPECT_FALSE(Has(arrow, "reset"));
    const auto dot = MemberLabels(p + "void f() { std::unique_ptr<P> u; u.| }\n", &index);
    EXPECT_TRUE(Has(dot, "reset"));
    EXPECT_FALSE(Has(dot, "x"));
    EXPECT_TRUE(Has(MemberLabels(p + "void f() { std::vector<P> v; v[0].| }\n", &index), "x"));
}

TEST(CompletionSpec, MemberAccessOnRawPointerRejectsDotOperator)
{
    const std::string p = "struct P { int x; };\n";
    // Arrow on raw pointer works
    const auto arrow = MemberLabels(p + "void f() { P* ptr = nullptr; ptr->| }\n", nullptr);
    EXPECT_TRUE(Has(arrow, "x"));
    // Dot on raw pointer should NOT offer pointee members
    const auto dot = MemberLabels(p + "void f() { P* ptr = nullptr; ptr.| }\n", nullptr);
    EXPECT_FALSE(Has(dot, "x"));
    // Dot on raw pointer should not crash and should return empty (no members)
    EXPECT_TRUE(dot.empty());
}

TEST(CompletionSpec, IndexRecordsAliasTargetsAndBases)
{
    const auto index = heimdall::CompletionEngine::IndexScopes(
        "struct B { int b; };\nstruct D : public B, private ns::Other<int> { };\nusing Alias = "
        "std::vector<int>;\n",
        {});
    bool saw_bases = false;
    bool saw_alias = false;
    for (const auto& scope : index)
    {
        if (scope.path == std::vector<std::string> { "D" })
        {
            saw_bases = scope.bases == std::vector<std::string> { "B", "ns::Other" };
        }

        for (const auto& member : scope.members)
        {
            saw_alias =
                saw_alias || (member.label == "Alias" && member.type_text == "std::vector<int>");
        }
    }

    EXPECT_TRUE(saw_bases);
    EXPECT_TRUE(saw_alias);
}

TEST(CompletionSpec, HoverKeepsDocCommentWhenAForwardDeclarationIsIndexedToo)
{
    // A bare forward declaration (no comment) used to win over the documented
    // definition and wipe the doc from the hover.
    const auto index = heimdall::CompletionEngine::IndexScopes(
        "namespace n { class element; }\n"
        "namespace n {\n"
        "/**\n * A JSON element.\n */\n"
        "class element { public: int v; };\n"
        "}\n",
        {});
    const std::string source = "n::element value;";
    const auto        hover  = heimdall::CompletionEngine::Hover(source, {}, 5, &index);
    ASSERT_TRUE(hover.has_value());
    EXPECT_EQ(hover->label, "element");
    EXPECT_NE(hover->documentation.find("A JSON element."), std::string::npos);
}

TEST(CompletionSpec, HoverShowsPlainCommentsAboveEnumsAndEnumerators)
{
    const std::string header =
        "namespace n {\n"
        "    // How single-statement blocks are treated.\n"
        "    // Second line.\n"
        "    enum class Style : unsigned char\n"
        "    {\n"
        "        // Leave them as written.\n"
        "        Keep,\n"
        "        // Collapse onto one line.\n"
        "        SingleLine,\n"
        "    };\n"
        "}\n";
    const std::string use = "void f() { n::Style s = n::Style::Keep; }\n";
    for (const std::string source : { header + use })
    {
        auto hover = [&](std::string_view needle, const heimdall::ScopeIndex* index) {
            return heimdall::CompletionEngine::Hover(source, {}, source.find(needle) + 1, index);
        };
        const auto enumeration = hover("Style :", nullptr);
        ASSERT_TRUE(enumeration.has_value());
        EXPECT_EQ(enumeration->documentation,
                  "How single-statement blocks are treated.\nSecond line.");
        const auto keep = hover("Keep,", nullptr);
        ASSERT_TRUE(keep.has_value());
        EXPECT_EQ(keep->documentation, "Leave them as written.");
        const auto second = hover("SingleLine,", nullptr);
        ASSERT_TRUE(second.has_value());
        EXPECT_EQ(second->documentation, "Collapse onto one line.");
        // Used elsewhere: the type reads as an enum, not a namespace.
        const auto used = hover("Style s", nullptr);
        ASSERT_TRUE(used.has_value());
        EXPECT_EQ(used->detail, "enum class n::Style");
        EXPECT_NE(used->documentation.find("How single-statement"), std::string::npos);
    }

    // Same through the header index.
    const auto        index  = heimdall::CompletionEngine::IndexScopes(header, {});
    const std::string source = use;
    const auto        indexed =
        heimdall::CompletionEngine::Hover(source, {}, source.find("Style s") + 1, &index);
    ASSERT_TRUE(indexed.has_value());
    EXPECT_NE(indexed->documentation.find("How single-statement"), std::string::npos);
    const auto keep =
        heimdall::CompletionEngine::Hover(source, {}, source.find("Keep") + 1, &index);
    ASSERT_TRUE(keep.has_value());
    EXPECT_EQ(keep->documentation, "Leave them as written.");
}

TEST(CompletionSpec, HoverShowsPlainCommentsAboveMethodsAndFields)
{
    const std::string source =
        "class Tree\n"
        "{\n"
        "public:\n"
        "    // Cooperative cancellation: polls `stop`\n"
        "    // between items.\n"
        "    static Tree Parse(int source,\n"
        "        int stop);\n"
        "    int count; // trailing note, not documentation\n"
        "    // Number of nodes.\n"
        "    int nodes;\n"
        "};\n"
        "void f() { Tree t; t.nodes; Tree::Parse(1, 2); t.count; }\n";
    auto hover = [&](std::string_view needle, std::size_t skip = 1) {
        return heimdall::CompletionEngine::Hover(source, {}, source.find(needle) + skip, nullptr);
    };
    const auto method = hover("Parse(int");
    ASSERT_TRUE(method.has_value());
    EXPECT_EQ(method->documentation, "Cooperative cancellation: polls `stop`\nbetween items.");
    const auto field = hover("nodes;");
    ASSERT_TRUE(field.has_value());
    EXPECT_EQ(field->documentation, "Number of nodes.");
    // After `.` and `::` (member access / qualified lookup).
    const auto member = hover("t.nodes", 3);
    ASSERT_TRUE(member.has_value());
    EXPECT_EQ(member->documentation, "Number of nodes.");
    const auto qualified = hover("Tree::Parse(1", 7);
    ASSERT_TRUE(qualified.has_value());
    EXPECT_EQ(qualified->documentation, "Cooperative cancellation: polls `stop`\nbetween items.");
    // A comment trailing the previous declaration documents that one only.
    const auto trailing = hover("count;");
    ASSERT_TRUE(trailing.has_value());
    EXPECT_TRUE(trailing->documentation.empty());
}

TEST(CompletionSpec, HoverOnClassQualifyingAMethodDefinition)
{
    const std::string_view source =
        "namespace app {\n"
        "class Server {\n"
        "public:\n"
        "    void Work(int stop);\n"
        "};\n"
        "void Server::Work(int stop) {}\n"
        "}\n";
    const auto hovered =
        heimdall::CompletionEngine::Hover(source, {}, source.find("Server::Work") + 2, nullptr);
    ASSERT_TRUE(hovered.has_value());
    EXPECT_EQ(hovered->label, "Server");
    EXPECT_EQ(hovered->kind, heimdall::CompletionKind::Type);
}

TEST(CompletionSpec, HoverOnStructQualifyingAMethodDefinitionAtGlobalScope)
{
    const std::string_view source =
        "struct Box { int Get() const; };\n"
        "int Box::Get() const { return 1; }\n";
    const auto hovered =
        heimdall::CompletionEngine::Hover(source, {}, source.find("Box::Get") + 1, nullptr);
    ASSERT_TRUE(hovered.has_value());
    EXPECT_EQ(hovered->label, "Box");
}

TEST(CompletionSpec, HoverOnHeaderClassQualifyingAMethodDefinition)
{
    const auto index = heimdall::CompletionEngine::IndexScopes(
        "namespace heimdall::lsp {\n"
        "class LanguageServer {\n"
        "public:\n"
        "    void DiagWorkerMain(int stop);\n"
        "};\n"
        "}\n",
        {});
    const std::string_view source =
        "namespace heimdall::lsp\n"
        "{\n"
        "    void LanguageServer::DiagWorkerMain(int stop)\n"
        "    {\n"
        "    }\n"
        "}\n";
    const auto hovered =
        heimdall::CompletionEngine::Hover(source, {}, source.find("LanguageServer::") + 3, &index);
    ASSERT_TRUE(hovered.has_value());
    EXPECT_EQ(hovered->label, "LanguageServer");
}

TEST(CompletionSpec, HeaderNamespaceMembersVisibleInNestedNamespaceOnly)
{
    const auto index = heimdall::CompletionEngine::IndexScopes(
        "namespace heimdall::lsp {\nclass LanguageServer {};\n}\n", {});
    auto hover = [&](std::string_view source) {
        return heimdall::CompletionEngine::Hover(
            source, {}, source.find("LanguageServer") + 3, &index);
    };

    EXPECT_TRUE(
        hover("namespace heimdall::lsp { namespace detail { LanguageServer* p; } }\n").has_value());
    EXPECT_TRUE(hover("namespace heimdall { namespace lsp { LanguageServer* p; } }\n").has_value());
    // Outside the namespace (or in a sibling one) the name stays qualified-only.
    EXPECT_FALSE(hover("LanguageServer* p;\n").has_value());
    EXPECT_FALSE(hover("namespace heimdall { LanguageServer* p; }\n").has_value());
    EXPECT_FALSE(hover("namespace other::lsp { LanguageServer* p; }\n").has_value());
}

TEST(CompletionSpec, HoverResolvesAutoFromTheCalledFunctionReturnType)
{
    const auto index = heimdall::CompletionEngine::IndexScopes(
        "namespace lib {\n"
        "struct Config {};\n"
        "std::expected<Config, std::string> Load(const char *path);\n"
        "const Config& Shared();\n"
        "}\n",
        {});
    const std::string source =
        "void run() {\n"
        "    auto loaded = lib::Load(\"x\");\n"
        "    const auto& shared = lib::Shared();\n"
        "    auto copy = lib::Shared();\n"
        "    auto made = lib::Config();\n"
        "    auto unknown = other();\n"
        "}\n";
    const auto detail = [&](const char* needle) {
        const auto hover =
            heimdall::CompletionEngine::Hover(source, {}, source.find(needle) + 1, &index);
        return hover.has_value() ? hover->detail : std::string("<none>");
    };
    EXPECT_EQ(detail("loaded"), "std::expected<Config, std::string>");
    EXPECT_EQ(detail("shared"), "const Config"); // the grammar drops the `&` of the declared type
    EXPECT_EQ(detail("copy"), "Config");
    EXPECT_EQ(detail("made"), "lib::Config");
    EXPECT_EQ(detail("unknown"), "auto");
}

TEST(CompletionSpec, MemberAccessOnAutoFromAHeaderFunctionReturningAStdType)
{
    const heimdall::ScopeIndex index = heimdall::CompletionEngine::IndexScopes(
        "namespace std {\n"
        "template<class T, class E> class expected { public: bool has_value() const; T& value(); "
        "E& error(); T* operator->(); };\n"
        "class string {};\n"
        "}\n"
        "namespace heimdall {\n"
        "struct RuleConfiguration { bool root; };\n"
        "std::expected<RuleConfiguration, std::string> LoadRuleConfiguration(const char *path);\n"
        "}\n",
        {});
    const std::string body = "void run() {\n    auto loaded = "
                             "heimdall::LoadRuleConfiguration(\"x\");\n    loaded.|\n}\n";
    const auto dot = MemberLabels(body, &index);
    EXPECT_TRUE(Has(dot, "has_value"));
    EXPECT_TRUE(Has(dot, "value"));
    const auto inside = MemberLabels("namespace heimdall {\n" + body + "}\n", &index);
    EXPECT_TRUE(Has(inside, "has_value"));
    // `->` reaches the held value.
    EXPECT_TRUE(Has(MemberLabels("void run() {\n    auto loaded = "
                                 "heimdall::LoadRuleConfiguration(\"x\");\n    loaded->|\n}\n",
                                 &index),
                    "root"));
}

TEST(CompletionSpec, IndexesRecordsWithAttributesBetweenKeywordAndName)
{
    const auto index = heimdall::CompletionEngine::IndexScopes(
        "namespace std {\n"
        "template<class T, class E> class expected;\n"
        "template<class T, class E>\n"
        "  class [[nodiscard]] expected { public: bool has_value() const; };\n"
        "struct [[deprecated(\"x\")]] Old { int a; };\n"
        "class alignas(8) Aligned { public: int b; };\n"
        "}\n",
        {});
    const auto members = [&](const std::vector<std::string>& path) {
        std::vector<std::string> labels;
        for (const auto& scope : index)
        {
            if (scope.path == path)
            {
                for (const auto& member : scope.members)
                {
                    labels.push_back(member.label);
                }
            }
        }

        return labels;
    };
    EXPECT_TRUE(Has(members({ "std", "expected" }), "has_value"));
    EXPECT_TRUE(Has(members({ "std", "Old" }), "a"));
    EXPECT_TRUE(Has(members({ "std", "Aligned" }), "b"));
}

namespace
{

    const heimdall::ScopeIndex& ChainIndex()
    {
        static const heimdall::ScopeIndex index = heimdall::CompletionEngine::IndexScopes(
            "namespace std {\n"
            "template<class T, class E> class expected { public: bool has_value() const; T& "
            "value(); E& error(); T* operator->(); };\n"
            "template<class T> class vector { public: T& front(); unsigned size() const; };\n"
            "class string { public: unsigned length() const; };\n"
            "}\n"
            "namespace lib {\n"
            "struct Item { int id; std::string name() const; };\n"
            "struct Repo { std::vector<Item> items(); Item& first(); static Repo& instance();\n"
            "              std::expected<Item, std::string> find(int); };\n"
            "Repo& GetRepo();\n"
            "Repo MakeRepo(int);\n"
            "}\n",
            {});
        return index;
    }

    // `statement` declares `a` inside a function; returns what `a.` completes to and how `a`
    // hovers.
    std::pair<std::vector<std::string>, std::string> AutoChain(const std::string& statement)
    {
        const std::string        source = "void run() {\n    " + statement + "\n    a.\n}\n";
        const auto               cursor = source.find("a.\n}") + 2;
        std::vector<std::string> labels;
        for (const auto& item :
             heimdall::CompletionEngine::Complete(source, {}, cursor, &ChainIndex()))
        {
            labels.push_back(item.label);
        }

        const auto hover =
            heimdall::CompletionEngine::Hover(source, {}, source.find("a ="), &ChainIndex());
        return { labels, hover.has_value() ? hover->detail : std::string("<none>") };
    }

} // namespace

TEST(CompletionSpec, AutoFollowsChainedCallsForCompletionAndHover)
{
    const auto first = AutoChain("auto a = lib::GetRepo().first();");
    EXPECT_TRUE(Has(first.first, "name"));
    EXPECT_EQ(first.second, "Item");

    const auto nested = AutoChain("auto a = lib::MakeRepo(2).first().name();");
    EXPECT_TRUE(Has(nested.first, "length"));
    EXPECT_EQ(nested.second, "std::string");

    const auto statics = AutoChain("auto a = lib::Repo::instance().first();");
    EXPECT_TRUE(Has(statics.first, "id"));
    EXPECT_EQ(statics.second, "Item");

    const auto variable = AutoChain("lib::Repo r; auto a = r.find(1)->name();");
    EXPECT_TRUE(Has(variable.first, "length"));
    EXPECT_EQ(variable.second, "std::string");

    const auto through_auto =
        AutoChain("auto r = lib::GetRepo(); auto b = r.first(); auto a = b.name();");
    EXPECT_TRUE(Has(through_auto.first, "length"));
    EXPECT_EQ(through_auto.second, "std::string");
}

TEST(CompletionSpec, AutoChainsSubstituteTheTemplateArgumentsOfTheReceiver)
{
    const auto value = AutoChain("auto a = lib::GetRepo().find(1).value();");
    EXPECT_TRUE(Has(value.first, "id"));
    EXPECT_EQ(value.second, "Item");

    const auto front = AutoChain("lib::Repo r; auto a = r.items().front();");
    EXPECT_TRUE(Has(front.first, "name"));
    EXPECT_EQ(front.second, "Item");

    const auto expected = AutoChain("auto a = lib::GetRepo().find(1);");
    EXPECT_TRUE(Has(expected.first, "has_value"));
    EXPECT_EQ(expected.second, "std::expected<Item, std::string>");
}

TEST(CompletionSpec, AutoChainsThroughParenthesesDereferencesAndNamedCasts)
{
    EXPECT_TRUE(Has(AutoChain("auto a = (lib::GetRepo()).first();").first, "name"));
    EXPECT_EQ(AutoChain("auto a = (lib::GetRepo()).first();").second, "Item");
    EXPECT_TRUE(Has(AutoChain("auto a = ((lib::GetRepo())).first();").first, "name"));
    EXPECT_TRUE(Has(AutoChain("auto a = (lib::GetRepo().first()).name();").first, "length"));
    EXPECT_TRUE(
        Has(AutoChain("std::vector<lib::Item> v; auto a = (v.front()).name();").first, "length"));
    // `(*p)` goes through a pointer-like; `(p)->` too.
    EXPECT_TRUE(Has(AutoChain("std::expected<lib::Item, std::string> p = lib::GetRepo().find(1);"
                              " auto a = (*p).name();")
                        .first,
                    "length"));
    EXPECT_TRUE(Has(AutoChain("std::expected<lib::Item, std::string> p = lib::GetRepo().find(1);"
                              " auto a = (p)->name();")
                        .first,
                    "length"));
    EXPECT_TRUE(Has(AutoChain("lib::Repo* r; auto a = (*r).first();").first, "name"));
    EXPECT_TRUE(
        Has(AutoChain("auto a = static_cast<lib::Repo&>(lib::GetRepo()).first();").first, "name"));
    EXPECT_EQ(AutoChain("auto a = static_cast<lib::Repo&>(lib::GetRepo()).first();").second,
              "Item");
    EXPECT_TRUE(Has(AutoChain("void* v; auto a = reinterpret_cast<lib::Item*>(v)->name();").first,
                    "length"));
    // Not a plain receiver: stays unresolved rather than guessing.
    EXPECT_TRUE(AutoChain("int x; auto a = (x + 1).first();").first.empty());
}

TEST(CompletionSpec, AutoChainsThroughCStyleCastsOnlyWhenTheTypeIsKnown)
{
    EXPECT_TRUE(Has(AutoChain("void* v; auto a = ((lib::Item*)v)->name();").first, "length"));
    EXPECT_TRUE(Has(AutoChain("void* v; auto a = ((const lib::Repo&)v).first();").first, "name"));
    EXPECT_EQ(AutoChain("void* v; auto a = ((lib::Repo*)v)->first();").second, "Item");
    // A type the index does not hold: no guess.
    EXPECT_TRUE(AutoChain("void* v; auto a = ((Unknown*)v)->first();").first.empty());
    // `(x)*p` with a variable `x` is a product, not a cast to `x`.
    EXPECT_TRUE(AutoChain("lib::Item x; lib::Item* p; auto a = ((x)*p).name();").first.empty());
}

TEST(CompletionSpec, TemplateParametersAreFoundBehindDeclarationSpecifiers)
{
    // libstdc++ spells it `constexpr const _Tp& value() const &`: the index keeps the specifiers.
    const auto index = heimdall::CompletionEngine::IndexScopes(
        "namespace std {\n"
        "template<typename _Tp, typename _Er> class [[nodiscard]] expected {\n"
        "public:\n"
        "  constexpr const _Tp& value() const &;\n"
        "  constexpr _Tp&& value() &&;\n"
        "  constexpr bool has_value() const noexcept;\n"
        "  static inline _Tp* make();\n"
        "};\n"
        "class string {};\n"
        "}\n"
        "namespace lib {\n"
        "struct Config { bool root; };\n"
        "std::expected<Config, std::string> Load(const char *path);\n"
        "}\n",
        {});
    const auto labels = [&](const std::string& statement) {
        const std::string source = "void run() {\n    " + statement + "\n    loaded.value().\n}\n";
        const auto        cursor = source.find("value().") + 8;
        std::vector<std::string> result;
        for (const auto& item : heimdall::CompletionEngine::Complete(source, {}, cursor, &index))
        {
            result.push_back(item.label);
        }

        return result;
    };
    EXPECT_TRUE(Has(labels("auto loaded = lib::Load(\"x\");"), "root"));
    EXPECT_TRUE(
        Has(labels("std::expected<lib::Config, std::string> loaded = lib::Load(\"x\");"), "root"));
}

TEST(CompletionSpec, HoverOfAutoFromANewExpressionIsAPointer)
{
    const std::string source =
        "struct Node { Node* next; int value; };\n"
        "int main() {\n"
        "    auto root = new Node{ .next = 0, .value = 10 };\n"
        "    auto plain = Node{ .next = 0, .value = 1 };\n"
        "}\n";
    const auto detail = [&](const char* needle) {
        const auto hover = heimdall::CompletionEngine::Hover(source, {}, source.find(needle) + 1);
        return hover.has_value() ? hover->detail : std::string("<none>");
    };
    EXPECT_EQ(detail("root"), "Node*");
    EXPECT_EQ(detail("plain"), "Node");
}

TEST(CompletionSpec, DesignatorsCompleteTheMembersOfTheInitializedClass)
{
    const auto labels = [](std::string source) {
        const auto at = source.find('|');
        source.erase(at, 1);
        return heimdall::CompletionEngine::Complete(source, at);
    };
    const std::string node = "struct Node { Node* next; int value; void run(); };\n";

    for (const char* form :
         { "void f() { auto p = new Node{ .| }; }\n", "void f() { Node n{ .| }; }\n",
           "void f() { Node n = { .| }; }\n", "void f() { auto n = Node{ .| }; }\n" })
    {
        const auto items = labels(node + form);
        EXPECT_TRUE(Contains(items, "next")) << form;
        EXPECT_TRUE(Contains(items, "value")) << form;
        EXPECT_FALSE(Contains(items, "run")) << form; // only data members are designators
    }

    // After a comma and with a prefix.
    const auto later = labels(node + "void f() { Node n{ .next = 0,\n .va| }; }\n");
    EXPECT_TRUE(Contains(later, "value"));
    EXPECT_FALSE(Contains(later, "next"));

    // Members already designated, before or after the cursor, are not offered again.
    const auto rest = labels(node + "void f() { auto p = new Node{\n .next = 0,\n .|\n }; }\n");
    EXPECT_TRUE(Contains(rest, "value"));
    EXPECT_FALSE(Contains(rest, "next"));
    const auto ahead = labels(node + "void f() { Node n{ .|, .value = 1 }; }\n");
    EXPECT_TRUE(Contains(ahead, "next"));
    EXPECT_FALSE(Contains(ahead, "value"));

    // A member access after an expression is not a designator.
    const auto access = labels(node + "void f(Node n) { int x = n.| }\n");
    EXPECT_TRUE(Contains(access, "run"));
}

namespace
{
    struct HoverLayout
    {
        bool          known = false;
        std::uint64_t size  = 0;
        std::uint64_t align = 0;
    };

    // Hovers the last occurrence of `name` in `source`.
    HoverLayout LayoutAtHover(std::string_view source, std::string_view name,
                              const heimdall::ScopeIndex* external = nullptr)
    {
        const auto hovered = heimdall::CompletionEngine::Hover(
            source, heimdall::ParserOptions {}, source.rfind(name) + 1, external);
        if (!hovered.has_value())
        {
            return {};
        }

        return { hovered->has_layout, hovered->size_bytes, hovered->align_bytes };
    }
} // namespace

TEST(CompletionSpec, HoverReportsSizeAndAlignmentOfFundamentalVariables)
{
    const auto integer = LayoutAtHover("int value = 1;\n", "value");
    ASSERT_TRUE(integer.known);
    EXPECT_EQ(integer.size, 4u);
    EXPECT_EQ(integer.align, 4u);

    const auto real = LayoutAtHover("double ratio = 1.0;\n", "ratio");
    ASSERT_TRUE(real.known);
    EXPECT_EQ(real.size, 8u);
    EXPECT_EQ(real.align, 8u);

    const auto flag = LayoutAtHover("unsigned char byte_value = 0;\n", "byte_value");
    ASSERT_TRUE(flag.known);
    EXPECT_EQ(flag.size, 1u);

    const auto wide = LayoutAtHover("long long big = 0;\n", "big");
    ASSERT_TRUE(wide.known);
    EXPECT_EQ(wide.size, 8u);
}

TEST(CompletionSpec, HoverReportsLayoutOnBuiltinTypeTokens)
{
    constexpr std::string_view source = "bool json = false; int count = 0; double ratio = 0;";
    const auto                 tree   = heimdall::ParseTree::Parse(source, {});
    for (const auto name : { "bool", "int", "double" })
    {
        const auto offset     = source.find(name) + 1;
        const auto text_hover = heimdall::CompletionEngine::Hover(source, {}, offset);
        const auto tree_hover = heimdall::CompletionEngine::Hover(tree, {}, offset);
        ASSERT_TRUE(text_hover.has_value());
        ASSERT_TRUE(tree_hover.has_value());
        EXPECT_TRUE(text_hover->has_layout);
        EXPECT_TRUE(tree_hover->has_layout);
        EXPECT_EQ(text_hover->size_bytes, tree_hover->size_bytes);
    }

    const auto boolean = LayoutAtHover(source, "bool");
    ASSERT_TRUE(boolean.known);
    EXPECT_EQ(boolean.size, sizeof(bool));
    EXPECT_EQ(boolean.align, alignof(bool));
}

TEST(CompletionSpec, HoverAliasesCarryOriginDocumentationAndLayout)
{
    constexpr std::string_view source =
        "namespace geo { /// Coordinates.\n"
        "struct Point { double x; double y; }; using Coord = Point; using Position = Coord; }\n"
        "using Flag = bool; geo::Position position; Flag flag;";
    const auto index = heimdall::CompletionEngine::IndexScopes(source, {});
    for (const auto* external : { static_cast<const heimdall::ScopeIndex*>(nullptr), &index })
    {
        const auto position =
            heimdall::CompletionEngine::Hover(source, {}, source.rfind("Position") + 1, external);
        ASSERT_TRUE(position.has_value());
        ASSERT_TRUE(position->has_layout);
        EXPECT_EQ(position->size_bytes, 16u);
        EXPECT_EQ(position->align_bytes, 8u);
        EXPECT_EQ(position->type_origin, "geo::Point");
        EXPECT_EQ(position->documentation, "Coordinates.");
        const auto flag =
            heimdall::CompletionEngine::Hover(source, {}, source.rfind("Flag") + 1, external);
        ASSERT_TRUE(flag.has_value());
        EXPECT_EQ(flag->type_origin, "bool");
        EXPECT_TRUE(flag->has_layout);
    }
}

TEST(CompletionSpec, OutOfLineNestedRecordDoesNotReplaceItsOwner)
{
    constexpr std::string_view header =
        "namespace std::filesystem { class path { public: class iterator; int storage; };\n"
        "class path::iterator { double cursor; }; }";
    const auto                 index  = heimdall::CompletionEngine::IndexScopes(header, {});
    constexpr std::string_view source = "std::filesystem::path file;";
    const auto                 hovered =
        heimdall::CompletionEngine::Hover(source, {}, source.find("path") + 1, &index);
    ASSERT_TRUE(hovered.has_value());
    EXPECT_EQ(hovered->detail, "class std::filesystem::path");
    EXPECT_TRUE(hovered->has_layout);
    EXPECT_EQ(hovered->size_bytes, sizeof(std::filesystem::path));
    EXPECT_EQ(hovered->align_bytes, alignof(std::filesystem::path));
    bool saw_iterator = false;
    for (const auto& scope : index)
    {
        saw_iterator =
            saw_iterator ||
            scope.path == std::vector<std::string> { "std", "filesystem", "path", "iterator" };
    }

    EXPECT_TRUE(saw_iterator);
}

TEST(CompletionSpec, HoverTypedefPreservesPointerAndArrayDeclarators)
{
    constexpr std::string_view source =
        "typedef int *Pointer; typedef short Samples[5]; Pointer pointer; Samples samples;";
    const auto pointer = LayoutAtHover(source, "Pointer");
    ASSERT_TRUE(pointer.known);
    EXPECT_EQ(pointer.size, sizeof(int*));
    const auto samples = LayoutAtHover(source, "Samples");
    ASSERT_TRUE(samples.known);
    EXPECT_EQ(samples.size, sizeof(short[5]));
}

TEST(CompletionSpec, HoverAliasUsesItsDeclarationScope)
{
    const auto index = heimdall::CompletionEngine::IndexScopes(
        "namespace a { struct Value { char data; }; using Alias = Value; }\n"
        "namespace b { struct Value { double data; }; using Alias = Value; }",
        {});
    constexpr std::string_view source = "b::Alias value;";
    const auto                 hover  = heimdall::CompletionEngine::Hover(source, {}, 4, &index);
    ASSERT_TRUE(hover.has_value());
    EXPECT_EQ(hover->type_origin, "b::Value");
    EXPECT_TRUE(hover->has_layout);
    EXPECT_EQ(hover->size_bytes, sizeof(double));
}

TEST(CompletionSpec, HoverBuiltinUsesTheWholeSpecifierAndSkipsComments)
{
    constexpr std::string_view source = "unsigned char byte; long long wide; long double precise;";
    const auto                 byte   = LayoutAtHover(source, "unsigned");
    EXPECT_TRUE(byte.known);
    EXPECT_EQ(byte.size, sizeof(unsigned char));
    const auto wide = LayoutAtHover(source, "long wide");
    EXPECT_TRUE(wide.known);
    EXPECT_EQ(wide.size, sizeof(long long));
    const auto precise = LayoutAtHover(source, "double");
    EXPECT_TRUE(precise.known);
    EXPECT_EQ(precise.size, sizeof(long double));
    EXPECT_FALSE(heimdall::CompletionEngine::Hover("// bool flag", {}, 4).has_value());
    EXPECT_FALSE(
        heimdall::CompletionEngine::Hover("const char *text = \"bool\";", {}, 21).has_value());
}

TEST(CompletionSpec, HoverAliasCyclesAndUnknownTargetsHaveNoLayout)
{
    EXPECT_FALSE(LayoutAtHover("using A = B; using B = A; A value;", "A value").known);
    EXPECT_FALSE(LayoutAtHover("using Alias = Missing; Alias value;", "Alias value").known);
}

TEST(CompletionSpec, FilesystemLayoutDoesNotAssumeAnotherDataModelOrLibrary)
{
    auto target                    = heimdall::LayoutTarget::FromMacros({});
    target.native_standard_library = false;
    const heimdall::TypeLayoutResolver foreign(nullptr, nullptr, target);
    EXPECT_FALSE(foreign.OfType("std::filesystem::path").has_value());
    target.native_standard_library = true;
    target.pointer_size            = sizeof(void*) == 8 ? 4 : 8;
    const heimdall::TypeLayoutResolver cross(nullptr, nullptr, target);
    EXPECT_FALSE(cross.OfType("std::filesystem::path").has_value());
}

TEST(CompletionSpec, HoverLayoutFollowsPointersAndArrays)
{
    const auto pointer = LayoutAtHover("char* cursor = nullptr;\n", "cursor");
    ASSERT_TRUE(pointer.known);
    EXPECT_EQ(pointer.size, 8u);
    EXPECT_EQ(pointer.align, 8u);

    const auto array = LayoutAtHover("short samples[10];\n", "samples");
    ASSERT_TRUE(array.known);
    EXPECT_EQ(array.size, 20u);
    EXPECT_EQ(array.align, 2u);

    const auto grid = LayoutAtHover("int grid[3][4];\n", "grid");
    ASSERT_TRUE(grid.known);
    EXPECT_EQ(grid.size, 48u);

    const auto reference = LayoutAtHover("int target = 0;\nint& alias = target;\n", "alias");
    ASSERT_TRUE(reference.known);
    EXPECT_EQ(reference.size, 4u);

    const auto sized = LayoutAtHover("#include <cstdint>\nstd::uint16_t port = 0;\n", "port");
    ASSERT_TRUE(sized.known);
    EXPECT_EQ(sized.size, 2u);
}

TEST(CompletionSpec, HoverLayoutPadsRecordMembers)
{
    constexpr std::string_view source =
        "struct Packet { char tag; int id; char flag; };\n"
        "Packet packet;\n";
    const auto declared = LayoutAtHover(source, "Packet packet");
    ASSERT_TRUE(declared.known);
    EXPECT_EQ(declared.size, 12u);
    EXPECT_EQ(declared.align, 4u);

    const auto variable = LayoutAtHover(source, "packet");
    ASSERT_TRUE(variable.known);
    EXPECT_EQ(variable.size, 12u);
    EXPECT_EQ(variable.align, 4u);
}

TEST(CompletionSpec, HoverLayoutCountsPointerMembersAndNestedRecords)
{
    constexpr std::string_view source =
        "struct Node { char tag; Node* next; };\n"
        "struct Pair { Node left; short extra; };\n";
    const auto node = LayoutAtHover(source, "Node");
    ASSERT_TRUE(node.known);
    EXPECT_EQ(node.size, 16u);
    EXPECT_EQ(node.align, 8u);

    const auto pair = LayoutAtHover(source, "Pair");
    ASSERT_TRUE(pair.known);
    EXPECT_EQ(pair.size, 24u);
    EXPECT_EQ(pair.align, 8u);
}

TEST(CompletionSpec, HoverLayoutHandlesUnionsEnumsBasesAndStatics)
{
    const auto bits =
        LayoutAtHover("union Word { char byte; int number; double real; };\n", "Word");
    ASSERT_TRUE(bits.known);
    EXPECT_EQ(bits.size, 8u);
    EXPECT_EQ(bits.align, 8u);

    const auto plain = LayoutAtHover("enum Color { Red, Green };\n", "Color");
    ASSERT_TRUE(plain.known);
    EXPECT_EQ(plain.size, 4u);

    const auto small = LayoutAtHover("enum class Level : unsigned char { Low, High };\n", "Level");
    ASSERT_TRUE(small.known);
    EXPECT_EQ(small.size, 1u);

    const auto derived =
        LayoutAtHover("struct Base { int a; };\nstruct Derived : Base { char b; };\n", "Derived");
    ASSERT_TRUE(derived.known);
    EXPECT_EQ(derived.size, 8u);

    const auto statics =
        LayoutAtHover("struct Counter { static int total; int local; };\n", "Counter");
    ASSERT_TRUE(statics.known);
    EXPECT_EQ(statics.size, 4u);

    const auto empty = LayoutAtHover("struct Tag {};\n", "Tag");
    ASSERT_TRUE(empty.known);
    EXPECT_EQ(empty.size, 1u);
}

TEST(CompletionSpec, HoverLayoutIsOmittedWhenItCannotBeProven)
{
    EXPECT_FALSE(
        LayoutAtHover("struct Shape { virtual void draw(); int sides; };\n", "Shape").known);
    EXPECT_FALSE(
        LayoutAtHover("struct Flags { unsigned ready : 1; unsigned done : 1; };\n", "Flags").known);
    EXPECT_FALSE(LayoutAtHover("template <class T> struct Box { T value; };\n", "Box").known);
    EXPECT_FALSE(LayoutAtHover("struct Aligned { alignas(16) char data[4]; };\n", "Aligned").known);
    EXPECT_FALSE(LayoutAtHover("Unknown mystery;\n", "mystery").known);
    EXPECT_FALSE(
        LayoutAtHover("struct Outer { struct { int a; int b; } inner; int c; };\n", "Outer").known);
}

TEST(CompletionSpec, HoverLayoutResolvesTypesFromHeaderIndex)
{
    const auto index = heimdall::CompletionEngine::IndexScopes(
        "namespace geo { struct Point { double x; double y; }; using Coord = Point; }\n", {});
    const auto point = LayoutAtHover("geo::Point origin;\n", "origin", &index);
    ASSERT_TRUE(point.known);
    EXPECT_EQ(point.size, 16u);
    EXPECT_EQ(point.align, 8u);

    const auto alias = LayoutAtHover("geo::Coord origin;\n", "origin", &index);
    ASSERT_TRUE(alias.known);
    EXPECT_EQ(alias.size, 16u);
}

TEST(CompletionSpec, HoverLayoutKnowsCommonStandardLibraryTypes)
{
    const auto text = LayoutAtHover("std::string name;\n", "name");
    ASSERT_TRUE(text.known);
    EXPECT_EQ(text.size, 32u);

    const auto list = LayoutAtHover("std::vector<int> items;\n", "items");
    ASSERT_TRUE(list.known);
    EXPECT_EQ(list.size, 24u);

    const auto fixed = LayoutAtHover("std::array<short, 5> window;\n", "window");
    ASSERT_TRUE(fixed.known);
    EXPECT_EQ(fixed.size, 10u);
    EXPECT_EQ(fixed.align, 2u);

    const auto owner = LayoutAtHover("std::unique_ptr<int> owner;\n", "owner");
    ASSERT_TRUE(owner.known);
    EXPECT_EQ(owner.size, 8u);
}

namespace
{
    // Mirrors the source below, so the compiler building the tests is the oracle.
    struct SelfLinkedNode
    {
        SelfLinkedNode* next;
        int             value;
    };
} // namespace

TEST(CompletionSpec, HoverLayoutOfSelfReferencingNodeMatchesTheCompiler)
{
    constexpr std::string_view source =
        "struct Node\n"
        "{\n"
        "    Node* next;\n"
        "    int value;\n"
        "};\n";
    const auto node = LayoutAtHover(source, "Node");
    ASSERT_TRUE(node.known);
    EXPECT_EQ(node.size, sizeof(SelfLinkedNode));
    EXPECT_EQ(node.align, alignof(SelfLinkedNode));
    if constexpr (sizeof(void*) == 8)
    {
        EXPECT_EQ(node.size, 16u);
        EXPECT_EQ(node.align, 8u);
    }
}

namespace
{
    struct OffsetProbe
    {
        char   tag;
        double weight;
        short  count;
    };

    struct HoverOffset
    {
        bool          known  = false;
        std::uint64_t offset = 0;
    };

    HoverOffset OffsetAtHover(std::string_view source, std::string_view name,
                              std::size_t occurrence_from_end = 0)
    {
        std::size_t at = std::string_view::npos;
        for (std::size_t n = 0; n <= occurrence_from_end; ++n)
        {
            at = source.rfind(name, at == std::string_view::npos ? std::string_view::npos : at - 1);
        }

        const auto hovered =
            heimdall::CompletionEngine::Hover(source, heimdall::ParserOptions {}, at + 1);
        if (!hovered.has_value())
        {
            return {};
        }

        return { hovered->has_field_offset, hovered->field_offset };
    }
} // namespace

TEST(CompletionSpec, HoverOnMemberReportsOffsetWithinRecord)
{
    constexpr std::string_view source = "struct Probe { char tag; double weight; short count; };\n";
    const auto                 tag    = OffsetAtHover(source, "tag");
    ASSERT_TRUE(tag.known);
    EXPECT_EQ(tag.offset, offsetof(OffsetProbe, tag));

    const auto weight = OffsetAtHover(source, "weight");
    ASSERT_TRUE(weight.known);
    EXPECT_EQ(weight.offset, offsetof(OffsetProbe, weight));

    const auto count = OffsetAtHover(source, "count");
    ASSERT_TRUE(count.known);
    EXPECT_EQ(count.offset, offsetof(OffsetProbe, count));
}

TEST(CompletionSpec, HoverOnMemberOffsetIsZeroInUnionsAndSkipsStatics)
{
    const auto member = OffsetAtHover("union Word { char byte; int number; };\n", "number");
    ASSERT_TRUE(member.known);
    EXPECT_EQ(member.offset, 0u);

    EXPECT_FALSE(OffsetAtHover("struct C { static int total; int local; };\n", "total").known);
    EXPECT_FALSE(OffsetAtHover("struct V { virtual void f(); int x; };\n", "x").known);
    EXPECT_FALSE(OffsetAtHover("int plain = 0;\n", "plain").known);
}

TEST(CompletionSpec, HoverOnOffsetofEvaluatesTheMember)
{
    constexpr std::string_view source =
        "struct Probe { char tag; double weight; short count; };\n"
        "auto where = offsetof(Probe, weight);\n";
    const auto folded = OffsetAtHover(source, "offsetof");
    ASSERT_TRUE(folded.known);
    EXPECT_EQ(folded.offset, offsetof(OffsetProbe, weight));

    EXPECT_FALSE(OffsetAtHover("auto w = offsetof(Missing, weight);\n", "offsetof").known);
    EXPECT_FALSE(
        OffsetAtHover("struct P { int a; };\nauto w = offsetof(P, nope);\n", "offsetof").known);
}
