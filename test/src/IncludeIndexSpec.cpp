#include <gtest/gtest.h>

#include <Heimdall/Completion.hpp>
#include <Heimdall/IncludeIndex.hpp>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
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

namespace
{

std::filesystem::path ResolveAt(std::string_view text, std::string_view marker)
{
    const auto command = CommandWithIncludes();
    return heimdall::IncludeIndex::ResolveIncludeAt(IncludeDir(), text, text.find(marker), &command);
}

} // namespace

TEST(IncludeIndexSpec, ResolveIncludeAtFindsQuotedAndAngledHeaders)
{
    constexpr std::string_view text = "#include \"mylib/core.hpp\"\n#include <mylib/extra.hpp>\n";
    EXPECT_EQ(ResolveAt(text, "mylib/core").filename(), "core.hpp");
    EXPECT_EQ(ResolveAt(text, "mylib/extra").filename(), "extra.hpp");
}

TEST(IncludeIndexSpec, ResolveIncludeAtAcceptsAnyColumnOfTheLine)
{
    constexpr std::string_view text = "#include <mylib/core.hpp>\n";
    EXPECT_EQ(ResolveAt(text, "#include").filename(), "core.hpp");
    EXPECT_EQ(ResolveAt(text, ">").filename(), "core.hpp");
}

TEST(IncludeIndexSpec, ResolveIncludeAtHandlesCrlfLineEndings)
{
    constexpr std::string_view text = "#include <gtest_missing.h>\r\n\r\n#include <mylib/core.hpp>\r\nint x;\r\n";
    EXPECT_EQ(ResolveAt(text, "mylib/core").filename(), "core.hpp");
}

TEST(IncludeIndexSpec, ResolveIncludeAtIgnoresOtherLinesAndMissingHeaders)
{
    constexpr std::string_view text = "#include <mylib/core.hpp>\nint x = 1;\n#include <nope/missing.hpp>\n";
    EXPECT_TRUE(ResolveAt(text, "int x").empty());
    EXPECT_TRUE(ResolveAt(text, "nope/missing").empty());
}

// Regression: `#include <vector>` must resolve to the header file (not to the
// homonymous type) through the compiler's system include directories.
TEST(IncludeIndexSpec, ResolveIncludeAtFindsSystemHeadersNamedLikeTypes)
{
    if (heimdall::IncludeIndex::SystemIncludes("c++").empty())
    {
        GTEST_SKIP() << "no C++ compiler on PATH";
    }

    constexpr std::string_view text = "#include <vector>\n#include <string>\n";
    for (const std::string_view name: {"vector", "string"})
    {
        const auto header = heimdall::IncludeIndex::ResolveIncludeAt(IncludeDir(), text, text.find(name), nullptr);
        ASSERT_FALSE(header.empty()) << name;
        EXPECT_EQ(header.filename().string(), name);
    }
}

TEST(IncludeIndexSpec, MemberAccessResolvesGuardedDecoratedHeaderTypes)
{
    auto command = CommandWithIncludes();
    command.defines["MYLIB_API"] = "";
    command.defines["MYLIB_NOEXCEPT"] = "noexcept";
    command.defines["MYLIB_NODISCARD"] = "[[nodiscard]]";
    constexpr std::string_view text = "#include \"mylib/shapes.hpp\"\n";
    const auto index = heimdall::IncludeIndex::Build(IncludeDir(), text, &command);

    const auto* shape = FindScope(index.Scopes(), {"mylib", "Shape"});
    ASSERT_NE(shape, nullptr);
    EXPECT_EQ(shape->bases, std::vector<std::string>{"Base"});

    std::string source = "void f() { mylib::ShapeAlias s; s.";
    heimdall::ParserOptions options;
    options.shared_macros = std::make_shared<const heimdall::Preprocessor::MacroMap>(command.defines);
    const auto items = heimdall::CompletionEngine::Complete(source, options, source.size(), &index.Scopes());
    EXPECT_TRUE(Contains(items, "area"));
    EXPECT_TRUE(Contains(items, "cached"));
    EXPECT_TRUE(Contains(items, "base_value")); // inherited
    EXPECT_TRUE(Contains(items, "base_run"));
}

namespace
{

