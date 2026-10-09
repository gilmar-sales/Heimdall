#include <gtest/gtest.h>

#include <Heimdall/CompileDatabase.hpp>

#include <filesystem>
#include <fstream>

TEST(CompileDatabaseSpec, ReadsArgumentsAndExtractsDefinesUndefinesAndIncludes)
{
    const auto root = std::filesystem::path(HEIMDALL_SOURCE_DIR);
    const auto path =
        std::filesystem::temp_directory_path() / "heimdall_compile_commands_test.json";
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << "[{\"directory\":\"" << root.generic_string()
            << "\",\"file\":\"src/main.cpp\",\"arguments\":[\"g++\",\"-std=c++26\","
               "\"-DDEBUG=1\",\"-DVALUE\",\"-UOLD\",\"-I\",\"include\"]}]";
    }

    auto database = heimdall::CompileDatabase::Load(path);
    ASSERT_TRUE(database) << (database ? "" : database.error());
    ASSERT_EQ(database->Commands().size(), 1);
    const auto* command = database->Find(root / "src" / "main.cpp");
    ASSERT_NE(command, nullptr);
    EXPECT_EQ(command->standard, heimdall::CppStandard::Cpp26);
    EXPECT_EQ(command->defines.at("DEBUG"), "1");
    EXPECT_EQ(command->defines.at("VALUE"), "1");
    EXPECT_EQ(command->undefines, (std::vector<std::string> { "OLD" }));
    ASSERT_EQ(command->include_directories.size(), 1);
    EXPECT_EQ(command->include_directories[0], (root / "include").lexically_normal());
    std::filesystem::remove(path);
}

TEST(CompileDatabaseSpec, ParsesCommandStringFallback)
{
    const auto root = std::filesystem::path(HEIMDALL_SOURCE_DIR);
    const auto path =
        std::filesystem::temp_directory_path() / "heimdall_compile_commands_command_test.json";
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << "[{\"directory\":\"" << root.generic_string()
            << "\",\"file\":\"src/main.cpp\",\"command\":\"g++ -DVALUE=42 -I include "
               "src/main.cpp\"}]";
    }
    auto database = heimdall::CompileDatabase::Load(path);
    ASSERT_TRUE(database);
    const auto* command = database->Find(root / "src" / "main.cpp");
    ASSERT_NE(command, nullptr);
    EXPECT_EQ(command->defines.at("VALUE"), "42");
    EXPECT_EQ(command->arguments.size(), 5);
    EXPECT_EQ(command->standard, heimdall::CppStandard::Cpp20);
    std::filesystem::remove(path);
}

TEST(CompileDatabaseSpec, SelectsLatestRecognizedLanguageStandardOption)
{
    const auto root = std::filesystem::path(HEIMDALL_SOURCE_DIR);
    const auto path =
        std::filesystem::temp_directory_path() / "heimdall_compile_commands_standard_test.json";
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << "[{\"directory\":\"" << root.generic_string()
            << "\",\"file\":\"src/main.cpp\",\"arguments\":[\"cl\",\"/std:c++20\",\"/"
               "std:c++23\"]}]";
    }
    auto database = heimdall::CompileDatabase::Load(path);
    ASSERT_TRUE(database);
    ASSERT_EQ(database->Commands().size(), 1);
    EXPECT_EQ(database->Commands()[0].standard, heimdall::CppStandard::Cpp23);
    std::filesystem::remove(path);
}

namespace
{

    std::filesystem::path WriteDatabase(const char* name, std::string_view entries)
    {
        const auto    path = std::filesystem::temp_directory_path() / name;
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << '[' << entries << ']';
        return path;
    }

} // namespace

