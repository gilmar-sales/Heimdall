#include <gtest/gtest.h>

#include <Heimdall/IncludeAnalyzer.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace
{

    class Project
    {
    public:
        Project()
        {
            static std::atomic<int> counter{0};
            m_root = std::filesystem::temp_directory_path() /
                ("heimdall-include-analyzer-" + std::to_string(counter.fetch_add(1)) + "-" +
                std::to_string(std::filesystem::file_time_type::clock::now().time_since_epoch().count()));
            std::filesystem::create_directories(m_root / "inc");
            m_command.include_directories.push_back(m_root / "inc");
        }

        ~Project()
        {
            std::error_code ec;
            std::filesystem::remove_all(m_root, ec);
        }

        Project(const Project &) = delete;
        Project & operator=(const Project &) = delete;

        void Header(const std::string & name, std::string_view content)
        {
            const auto path = m_root / "inc" / name;
            std::filesystem::create_directories(path.parent_path());
            std::ofstream(path, std::ios::binary) << content;
        }

        const heimdall::CompileCommand * Command() const
        {
            return &m_command;
        }

        std::filesystem::path File(const std::string & name) const
        {
            return m_root / name;
        }

        std::vector<heimdall::Diagnostic> Analyze(std::string_view source, const std::string & file = "main.cpp") const
        {
            const auto tree = heimdall::ParseTree::Parse(source, {});
            const auto profile = heimdall::IncludeAnalyzer::BuildProfile(File(file), tree, &m_command);
            return heimdall::IncludeAnalyzer::Analyze(tree, *profile);
        }

    private:
        std::filesystem::path m_root;
        heimdall::CompileCommand m_command;
    };

    std::vector<std::string> Targets(const std::vector<heimdall::Diagnostic> & diagnostics)
    {
        std::vector<std::string> result;
        for (const auto & diagnostic: diagnostics)
        {
            EXPECT_EQ(diagnostic.code, "cpp/no-unused-include");
            result.push_back(diagnostic.message);
        }

        return result;
    }

} // namespace

TEST(IncludeAnalyzerSpec, ReportsIncludeWhoseNamesAreNeverUsed)
{
    Project project;
    project.Header("a.hpp", "#pragma once\nstruct Alpha { int x; };\n");
    project.Header("b.hpp", "#pragma once\nstruct Beta { int y; };\n");
    constexpr std::string_view source =
        "#include <a.hpp>\n"
        "#include <b.hpp>\n"
        "Alpha value;\n";
    const auto diagnostics = project.Analyze(source);
    ASSERT_EQ(diagnostics.size(), 1);
    EXPECT_EQ(diagnostics[0].message, "included header <b.hpp> is not used directly");
    EXPECT_EQ(diagnostics[0].line, 2);
    EXPECT_EQ(diagnostics[0].rule, heimdall::RuleId::UnusedInclude);
    EXPECT_EQ(source.substr(diagnostics[0].offset, diagnostics[0].length), "<b.hpp>");
}

TEST(IncludeAnalyzerSpec, RecognizesEveryKindOfProvidedName)
{
    Project project;
    project.Header("types.hpp",
        "#pragma once\n"
        "namespace lib {\n"
        "class Widget;\n"
        "enum Color { Red, Green };\n"
        "using Handle = int;\n"
        "typedef long Long;\n"
        "extern int global_counter;\n"
        "int helper(int);\n"
        "template <typename T> struct Box { T value; };\n"
        "}\n"
        "#define SQUARE(x) ((x) * (x))\n"
        "#define TYPES_VERSION 3\n");
    for (const std::string_view use: {"lib::Widget* w;", "int c = lib::Red;", "lib::Handle h;", "lib::Long l;",
        "int n = lib::global_counter;", "int r = lib::helper(1);", "lib::Box<int> b;", "int s = SQUARE(2);",
        "int v = TYPES_VERSION;"})
    {
        const std::string source = "#include <types.hpp>\n" + std::string(use) + "\n";
        EXPECT_TRUE(project.Analyze(source).empty()) << use;
    }

    EXPECT_EQ(project.Analyze("#include <types.hpp>\nint unrelated;\n").size(), 1);
}

