#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <filesystem>
#include <memory>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include <Heimdall/SemanticHighlight.hpp>
#include <Heimdall/IncludeIndex.hpp>

namespace
{
    using namespace heimdall;

    std::vector<HighlightSpan> Highlight(
        std::string_view source,
        const SemanticHighlightIndex* headers = nullptr)
    {
        const auto tree = ParseTree::Parse(source);
        const auto model = Binder::Bind(tree);
        HighlightContext context;
        context.activeRanges = tree.ActiveRanges();
        return SemanticHighlight::Extract(model, SyntaxHighlight::Extract(tree, context), headers);
    }

    std::optional<HighlightSpan> At(
        std::string_view source,
        const std::vector<HighlightSpan>& spans,
        std::string_view text,
        std::size_t occurrence = 0)
    {
        auto offset = source.find(text);
        while (occurrence-- > 0 && offset != std::string_view::npos)
        {
            offset = source.find(text, offset + text.size());
        }

        const auto found = std::ranges::find(spans, offset, &HighlightSpan::offset);
        return found == spans.end() ? std::nullopt : std::optional
        {
            *found
        };
    }

    SemanticHighlightIndex Header(std::string_view source)
    {
        const auto tree = ParseTree::Parse(source);
        return SemanticHighlightIndex::FromModel(Binder::Bind(tree));
    }
}

TEST(SemanticHighlightSpec, MemberNamesDoNotInheritParameterIdentity)
{
    constexpr std::string_view source =
        "struct Item { int value; }; int F(Item object, int value) { return object.value + value; }";

    const auto spans = Highlight(source);

    ASSERT_TRUE(At(source, spans, "value", 2));
    EXPECT_EQ(At(source, spans, "value", 2) -> kind, HighlightKind::Property);
    ASSERT_TRUE(At(source, spans, "value", 3));
    EXPECT_EQ(At(source, spans, "value", 3) -> kind, HighlightKind::Parameter);
    EXPECT_EQ(At(source, spans, "object", 1) -> kind, HighlightKind::Parameter);
}

TEST(SemanticHighlightSpec, ShadowingChangesTheResolvedKind)
{
    constexpr std::string_view source =
        "int F(int value) { { int value = 1; value += 1; } return value; }";

    const auto spans = Highlight(source);

    ASSERT_TRUE(At(source, spans, "value", 2));
    EXPECT_EQ(At(source, spans, "value", 2) -> kind, HighlightKind::Variable);
    ASSERT_TRUE(At(source, spans, "value", 3));
    EXPECT_EQ(At(source, spans, "value", 3) -> kind, HighlightKind::Parameter);
}

TEST(SemanticHighlightSpec, LocalsAndLoopUsesAreHighlighted)
{
    constexpr std::string_view source =
        "int F() { int sum = 0; for (int i = 0; i < 4; ++i) { sum += i; } return sum; }";

    const auto spans = Highlight(source);

    for (std::size_t occurrence = 0; occurrence < 3; ++occurrence)
    {
        ASSERT_TRUE(At(source, spans, "sum", occurrence));
        EXPECT_EQ(At(source, spans, "sum", occurrence) -> kind, HighlightKind::Variable);
    }

    EXPECT_NE(At(source, spans, "sum")->modifiers & HighlightMask(HighlightModifier::Definition), 0u);
}

TEST(SemanticHighlightSpec, DirectInitializedLocalsAndTheirUsesAreVariables)
{
    constexpr std::string_view source =
        "namespace { struct Item { Item(int, void*); int Run(); }; } "
        "int F() { Item item(1, nullptr); return item.Run(); }";

    const auto spans = Highlight(source);

    ASSERT_TRUE(At(source, spans, "item"));
    EXPECT_EQ(At(source, spans, "item") -> kind, HighlightKind::Variable);
    ASSERT_TRUE(At(source, spans, "item", 1));
    EXPECT_EQ(At(source, spans, "item", 1) -> kind, HighlightKind::Variable);
}

