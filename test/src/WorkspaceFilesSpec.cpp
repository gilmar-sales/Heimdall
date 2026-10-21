#include "Document.hpp"
#include "WorkspaceFiles.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <stop_token>
#include <string>
#include <vector>

namespace
{

    namespace fs = std::filesystem;

    class WorkspaceFilesSpec : public::testing::Test
    {
    protected:
        void SetUp() override
        {
            static std::atomic<int> counter {0};
            const std::string unique = std::to_string(counter.fetch_add(1)) + "_" +
                testing::UnitTest::GetInstance()->current_test_info()->name();
            mRoot = fs::temp_directory_path() /("heimdall_workspace_files_" + unique);
            fs::remove_all(mRoot);
            fs::create_directories(mRoot);
        }

        void TearDown() override
        {
            std::error_code error;
            fs::remove_all(mRoot, error);
        }

        void Write(const std::string& relative, const std::string& text = "int x;\n")
        {
            const fs::path path = mRoot / relative;
            fs::create_directories(path.parent_path());
            std::ofstream(path, std::ios::binary) << text;
        }

        [[nodiscard]] std::vector<std::string> Relative(const std::vector<fs::path>& files) const
        {
            std::vector<std::string> names;
            for (const fs::path& file : files)
            {
                names.push_back(fs::relative(file, mRoot).generic_string());
            }

            return names;
        }

        fs::path mRoot;
    };

} // namespace

TEST_F(WorkspaceFilesSpec, UnrenderedErbTemplatesDoNotEnterTheWorkspaceIndex)
{
    Write("src/generated.erb.cpp", "<%= input_size %>\n");
    Write("include/generated.erb.hpp", "<%= input_size %>\n");
    Write("src/generated.cpp");

    const auto files = heimdall::lsp::DiscoverWorkspaceSources(mRoot, {});

    EXPECT_EQ(Relative(files), (std::vector<std::string> {"src/generated.cpp"}));
    EXPECT_FALSE(heimdall::lsp::IsWorkspaceSource("generated.erb.cpp"));
    EXPECT_TRUE(heimdall::lsp::IsWorkspaceSource("erb.cpp"));
}

TEST_F(WorkspaceFilesSpec, RecognizesCppSourcesAndHeaders)
{
    using heimdall::lsp::IsWorkspaceSource;

    for (const char* name :
        {
            "a.cpp", "a.cc", "a.cxx", "a.c++", "a.hpp", "a.hh", "a.hxx", "a.h",
            "a.ipp", "a.tpp", "a.inl", "a.cppm", "a.ixx"
    })
    {
        EXPECT_TRUE(IsWorkspaceSource(name)) << name;
    }

    for (const char* name :
        {
            "a.txt", "a.py", "Makefile", "a.cpp.bak", "a", "a.hpp.in"
    })
    {
        EXPECT_FALSE(IsWorkspaceSource(name)) << name;
    }
}

TEST_F(WorkspaceFilesSpec, SkipsBuildAndToolDirectories)
{
    using heimdall::lsp::IsSkippedDirectory;

    for (const char* name :
        {
            ".git", ".vscode", "build", "build-release", "cmake-build-debug",
            "node_modules", "_deps", "vcpkg_installed", "out"
    })
    {
        EXPECT_TRUE(IsSkippedDirectory(name)) << name;
    }

    for (const char* name :
        {
            "src", "include", "rebuild", "output", "lib"
    })
    {
        EXPECT_FALSE(IsSkippedDirectory(name)) << name;
    }
}

TEST_F(WorkspaceFilesSpec, DiscoversSortedSourcesAndSkipsIgnoredDirectories)
{
    Write("src/b.cpp");
    Write("src/a.hpp");
    Write("src/deep/er/c.h");
    Write("build/generated.cpp");
    Write(".git/hooks.cpp");
    Write("node_modules/pkg/addon.cpp");
    Write("cmake-build-debug/x.cpp");
    Write("docs/readme.md");
    Write("top.cc");

    const auto files = heimdall::lsp::DiscoverWorkspaceSources(mRoot, std::stop_token {});

    EXPECT_EQ(Relative(files), (std::vector<std::string> {"src/a.hpp", "src/b.cpp",
                                "src/deep/er/c.h", "top.cc"}));
}

