#include <gtest/gtest.h>

#include <Heimdall/Completion.hpp>
#include <Heimdall/IncludeIndex.hpp>

#include <filesystem>
#include <string_view>

namespace
{

std::filesystem::path IncludeDir()
{
    return std::filesystem::path(HEIMDALL_SOURCE_DIR) / "test" / "fixtures" / "include";
}

heimdall::CompileCommand CommandWithIncludes()
{
    heimdall::CompileCommand command;
    command.include_directories.push_back(IncludeDir());
    return command;
}

bool Contains(const std::vector<heimdall::CompletionItem>& items, std::string_view label)
{
    for (const auto& item : items)
    {
        if (item.label == label) return true;
    }
    return false;
}

const heimdall::IndexedScope* FindScope(const heimdall::ScopeIndex& index,
                                        std::vector<std::string> path)
{
    for (const auto& scope : index)
    {
        if (scope.path == path) return &scope;
    }
    return nullptr;
}

} // namespace

TEST(IncludeIndexSpec, ResolvesTransitiveQuotedHeaders)
{
    const auto command = CommandWithIncludes();
    constexpr std::string_view text = "#include \"mylib/extra.hpp\"\n";
    const auto headers = heimdall::IncludeIndex::ResolveHeaders(IncludeDir(), text, &command);
    ASSERT_EQ(headers.size(), 2);
    EXPECT_EQ(headers.front().filename(), "extra.hpp");
    EXPECT_EQ(headers.back().filename(), "core.hpp");
}

TEST(IncludeIndexSpec, SkipsMissingHeaders)
{
    const auto command = CommandWithIncludes();
    constexpr std::string_view text = "#include \"nope/missing.hpp\"\nint x = 1;\n";
    EXPECT_TRUE(heimdall::IncludeIndex::ResolveHeaders(IncludeDir(), text, &command).empty());
}

TEST(IncludeIndexSpec, BuildsMergedMemberIndexSkippingReservedNames)
{
    const auto command = CommandWithIncludes();
    constexpr std::string_view text = "#include \"mylib/extra.hpp\"\n";
    const auto index = heimdall::IncludeIndex::Build(IncludeDir(), text, &command);
    ASSERT_FALSE(index.Empty());

    const auto* root = FindScope(index.Scopes(), {});
    ASSERT_NE(root, nullptr);
    EXPECT_TRUE(Contains(root->members, "MYLIB_MODE"));
    EXPECT_TRUE(Contains(root->members, "mylib"));
    EXPECT_FALSE(Contains(root->members, "_MYLIB_CORE_GUARD"));

    const auto* mylib = FindScope(index.Scopes(), { "mylib" });
    ASSERT_NE(mylib, nullptr) << "expected a mylib scope in the index";
    EXPECT_TRUE(Contains(mylib->members, "Widget"));
    EXPECT_TRUE(Contains(mylib->members, "run"));
    EXPECT_TRUE(Contains(mylib->members, "helper_value"));
    EXPECT_TRUE(Contains(mylib->members, "extra_value"));
    EXPECT_FALSE(Contains(mylib->members, "hidden_helper"));
}

TEST(IncludeIndexSpec, CompletesQualifiedMembersFromHeaders)
{
    const auto command = CommandWithIncludes();
    constexpr std::string_view text = "#include \"mylib/extra.hpp\"\nint w = mylib::Wid;\n";
    const auto index = heimdall::IncludeIndex::Build(IncludeDir(), text, &command);
    heimdall::ParserOptions options;
    const auto items = heimdall::CompletionEngine::Complete(text, options, text.size() - 2,
                                                           &index.Scopes());
    EXPECT_TRUE(Contains(items, "Widget"));
    EXPECT_FALSE(Contains(items, "MYLIB_MODE"));
}

TEST(IncludeIndexSpec, CacheKeyTracksHeaderSets)
{
    const auto command = CommandWithIncludes();
    constexpr std::string_view with_extra = "#include \"mylib/extra.hpp\"\n";
    constexpr std::string_view empty = "int x = 1;\n";
    const auto headers = heimdall::IncludeIndex::ResolveHeaders(IncludeDir(), with_extra, &command);
    const auto none = heimdall::IncludeIndex::ResolveHeaders(IncludeDir(), empty, &command);
    EXPECT_NE(heimdall::IncludeIndex::CacheKey(headers, &command),
              heimdall::IncludeIndex::CacheKey(none, &command));
    EXPECT_EQ(heimdall::IncludeIndex::CacheKey(headers, &command),
              heimdall::IncludeIndex::CacheKey(headers, &command));
}

TEST(IncludeIndexSpec, SystemIncludeDiscoveryDoesNotCrash)
{
    // Toolchain-dependent: only asserts the query itself is safe and cached.
    const auto first = heimdall::IncludeIndex::SystemIncludes("c++");
    const auto second = heimdall::IncludeIndex::SystemIncludes("c++");
    EXPECT_EQ(first, second);
}

TEST(IncludeIndexSpec, ProjectIncludesComeBeforeSystemIncludesWhenCapped)
{
    // With a header cap, the quoted (project) include must win over a later
    // angled one: the walk used to spend the whole budget inside the last
    // angled header and never reach the project's own.
    const auto command = CommandWithIncludes();
    constexpr std::string_view text = "#include \"mylib/extra.hpp\"\n#include <mylib/core.hpp>\n";
    heimdall::IncludeIndex::Limits limits;
    limits.max_headers = 1;
    const auto headers = heimdall::IncludeIndex::ResolveHeaders(IncludeDir(), text, &command, limits);
    ASSERT_EQ(headers.size(), 1);
    EXPECT_EQ(headers.front().filename(), "extra.hpp");
}

TEST(IncludeIndexSpec, IndexedItemsRecordTheirHeaderFileAndOffset)
{
    const auto command = CommandWithIncludes();
    constexpr std::string_view text = "#include \"mylib/extra.hpp\"\n";
    const auto index = heimdall::IncludeIndex::Build(IncludeDir(), text, &command);
    ASSERT_FALSE(index.Files().empty());
    bool saw_located = false;
    for (const auto& scope : index.Scopes())
    {
        for (const auto& member : scope.members)
        {
            if (!member.has_location) continue;
            saw_located = true;
            ASSERT_GE(member.file, 0);
            ASSERT_LT(static_cast<std::size_t>(member.file), index.Files().size());
        }
    }
    EXPECT_TRUE(saw_located);
}
