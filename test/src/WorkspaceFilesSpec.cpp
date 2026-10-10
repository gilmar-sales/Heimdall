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

    class WorkspaceFilesSpec : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            static std::atomic<int> counter {0};
            const std::string unique = std::to_string(counter.fetch_add(1)) + "_" +
                                       testing::UnitTest::GetInstance()->current_test_info()->name();
            mRoot = fs::temp_directory_path() / ("heimdall_workspace_files_" + unique);
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

TEST_F(WorkspaceFilesSpec, RecognizesCppSourcesAndHeaders)
{
    using heimdall::lsp::IsWorkspaceSource;

    for (const char* name : {"a.cpp", "a.cc", "a.cxx", "a.c++", "a.hpp", "a.hh", "a.hxx", "a.h",
                             "a.ipp", "a.tpp", "a.inl", "a.cppm", "a.ixx"})
    {
        EXPECT_TRUE(IsWorkspaceSource(name)) << name;
    }

    for (const char* name : {"a.txt", "a.py", "Makefile", "a.cpp.bak", "a", "a.hpp.in"})
    {
        EXPECT_FALSE(IsWorkspaceSource(name)) << name;
    }
}

TEST_F(WorkspaceFilesSpec, SkipsBuildAndToolDirectories)
{
    using heimdall::lsp::IsSkippedDirectory;

    for (const char* name : {".git", ".vscode", "build", "build-release", "cmake-build-debug",
                             "node_modules", "_deps", "vcpkg_installed", "out"})
    {
        EXPECT_TRUE(IsSkippedDirectory(name)) << name;
    }

    for (const char* name : {"src", "include", "rebuild", "output", "lib"})
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
