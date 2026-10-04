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

namespace
{

    // `|` marks the cursor. Returns the labels offered there.
    std::vector<std::string> MemberLabels(std::string source, const heimdall::ScopeIndex *external = nullptr)
    {
        const std::size_t cursor = source.find('|');
        source.erase(cursor, 1);
        std::vector<std::string> labels;
        for (const auto & item: heimdall::CompletionEngine::Complete(source, {}, cursor, external))
        {
            labels.push_back(item.label);
        }

        return labels;
    }

    bool Has(const std::vector<std::string> & labels, std::string_view label)
    {
        return std::find(labels.begin(), labels.end(), label) != labels.end();
    }

    constexpr std::string_view kStdLike =
    "namespace std {\n"
    "template<class T> class vector { public: void push_back(const T&); void clear(); T& front(); unsigned size() const; };\n"
    "class basic_string { public: basic_string(); void resize(int); void clear(); };\n"
    "typedef basic_string string;\n"
    "template<class T> class unique_ptr { public: T* get(); void reset(); };\n"
    "}\n";

} // namespace

TEST(CompletionSpec, MemberAccessListsMembersOfLocalAndParameterTypes)
{
    const auto dot = MemberLabels("struct P { int x; void run(); private: int y; void hid(); P(); ~P(); };\n"
        "void f() { P p; p.| }\n");
    EXPECT_TRUE(Has(dot, "x"));
    EXPECT_TRUE(Has(dot, "run"));
    EXPECT_TRUE(Has(dot, "y"));
    EXPECT_TRUE(Has(dot, "hid"));
    EXPECT_FALSE(Has(dot, "P")); // constructors are not members you can name

    const auto arrow = MemberLabels("struct P { int x; void run(); };\nvoid f(P* p) { p->r| }\n");
    EXPECT_EQ(arrow, std::vector<std::string>{"run"});
    const auto reference = MemberLabels("struct P { int x; };\nvoid f(const P& r) { r.| }\n");
    EXPECT_TRUE(Has(reference, "x"));
    EXPECT_TRUE(MemberLabels("struct P { int x; };\nvoid f(const P& r, int z) { z.| }\n").empty());
}

