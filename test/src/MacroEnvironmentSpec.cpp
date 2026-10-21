#include <Heimdall/CompileDatabase.hpp>
#include <Heimdall/MacroEnvironment.hpp>
#include <Heimdall/SourceOverlay.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <array>
#include <barrier>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

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

TEST(MacroEnvironmentSpec, WarmReplayObservesNewHasIncludeTargets)
{
    Project project;
    project.Write("outer.h", "#if __has_include(\"optional.h\")\n#define FOUND 1\n#endif\n");
    const std::string text = "#include \"outer.h\"\n";
    const auto main = project.Write("main.cpp", text);
    const auto before = MacroEnvironment::Build(main, text, nullptr, Offline());

    project.Write("optional.h", "");
    const auto after = MacroEnvironment::Build(main, text, nullptr, Offline());

    EXPECT_FALSE(Defined(before.Final(), "FOUND"));
    EXPECT_TRUE(Defined(after.Final(), "FOUND"));
}

TEST(MacroEnvironmentSpec, ReplayRespectsEarlierPragmaOnceIncludes)
{
    Project project;
    project.Write("once.h", "#pragma once\n#define VALUE 1\n");
    project.Write("outer.h", "#include \"once.h\"\n");
    const std::string first = "#include \"outer.h\"\n";
    const auto main = project.Write("main.cpp", first);
    const auto cold = MacroEnvironment::Build(main, first, nullptr, Offline());
    const std::string changed = "#include \"once.h\"\n#undef VALUE\n#include \"outer.h\"\n";

    const auto warm = MacroEnvironment::Build(main, changed, nullptr, Offline());
    auto options = Offline();
    options.reuseEffects = false;
    const auto reference = MacroEnvironment::Build(main, changed, nullptr, options);

    EXPECT_TRUE(Defined(cold.Final(), "VALUE"));
    EXPECT_FALSE(Defined(warm.Final(), "VALUE"));
    EXPECT_EQ(Defined(warm.Final(), "VALUE"), Defined(reference.Final(), "VALUE"));
    const auto reordered = MacroEnvironment::Build(main, first, nullptr, Offline());
    EXPECT_TRUE(Defined(reordered.Final(), "VALUE"));
}

TEST(MacroEnvironmentSpec, ReplayedUndefinitionsStillCountPreviouslyDefinedNames)
{
    Project project;
    project.Write("first.h", "#define TEMPORARY 1\n#undef TEMPORARY\n");
    project.Write("second.h", "#define NEXT 1\n");
    project.Write("third.h", "#define VALUE 1\n");
    const std::string text = "#include \"first.h\"\n#include \"second.h\"\n#include \"third.h\"\n";
    const auto main = project.Write("main.cpp", text);
    auto options = Offline();
    options.maxMacros = 3;
    const auto cold = MacroEnvironment::Build(main, text, nullptr, options);

    const auto warm = MacroEnvironment::Build(main, text, nullptr, options);

    EXPECT_FALSE(Defined(cold.Final(), "VALUE"));
    EXPECT_FALSE(Defined(warm.Final(), "VALUE"));
    EXPECT_EQ(cold.Complete(), warm.Complete());
}

TEST(MacroEnvironmentSpec, CachedContentDoesNotReplaceEditedOverlayBuffers)
{
    Project project;
    const auto header = project.Write("config.h", "#define VALUE 1\n");
    const std::string text = "#include \"config.h\"\n";
    const auto main = project.Write("main.cpp", text);
    const auto cold = MacroEnvironment::Build(main, text, nullptr, Offline());
    heimdall::SourceOverlay overlay;
    overlay.Add(header, std::make_shared<const std::string>("#define VALUE 2\n"));
    auto options = Offline();
    options.overlay = &overlay;

    const auto edited = MacroEnvironment::Build(main, text, nullptr, options);
    const auto disk = MacroEnvironment::Build(main, text, nullptr, Offline());

    ASSERT_NE(edited.Final().Find("VALUE"), nullptr);
    EXPECT_EQ(edited.Final().Find("VALUE") -> body, "2");
    EXPECT_EQ(disk.Final().Find("VALUE") -> body, "1");
    EXPECT_EQ(cold.Final().Find("VALUE") -> body, "1");
}

TEST(MacroEnvironmentSpec, UncachedEffectsReuseOnlyRevalidatedFileContents)
{
    Project project;
    project.Write("config.h", "#define VALUE 1\n");
    const std::string text = "#include \"config.h\"\n";
    const auto main = project.Write("main.cpp", text);
    auto options = Offline();
    options.reuseEffects = false;
    const auto first = MacroEnvironment::Build(main, text, nullptr, options);

    const auto second = MacroEnvironment::Build(main, text, nullptr, options);
    project.Write("config.h", "#define VALUE 222\n");
    const auto edited = MacroEnvironment::Build(main, text, nullptr, options);

    EXPECT_GT(first.Metrics().filesRead, 0u);
    EXPECT_EQ(second.Metrics().filesRead, 0u);
    EXPECT_GT(second.Metrics().metadataQueries, 0u);
    EXPECT_EQ(edited.Final().Find("VALUE") -> body, "222");
}