#if defined(_WIN32)
// Editors send `c:` while compile databases record `C:`; the lookup must match.
TEST(CompileDatabaseSpec, FindIgnoresPathCaseOnWindows)
{
    const auto path = WriteDatabase(
        "heimdall_compile_commands_case_test.json",
        R"({"directory":"C:/proj/build","file":"C:/proj/test/Spec.cpp","arguments":["g++","-IC:/proj/include"]})");
    auto database = heimdall::CompileDatabase::Load(path);
    ASSERT_TRUE(database);
    EXPECT_NE(database->Find("c:/proj/test/spec.cpp"), nullptr);
    EXPECT_NE(database->Find(R"(c:\proj\test\Spec.cpp)"), nullptr);
    std::filesystem::remove(path);
}
#endif

TEST(CompileDatabaseSpec, FindOrNearestFallsBackToClosestDirectoryForHeaders)
{
    const auto        root = std::filesystem::path(HEIMDALL_SOURCE_DIR);
    const std::string dir  = root.generic_string();
    const auto        path = WriteDatabase(
        "heimdall_compile_commands_nearest_test.json",
        "{\"directory\":\"" + dir +
            "\",\"file\":\"other/a.cpp\",\"arguments\":[\"g++\",\"-Iother_inc\"]},"
            "{\"directory\":\"" +
            dir + "\",\"file\":\"core/src/b.cpp\",\"arguments\":[\"g++\",\"-Icore_inc\"]}");
    auto database = heimdall::CompileDatabase::Load(path);
    ASSERT_TRUE(database);

    EXPECT_EQ(database->Find(root / "core" / "include" / "x.hpp"), nullptr);
    const auto* nearest = database->FindOrNearest(root / "core" / "include" / "x.hpp");
    ASSERT_NE(nearest, nullptr);
    EXPECT_EQ(nearest->file.filename(), "b.cpp");
    // An exact entry always wins.
    EXPECT_EQ(database->FindOrNearest(root / "other" / "a.cpp")->file.filename(), "a.cpp");
    std::filesystem::remove(path);
}

// Regression: `C:\mingw64\bin\g++.exe` lost its backslashes, so the compiler
// driver was invalid and system include directories (<vector>) never resolved.
TEST(CompileDatabaseSpec, CommandStringKeepsWindowsPathSeparators)
{
#if !defined(_WIN32)
    GTEST_SKIP() << "Windows path separators require Windows std::filesystem semantics.";
#endif
    const auto path = WriteDatabase(
        "heimdall_compile_commands_winpath_test.json",
        R"({"directory":"C:/proj/build","file":"C:/proj/a.cpp","command":"C:\\mingw64\\bin\\g++.exe -IC:\\proj\\inc -DNAME=\\\"x\\\" -c C:\\proj\\a.cpp"})");
    auto database = heimdall::CompileDatabase::Load(path);
    ASSERT_TRUE(database);
    ASSERT_EQ(database->Commands().size(), 1);
    const auto& command = database->Commands()[0];
    EXPECT_EQ(command.arguments.front(), R"(C:\mingw64\bin\g++.exe)");
    ASSERT_EQ(command.include_directories.size(), 1);
    EXPECT_EQ(command.include_directories[0].generic_string(), "C:/proj/inc");
    EXPECT_EQ(command.defines.at("NAME"), "\"x\"");
    std::filesystem::remove(path);
}

TEST(CompileDatabaseSpec, ReportsMalformedJsonWithoutThrowing)
{
    const auto path =
        std::filesystem::temp_directory_path() / "heimdall_compile_commands_malformed_test.json";
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << "[{not-json]";
    }
    const auto database = heimdall::CompileDatabase::Load(path);
    EXPECT_FALSE(database);
    EXPECT_NE(database.error().find(path.string()), std::string::npos);
    std::filesystem::remove(path);
}

TEST(CompileDatabaseSpec, ReportsUnreadableDatabase)
{
    const auto path =
        std::filesystem::temp_directory_path() / "heimdall_missing_compile_commands.json";
    std::filesystem::remove(path);
    const auto database = heimdall::CompileDatabase::Load(path);
    EXPECT_FALSE(database);
    EXPECT_NE(database.error().find("cannot parse compile database"), std::string::npos);
}
