#include <Heimdall/ParseTree.hpp>
#include <Heimdall/SymbolOutline.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace
{

    using heimdall::kNoOutlineParent;
    using heimdall::OutlineKind;
    using heimdall::OutlineSymbol;

    std::vector<OutlineSymbol> Outline(std::string_view source)
    {
        const heimdall::ParseTree tree = heimdall::ParseTree::Parse(source);
        return heimdall::SymbolOutline::Extract(tree);
    }

    const OutlineSymbol* Named(const std::vector<OutlineSymbol>& symbols, std::string_view name,
                               std::size_t nth = 0)
    {
        for (const OutlineSymbol& symbol : symbols)
        {
            if (symbol.name == name && nth-- == 0)
            {
                return &symbol;
            }
        }

        return nullptr;
    }

    std::vector<std::string> Names(const std::vector<OutlineSymbol>& symbols)
    {
        std::vector<std::string> names;
        for (const OutlineSymbol& symbol : symbols)
        {
            names.push_back(symbol.name);
        }

        return names;
    }

    std::string_view NameText(std::string_view source, const OutlineSymbol& symbol)
    {
        return source.substr(symbol.nameOffset, symbol.nameLength);
    }

    std::string_view RangeText(std::string_view source, const OutlineSymbol& symbol)
    {
        return source.substr(symbol.rangeOffset, symbol.rangeLength);
    }

} // namespace

TEST(SymbolOutlineSpec, EmptySourceHasNoSymbols)
{
    EXPECT_TRUE(Outline("").empty());
    EXPECT_TRUE(Outline("\n\n  // only a comment\n").empty());
}

TEST(SymbolOutlineSpec, NestsMembersUnderNamespaceAndType)
{
    constexpr std::string_view source = "namespace app {\n"
                                        "class Widget\n"
                                        "{\n"
                                        "public:\n"
                                        "    Widget();\n"
                                        "    ~Widget();\n"
                                        "    void Run(int times) const;\n"
                                        "    int count;\n"
                                        "};\n"
                                        "}\n";

    const auto symbols = Outline(source);

    ASSERT_EQ(Names(symbols), (std::vector<std::string> {"app", "Widget", "Widget", "~Widget",
                                                         "Run", "count"}));
    EXPECT_EQ(symbols[0].kind, OutlineKind::Namespace);
    EXPECT_EQ(symbols[0].parent, kNoOutlineParent);
    EXPECT_EQ(symbols[1].kind, OutlineKind::Class);
    EXPECT_EQ(symbols[1].parent, 0u);
    EXPECT_EQ(symbols[1].container, "app");
    EXPECT_EQ(symbols[2].kind, OutlineKind::Constructor);
    EXPECT_EQ(symbols[3].kind, OutlineKind::Destructor);
    EXPECT_EQ(symbols[4].kind, OutlineKind::Method);
    EXPECT_EQ(symbols[4].detail, "(int times) const");
    EXPECT_EQ(symbols[4].container, "app::Widget");
    EXPECT_EQ(symbols[4].parent, 1u);
    EXPECT_EQ(symbols[5].kind, OutlineKind::Field);
    EXPECT_EQ(symbols[5].detail, "int");
}

TEST(SymbolOutlineSpec, SelectsTheNameAndCoversTheDeclaration)
{
    constexpr std::string_view source = "struct Point { int x; };\nint Add(int a, int b) { return a + b; }\n";

    const auto symbols = Outline(source);

    const OutlineSymbol* point = Named(symbols, "Point");
    const OutlineSymbol* add   = Named(symbols, "Add");
    ASSERT_NE(point, nullptr);
    ASSERT_NE(add, nullptr);
    EXPECT_EQ(NameText(source, *point), "Point");
    EXPECT_EQ(RangeText(source, *point), "struct Point { int x; }");
    EXPECT_EQ(NameText(source, *add), "Add");
    EXPECT_EQ(RangeText(source, *add), "int Add(int a, int b) { return a + b; }");
}

TEST(SymbolOutlineSpec, ListsEveryOverloadSeparately)
{
    constexpr std::string_view source = "void Log(int value);\n"
                                        "void Log(const char* text);\n"
                                        "void Log(int value, int level) {}\n";

    const auto symbols = Outline(source);

    ASSERT_EQ(symbols.size(), 3u);
    EXPECT_EQ(symbols[0].detail, "(int value)");
    EXPECT_EQ(symbols[1].detail, "(const char* text)");
    EXPECT_EQ(symbols[2].detail, "(int value, int level)");
    EXPECT_TRUE(std::ranges::all_of(
        symbols, [](const OutlineSymbol& symbol) { return symbol.kind == OutlineKind::Function; }));
}