TEST_F(WorkspaceFilesSpec, DiscoveryHonorsTheFileLimit)
{
    for (int i = 0; i < 10; ++i)
    {
        Write("f" + std::to_string(i) + ".cpp");
    }

    const auto files = heimdall::lsp::DiscoverWorkspaceSources(mRoot, std::stop_token {}, 4);

    EXPECT_EQ(files.size(), 4u);
}

TEST_F(WorkspaceFilesSpec, DiscoversDependencySourcesInsideBuildDirectories)
{
    Write("build/_deps/freyr-src/src/Core.cpp");
    Write("cmake-build-debug/_deps/skirnir-src/include/Services.hpp");
    Write("_deps/library-src/include/Library.hpp");
    Write("build/_deps/freyr-build/generated.cpp");
    Write("build/_deps/freyr-subbuild/probe.cpp");
    Write("build/generated.cpp");

    const auto files = heimdall::lsp::DiscoverWorkspaceSources(mRoot, {});

    EXPECT_EQ(Relative(files), (std::vector<std::string> {
            "_deps/library-src/include/Library.hpp",
            "build/_deps/freyr-src/src/Core.cpp",
            "cmake-build-debug/_deps/skirnir-src/include/Services.hpp"}));
}

TEST_F(WorkspaceFilesSpec, RecognizesDependencyDirectories)
{
    using heimdall::lsp::IsDependencyDirectory;

    for (const char* name :
        {
            "_deps", "third_party", "thirdparty", "external", "extern", "vendor"
    })
    {
        EXPECT_TRUE(IsDependencyDirectory(name)) << name;
    }

    for (const char* name :
        {
            "src", "include", "build", "out", "deps", "third-party", "externals"
    })
    {
        EXPECT_FALSE(IsDependencyDirectory(name)) << name;
    }
}

TEST_F(WorkspaceFilesSpec, RecognizesDependencySourcesBelowTheRoot)
{
    using heimdall::lsp::IsDependencySource;

    EXPECT_TRUE(IsDependencySource(mRoot, mRoot / "build" / "_deps" / "simdjson-src" / "simdjson.cpp"));
    EXPECT_TRUE(IsDependencySource(mRoot, mRoot / "_deps" / "library-src" / "Library.hpp"));
    EXPECT_TRUE(IsDependencySource(mRoot, mRoot / "third_party" / "simdjson" / "simdjson.cpp"));
    EXPECT_TRUE(IsDependencySource(mRoot, mRoot / "libs" / "external" / "widget" / "Widget.hpp"));
    EXPECT_TRUE(IsDependencySource(mRoot, mRoot / "vendor" / "json.hpp"));

    EXPECT_FALSE(IsDependencySource(mRoot, mRoot / "src" / "Core.cpp"));
    EXPECT_FALSE(IsDependencySource(mRoot, mRoot / "src" / "deps" / "Helper.cpp"));
    EXPECT_FALSE(IsDependencySource(mRoot / "project", mRoot / "other" / "a.cpp"));
    EXPECT_FALSE(IsDependencySource(mRoot, mRoot));
    EXPECT_FALSE(IsDependencySource(fs::path {}, mRoot / "_deps" / "a.cpp"));
}

