#include <Heimdall/CompileDatabase.hpp>
#include <Heimdall/MacroEnvironment.hpp>
#include <Heimdall/SourceOverlay.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>

namespace
{

    using heimdall::MacroEnvironment;
    using heimdall::MacroEnvironmentOptions;

    class Project
    {
    public:
        Project()
        {
            static std::atomic<unsigned> counter {0};
            mRoot = std::filesystem::temp_directory_path() /
                    ("heimdall-macro-env-" + std::to_string(counter.fetch_add(1)) + "-" +
                std::to_string(reinterpret_cast<std::uintptr_t>(this) % 100000));
            std::filesystem::create_directories(mRoot);
        }

        ~Project()
        {
            std::error_code ec;
            std::filesystem::remove_all(mRoot, ec);
        }

        Project(const Project&) = delete;

        Project& operator= (const Project&) = delete;

        std::filesystem::path Write(std::string_view relative, std::string_view text) const
        {
            const std::filesystem::path path = mRoot / relative;
            std::filesystem::create_directories(path.parent_path());
            std::ofstream out(path, std::ios::binary);
            out << text;
            return path;
        }

        const std::filesystem::path& Root() const noexcept
        {
            return mRoot;
        }

    private:
        std::filesystem::path mRoot;
    };

    MacroEnvironmentOptions Offline()
    {
        MacroEnvironmentOptions options;
        options.probeCompiler = false;
        return options;
    }

    bool Defined(const heimdall::MacroTable& table, std::string_view name)
    {
        return table.Find(name) != nullptr;
    }

} // namespace

TEST(MacroEnvironmentSpec, SeesMacrosFromQuotedIncludesAtTheirPosition)
{
    Project project;
    project.Write("config.h", "#define FEATURE 1\n");
    const std::string text = "int before;\n#include \"config.h\"\nint after;\n";
    const auto main = project.Write("main.cpp", text);

    const auto env = MacroEnvironment::Build(main, text, nullptr, Offline());

    EXPECT_TRUE(Defined(env.Final(), "FEATURE"));
    EXPECT_FALSE(Defined(env.At(text.find("before")), "FEATURE"));
    EXPECT_TRUE(Defined(env.At(text.find("after")), "FEATURE"));
    EXPECT_EQ(env.Final().Find("FEATURE") -> file,
              (project.Root() / "config.h").lexically_normal().generic_string());
    EXPECT_TRUE(env.Complete());
}

TEST(MacroEnvironmentSpec, FollowsIncludeDirectoriesAndNestedHeaders)
{
    Project project;
    project.Write("inc/outer.h", "#include <inner.h>\n#define OUTER INNER_VALUE\n");
    project.Write("inc/inner.h", "#define INNER_VALUE 7\n");
    heimdall::CompileCommand command;
    command.directory = project.Root();
    command.include_directories = {project.Root() / "inc"};
    const std::string text = "#include <outer.h>\n";
    const auto main = project.Write("main.cpp", text);

    const auto env = MacroEnvironment::Build(main, text, &command, Offline());

    EXPECT_TRUE(Defined(env.Final(), "OUTER"));
    EXPECT_TRUE(Defined(env.Final(), "INNER_VALUE"));
    EXPECT_EQ(env.Files().size(), 2u);
}

TEST(MacroEnvironmentSpec, AppliesCommandLineDefinesBeforeTheFile)
{
    Project project;
    heimdall::CompileCommand command;
    command.directory = project.Root();
    command.defines.emplace("LEVEL", "3");
    command.defines.emplace("GONE", "1");
    command.undefines = {"GONE"};
    const std::string text = "#if LEVEL > 2\n#define HIGH 1\n#endif\n";
    const auto main = project.Write("main.cpp", text);

    const auto env = MacroEnvironment::Build(main, text, &command, Offline());

    EXPECT_TRUE(Defined(env.Final(), "HIGH"));
    EXPECT_FALSE(Defined(env.Final(), "GONE"));
    EXPECT_EQ(env.Final().Find("LEVEL") -> origin, heimdall::MacroOrigin::CommandLine);
}

TEST(MacroEnvironmentSpec, EntersForcedIncludesFirst)
{
    Project project;
    const auto forced = project.Write("force.h", "#define FORCED 1\n");
    heimdall::CompileCommand command;
    command.directory = project.Root();
    command.arguments = {"c++", "-include", forced.string()};
    const std::string text = "#ifdef FORCED\n#define SEEN 1\n#endif\n";
    const auto main = project.Write("main.cpp", text);

    const auto env = MacroEnvironment::Build(main, text, &command, Offline());

    EXPECT_TRUE(Defined(env.Final(), "SEEN"));
}