TEST(IncludeAnalyzerSpec, IgnoresNamesThatOnlyAppearInCommentsAndStrings)
{
    Project project;
    project.Header("a.hpp", "struct Alpha {};\n");
    constexpr std::string_view source =
        "#include <a.hpp>\n"
        "// Alpha is documented here\n"
        "const char * name = \"Alpha\";\n"
        "int other;\n";
    EXPECT_EQ(project.Analyze(source).size(), 1);
}

TEST(IncludeAnalyzerSpec, ClassMembersAndParametersDoNotCountAsProvidedNames)
{
    Project project;
    project.Header("a.hpp",
        "struct Alpha { int size; void resize(int count); };\n"
        "inline int twice(int value) { int local = value; return local * 2; }\n");
    EXPECT_EQ(project.Analyze("#include <a.hpp>\nint size; int count; int value; int local;\n").size(), 1);
    EXPECT_TRUE(project.Analyze("#include <a.hpp>\nint x = twice(2);\n").empty());
}

TEST(IncludeAnalyzerSpec, TransitiveNamesKeepTheDirectIncludeUsed)
{
    Project project;
    project.Header("base.hpp", "struct Base {};\n");
    project.Header("wrapper.hpp", "#include <base.hpp>\nstruct Wrapper {};\n");
    EXPECT_TRUE(project.Analyze("#include <wrapper.hpp>\nBase b;\n").empty());
}

TEST(IncludeAnalyzerSpec, NamesCoveredByAnotherDirectIncludeDoNotKeepAnIncludeAlive)
{
    Project project;
    project.Header("base.hpp", "struct Base {};\n");
    project.Header("wrapper.hpp", "#include <base.hpp>\nstruct Wrapper {};\n");
    const auto diagnostics = project.Analyze(
        "#include <base.hpp>\n"
        "#include <wrapper.hpp>\n"
        "Base b;\n");
    ASSERT_EQ(diagnostics.size(), 1);
    EXPECT_EQ(diagnostics[0].message, "included header <wrapper.hpp> is not used directly");
}

TEST(IncludeAnalyzerSpec, NeverReportsPrimaryHeaderPragmaKeepOrConditionalIncludes)
{
    Project project;
    project.Header("main.hpp", "struct Primary {};\n");
    project.Header("a.hpp", "struct Alpha {};\n");
    project.Header("b.hpp", "struct Beta {};\n");
    constexpr std::string_view source =
        "#include <main.hpp>\n"
        "#include <a.hpp> // IWYU pragma: keep\n"
        "#ifdef FEATURE\n"
        "#include <b.hpp>\n"
        "#endif\n"
        "int other;\n";
    EXPECT_TRUE(project.Analyze(source).empty());
}

TEST(IncludeAnalyzerSpec, StaysSilentWhenTheHeaderOrItsClosureCannotBeResolved)
{
    Project project;
    project.Header("partial.hpp", "#include <missing_dependency.hpp>\nstruct Partial {};\n");
    constexpr std::string_view source =
        "#include <not_found.hpp>\n"
        "#include <partial.hpp>\n"
        "int other;\n";
    EXPECT_TRUE(project.Analyze(source).empty());
}

TEST(IncludeAnalyzerSpec, StaysSilentForHeadersUsedWithoutBeingNamed)
{
    Project project;
    project.Header("ops.hpp", "struct Money {};\nbool operator==(Money, Money);\n");
    project.Header("hash.hpp", "namespace std { template <> struct hash<int> {}; }\n");
    project.Header("table.inc", "ENTRY(first)\n");
    constexpr std::string_view source =
        "#include <ops.hpp>\n"
        "#include <hash.hpp>\n"
        "#include <table.inc>\n"
        "int other;\n";
    EXPECT_TRUE(project.Analyze(source).empty());
    EXPECT_TRUE(project.Analyze("#include <initializer_list>\nint other;\n").empty());
}

TEST(IncludeAnalyzerSpec, FilesWithOnlyIncludesAreUmbrellaHeaders)
{
    Project project;
    project.Header("a.hpp", "struct Alpha {};\n");
    EXPECT_TRUE(project.Analyze("#pragma once\n#include <a.hpp>\n").empty());
}

TEST(IncludeAnalyzerSpec, IncludeGuardDoesNotMakeIncludesConditional)
{
    Project project;
    project.Header("a.hpp", "struct Alpha {};\n");
    constexpr std::string_view source =
        "#ifndef GUARD_HPP\n"
        "#define GUARD_HPP\n"
        "#include <a.hpp>\n"
        "int other;\n"
        "#endif\n";
    EXPECT_EQ(project.Analyze(source).size(), 1);
}