TEST_F(WorkspaceFilesSpec, ProjectDiscoverySkipsDependenciesButKeepsThemIndexed)
{
    Write("src/Core.cpp");
    Write("build/_deps/simdjson-src/simdjson.cpp");
    Write("_deps/library-src/include/Library.hpp");
    Write("third_party/fmt/format.cpp");
    Write("external/widget/Widget.hpp");
    Write("vendor/json.hpp");

    const auto full = heimdall::lsp::DiscoverWorkspaceSources(mRoot, {});
    const auto project = heimdall::lsp::DiscoverProjectSources(mRoot, {});

    // Navigation and completion still see every source, including dependencies.
    EXPECT_EQ(Relative(full), (std::vector<std::string> {
            "_deps/library-src/include/Library.hpp",
            "build/_deps/simdjson-src/simdjson.cpp",
            "external/widget/Widget.hpp",
            "src/Core.cpp",
            "third_party/fmt/format.cpp",
            "vendor/json.hpp"}));

    // Background diagnostics cover the open project only.
    EXPECT_EQ(Relative(project), (std::vector<std::string> {"src/Core.cpp"}));
}

TEST_F(WorkspaceFilesSpec, ProjectDiscoveryHonorsLimitsAndCancellation)
{
    Write("src/First.cpp");
    Write("src/Second.cpp");
    Write("build/_deps/library-src/Third.cpp");

    const auto limited = heimdall::lsp::DiscoverProjectSourcesDetailed(mRoot, {}, 1);
    std::stop_source stop;
    stop.request_stop();
    const auto cancelled = heimdall::lsp::DiscoverProjectSourcesDetailed(mRoot, stop.get_token());

    EXPECT_EQ(limited.files.size(), 1u);
    EXPECT_TRUE(limited.truncated);
    EXPECT_FALSE(limited.complete);
    EXPECT_TRUE(cancelled.files.empty());
    EXPECT_TRUE(cancelled.cancelled);
}

TEST_F(WorkspaceFilesSpec, DependencyDiscoveryHonorsLimitsAndCancellation)
{
    Write("build/_deps/library-src/First.cpp");
    Write("build/_deps/library-src/Second.cpp");

    const auto limited = heimdall::lsp::DiscoverWorkspaceSourcesDetailed(mRoot, {}, 1);
    std::stop_source stop;
    stop.request_stop();
    const auto cancelled = heimdall::lsp::DiscoverWorkspaceSourcesDetailed(mRoot, stop.get_token());

    EXPECT_EQ(limited.files.size(), 1u);
    EXPECT_TRUE(limited.truncated);
    EXPECT_FALSE(limited.complete);
    EXPECT_TRUE(cancelled.files.empty());
    EXPECT_TRUE(cancelled.cancelled);
}

TEST_F(WorkspaceFilesSpec, DiscoveryStopsWhenCancelled)
{
    for (int i = 0; i < 10; ++i)
    {
        Write("f" + std::to_string(i) + ".cpp");
    }

    std::stop_source source;
    source.request_stop();

    EXPECT_TRUE(heimdall::lsp::DiscoverWorkspaceSources(mRoot, source.get_token()).empty());
}

TEST_F(WorkspaceFilesSpec, DiscoveryOfAMissingRootIsEmpty)
{
    EXPECT_TRUE(heimdall::lsp::DiscoverWorkspaceSources(mRoot / "missing", std::stop_token {}).empty());
    EXPECT_TRUE(heimdall::lsp::DiscoverWorkspaceSources({}, std::stop_token {}).empty());
}

TEST_F(WorkspaceFilesSpec, DiscoveryEvidenceDistinguishesExactLimitFromTruncation)
{
    Write("first.cpp");
    Write("second.cpp");

    const auto exact = heimdall::lsp::DiscoverWorkspaceSourcesDetailed(mRoot, {}, 2);
    const auto truncated = heimdall::lsp::DiscoverWorkspaceSourcesDetailed(mRoot, {}, 1);

    EXPECT_TRUE(exact.complete);
    EXPECT_FALSE(exact.truncated);
    EXPECT_FALSE(truncated.complete);
    EXPECT_TRUE(truncated.truncated);
}