    class IncludeTree
    {
    public:
        IncludeTree()
        {
            static std::atomic<int> counter{0};
            m_root = std::filesystem::temp_directory_path() /
                ("heimdall-include-completion-" + std::to_string(counter.fetch_add(1)) + "-" +
                std::to_string(std::filesystem::file_time_type::clock::now().time_since_epoch().count()));
            Touch("src/local.hpp");
            Touch("src/notes.txt");
            Touch("src/main.cpp");
            Touch("src/.hidden.hpp");
            Touch("src/sub/nested.hpp");
            Touch("inc/lib/api.hpp");
            Touch("inc/lib/detail/impl.hpp");
            Touch("inc/shared.hpp");
            Touch("inc/local.hpp");
            Touch("quote/only_quoted.hpp");
            Touch("extra/extra.h");
            m_command.include_directories = {m_root / "inc", m_root / "extra"};
            m_command.quote_directories = {m_root / "quote"};
        }

        ~IncludeTree()
        {
            std::error_code ec;
            std::filesystem::remove_all(m_root, ec);
        }

        IncludeTree(const IncludeTree &) = delete;
        IncludeTree & operator=(const IncludeTree &) = delete;

        std::filesystem::path Src() const
        {
            return m_root / "src";
        }

        const heimdall::CompileCommand * Command() const
        {
            return &m_command;
        }

        std::vector<std::string> Labels(bool angled, std::string_view typed) const
        {
            std::vector<std::string> labels;
            for (const auto & candidate: heimdall::IncludeIndex::CompleteIncludePath(Src(), angled, typed, &m_command))
            {
                if (candidate.origin != heimdall::IncludeOrigin::System)
                {
                    labels.push_back(candidate.label);
                }
            }

            return labels;
        }

    private:
        void Touch(const std::string & relative) const
        {
            const auto path = m_root / relative;
            std::filesystem::create_directories(path.parent_path());
            std::ofstream(path, std::ios::binary) << "// fixture\n";
        }

        std::filesystem::path m_root;
        heimdall::CompileCommand m_command;
    };

    bool Has(const std::vector<std::string> & labels, std::string_view label)
    {
        return std::find(labels.begin(), labels.end(), label) != labels.end();
    }

} // namespace

TEST(IncludeCompletionSpec, QuotedIncludesSearchLocalAndQuoteDirectoriesFirst)
{
    IncludeTree tree;
    const auto labels = tree.Labels(false, "");
    EXPECT_TRUE(Has(labels, "local.hpp"));
    EXPECT_TRUE(Has(labels, "sub/"));
    EXPECT_TRUE(Has(labels, "only_quoted.hpp"));
    EXPECT_TRUE(Has(labels, "shared.hpp"));
    EXPECT_TRUE(Has(labels, "lib/"));
    EXPECT_TRUE(Has(labels, "extra.h"));
}

TEST(IncludeCompletionSpec, AngledIncludesNeverSearchTheIncludingDirectoryOrQuoteDirectories)
{
    IncludeTree tree;
    const auto labels = tree.Labels(true, "");
    EXPECT_FALSE(Has(labels, "sub/"));
    EXPECT_FALSE(Has(labels, "only_quoted.hpp"));
    EXPECT_TRUE(Has(labels, "shared.hpp"));
    EXPECT_TRUE(Has(labels, "lib/"));
    EXPECT_TRUE(Has(labels, "extra.h"));
    // `local.hpp` exists next to the file and in -I: only the -I copy may be offered.
    const auto candidates = heimdall::IncludeIndex::CompleteIncludePath(tree.Src(), true, "local", tree.Command());
    std::vector<heimdall::IncludeCandidate> project;
    for (const auto & candidate: candidates)
    {
        if (candidate.origin != heimdall::IncludeOrigin::System)
        {
            project.push_back(candidate);
        }
    }

    ASSERT_EQ(project.size(), 1);
    EXPECT_EQ(project[0].origin, heimdall::IncludeOrigin::Include);
}

TEST(IncludeCompletionSpec, QuotedDuplicateKeepsTheFirstDirectoryInSearchOrder)
{
    IncludeTree tree;
    const auto candidates = heimdall::IncludeIndex::CompleteIncludePath(tree.Src(), false, "local", tree.Command());
    std::size_t count = 0;
    for (const auto & candidate: candidates)
    {
        if (candidate.label == "local.hpp")
        {
            ++count;
            EXPECT_EQ(candidate.origin, heimdall::IncludeOrigin::Local);
        }
    }

    EXPECT_EQ(count, 1);
}