TEST(SymbolOutlineSpec, ListsEnumeratorsUnderTheirEnum)
{
    constexpr std::string_view source = "enum class Mode : char { Fast, Slow = 2 };\n"
                                        "enum Legacy { Old, Older };\n";

    const auto symbols = Outline(source);

    ASSERT_EQ(Names(symbols), (std::vector<std::string> {"Mode", "Fast", "Slow", "Legacy", "Old",
                                                         "Older"}));
    EXPECT_EQ(symbols[0].kind, OutlineKind::Enum);
    EXPECT_EQ(symbols[1].kind, OutlineKind::EnumMember);
    EXPECT_EQ(symbols[1].parent, 0u);
    EXPECT_EQ(symbols[1].container, "Mode");
    EXPECT_EQ(symbols[3].parent, kNoOutlineParent);
    EXPECT_EQ(symbols[4].parent, 3u);
}

TEST(SymbolOutlineSpec, KeepsTheQualifierOfOutOfLineDefinitions)
{
    constexpr std::string_view source = "namespace app {\n"
                                        "void Widget::Run() {}\n"
                                        "Widget::Widget() {}\n"
                                        "Widget::~Widget() {}\n"
                                        "}\n";

    const auto symbols = Outline(source);

    const OutlineSymbol* run = Named(symbols, "Widget::Run");
    const OutlineSymbol* ctor = Named(symbols, "Widget::Widget");
    const OutlineSymbol* dtor = Named(symbols, "Widget::~Widget");
    ASSERT_NE(run, nullptr);
    ASSERT_NE(ctor, nullptr);
    ASSERT_NE(dtor, nullptr);
    EXPECT_EQ(run->kind, OutlineKind::Method);
    EXPECT_EQ(ctor->kind, OutlineKind::Constructor);
    EXPECT_EQ(dtor->kind, OutlineKind::Destructor);
    EXPECT_EQ(run->container, "app");
    EXPECT_EQ(NameText(source, *run), "Run");
    EXPECT_EQ(NameText(source, *dtor), "~Widget");
}

TEST(SymbolOutlineSpec, RecognizesConstructorAtTheStartOfTheFile)
{
    const auto symbols = Outline("Widget::Widget() {}\n");

    ASSERT_EQ(symbols.size(), 1u);
    EXPECT_EQ(symbols[0].kind, OutlineKind::Constructor);
}

TEST(SymbolOutlineSpec, NamesOperatorsAsWritten)
{
    constexpr std::string_view source = "struct Foo\n"
                                        "{\n"
                                        "    bool operator==(const Foo&) const;\n"
                                        "    int operator()(int) const;\n"
                                        "    operator bool() const;\n"
                                        "};\n"
                                        "Foo operator+(Foo a, Foo b);\n";

    const auto symbols = Outline(source);

    ASSERT_EQ(Names(symbols), (std::vector<std::string> {"Foo", "operator==", "operator()",
                                                         "operator bool", "operator+"}));
    for (std::size_t i = 1; i < symbols.size(); ++i)
    {
        EXPECT_EQ(symbols[i].kind, OutlineKind::Operator) << symbols[i].name;
    }

    EXPECT_EQ(NameText(source, symbols[3]), "operator bool");
}

TEST(SymbolOutlineSpec, ListsAliasesAndTypedefs)
{
    constexpr std::string_view source = "using Id = unsigned long;\n"
                                        "typedef int Count;\n"
                                        "template <typename T> using Vec = Box<T>;\n"
                                        "using namespace std;\n"
                                        "using std::string;\n";

    const auto symbols = Outline(source);

    ASSERT_EQ(Names(symbols), (std::vector<std::string> {"Id", "Count", "Vec"}));
    EXPECT_EQ(symbols[0].kind, OutlineKind::TypeAlias);
    EXPECT_EQ(symbols[0].detail, "unsigned long");
    EXPECT_EQ(symbols[1].kind, OutlineKind::TypeAlias);
    EXPECT_EQ(symbols[1].detail, "int");
    EXPECT_EQ(symbols[2].detail, "Box<T>");
}

TEST(SymbolOutlineSpec, ExtendsTheRangeOverTheTemplateHeader)
{
    constexpr std::string_view source = "template <typename T>\nclass Box { T value; };\n"
                                        "template <typename T>\nT Twice(T v) { return v; }\n";

    const auto symbols = Outline(source);

    const OutlineSymbol* box   = Named(symbols, "Box");
    const OutlineSymbol* twice = Named(symbols, "Twice");
    ASSERT_NE(box, nullptr);
    ASSERT_NE(twice, nullptr);
    EXPECT_TRUE(RangeText(source, *box).starts_with("template <typename T>"));
    EXPECT_TRUE(RangeText(source, *twice).starts_with("template <typename T>"));
    EXPECT_EQ(NameText(source, *box), "Box");
}