TEST(MacroEnvironmentSpec, SkipsGuardedAndPragmaOnceHeadersTheSecondTime)
{
    Project project;
    project.Write("guard.h", "#ifndef GUARD_H\n#define GUARD_H\n#define COUNT_G 1\n#endif\n");
    project.Write("once.h", "#pragma once\n#define COUNT_O 1\n");
    const std::string text =
        "#include \"guard.h\"\n#undef COUNT_G\n#undef GUARD_H_NOT\n#include \"guard.h\"\n"
        "#include \"once.h\"\n#undef COUNT_O\n#include \"once.h\"\n";
    const auto main = project.Write("main.cpp", text);

    const auto env = MacroEnvironment::Build(main, text, nullptr, Offline());

    EXPECT_FALSE(Defined(env.Final(), "COUNT_G"));
    EXPECT_FALSE(Defined(env.Final(), "COUNT_O"));
}

TEST(MacroEnvironmentSpec, ReentersAHeaderWithoutAGuardEveryTime)
{
    Project project;
    project.Write("xmacro.h", "#ifdef ITEM\nITEM(a)\n#endif\n#undef ITEM\n#define N_ENTERED 1\n");
    const std::string text = "#define ITEM(x) x\n#include \"xmacro.h\"\n#define ITEM(x) x\n"
        "#include \"xmacro.h\"\n";
    const auto main = project.Write("main.cpp", text);

    const auto env = MacroEnvironment::Build(main, text, nullptr, Offline());

    EXPECT_TRUE(Defined(env.Final(), "N_ENTERED"));
    EXPECT_FALSE(Defined(env.Final(), "ITEM"));
    EXPECT_EQ(env.Files().size(), 1u);
}

TEST(MacroEnvironmentSpec, ChoosesIncludeBranchesFromEarlierMacros)
{
    Project project;
    project.Write("a.h", "#define PICKED_A 1\n");
    project.Write("b.h", "#define PICKED_B 1\n");
    const std::string text = "#define USE_A\n#ifdef USE_A\n#include \"a.h\"\n#else\n"
        "#include \"b.h\"\n#endif\n";
    const auto main = project.Write("main.cpp", text);

    const auto env = MacroEnvironment::Build(main, text, nullptr, Offline());

    EXPECT_TRUE(Defined(env.Final(), "PICKED_A"));
    EXPECT_FALSE(Defined(env.Final(), "PICKED_B"));
}

TEST(MacroEnvironmentSpec, ReportsMissingHeadersAsIncomplete)
{
    Project project;
    const std::string text = "#include \"missing.h\"\n#define STILL 1\n";
    const auto main = project.Write("main.cpp", text);

    const auto env = MacroEnvironment::Build(main, text, nullptr, Offline());

    EXPECT_FALSE(env.Complete());
    EXPECT_TRUE(Defined(env.Final(), "STILL"));
    EXPECT_FALSE(env.Main().diagnostics.empty());
}

TEST(MacroEnvironmentSpec, StopsAtTheIncludeDepthLimit)
{
    Project project;
    project.Write("loop.h", "#include \"loop.h\"\n#define DEPTH 1\n");
    const std::string text = "#include \"loop.h\"\n";
    const auto main = project.Write("main.cpp", text);
    auto options = Offline();
    options.maxIncludeDepth = 8;

    const auto env = MacroEnvironment::Build(main, text, nullptr, options);

    EXPECT_FALSE(env.Complete());
    EXPECT_TRUE(Defined(env.Final(), "DEPTH"));
}

TEST(MacroEnvironmentSpec, PrefersOpenBuffersOverDisk)
{
    Project project;
    project.Write("cfg.h", "#define FROM_DISK 1\n");
    const auto header = project.Root() / "cfg.h";
    auto overlay = std::make_shared<heimdall::SourceOverlay>();
    overlay->Add(header, std::make_shared<const std::string>("#define FROM_BUFFER 1\n"));
    auto options = Offline();
    options.overlay = overlay.get();
    const std::string text = "#include \"cfg.h\"\n";
    const auto main = project.Write("main.cpp", text);

    const auto env = MacroEnvironment::Build(main, text, nullptr, options);

    EXPECT_TRUE(Defined(env.Final(), "FROM_BUFFER"));
    EXPECT_FALSE(Defined(env.Final(), "FROM_DISK"));
}