TEST(SemanticHighlightSpec, ConditionsDoNotInventVariableDeclarations)
{
    constexpr std::string_view source =
        "struct Item { bool ready; int Text(int index); int F(int index) { "
        "if (ready && Text(index) == 0) { return Text(index); } return 0; } };";

    const auto spans = Highlight(source);

    ASSERT_TRUE(At(source, spans, "Text", 1));
    EXPECT_EQ(At(source, spans, "Text", 1) -> kind, HighlightKind::Method);
    EXPECT_EQ(At(source, spans, "Text", 1)->modifiers & kHighlightDeclaration, 0u);
    ASSERT_TRUE(At(source, spans, "Text", 2));
    EXPECT_EQ(At(source, spans, "Text", 2) -> kind, HighlightKind::Method);
    ASSERT_TRUE(At(source, spans, "ready", 1));
    EXPECT_EQ(At(source, spans, "ready", 1) -> kind, HighlightKind::Property);
}

TEST(SemanticHighlightSpec, EnumArgumentsAreNotMethods)
{
    constexpr std::string_view source =
        "enum class Kind { Comment }; void Emit(Kind kind); void F() { Emit(Kind::Comment); }";

    const auto spans = Highlight(source);

    ASSERT_TRUE(At(source, spans, "Comment", 1));
    EXPECT_EQ(At(source, spans, "Comment", 1) -> kind, HighlightKind::EnumMember);
    EXPECT_NE(At(source, spans, "Comment", 1)->modifiers & HighlightMask(HighlightModifier::Readonly),
              0u);
}

TEST(SemanticHighlightSpec, ConstPointeeDoesNotMakePointerReadonly)
{
    constexpr std::string_view source =
        "const int* pointee; int* const pointer = nullptr; const int number = 1;";

    const auto spans = Highlight(source);

    ASSERT_TRUE(At(source, spans, "pointee"));
    EXPECT_EQ(At(source, spans, "pointee")->modifiers & HighlightMask(HighlightModifier::Readonly), 0u);
    EXPECT_NE(At(source, spans, "pointer")->modifiers & HighlightMask(HighlightModifier::Readonly), 0u);
    EXPECT_NE(At(source, spans, "number")->modifiers & HighlightMask(HighlightModifier::Readonly), 0u);
}

TEST(SemanticHighlightSpec, DeclarationAndDefinitionHaveDifferentModifiers)
{
    constexpr std::string_view source = "void F(); void F() {} extern int value; int local;";

    const auto spans = Highlight(source);

    EXPECT_EQ(At(source, spans, "F")->modifiers & HighlightMask(HighlightModifier::Definition), 0u);
    EXPECT_NE(At(source, spans, "F", 1)->modifiers & HighlightMask(HighlightModifier::Definition), 0u);
    EXPECT_EQ(At(source, spans, "value")->modifiers & HighlightMask(HighlightModifier::Definition), 0u);
    EXPECT_NE(At(source, spans, "local")->modifiers & HighlightMask(HighlightModifier::Definition), 0u);
}

TEST(SemanticHighlightSpec, StaticMembersUseFunctionAndVariableKinds)
{
    constexpr std::string_view source =
        "struct Item { static void Run(); void Work(); static int count; int field; };";

    const auto spans = Highlight(source);

    EXPECT_EQ(At(source, spans, "Run") -> kind, HighlightKind::Function);
    EXPECT_NE(At(source, spans, "Run")->modifiers & HighlightMask(HighlightModifier::Static), 0u);
    EXPECT_EQ(At(source, spans, "Work") -> kind, HighlightKind::Method);
    EXPECT_EQ(At(source, spans, "count") -> kind, HighlightKind::Variable);
    EXPECT_EQ(At(source, spans, "field") -> kind, HighlightKind::Property);
}

TEST(SemanticHighlightSpec, HeadersKeepQualifiedEnumAndTypeIdentity)
{
    auto headers = Header("namespace a { enum class Kind { Count }; struct item {}; using Alias = Kind; }");
    headers.Merge(Header("namespace b { struct Kind {}; }"));
    constexpr std::string_view source =
        "void F() { auto x = a::Kind::Count; a::item value; b::Kind other; auto y = a::Alias::Count; }";

    const auto spans = Highlight(source, &headers);

    EXPECT_EQ(At(source, spans, "Kind") -> kind, HighlightKind::Enum);
    EXPECT_EQ(At(source, spans, "Count") -> kind, HighlightKind::EnumMember);
    EXPECT_EQ(At(source, spans, "item") -> kind, HighlightKind::Class);
    EXPECT_EQ(At(source, spans, "Kind", 1) -> kind, HighlightKind::Class);
    ASSERT_TRUE(At(source, spans, "Count", 1));
    EXPECT_EQ(At(source, spans, "Count", 1) -> kind, HighlightKind::EnumMember);
}

