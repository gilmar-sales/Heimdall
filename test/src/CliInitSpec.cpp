#include <gtest/gtest.h>

#include "CliOptions.hpp"
#include "Init.hpp"

#include <Heimdall/RuleConfig.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{

    std::filesystem::path MakeInitDir(std::string_view suffix)
    {
        const auto path =
            std::filesystem::temp_directory_path() / ("heimdall_init_" + std::string(suffix));
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
        return path;
    }

    bool ParseArgs(std::vector<std::string> args, heimdall::cli::Options& options)
    {
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>("heimdall"));
        for (auto& arg : args)
        {
            argv.push_back(arg.data());
        }

        return heimdall::cli::ParseOptions(static_cast<int>(argv.size()), argv.data(), options);
    }

} // namespace

TEST(CliInitSpec, ParsesBareInit)
{
    heimdall::cli::Options options {};
    EXPECT_TRUE(ParseArgs({ "init" }, options));
    EXPECT_EQ(options.command, heimdall::cli::Command::Init);
    EXPECT_FALSE(options.force);
    EXPECT_TRUE(options.inputs.empty());
}

TEST(CliInitSpec, ParsesInitWithDirectoryAndForce)
{
    heimdall::cli::Options options {};
    EXPECT_TRUE(ParseArgs({ "init", "myproj", "--force" }, options));
    EXPECT_EQ(options.command, heimdall::cli::Command::Init);
    EXPECT_TRUE(options.force);
    ASSERT_EQ(options.inputs.size(), 1);
    EXPECT_EQ(options.inputs[0], "myproj");
}

TEST(CliInitSpec, ParsesInitWithShortForce)
{
    heimdall::cli::Options options {};
    EXPECT_TRUE(ParseArgs({ "init", "-f" }, options));
    EXPECT_TRUE(options.force);
}

TEST(CliInitSpec, RejectsSecondDirectory)
{
    heimdall::cli::Options options {};
    EXPECT_FALSE(ParseArgs({ "init", "a", "b" }, options));
}

TEST(CliInitSpec, RejectsUnknownInitOption)
{
    heimdall::cli::Options options {};
    EXPECT_FALSE(ParseArgs({ "init", "--json" }, options));
}

TEST(CliInitSpec, RejectsForceOnOtherCommands)
{
    heimdall::cli::Options options {};
    EXPECT_FALSE(ParseArgs({ "lint", "--force", "src" }, options));
}

TEST(CliInitSpec, DefaultTemplateLoadsAsValidConfig)
{
    const auto directory = MakeInitDir("template");
    const auto path      = directory / heimdall::RuleConfigFileName;
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << heimdall::cli::DefaultConfigText();
    }

    auto loaded = heimdall::LoadRuleConfiguration(path);
    ASSERT_TRUE(loaded) << (loaded ? "" : loaded.error());
    EXPECT_TRUE(loaded->root);
    EXPECT_FALSE(loaded->options.overrides.empty());
    std::filesystem::remove_all(directory);
}

TEST(CliInitSpec, RunInitCreatesFileAndRespectsForce)
{
    const auto directory = MakeInitDir("run");
    const auto target    = directory / "proj";

    heimdall::cli::Options options {};
    options.command = heimdall::cli::Command::Init;
    options.inputs.push_back(target);

    std::string           error;
    std::filesystem::path created;
    EXPECT_TRUE(heimdall::cli::RunInit(options, error, created)) << error;
    EXPECT_EQ(created, target / heimdall::RuleConfigFileName);
    EXPECT_TRUE(std::filesystem::exists(created));
    EXPECT_TRUE(heimdall::LoadRuleConfiguration(created));

    // Second run without --force must fail.
    std::filesystem::path second;
    EXPECT_FALSE(heimdall::cli::RunInit(options, error, second));
    EXPECT_FALSE(error.empty());

    // With --force it overwrites.
    options.force = true;
    std::filesystem::path third;
    EXPECT_TRUE(heimdall::cli::RunInit(options, error, third)) << error;
    EXPECT_EQ(third, created);

    std::filesystem::remove_all(directory);
}

TEST(CliInitSpec, SchemaFileExistsAndMentionsRules)
{
    const auto schema =
        std::filesystem::path(HEIMDALL_SOURCE_DIR) / "schemas" / "heimdall.schema.json";
    EXPECT_TRUE(std::filesystem::exists(schema));

    std::ifstream in(schema);
    ASSERT_TRUE(in);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_NE(text.find("cpp/no-null"), std::string::npos);
    EXPECT_NE(text.find("include-order"), std::string::npos);
    EXPECT_NE(text.find("cpp/sort-includes"), std::string::npos);
}