TEST(CompletionSpec, MemberAccessIncludesInheritedMembers)
{
    const auto labels = MemberLabels("struct B { int bx; void bm(); };\nstruct M : public B { int mx; };\n"
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
    EXPECT_TRUE(Has(MemberLabels("struct P { int x; };\nusing Q = P;\nvoid f() { Q q; q.| }\n"), "x"));
    EXPECT_TRUE(Has(MemberLabels("struct P { int x; };\ntypedef P R;\nvoid f() { R r; r.| }\n"), "x"));
    EXPECT_TRUE(Has(MemberLabels("namespace n { struct P { int x; }; }\nvoid f() { n::P a; a.| }\n"), "x"));
    EXPECT_TRUE(Has(MemberLabels("namespace n { struct P { int x; }; }\nusing namespace n;\n"
        "void f() { P a; a.| }\n"), "x"));
    EXPECT_TRUE(Has(MemberLabels("namespace n { struct P { int x; }; }\nnamespace n { void f() { P a; a.| } }\n"), "x"));
    EXPECT_TRUE(Has(MemberLabels("struct P { int x; };\nvoid f() { auto a = P(); a.| }\n"), "x"));
    EXPECT_TRUE(Has(MemberLabels("struct P { int x; };\nvoid f() { auto a = new P(); a->| }\n"), "x"));
    EXPECT_TRUE(Has(MemberLabels("struct P { int x; };\nP g;\nvoid f() { g.| }\n"), "x"));
}

TEST(CompletionSpec, MemberAccessOnThisAndImplicitFields)
{
    const auto labels = MemberLabels("struct I { int deep; };\n"
        "struct P { int x; I in; void m(); };\n"
        "void P::m() { this->| }\n");
    EXPECT_TRUE(Has(labels, "x"));
    EXPECT_TRUE(Has(labels, "m"));
    EXPECT_TRUE(Has(MemberLabels("struct I { int deep; };\nstruct P { I in; void m() { in.| } };\n"), "deep"));
    EXPECT_TRUE(Has(MemberLabels("struct I { int deep; };\nstruct P { I in; void m(); };\nvoid P::m() { in.| }\n"),
        "deep"));
}

TEST(CompletionSpec, MemberAccessUsesTheHeaderIndexForStandardLikeTypes)
{
    const heimdall::ScopeIndex index = heimdall::CompletionEngine::IndexScopes(kStdLike, {});

    // `std::string` is a typedef of another record: resolved through the alias.
    const auto text = MemberLabels("void f() { std::string body; body.| }\n", &index);
    EXPECT_TRUE(Has(text, "resize"));
    EXPECT_TRUE(Has(text, "clear"));
    EXPECT_EQ(MemberLabels("void f() { std::string s; s.cl| }\n", &index), std::vector<std::string>{"clear"});
    EXPECT_TRUE(Has(MemberLabels("using namespace std;\nvoid f() { string s; s.| }\n", &index), "resize"));
    EXPECT_TRUE(Has(MemberLabels("void f() { auto s = std::string(); s.| }\n", &index), "resize"));

    const auto vector = MemberLabels("void f() { std::vector<int> v; v.| }\n", &index);
    EXPECT_TRUE(Has(vector, "push_back"));
    EXPECT_TRUE(Has(vector, "size"));

    // Member of a header type that is itself a header type.
    EXPECT_TRUE(Has(MemberLabels("struct P { std::string name; };\nvoid f(P p) { p.name.| }\n", &index), "resize"));
}

TEST(CompletionSpec, MemberAccessThroughSmartPointersAndContainers)
{
    const heimdall::ScopeIndex index = heimdall::CompletionEngine::IndexScopes(kStdLike, {});
    const std::string p = "struct P { int x; };\n";
    const auto arrow = MemberLabels(p + "void f() { std::unique_ptr<P> u; u->| }\n", &index);
    EXPECT_TRUE(Has(arrow, "x"));
    EXPECT_FALSE(Has(arrow, "reset"));
    const auto dot = MemberLabels(p + "void f() { std::unique_ptr<P> u; u.| }\n", &index);
    EXPECT_TRUE(Has(dot, "reset"));
    EXPECT_FALSE(Has(dot, "x"));
    EXPECT_TRUE(Has(MemberLabels(p + "void f() { std::vector<P> v; v[0].| }\n", &index), "x"));
}

TEST(CompletionSpec, IndexRecordsAliasTargetsAndBases)
{
    const auto index = heimdall::CompletionEngine::IndexScopes(
        "struct B { int b; };\nstruct D : public B, private ns::Other<int> { };\nusing Alias = std::vector<int>;\n", {});
    bool saw_bases = false;
    bool saw_alias = false;
    for (const auto & scope: index)
    {
        if (scope.path == std::vector<std::string>{"D"})
        {
            saw_bases = scope.bases == std::vector<std::string>{"B", "ns::Other"};
        }

        for (const auto & member: scope.members)
        {
            saw_alias = saw_alias || (member.label == "Alias" && member.type_text == "std::vector<int>");
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
        "}\n", {});
    const std::string source = "n::element value;";
    const auto hover = heimdall::CompletionEngine::Hover(source, {}, 5, &index);
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
    for (const std::string source: {header + use})
    {
        auto hover = [&](std::string_view needle, const heimdall::ScopeIndex *index)
        {
            return heimdall::CompletionEngine::Hover(source, {}, source.find(needle) + 1, index);
        };
        const auto enumeration = hover("Style :", nullptr);
        ASSERT_TRUE(enumeration.has_value());
        EXPECT_EQ(enumeration->documentation, "How single-statement blocks are treated.\nSecond line.");
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
    const auto index = heimdall::CompletionEngine::IndexScopes(header, {});
    const std::string source = use;
    const auto indexed = heimdall::CompletionEngine::Hover(source, {}, source.find("Style s") + 1, &index);
    ASSERT_TRUE(indexed.has_value());
    EXPECT_NE(indexed->documentation.find("How single-statement"), std::string::npos);
    const auto keep = heimdall::CompletionEngine::Hover(source, {}, source.find("Keep") + 1, &index);
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
    auto hover = [&](std::string_view needle, std::size_t skip = 1)
    {
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
    const auto hovered = heimdall::CompletionEngine::Hover(
        source, {}, source.find("Server::Work") + 2, nullptr);
    ASSERT_TRUE(hovered.has_value());
    EXPECT_EQ(hovered->label, "Server");
    EXPECT_EQ(hovered->kind, heimdall::CompletionKind::Type);
}

TEST(CompletionSpec, HoverOnStructQualifyingAMethodDefinitionAtGlobalScope)
{
    const std::string_view source =
        "struct Box { int Get() const; };\n"
        "int Box::Get() const { return 1; }\n";
    const auto hovered = heimdall::CompletionEngine::Hover(
        source, {}, source.find("Box::Get") + 1, nullptr);
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
        "}\n", {});
    const std::string_view source =
        "namespace heimdall::lsp\n"
        "{\n"
        "    void LanguageServer::DiagWorkerMain(int stop)\n"
        "    {\n"
        "    }\n"
        "}\n";
    const auto hovered = heimdall::CompletionEngine::Hover(
        source, {}, source.find("LanguageServer::") + 3, &index);
    ASSERT_TRUE(hovered.has_value());
    EXPECT_EQ(hovered->label, "LanguageServer");
}

TEST(CompletionSpec, HeaderNamespaceMembersVisibleInNestedNamespaceOnly)
{
    const auto index = heimdall::CompletionEngine::IndexScopes(
        "namespace heimdall::lsp {\nclass LanguageServer {};\n}\n", {});
    auto hover = [&](std::string_view source)
    {
        return heimdall::CompletionEngine::Hover(source, {}, source.find("LanguageServer") + 3, &index);
    };

    EXPECT_TRUE(hover("namespace heimdall::lsp { namespace detail { LanguageServer* p; } }\n").has_value());
    EXPECT_TRUE(hover("namespace heimdall { namespace lsp { LanguageServer* p; } }\n").has_value());
    // Outside the namespace (or in a sibling one) the name stays qualified-only.
    EXPECT_FALSE(hover("LanguageServer* p;\n").has_value());
    EXPECT_FALSE(hover("namespace heimdall { LanguageServer* p; }\n").has_value());
    EXPECT_FALSE(hover("namespace other::lsp { LanguageServer* p; }\n").has_value());
}