TEST(SemanticHighlightSpec, ConflictingHeadersDoNotChooseAnArbitraryKind)
{
    auto headers = Header("namespace a { struct Name {}; }");
    headers.Merge(Header("namespace a { enum class Name { Value }; }"));

    EXPECT_EQ(headers.Find("a::Name"), nullptr);
}

TEST(SemanticHighlightSpec, ExternalMembersAndLocalAliasesUseHeaderIdentity)
{
    const auto headers = Header("namespace a { struct Item { static int count; int value; }; enum class Kind { Value }; }");
    constexpr std::string_view source =
        "using Alias = a::Kind; int F(a::Item object, a::Item* pointer) { auto kind = Alias::Value; return object.count + pointer->value; }";

    const auto spans = Highlight(source, &headers);

    ASSERT_TRUE(At(source, spans, "Value"));
    EXPECT_EQ(At(source, spans, "Value") -> kind, HighlightKind::EnumMember);
    ASSERT_TRUE(At(source, spans, "count"));
    EXPECT_EQ(At(source, spans, "count") -> kind, HighlightKind::Variable);
    EXPECT_NE(At(source, spans, "count")->modifiers & HighlightMask(HighlightModifier::Static), 0u);
    ASSERT_TRUE(At(source, spans, "value"));
    EXPECT_EQ(At(source, spans, "value") -> kind, HighlightKind::Property);
}

TEST(SemanticHighlightSpec, CyclicHeaderAliasesDoNotLoop)
{
    const auto headers = Header("using A = B; using B = A;");

    EXPECT_EQ(headers.Find("A::Value"), nullptr);
}

TEST(SemanticHighlightSpec, LocalEnumAliasesResolveTheirMembers)
{
    constexpr std::string_view source =
        "enum class Kind { Count }; using Alias = Kind; auto F() { return Alias::Count; }";

    const auto spans = Highlight(source);

    ASSERT_TRUE(At(source, spans, "Count", 1));
    EXPECT_EQ(At(source, spans, "Count", 1) -> kind, HighlightKind::EnumMember);
}

TEST(SemanticHighlightSpec, HeaderTemplateParametersAreNotExports)
{
    const auto headers = Header("template<class T, int N> struct Box { T value; };");

    EXPECT_EQ(headers.Find("T"), nullptr);
    EXPECT_EQ(headers.Find("N"), nullptr);
    EXPECT_NE(headers.Find("Box"), nullptr);
}

TEST(SemanticHighlightSpec, AbsoluteQualificationDoesNotIncludeTheReturnKeyword)
{
    const auto headers = Header("namespace a { enum class Kind { Count }; }");
    constexpr std::string_view source = "auto F() { return ::a::Kind::Count; }";

    const auto spans = Highlight(source, &headers);

    ASSERT_TRUE(At(source, spans, "Count"));
    EXPECT_EQ(At(source, spans, "Count") -> kind, HighlightKind::EnumMember);
}

TEST(SemanticHighlightSpec, UnresolvedTemplateQualifiersDoNotSelectGlobalNames)
{
    const auto headers = Header("int Count;");
    constexpr std::string_view source =
        "template<class T> auto F() { return Missing<T>::Count; }";

    const auto spans = Highlight(source, &headers);

    EXPECT_FALSE(At(source, spans, "Count"));
}

TEST(SemanticHighlightSpec, UnknownIdentifiersAreNotInventedVariables)
{
    constexpr std::string_view source = "void F() { unknown; }";

    const auto spans = Highlight(source);

    EXPECT_FALSE(At(source, spans, "unknown"));
}

