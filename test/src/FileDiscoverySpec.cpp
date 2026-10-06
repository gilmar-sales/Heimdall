#include <gtest/gtest.h>

#include "FileDiscovery.hpp"

#include <filesystem>
#include <fstream>

namespace
{

    std::filesystem::path MakeGlobDir(std::string_view suffix)
    {
        const auto path = std::filesystem::temp_directory_path() /("heimdall_glob_" + std::string(suffix));
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
        std::filesystem::create_directories(path / "src" / "sub", ec);
        std::filesystem::create_directories(path / "other", ec);

        const auto touch =[](const std::filesystem::path& file)
        {
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            out << "int x = 0;\n";
        };
        touch(path / "src" / "a.cpp");
        touch(path / "src" / "b.hpp");
        touch(path / "src" / "c.txt");
        touch(path / "src" / "sub" / "d.cpp");
        touch(path / "src" / "sub" / "e.hpp");
        touch(path / "other" / "f.cpp");
        return path;
    }

    bool Collect(const std::vector<std::filesystem::path>& inputs,
        std::vector<std::filesystem::path>& files)
    {
        return heimdall::cli::CollectFiles(inputs, files);
    }

} // namespace

TEST(FileDiscoveryGlob, MatchesPatternSyntax)
{
    using heimdall::cli::MatchGlobPattern;
    EXPECT_TRUE(MatchGlobPattern("*.cpp", "a.cpp"));
    EXPECT_FALSE(MatchGlobPattern("*.cpp", "a.hpp"));
    EXPECT_FALSE(MatchGlobPattern("*.cpp", "sub/a.cpp"));
    EXPECT_TRUE(MatchGlobPattern("**/*.cpp", "a.cpp"));
    EXPECT_TRUE(MatchGlobPattern("**/*.cpp", "sub/a.cpp"));
    EXPECT_TRUE(MatchGlobPattern("**/*.cpp", "a/b/c.cpp"));
    EXPECT_FALSE(MatchGlobPattern("**/*.cpp", "a/b/c.hpp"));
    EXPECT_TRUE(MatchGlobPattern("**.cpp", "a.cpp"));
    EXPECT_TRUE(MatchGlobPattern("**.cpp", "sub/a.cpp"));
    EXPECT_TRUE(MatchGlobPattern("src/*.cpp", "src/a.cpp"));
    EXPECT_FALSE(MatchGlobPattern("src/*.cpp", "src/sub/a.cpp"));
    EXPECT_TRUE(MatchGlobPattern("src/**/*.cpp", "src/sub/a.cpp"));
    EXPECT_TRUE(MatchGlobPattern("?.cpp", "a.cpp"));
    EXPECT_FALSE(MatchGlobPattern("?.cpp", "ab.cpp"));
    EXPECT_TRUE(MatchGlobPattern("[ab].cpp", "a.cpp"));
    EXPECT_FALSE(MatchGlobPattern("[ab].cpp", "c.cpp"));
}

TEST(FileDiscoveryGlob, ExpandsRecursiveGlob)
{
    const auto root = MakeGlobDir("recursive");
    const auto pattern = root / "src" / "**" / "*.cpp";

    std::vector<std::filesystem::path> files;
    EXPECT_TRUE(Collect({pattern}, files));
    EXPECT_EQ(files.size(), 2u);

    const auto pattern_dot_cpp = root / "src" / "**.cpp";
    files.clear();
    EXPECT_TRUE(Collect({pattern_dot_cpp}, files));
    EXPECT_EQ(files.size(), 2u);

    std::filesystem::remove_all(root);
}

TEST(FileDiscoveryGlob, ExpandsShallowGlob)
{
    const auto root = MakeGlobDir("shallow");

    std::vector<std::filesystem::path> files;
    EXPECT_TRUE(Collect({root / "src" / "*.cpp"}, files));
    ASSERT_EQ(files.size(), 1u);
    EXPECT_EQ(files[0].filename(), "a.cpp");

    std::filesystem::remove_all(root);
}

TEST(FileDiscoveryGlob, MixedGlobAndPlainInputs)
{
    const auto root = MakeGlobDir("mixed");

    std::vector<std::filesystem::path> files;
    EXPECT_TRUE(Collect({root / "src" / "*.cpp", root / "other" / "f.cpp"}, files));
    EXPECT_EQ(files.size(), 2u);

    std::filesystem::remove_all(root);
}