TEST(MacroEnvironmentSpec, WarmReplayObservesHigherPriorityHeaders)
{
    Project project;
    project.Write("outer.h", "#include <choice.h>\n");
    project.Write("low/choice.h", "#define VALUE 1\n");
    std::filesystem::create_directories(project.Root() / "high");
    const std::string text = "#include \"outer.h\"\n";
    const auto main = project.Write("main.cpp", text);
    heimdall::CompileCommand command;
    command.directory = project.Root();
    command.include_directories = {project.Root() / "high", project.Root() / "low"};
    const auto before = MacroEnvironment::Build(main, text, &command, Offline());

    project.Write("high/choice.h", "#define VALUE 2\n");
    const auto after = MacroEnvironment::Build(main, text, &command, Offline());

    ASSERT_NE(before.Final().Find("VALUE"), nullptr);
    ASSERT_NE(after.Final().Find("VALUE"), nullptr);
    EXPECT_EQ(before.Final().Find("VALUE") -> body, "1");
    EXPECT_EQ(after.Final().Find("VALUE") -> body, "2");
}

TEST(MacroEnvironmentSpec, ReplayedEffectsRespectFileAndDepthLimits)
{
    Project project;
    project.Write("outer.h", "#include \"inner.h\"\n");
    project.Write("inner.h", "#define VALUE 1\n");
    const std::string text = "#include \"outer.h\"\n";
    const auto main = project.Write("main.cpp", text);
    const auto warm = MacroEnvironment::Build(main, text, nullptr, Offline());

    auto small = Offline();
    small.maxFileBytes = 1;
    const auto sizeLimited = MacroEnvironment::Build(main, text, nullptr, small);
    auto shallow = Offline();
    shallow.maxIncludeDepth = 1;
    const auto depthLimited = MacroEnvironment::Build(main, text, nullptr, shallow);

    EXPECT_TRUE(Defined(warm.Final(), "VALUE"));
    EXPECT_FALSE(Defined(sizeLimited.Final(), "VALUE"));
    EXPECT_FALSE(Defined(depthLimited.Final(), "VALUE"));
    EXPECT_FALSE(sizeLimited.Complete());
    EXPECT_FALSE(depthLimited.Complete());
}

TEST(MacroEnvironmentSpec, RelativeQuoteDirectoriesUseCompileWorkingDirectory)
{
    Project project;
    project.Write("include/config.h", "#define VALUE 1\n");
    const std::string text = "#include \"config.h\"\n";
    const auto main = project.Write("src/main.cpp", text);
    heimdall::CompileCommand command;
    command.directory = project.Root();
    command.quote_directories = {"include"};

    const auto environment = MacroEnvironment::Build(main, text, &command, Offline());

    EXPECT_TRUE(Defined(environment.Final(), "VALUE"));
    EXPECT_TRUE(environment.Complete());
}

TEST(MacroEnvironmentSpec, ReplayValidatesEachDependencyOncePerBuild)
{
    Project project;
    project.Write("common.h", "#pragma once\n#define VALUE 1\n");
    project.Write("left.h", "#include \"common.h\"\n");
    project.Write("right.h", "#include \"common.h\"\n");
    const std::string text = "#include \"left.h\"\n#include \"right.h\"\n";
    const auto main = project.Write("main.cpp", text);
    const auto cold = MacroEnvironment::Build(main, text, nullptr, Offline());

    const auto warm = MacroEnvironment::Build(main, text, nullptr, Offline());
    auto uncached = Offline();
    uncached.reuseEffects = false;
    const auto reference = MacroEnvironment::Build(main, text, nullptr, uncached);

    EXPECT_GT(warm.Metrics().effectsReplayed, 0u);
    EXPECT_LE(warm.Metrics().metadataQueries, warm.Files().size());
    EXPECT_EQ(warm.Final().Find("VALUE") -> body, reference.Final().Find("VALUE")->body);
    EXPECT_EQ(warm.Files(), reference.Files());
    EXPECT_EQ(cold.Complete(), warm.Complete());
}

TEST(MacroEnvironmentSpec, ConcurrentContextsKeepIndependentMacroEffects)
{
    Project project;
    project.Write("config.h", "#if MODE == 1\n#define VALUE 10\n#else\n#define VALUE 20\n#endif\n");
    const std::string text = "#include \"config.h\"\n";
    const auto main = project.Write("main.cpp", text);
    constexpr std::size_t kWorkers = 4;
    std::barrier start(static_cast<std::ptrdiff_t>(kWorkers));
    std::vector<std::jthread> workers;
    std::array<std::string, kWorkers> values;

    for (std::size_t i = 0; i < kWorkers; ++i)
    {
        workers.emplace_back([&, i]
            {
                heimdall::CompileCommand command;
                command.defines["MODE"] = i % 2 == 0 ? "1" : "2";
                start.arrive_and_wait();
                for (int repeat = 0; repeat < 4; ++repeat)
                {
                    const auto environment = MacroEnvironment::Build(main, text, &command, Offline());
                    if (const auto value = environment.Final().Find("VALUE"))
                    {
                        values[i] = value->body;
                    }
                }
            });
    }

    workers.clear();

    for (std::size_t i = 0; i < kWorkers; ++i)
    {
        EXPECT_EQ(values[i], i % 2 == 0 ? "10" : "20");
    }
}

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

TEST(MacroEnvironmentSpec, StopsEnteringHeadersPastTheMacroLimit)
{
    Project project;
    std::string many = "#pragma once\n";
    for (int i = 0; i < 12; ++i)
    {
        many += "#define BULK_" + std::to_string(i) + " " + std::to_string(i) + "\n";
    }

    project.Write("bulk.h", many);
    project.Write("late.h", "#pragma once\n#define LATE 1\n");
    const std::string text = "#include \"bulk.h\"\n#include \"late.h\"\n";
    const auto main = project.Write("main.cpp", text);
    auto options = Offline();
    options.maxMacros = 8;

    const auto env = MacroEnvironment::Build(main, text, nullptr, options);

    EXPECT_FALSE(env.Complete());
    EXPECT_TRUE(Defined(env.Final(), "BULK_0"));
    EXPECT_FALSE(Defined(env.Final(), "LATE"));
}