TEST(IncludeCompletionSpec, CompletesInsideDirectoriesAlreadyTyped)
{
    IncludeTree tree;
    EXPECT_EQ(tree.Labels(true, "lib/"), (std::vector<std::string>{"detail/", "api.hpp"}));
    EXPECT_EQ(tree.Labels(true, "lib/detail/"), (std::vector<std::string>{"impl.hpp"}));
    EXPECT_EQ(tree.Labels(false, "sub/"), (std::vector<std::string>{"nested.hpp"}));
    EXPECT_TRUE(tree.Labels(true, "sub/").empty());
    EXPECT_TRUE(tree.Labels(true, "missing/").empty());
}

TEST(IncludeCompletionSpec, FiltersByPrefixIgnoringCase)
{
    IncludeTree tree;
    EXPECT_EQ(tree.Labels(true, "SHA"), (std::vector<std::string>{"shared.hpp"}));
    EXPECT_EQ(tree.Labels(true, "lib/AP"), (std::vector<std::string>{"api.hpp"}));
    EXPECT_TRUE(tree.Labels(true, "zzz").empty());
}

TEST(IncludeCompletionSpec, SkipsSourcesBinariesAndDotFiles)
{
    IncludeTree tree;
    const auto labels = tree.Labels(false, "");
    EXPECT_FALSE(Has(labels, "main.cpp"));
    EXPECT_FALSE(Has(labels, "notes.txt"));
    EXPECT_FALSE(Has(labels, ".hidden.hpp"));
}

TEST(IncludeCompletionSpec, ProjectEntriesComeBeforeSystemOnesAndDirectoriesFirstWithinAnOrigin)
{
    IncludeTree tree;
    const auto candidates = heimdall::IncludeIndex::CompleteIncludePath(tree.Src(), true, "", tree.Command());
    for (std::size_t i = 0; i < candidates.size(); ++i)
    {
        EXPECT_EQ(candidates[i].directory, candidates[i].label.back() == '/');
        if (i == 0)
        {
            continue;
        }

        const auto & previous = candidates[i - 1];
        EXPECT_LE(static_cast<int>(previous.origin), static_cast<int>(candidates[i].origin));
        if (previous.origin == candidates[i].origin)
        {
            EXPECT_FALSE(!previous.directory && candidates[i].directory) << candidates[i].label;
        }
    }
}

TEST(IncludeCompletionSpec, TheLimitNeverDropsProjectHeadersForSystemOnes)
{
    IncludeTree tree;
    const auto candidates = heimdall::IncludeIndex::CompleteIncludePath(tree.Src(), true, "", tree.Command(), 4);
    ASSERT_EQ(candidates.size(), 4);
    for (const auto & candidate: candidates)
    {
        EXPECT_EQ(candidate.origin, heimdall::IncludeOrigin::Include) << candidate.label;
    }
}

TEST(IncludeCompletionSpec, AbsolutePathsAreListedAsTyped)
{
    IncludeTree tree;
    const std::string typed = (tree.Src() / "sub").generic_string() + "/";
    const auto candidates = heimdall::IncludeIndex::CompleteIncludePath({}, false, typed, tree.Command());
    ASSERT_EQ(candidates.size(), 1);
    EXPECT_EQ(candidates[0].label, "nested.hpp");
    EXPECT_EQ(candidates[0].origin, heimdall::IncludeOrigin::Absolute);
}

TEST(IncludeCompletionSpec, WorksWithoutACompileCommand)
{
    IncludeTree tree;
    const auto candidates = heimdall::IncludeIndex::CompleteIncludePath(tree.Src(), false, "loc", nullptr);
    const auto local = std::find_if(candidates.begin(), candidates.end(),
        [](const heimdall::IncludeCandidate &candidate) { return candidate.label == "local.hpp"; });
    ASSERT_NE(local, candidates.end());
    EXPECT_EQ(local->origin, heimdall::IncludeOrigin::Local);
}

TEST(IncludeCompletionSpec, HonorsTheResultLimit)
{
    IncludeTree tree;
    EXPECT_EQ(heimdall::IncludeIndex::CompleteIncludePath(tree.Src(), false, "", tree.Command(), 2).size(), 2);
}