TEST(MacroEnvironmentSpec, FallbackBuiltinsFollowTheStandard)
{
    heimdall::CompileCommand command;
    command.standard = heimdall::CppStandard::Cpp23;

    const auto table = MacroEnvironment::Builtins(&command, false);

    ASSERT_NE(table.Find("__cplusplus"), nullptr);
    EXPECT_EQ(table.Find("__cplusplus") -> body, "202302L");
}

TEST(MacroEnvironmentSpec, ProbedBuiltinsComeFromTheCompilerAndAreCached)
{
    const auto first = MacroEnvironment::Builtins(nullptr, true);
    const auto second = MacroEnvironment::Builtins(nullptr, true);

    EXPECT_NE(first.Find("__cplusplus"), nullptr);
    EXPECT_EQ(first.Size(), second.Size());
}

TEST(MacroEnvironmentSpec, ReplayedHeaderEffectsStillDependOnTheIncomingMacros)
{
    Project project;
    project.Write("pick.h", "#ifdef MODE_A\n#define PICK 1\n#else\n#define PICK 2\n#endif\n");
    const std::string withMode = "#define MODE_A\n#include \"pick.h\"\n";
    const std::string withoutMode = "#include \"pick.h\"\n";
    const auto main = project.Write("main.cpp", withMode);

    const auto first = MacroEnvironment::Build(main, withMode, nullptr, Offline());
    const auto second = MacroEnvironment::Build(main, withoutMode, nullptr, Offline());
    const auto third = MacroEnvironment::Build(main, withMode, nullptr, Offline());

    EXPECT_EQ(first.Final().Find("PICK") -> body, "1");
    EXPECT_EQ(second.Final().Find("PICK") -> body, "2");
    EXPECT_EQ(third.Final().Find("PICK") -> body, "1");
}

TEST(MacroEnvironmentSpec, EditedHeadersAreNotServedFromTheEffectCache)
{
    Project project;
    project.Write("value.h", "#define VALUE 1\n");
    const std::string text = "#include \"value.h\"\n";
    const auto main = project.Write("main.cpp", text);
    const auto before = MacroEnvironment::Build(main, text, nullptr, Offline());

    project.Write("value.h", "#define VALUE 2222\n");
    const auto after = MacroEnvironment::Build(main, text, nullptr, Offline());

    EXPECT_EQ(before.Final().Find("VALUE") -> body, "1");
    EXPECT_EQ(after.Final().Find("VALUE") -> body, "2222");
}

TEST(MacroEnvironmentSpec, ReplayKeepsNestedHeadersAndGuards)
{
    Project project;
    project.Write("outer.h", "#ifndef OUTER_H\n#define OUTER_H\n#include \"inner.h\"\n#endif\n");
    project.Write("inner.h", "#ifndef INNER_H\n#define INNER_H\n#define DEEP 1\n#endif\n");
    const std::string text = "#include \"outer.h\"\n#undef DEEP\n#include \"inner.h\"\n";
    const auto main = project.Write("main.cpp", text);

    const auto cold = MacroEnvironment::Build(main, text, nullptr, Offline());
    const auto warm = MacroEnvironment::Build(main, text, nullptr, Offline());

    EXPECT_FALSE(Defined(cold.Final(), "DEEP"));
    EXPECT_FALSE(Defined(warm.Final(), "DEEP"));
    EXPECT_EQ(cold.Files().size(), warm.Files().size());
    EXPECT_TRUE(Defined(warm.Final(), "INNER_H"));
}

TEST(MacroEnvironmentSpec, ChangedNestedHeaderInvalidatesTheIncluder)
{
    Project project;
    project.Write("outer.h", "#include \"inner.h\"\n#define OUTER 1\n");
    project.Write("inner.h", "#define INNER 1\n");
    const std::string text = "#include \"outer.h\"\n";
    const auto main = project.Write("main.cpp", text);
    (void) MacroEnvironment::Build(main, text, nullptr, Offline());

    project.Write("inner.h", "#define INNER_RENAMED 1\n");
    const auto after = MacroEnvironment::Build(main, text, nullptr, Offline());

    EXPECT_FALSE(Defined(after.Final(), "INNER"));
    EXPECT_TRUE(Defined(after.Final(), "INNER_RENAMED"));
}