TEST(SymbolOutlineSpec, NamesAnonymousScopes)
{
    constexpr std::string_view source = "namespace { int hidden; }\n"
                                        "struct { int a; } instance;\n"
                                        "enum { Value };\n";

    const auto symbols = Outline(source);

    ASSERT_GE(symbols.size(), 5u);
    EXPECT_EQ(symbols[0].name, "(anonymous namespace)");
    EXPECT_EQ(symbols[0].kind, OutlineKind::Namespace);
    EXPECT_EQ(symbols[1].name, "hidden");
    EXPECT_EQ(symbols[1].container, "(anonymous namespace)");
    EXPECT_EQ(symbols[2].name, "(anonymous struct)");
    EXPECT_EQ(symbols[3].name, "a");
    EXPECT_EQ(symbols[3].parent, 2u);
    EXPECT_NE(Named(symbols, "(anonymous enum)"), nullptr);
    EXPECT_NE(Named(symbols, "Value"), nullptr);
}

TEST(SymbolOutlineSpec, ParsesInlineNamespacesAsNamespaces)
{
    constexpr std::string_view source = "inline namespace v1 { void Stable(); }\n"
                                        "namespace outer { inline namespace v2 { int Value; } }\n";

    const auto symbols = Outline(source);

    ASSERT_EQ(Names(symbols), (std::vector<std::string> {"v1", "Stable", "outer", "v2", "Value"}));
    EXPECT_EQ(symbols[0].kind, OutlineKind::Namespace);
    EXPECT_EQ(symbols[1].parent, 0u);
    EXPECT_EQ(symbols[4].container, "outer::v2");
}

TEST(SymbolOutlineSpec, WrittenQualifiedNamespaceIsOneSymbol)
{
    constexpr std::string_view source = "namespace a::b { void Fn(); }\n";

    const auto symbols = Outline(source);

    ASSERT_EQ(symbols.size(), 2u);
    EXPECT_EQ(symbols[0].name, "a::b");
    EXPECT_EQ(symbols[1].container, "a::b");
}

TEST(SymbolOutlineSpec, OmitsLocalsAndParameters)
{
    constexpr std::string_view source = "int Compute(int input)\n"
                                        "{\n"
                                        "    int local = input;\n"
                                        "    struct Hidden { int inner; };\n"
                                        "    auto fn = [](int p) { return p; };\n"
                                        "    return local;\n"
                                        "}\n";

    const auto symbols = Outline(source);

    ASSERT_EQ(symbols.size(), 1u);
    EXPECT_EQ(symbols[0].name, "Compute");
}

TEST(SymbolOutlineSpec, SkipsForwardAndFriendDeclarations)
{
    constexpr std::string_view source = "struct Forward;\n"
                                        "enum Opaque : int;\n"
                                        "class Guard\n"
                                        "{\n"
                                        "    friend void Peek(Guard&);\n"
                                        "    friend class Other;\n"
                                        "    int secret;\n"
                                        "};\n";

    const auto symbols = Outline(source);

    EXPECT_EQ(Names(symbols), (std::vector<std::string> {"Guard", "secret"}));
}

TEST(SymbolOutlineSpec, ListsEveryDeclaratorOfOneDeclaration)
{
    constexpr std::string_view source = "int first, second = 2;\n";

    const auto symbols = Outline(source);

    ASSERT_EQ(Names(symbols), (std::vector<std::string> {"first", "second"}));
    EXPECT_EQ(symbols[0].kind, OutlineKind::Variable);
    EXPECT_EQ(symbols[1].kind, OutlineKind::Variable);
    EXPECT_EQ(symbols[1].detail, "int");
}

TEST(SymbolOutlineSpec, DistinguishesVariablesFromFunctions)
{
    constexpr std::string_view source = "int values[4];\n"
                                        "int (*callback)(int);\n"
                                        "template <typename T> constexpr T pi = T(3);\n"
                                        "int Compute(int);\n";

    const auto symbols = Outline(source);

    const OutlineSymbol* values   = Named(symbols, "values");
    const OutlineSymbol* callback = Named(symbols, "callback");
    const OutlineSymbol* pi       = Named(symbols, "pi");
    const OutlineSymbol* compute  = Named(symbols, "Compute");
    ASSERT_NE(values, nullptr);
    ASSERT_NE(callback, nullptr);
    ASSERT_NE(pi, nullptr);
    ASSERT_NE(compute, nullptr);
    EXPECT_EQ(values->kind, OutlineKind::Variable);
    EXPECT_EQ(callback->kind, OutlineKind::Variable);
    EXPECT_EQ(pi->kind, OutlineKind::Variable);
    EXPECT_EQ(compute->kind, OutlineKind::Function);
}

TEST(SymbolOutlineSpec, ListsConcepts)
{
    const auto symbols = Outline("template <typename T> concept Small = sizeof(T) < 8;\n");

    ASSERT_EQ(symbols.size(), 1u);
    EXPECT_EQ(symbols[0].name, "Small");
    EXPECT_EQ(symbols[0].kind, OutlineKind::Concept);
}