TEST(IncludeContextSpec, DetectsTheDelimiterAndTheTypedOffset)
{
    constexpr std::string_view text = "int a;\n#include <vec";
    const auto angled = heimdall::IncludeIndex::IncludeContextAt(text, text.size());
    ASSERT_TRUE(angled.has_value());
    EXPECT_TRUE(angled->angled);
    EXPECT_EQ(text.substr(angled->typed_offset), "vec");

    constexpr std::string_view quoted = "  #  include_next \"dir/";
    const auto context = heimdall::IncludeIndex::IncludeContextAt(quoted, quoted.size());
    ASSERT_TRUE(context.has_value());
    EXPECT_FALSE(context->angled);
    EXPECT_EQ(quoted.substr(context->typed_offset), "dir/");

    const auto empty = heimdall::IncludeIndex::IncludeContextAt("#include <", 10);
    ASSERT_TRUE(empty.has_value());
    EXPECT_EQ(empty->typed_offset, 10);
}

TEST(IncludeContextSpec, RejectsEverythingThatIsNotAnOpenIncludeDelimiter)
{
    using heimdall::IncludeIndex;
    EXPECT_FALSE(IncludeIndex::IncludeContextAt("#include <vector>", 17).has_value());
    EXPECT_FALSE(IncludeIndex::IncludeContextAt("#include \"a.h\"", 14).has_value());
    EXPECT_FALSE(IncludeIndex::IncludeContextAt("#include ", 9).has_value());
    EXPECT_FALSE(IncludeIndex::IncludeContextAt("#define X <", 11).has_value());
    EXPECT_FALSE(IncludeIndex::IncludeContextAt("int a = 1 < 2;", 12).has_value());
    EXPECT_FALSE(IncludeIndex::IncludeContextAt("// #include <", 13).has_value());
    EXPECT_FALSE(IncludeIndex::IncludeContextAt("#includes <a", 12).has_value());
    // Cursor in the middle of a closed include: the name is being edited, not started.
    const auto inside = heimdall::IncludeIndex::IncludeContextAt("#include <vector>", 12);
    ASSERT_TRUE(inside.has_value());
    EXPECT_TRUE(inside->angled);
}

TEST(IncludeIndexSpec, TypeNamesFeedTheParserOfTheIncludingFile)
{
    auto command = CommandWithIncludes();
    command.defines["MYLIB_API"] = "";
    command.defines["MYLIB_NOEXCEPT"] = "noexcept";
    command.defines["MYLIB_NODISCARD"] = "[[nodiscard]]";
    const std::string text =
        "#include \"mylib/shapes.hpp\"\n"
        "void f(void *p, int a, int b) {\n"
        "    auto s = (mylib::Shape*)p;\n"
        "    mylib::Base * base = nullptr;\n"
        "    a * b;\n"
        "}\n";
    const auto index = heimdall::IncludeIndex::Build(IncludeDir(), text, &command);
    ASSERT_NE(index.TypeNames(), nullptr);
    EXPECT_TRUE(index.TypeNames()->IsType("Shape"));
    EXPECT_TRUE(index.TypeNames()->IsType("Base"));
    EXPECT_TRUE(index.TypeNames()->IsType("ShapeAlias"));
    EXPECT_FALSE(index.TypeNames()->IsType("area")); // a member function
    EXPECT_FALSE(index.TypeNames()->IsType("base_value"));

    const auto count = [](const heimdall::ParseTree &tree, heimdall::GrammarKind kind)
    {
        std::size_t n = 0;
        for (const auto &node: tree.Nodes())
        {
            n += node.kind == kind;
        }

        return n;
    };
    heimdall::ParserOptions options;
    options.type_names = index.TypeNames();
    const auto with_headers = heimdall::ParseTree::Parse(text, options);
    EXPECT_EQ(count(with_headers, heimdall::GrammarKind::CastExpression), 1);
    EXPECT_EQ(count(with_headers, heimdall::GrammarKind::ExpressionStatement), 1); // `a * b;`

    // A second index over the same headers answers the same: reuse across parses stays valid.
    const auto again = heimdall::IncludeIndex::Build(IncludeDir(), text, &command);
    EXPECT_EQ(again.TypeNames()->Fingerprint(), index.TypeNames()->Fingerprint());
    const auto without = heimdall::ParseTree::Parse(text, {});
    EXPECT_EQ(count(without, heimdall::GrammarKind::CastExpression), 0);
}
