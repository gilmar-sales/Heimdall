#include <gtest/gtest.h>

#include <Heimdall/CompileDatabase.hpp>

#include <filesystem>
#include <fstream>

TEST(CompileDatabaseSpec, ReadsArgumentsAndExtractsDefinesUndefinesAndIncludes)
{
    const auto root = std::filesystem::path(HEIMDALL_SOURCE_DIR);
    const auto path = std::filesystem::temp_directory_path() / "heimdall_compile_commands_test.json";
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
    const auto path = std::filesystem::temp_directory_path() / "heimdall_compile_commands_command_test.json";
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << "[{\"directory\":\"" << root.generic_string()
            << "\",\"file\":\"src/main.cpp\",\"command\":\"g++ -DVALUE=42 -I include src/main.cpp\"}]";
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
    const auto path = std::filesystem::temp_directory_path() / "heimdall_compile_commands_standard_test.json";
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << "[{\"directory\":\"" << root.generic_string()
            << "\",\"file\":\"src/main.cpp\",\"arguments\":[\"cl\",\"/std:c++20\",\"/std:c++23\"]}]";
    }
    auto database = heimdall::CompileDatabase::Load(path);
    ASSERT_TRUE(database);
    ASSERT_EQ(database->Commands().size(), 1);
    EXPECT_EQ(database->Commands()[0].standard, heimdall::CppStandard::Cpp23);
    std::filesystem::remove(path);
}