TEST(SymbolOutlineSpec, SeesThroughLanguageLinkage)
{
    const auto symbols = Outline("extern \"C\" { void c_api(int); int c_value; }\n");

    EXPECT_EQ(Names(symbols), (std::vector<std::string> {"c_api", "c_value"}));
    EXPECT_EQ(symbols[0].parent, kNoOutlineParent);
}

TEST(SymbolOutlineSpec, SkipsExportMacrosBeforeTheTypeName)
{
    constexpr std::string_view source = "class DLL_API Exported { int v; };\n"
                                        "struct [[nodiscard]] Tagged { int w; };\n"
                                        "struct alignas(16) Aligned { int x; };\n";

    const auto symbols = Outline(source);

    EXPECT_NE(Named(symbols, "Exported"), nullptr);
    EXPECT_NE(Named(symbols, "Tagged"), nullptr);
    EXPECT_NE(Named(symbols, "Aligned"), nullptr);
    EXPECT_EQ(Named(symbols, "DLL_API"), nullptr);
}

TEST(SymbolOutlineSpec, KeepsTemplateArgumentsOfSpecializations)
{
    constexpr std::string_view source = "template <typename T> struct Box {};\n"
                                        "template <> struct Box<int> { int v; };\n";

    const auto symbols = Outline(source);

    EXPECT_NE(Named(symbols, "Box"), nullptr);
    EXPECT_NE(Named(symbols, "Box<int>"), nullptr);
}

TEST(SymbolOutlineSpec, TruncatesVeryLongDetails)
{
    std::string source = "void Wide(";
    for (int i = 0; i < 60; ++i)
    {
        source += "int parameter" + std::to_string(i) + (i < 59 ? ", " : "");
    }

    source += ");\n";

    const auto symbols = Outline(source);

    ASSERT_EQ(symbols.size(), 1u);
    EXPECT_LE(symbols[0].detail.size(), 200u);
    EXPECT_TRUE(symbols[0].detail.ends_with("..."));
}

TEST(SymbolOutlineSpec, KeepsRangesAndOrderConsistent)
{
    constexpr std::string_view source = "namespace n {\n"
                                        "struct A { int x; void f(); struct B { int y; }; };\n"
                                        "enum E { P, Q };\n"
                                        "template <typename T> class C { T t; };\n"
                                        "void A::f() {}\n"
                                        "}\n"
                                        "int z;\n";

    const auto symbols = Outline(source);

    ASSERT_FALSE(symbols.empty());
    std::vector<std::size_t> lastChildOffset(symbols.size() + 1, 0);
    for (std::size_t i = 0; i < symbols.size(); ++i)
    {
        const OutlineSymbol& symbol = symbols[i];
        EXPECT_GE(symbol.nameOffset, symbol.rangeOffset) << symbol.name;
        EXPECT_LE(symbol.nameOffset + symbol.nameLength, symbol.rangeOffset + symbol.rangeLength)
            << symbol.name;
        EXPECT_LE(symbol.rangeOffset + symbol.rangeLength, source.size()) << symbol.name;
        if (symbol.parent != kNoOutlineParent)
        {
            ASSERT_LT(symbol.parent, i) << symbol.name;
            const OutlineSymbol& parent = symbols[symbol.parent];
            EXPECT_GE(symbol.rangeOffset, parent.rangeOffset) << symbol.name;
            EXPECT_LE(symbol.rangeOffset + symbol.rangeLength, parent.rangeOffset + parent.rangeLength)
                << symbol.name;
        }

        const std::size_t slot = symbol.parent == kNoOutlineParent ? symbols.size() : symbol.parent;
        EXPECT_GE(symbol.rangeOffset, lastChildOffset[slot]) << "siblings out of order: " << symbol.name;
        lastChildOffset[slot] = symbol.rangeOffset;
    }
}

TEST(SymbolOutlineSpec, RecoversSymbolsFromIncompleteCode)
{
    constexpr std::string_view source = "namespace app {\n"
                                        "struct Done { int a; };\n"
                                        "struct Broken {\n"
                                        "    void half(int\n";

    const auto symbols = Outline(source);

    EXPECT_NE(Named(symbols, "app"), nullptr);
    EXPECT_NE(Named(symbols, "Done"), nullptr);
}

TEST(SymbolOutlineSpec, SurvivesGarbage)
{
    EXPECT_NO_FATAL_FAILURE((void) Outline("}}}} ;;; namespace { struct enum class ( ) < >"));
    EXPECT_NO_FATAL_FAILURE((void) Outline("template <"));
    EXPECT_NO_FATAL_FAILURE((void) Outline("operator"));
}
