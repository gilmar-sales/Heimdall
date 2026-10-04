#include <gtest/gtest.h>

#include <Heimdall/RuleConfig.hpp>

#include <filesystem>
#include <fstream>

namespace
{

std::filesystem::path MakeConfigDir(std::string_view suffix)
{
    const auto path = std::filesystem::temp_directory_path() / ("heimdall_rule_config_" + std::string(suffix));
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
}

void WriteConfig(const std::filesystem::path &directory, std::string_view contents)
{
    std::ofstream out(directory / heimdall::RuleConfigFileName, std::ios::binary | std::ios::trunc);
    out << contents;
}

} // namespace

TEST(RuleConfigSpec, LoadsRuleSeveritiesDisabledRulesAndSuppressionsSetting)
{
    const auto directory = MakeConfigDir("settings");
    WriteConfig(directory, R"({"rules":{"cpp/no-null":"error","format/require-final-newline":"off"},"suppressions":false,"root":true})");

    auto loaded = heimdall::LoadRuleConfiguration(directory / heimdall::RuleConfigFileName);
    ASSERT_TRUE(loaded) << (loaded ? "" : loaded.error());
    ASSERT_EQ(loaded->options.overrides.size(), 2);
    EXPECT_EQ(loaded->options.overrides[0].code, "cpp/no-null");
    EXPECT_EQ(loaded->options.overrides[0].severity, heimdall::Severity::Error);
    EXPECT_FALSE(loaded->options.overrides[1].enabled);
    EXPECT_FALSE(loaded->options.honor_suppressions);
    EXPECT_TRUE(loaded->root);
    std::filesystem::remove_all(directory);
}

TEST(RuleConfigSpec, FindsNearestConfigAndMergesParentSettings)
{
    const auto directory = MakeConfigDir("merge");
    const auto child = directory / "child";
    std::filesystem::create_directories(child);
    WriteConfig(directory, R"({"rules":{"cpp/no-null":"off"}})");
    WriteConfig(child, R"({"rules":{"cpp/no-null":"error","format/require-final-newline":"off"}})");

    auto loaded = heimdall::FindRuleOptions(child);
    ASSERT_TRUE(loaded) << (loaded ? "" : loaded.error());
    ASSERT_TRUE(*loaded);
    ASSERT_EQ((**loaded).overrides.size(), 3);
    EXPECT_EQ((**loaded).overrides[0].severity, heimdall::Severity::Warning);
    EXPECT_EQ((**loaded).overrides[1].severity, heimdall::Severity::Error);
    const auto diagnostics = heimdall::RuleEngine(**loaded).Analyze("auto p = NULL;\n");
    ASSERT_EQ(diagnostics.size(), 1);
    EXPECT_EQ(diagnostics[0].severity, heimdall::Severity::Error);
    std::filesystem::remove_all(directory);
}

TEST(RuleConfigSpec, RootConfigStopsInheritanceFromParentDirectories)
{
    const auto directory = MakeConfigDir("root-marker");
    const auto project = directory / "project";
    const auto nested = project / "nested";
    std::filesystem::create_directories(nested);
    WriteConfig(directory, R"({"rules":{"cpp/no-null":"off"}})");
    WriteConfig(project, R"({"root":true,"rules":{"cpp/no-null":"error"}})");

    auto loaded = heimdall::FindRuleOptions(nested);
    ASSERT_TRUE(loaded) << (loaded ? "" : loaded.error());
    ASSERT_TRUE(*loaded);
    ASSERT_EQ((**loaded).overrides.size(), 1);
    EXPECT_EQ((**loaded).overrides[0].severity, heimdall::Severity::Error);
    std::filesystem::remove_all(directory);
}

TEST(RuleConfigSpec, RejectsUnknownRulesAndInvalidSettings)
{
    const auto directory = MakeConfigDir("invalid");
    const auto path = directory / heimdall::RuleConfigFileName;
    WriteConfig(directory, R"({"rules":{"cpp/not-real":"warning"}})");
    EXPECT_FALSE(heimdall::LoadRuleConfiguration(path));
    WriteConfig(directory, R"({"rules":{"cpp/no-null":"sometimes"}})");
    EXPECT_FALSE(heimdall::LoadRuleConfiguration(path));
    std::filesystem::remove_all(directory);
}