TEST(SemanticHighlightSpec, InactiveAndLexicalSpansSurviveEnrichment)
{
    constexpr std::string_view source = "#if 0\nint hidden;\n#endif\nint value = 42; // keep\n";

    const auto spans = Highlight(source);

    ASSERT_TRUE(At(source, spans, "hidden"));
    EXPECT_NE(At(source, spans, "hidden")->modifiers & kHighlightInactive, 0u);
    EXPECT_EQ(At(source, spans, "42") -> kind, HighlightKind::Number);
    EXPECT_EQ(At(source, spans, "// keep") -> kind, HighlightKind::Comment);
    for (std::size_t index = 1; index < spans.size(); ++index)
    {
        EXPECT_LE(spans[index - 1].offset + spans[index - 1].length, spans[index].offset);
    }
}

TEST(SemanticHighlightSpec, TemplateParametersRetainTheirCategories)
{
    constexpr std::string_view source = "template<class T, int N> struct Box { T data[N]; };";

    const auto spans = Highlight(source);

    EXPECT_EQ(At(source, spans, "T") -> kind, HighlightKind::TypeParameter);
    EXPECT_EQ(At(source, spans, "N") -> kind, HighlightKind::Parameter);
}

TEST(SemanticHighlightSpec, LambdaParametersDoNotRetagOuterLocalUses)
{
    constexpr std::string_view source =
        "int F() { int value = 1; auto fn = [](int value) { return value; }; return value; }";

    const auto spans = Highlight(source);

    ASSERT_TRUE(At(source, spans, "value", 2));
    EXPECT_EQ(At(source, spans, "value", 2) -> kind, HighlightKind::Parameter);
    ASSERT_TRUE(At(source, spans, "value", 3));
    EXPECT_EQ(At(source, spans, "value", 3) -> kind, HighlightKind::Variable);
}

TEST(SemanticHighlightSpec, StructuredBindingsAreLocalVariables)
{
    constexpr std::string_view source =
        "struct Pair { int first; int second; }; int F(Pair pair) { auto [left, right] = pair; return left + right; }";

    const auto spans = Highlight(source);

    ASSERT_TRUE(At(source, spans, "left", 1));
    EXPECT_EQ(At(source, spans, "left", 1) -> kind, HighlightKind::Variable);
    ASSERT_TRUE(At(source, spans, "right", 1));
    EXPECT_EQ(At(source, spans, "right", 1) -> kind, HighlightKind::Variable);
}

TEST(SemanticHighlightSpec, HeaderSnapshotsTrackContentAndCompileDefines)
{
    const auto path = std::filesystem::path(HEIMDALL_SOURCE_DIR) / "test/fixtures/include/highlight_overlay.hpp";
    SourceOverlay overlay;
    overlay.Add(path, std::make_shared<const std::string>(
        "#ifdef ENUM_KIND\nnamespace edited { enum class Kind { Value }; }\n"
        "#else\nnamespace edited { struct Kind {}; }\n#endif\n"));
    CompileCommand command;
    command.arguments = {"heimdall-fixture-compiler"};
    const std::vector<std::filesystem::path> headers
    {
        path
    };
    const auto initialKey = IncludeIndex::CacheKey(headers, &command, &overlay);

    const auto first = IncludeIndex::Build(headers, &command, {}, &overlay);
    command.defines["ENUM_KIND"] = "1";
    const auto changed = IncludeIndex::Build(headers, &command, {}, &overlay);

    ASSERT_NE(first.Highlights().Find("edited::Kind"), nullptr);
    EXPECT_EQ(first.Highlights().Find("edited::Kind") -> kind, HighlightKind::Class);
    ASSERT_NE(changed.Highlights().Find("edited::Kind"), nullptr);
    EXPECT_EQ(changed.Highlights().Find("edited::Kind") -> kind, HighlightKind::Enum);
    EXPECT_NE(initialKey, IncludeIndex::CacheKey(headers, &command, &overlay));

    const auto definedKey = IncludeIndex::CacheKey(headers, &command, &overlay);
    overlay.Add(path, std::make_shared<const std::string>("namespace edited { struct Fresh {}; }"));
    const auto edited = IncludeIndex::Build(headers, &command, {}, &overlay);

    EXPECT_EQ(edited.Highlights().Find("edited::Kind"), nullptr);
    EXPECT_NE(edited.Highlights().Find("edited::Fresh"), nullptr);
    EXPECT_NE(definedKey, IncludeIndex::CacheKey(headers, &command, &overlay));
}