TEST_F(WorkspaceFilesSpec, DiscoveryEvidenceDoesNotCertifyCancelledOrMissingRoots)
{
    std::stop_source stop;
    stop.request_stop();

    const auto cancelled = heimdall::lsp::DiscoverWorkspaceSourcesDetailed(mRoot, stop.get_token());
    const auto missing = heimdall::lsp::DiscoverWorkspaceSourcesDetailed(mRoot / "missing", {});

    EXPECT_TRUE(cancelled.cancelled);
    EXPECT_FALSE(cancelled.complete);
    EXPECT_FALSE(missing.complete);
}

TEST_F(WorkspaceFilesSpec, ReadsWholeFilesAndRejectsOversizedOnes)
{
    Write("small.cpp", "int small;\n");
    Write("huge.cpp", std::string(heimdall::lsp::kMaxWorkspaceFileBytes + 1, 'x'));
    Write("limit.cpp", std::string(heimdall::lsp::kMaxWorkspaceFileBytes, 'y'));

    const auto small = heimdall::lsp::ReadWorkspaceFile(mRoot / "small.cpp");

    ASSERT_TRUE(small.has_value());
    EXPECT_EQ(*small, "int small;\n");
    EXPECT_FALSE(heimdall::lsp::ReadWorkspaceFile(mRoot / "huge.cpp").has_value());
    EXPECT_FALSE(heimdall::lsp::ReadWorkspaceFile(mRoot / "missing.cpp").has_value());
    const auto limit = heimdall::lsp::ReadWorkspaceFile(mRoot / "limit.cpp");
    ASSERT_TRUE(limit.has_value());
    EXPECT_EQ(limit->size(), heimdall::lsp::kMaxWorkspaceFileBytes);
}

TEST_F(WorkspaceFilesSpec, FileKeyIgnoresHowAPathIsSpelled)
{
    using heimdall::lsp::FileKey;

    EXPECT_EQ(FileKey(mRoot / "a" / ".." / "b.cpp"), FileKey(mRoot / "b.cpp"));
    EXPECT_NE(FileKey(mRoot / "b.cpp"), FileKey(mRoot / "c.cpp"));
}

TEST_F(WorkspaceFilesSpec, FileKeyMatchesAcrossUriSpellings)
{
    using heimdall::lsp::FileKey;
    using heimdall::lsp::PathFromUri;
    using heimdall::lsp::UriFromPath;

    const fs::path file = mRoot / "dir with space" / "a.cpp";

    EXPECT_EQ(FileKey(PathFromUri(UriFromPath(file))), FileKey(file));
#if defined(_WIN32)
    // VS Code writes `file:///c%3A/...` with a lower-case drive letter.
    std::string uri = UriFromPath(file);
    const auto colon = uri.find(':', 8);
    ASSERT_NE(colon, std::string::npos);
    uri.replace(colon, 1, "%3A");
    uri[8] = static_cast<char>(std::tolower(static_cast<unsigned char>(uri[8])));
    EXPECT_EQ(FileKey(PathFromUri(uri)), FileKey(file));
#endif
}

TEST_F(WorkspaceFilesSpec, UnderRootRequiresAWholePathComponent)
{
    using heimdall::lsp::IsUnderRoot;

    const fs::path root = mRoot / "project";

    EXPECT_TRUE(IsUnderRoot(root / "src" / "a.cpp", root));
    EXPECT_TRUE(IsUnderRoot(root / "a.cpp", root / "."));
    EXPECT_FALSE(IsUnderRoot(mRoot / "project2" / "a.cpp", root));
    EXPECT_FALSE(IsUnderRoot(mRoot / "a.cpp", root));
    EXPECT_FALSE(IsUnderRoot(root / "a.cpp", {}));
}

TEST_F(WorkspaceFilesSpec, IncludesEveryModuleInterfaceExtension)
{
    for (const char* name : {"core.cppm", "core.ixx", "core.mpp", "core.ccm", "core.cxxm", "core.c++m"})
    {
        EXPECT_TRUE(heimdall::lsp::IsWorkspaceSource(name)) << name;
    }
}