TEST(IncludeAnalyzerSpec, CountsMacroBodiesAndPreprocessorConditionsAsUses)
{
    Project project;
    project.Header("a.hpp", "struct Alpha {};\n#define FEATURE_A 1\n");
    EXPECT_TRUE(project.Analyze("#include <a.hpp>\n#define MAKE() Alpha{}\nint other;\n").empty());
    EXPECT_TRUE(project.Analyze("#include <a.hpp>\n#ifdef FEATURE_A\nint other;\n#endif\n").empty());
}

TEST(IncludeAnalyzerSpec, ExpandsEnumeratorsOfUnscopedEnums)
{
    Project project;
    project.Header("mode.hpp", "enum Mode { Fast = 1, Slow = Fast + 1, Off };\n");
    EXPECT_TRUE(project.Analyze("#include <mode.hpp>\nint m = Off;\n").empty());
}

TEST(IncludeAnalyzerSpec, FollowsIncludeNextIntoLaterSearchDirectories)
{
    Project project;
    const auto second = project.File("second");
    std::filesystem::create_directories(second);
    std::ofstream(second / "wrap.hpp", std::ios::binary) << "#define FROM_SECOND 1\n";
    project.Header("wrap.hpp", "#include_next <wrap.hpp>\n");
    heimdall::CompileCommand command;
    command.include_directories = {project.File("inc"), second};
    constexpr std::string_view source = "#include <wrap.hpp>\nint v = FROM_SECOND;\n";
    const auto tree = heimdall::ParseTree::Parse(source, {});
    const auto profile = heimdall::IncludeAnalyzer::BuildProfile(project.File("main.cpp"), tree, &command);
    EXPECT_TRUE(heimdall::IncludeAnalyzer::Analyze(tree, *profile).empty());
}

TEST(IncludeAnalyzerSpec, QuickFixDeletesTheWholeDirectiveLineButIsNotSafe)
{
    Project project;
    project.Header("a.hpp", "struct Alpha {};\n");
    constexpr std::string_view source = "#include <a.hpp>\nint other;\n";
    const auto diagnostics = project.Analyze(source);
    ASSERT_EQ(diagnostics.size(), 1);
    EXPECT_TRUE(diagnostics[0].has_fix);
    EXPECT_FALSE(diagnostics[0].fix_is_safe);
    EXPECT_EQ(heimdall::RuleEngine::ApplyFixes(source, diagnostics), source);
    std::string fixed(source);
    fixed.replace(diagnostics[0].fix.offset, diagnostics[0].fix.length, diagnostics[0].fix.replacement);
    EXPECT_EQ(fixed, "int other;\n");
}

TEST(IncludeAnalyzerSpec, PolicyHonorsOverridesAndSuppressions)
{
    Project project;
    project.Header("a.hpp", "struct Alpha {};\n");
    project.Header("b.hpp", "struct Beta {};\n");
    constexpr std::string_view source =
        "#include <a.hpp> // heimdall-disable-line cpp/no-unused-include\n"
        "#include <b.hpp>\n"
        "int other;\n";
    const auto tree = heimdall::ParseTree::Parse(source, {});
    const auto profile = heimdall::IncludeAnalyzer::BuildProfile(project.File("main.cpp"), tree, project.Command());
    const auto raw = heimdall::IncludeAnalyzer::Analyze(tree, *profile);
    ASSERT_EQ(raw.size(), 2);

    const auto filtered = heimdall::RuleEngine().ApplyPolicy(raw, tree);
    ASSERT_EQ(filtered.size(), 1);
    EXPECT_EQ(filtered[0].line, 2);

    heimdall::RuleOptions options;
    options.overrides.push_back({"cpp/no-unused-include", true, heimdall::Severity::Error});
    EXPECT_EQ(heimdall::RuleEngine(options).ApplyPolicy(raw, tree)[0].severity, heimdall::Severity::Error);
    options.overrides.push_back({"cpp/no-unused-include", false, heimdall::Severity::Warning});
    EXPECT_TRUE(heimdall::RuleEngine(options).ApplyPolicy(raw, tree).empty());
}

TEST(IncludeAnalyzerSpec, ProfileIsStaleOnlyWhenAHeaderChangesOnDisk)
{
    Project project;
    project.Header("a.hpp", "struct Alpha {};\n");
    constexpr std::string_view source = "#include <a.hpp>\nAlpha value;\n";
    const auto tree = heimdall::ParseTree::Parse(source, {});
    const auto profile = heimdall::IncludeAnalyzer::BuildProfile(project.File("main.cpp"), tree, project.Command());
    EXPECT_TRUE(heimdall::IncludeAnalyzer::IsFresh(*profile));
    EXPECT_EQ(profile->fingerprint, heimdall::IncludeAnalyzer::Fingerprint(project.File("main.cpp"), tree,
        project.Command()));
    project.Header("a.hpp", "struct Alpha {};\nstruct Changed {};\n");
    EXPECT_FALSE(heimdall::IncludeAnalyzer::IsFresh(*profile));
}

TEST(IncludeAnalyzerSpec, MismatchedProfileProducesNothing)
{
    Project project;
    project.Header("a.hpp", "struct Alpha {};\n");
    const auto before = heimdall::ParseTree::Parse("#include <a.hpp>\nint other;\n", {});
    const auto profile = heimdall::IncludeAnalyzer::BuildProfile(project.File("main.cpp"), before, project.Command());
    const auto after = heimdall::ParseTree::Parse("#include <a.hpp>\n#include <a.hpp>\nint other;\n", {});
    EXPECT_TRUE(heimdall::IncludeAnalyzer::Analyze(after, *profile).empty());
}

TEST(IncludeAnalyzerSpec, MemberAccessNamesDoNotCountAsUses)
{
    Project project;
    project.Header("a.hpp", "int size(int);\nstruct Alpha {};\n");
    EXPECT_EQ(project.Analyze("#include <a.hpp>\nint f(auto s, auto* p) { return s.size + p->size; }\n").size(), 1);
    EXPECT_TRUE(project.Analyze("#include <a.hpp>\nint f() { return size(1); }\n").empty());
}

// Real standard library headers: only meaningful where a C++ compiler can
// report its system include directories.
TEST(IncludeAnalyzerSpec, StandardHeadersAreUsedWhenTheirNamesAreAndReportedOtherwise)
{
    Project project;
    if (project.Analyze("#include <vector>\nint x;\n").empty())
    {
        GTEST_SKIP() << "no system include directories available";
    }

    struct Case
    {
        const char *header;
        const char *use;
    };

    const Case cases[] = {
        {"vector", "std::vector<int> v;"},
        {"string", "std::string s;"},
        {"map", "std::map<int, int> m;"},
        {"iostream", "void f() { std::cout << 1; }"},
        {"algorithm", "void f(int* a) { std::sort(a, a + 3); }"},
        {"memory", "auto p = std::make_unique<int>(1);"},
        {"cstdio", "void f() { std::puts(\"x\"); }"},
        {"climits", "int m = INT_MAX;"},
        {"cstdint", "std::uint32_t n;"},
        {"cassert", "void f() { assert(1); }"},
        {"chrono", "auto d = std::chrono::seconds(1);"},
        {"optional", "std::optional<int> o;"},
        {"mutex", "std::mutex m;"},
        {"utility", "auto p = std::pair<int, int>(1, 2);"},
    };
    for (const auto & test_case: cases)
    {
        const std::string include = std::string("#include <") + test_case.header + ">\n";
        EXPECT_TRUE(project.Analyze(include + test_case.use + "\n").empty()) << test_case.header;
        EXPECT_EQ(project.Analyze(include + "int unrelated;\n").size(), 1) << test_case.header;
    }
}

TEST(IncludeAnalyzerSpec, StandardHeaderNeededOnlyForAMemberCallIsStillReportedWhenUnnamed)
{
    Project project;
    if (project.Analyze("#include <vector>\nint x;\n").empty())
    {
        GTEST_SKIP() << "no system include directories available";
    }

    const auto diagnostics = project.Analyze(
        "#include <vector>\n#include <map>\nstd::vector<int> v;\nint f() { return v.size(); }\n");
    ASSERT_EQ(diagnostics.size(), 1);
    EXPECT_EQ(diagnostics[0].message, "included header <map> is not used directly");
}
