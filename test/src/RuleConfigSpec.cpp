#include <gtest/gtest.h>

#include <Heimdall/RuleConfig.hpp>
#include <Heimdall/RuleEngine.hpp>

#include <filesystem>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <regex>
#include <set>
#include <simdjson.h>
#include <string>
#include <string_view>

namespace
{

    std::filesystem::path MakeConfigDir(std::string_view suffix)
    {
        const auto path = std::filesystem::temp_directory_path() /
                          ("heimdall_rule_config_" + std::string(suffix));
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
        return path;
    }

    void WriteConfig(const std::filesystem::path& directory, std::string_view contents)
    {
        std::ofstream out(
            directory / heimdall::RuleConfigFileName,
            std::ios::binary | std::ios::trunc);
        out << contents;
    }

} // namespace

TEST(RuleConfigSpec, SchemaDefinesExactlyEveryCatalogRuleWithSupportedSeverities)
{
    const auto path = std::filesystem::path(HEIMDALL_SOURCE_DIR) / "schemas" / "heimdall.schema.json";
    simdjson::dom::parser parser;
    simdjson::dom::element schema;
    ASSERT_EQ(parser.load(path.string()).get(schema), simdjson::SUCCESS);
    simdjson::dom::object rules;
    ASSERT_EQ(schema["properties"]["rules"].get_object().get(rules), simdjson::SUCCESS);
    simdjson::dom::object definitions;
    ASSERT_EQ(rules["properties"].get_object().get(definitions), simdjson::SUCCESS);
    std::set<std::string> actualCodes;
    const std::set<std::string> expectedSeverities
    {
        "off", "warning", "error"
    };

    for (const auto field : definitions)
    {
        SCOPED_TRACE(field.key);
        EXPECT_TRUE(actualCodes.emplace(field.key).second);
        simdjson::dom::array levels;
        ASSERT_EQ(field.value["enum"].get_array().get(levels), simdjson::SUCCESS);
        std::set<std::string> severities;
        for (const auto level : levels)
        {
            std::string_view value;
            ASSERT_EQ(level.get_string().get(value), simdjson::SUCCESS);
            EXPECT_TRUE(severities.emplace(value).second);
        }

        EXPECT_EQ(severities, expectedSeverities);
    }

    std::set<std::string> expectedCodes;
    for (const auto& rule : heimdall::RuleCatalog())
    {
        EXPECT_TRUE(expectedCodes.emplace(rule.code).second);
    }

    EXPECT_EQ(actualCodes, expectedCodes);
    bool additionalRules = true;
    ASSERT_EQ(rules["additionalProperties"].get_bool().get(additionalRules), simdjson::SUCCESS);
    EXPECT_FALSE(additionalRules);
}

TEST(RuleConfigSpec, EveryCatalogRuleCanBeConfiguredAtEverySchemaSeverity)
{
    const auto directory = MakeConfigDir("schema-severities");
    for (const std::string_view level :
        {
            "off", "warning", "error"
    })
    {
        SCOPED_TRACE(level);
        std::string config = "{\"root\":true,\"rules\":{";
        for (const auto& rule : heimdall::RuleCatalog())
        {
            if (config.back() != '{')
            {
                config += ',';
            }

            config += '"';
            config += rule.code;
            config += "\":\"";
            config += level;
            config += '"';
        }

        config += "}}";
        WriteConfig(directory, config);

        const auto loaded = heimdall::LoadRuleConfiguration(directory / heimdall::RuleConfigFileName);

        ASSERT_TRUE(loaded) << loaded.error();
        ASSERT_EQ(loaded->options.overrides.size(), heimdall::RuleCatalog().size());
        for (std::size_t index = 0; index < heimdall::RuleCatalog().size(); ++index)
        {
            const auto& setting = loaded->options.overrides[index];
            EXPECT_EQ(setting.code, heimdall::RuleCatalog()[index].code);
            EXPECT_EQ(setting.enabled, level != "off");
            if (setting.enabled)
            {
                EXPECT_EQ(setting.severity,
                          level == "error" ? heimdall::Severity::Error : heimdall::Severity::Warning);
            }
        }
    }

    std::filesystem::remove_all(directory);
}

TEST(RuleConfigSpec, EveryConfigurableRuleCodeInSourcesIsRegistered)
{
    const std::regex pattern(R"rule("((?:cpp|api|doc|format)/[a-z0-9-]+)")rule");
    const auto root = std::filesystem::path(HEIMDALL_SOURCE_DIR);

    for (const auto directory :
        {
            "core/src", "semantic/src"
    })
    {
        for (const auto& entry : std::filesystem::directory_iterator(root / directory))
        {
            if (entry.path().extension() != ".cpp")
            {
                continue;
            }

            SCOPED_TRACE(entry.path().string());
            std::ifstream file(entry.path());
            ASSERT_TRUE(file);
            const std::string source
            {
                std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()
            };
            for (auto match = std::sregex_iterator(source.begin(), source.end(), pattern);
                match != std::sregex_iterator(); ++match)
            {
                const auto code = (*match)[1].str();

                EXPECT_TRUE(heimdall::IsKnownRuleCode(code)) << code;
            }
        }
    }
}

TEST(RuleConfigSpec, LoadsRuleSeveritiesDisabledRulesAndSuppressionsSetting)
{
    const auto directory = MakeConfigDir("settings");
    WriteConfig(
        directory,
        R"({"rules":{"cpp/no-null":"error","format/require-final-newline":"off"},"suppressions":false,"root":true})");

    auto loaded = heimdall::LoadRuleConfiguration(directory / heimdall::RuleConfigFileName);
    ASSERT_TRUE(loaded) <<(loaded ? "" : loaded.error());
    ASSERT_EQ(loaded->options.overrides.size(), 2);
    EXPECT_EQ(loaded->options.overrides[0].code, "cpp/no-null");
    EXPECT_EQ(loaded->options.overrides[0].severity, heimdall::Severity::Error);
    EXPECT_FALSE(loaded->options.overrides[1].enabled);
    EXPECT_FALSE(loaded->options.honor_suppressions);
    EXPECT_TRUE(loaded->root);
    std::filesystem::remove_all(directory);
}

TEST(RuleConfigSpec, LoadsConstantNamingConvention)
{
    const auto directory = MakeConfigDir("constant-naming");
    WriteConfig(directory,
                R"({"rules":{"cpp/constant-naming":"warning"},"naming":{"constants":"screaming-snake-case"}})");
    const auto loaded = heimdall::LoadRuleConfiguration(directory / heimdall::RuleConfigFileName);
    ASSERT_TRUE(loaded) <<(loaded ? "" : loaded.error());
    EXPECT_TRUE(loaded->has_constant_name_casing);
    EXPECT_EQ(loaded->options.constant_name_casing,
              heimdall::ConstantNameCasing::ScreamingSnakeCase);
    WriteConfig(directory, R"({"naming":{"constants":"camelCase"}})");
    EXPECT_FALSE(heimdall::LoadRuleConfiguration(directory / heimdall::RuleConfigFileName));
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
    ASSERT_TRUE(loaded) <<(loaded ? "" : loaded.error());
    ASSERT_TRUE(*loaded);
    ASSERT_EQ((* *loaded).overrides.size(), 3);
    EXPECT_EQ((* *loaded).overrides[0].severity, heimdall::Severity::Warning);
    EXPECT_EQ((* *loaded).overrides[1].severity, heimdall::Severity::Error);
    const auto diagnostics = heimdall::RuleEngine(* *loaded).Analyze("auto p = NULL;\n");
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
    ASSERT_TRUE(loaded) <<(loaded ? "" : loaded.error());
    ASSERT_TRUE(*loaded);
    ASSERT_EQ((* *loaded).overrides.size(), 1);
    EXPECT_EQ((* *loaded).overrides[0].severity, heimdall::Severity::Error);
    std::filesystem::remove_all(directory);
}

TEST(RuleConfigSpec, FinalNewlineRuleControlsFormattingAndInheritsOverrides)
{
    const auto directory = MakeConfigDir("formatter-final-newline");
    const auto child = directory / "child";
    std::filesystem::create_directories(child);
    WriteConfig(directory, R"({"root":true,"rules":{"format/require-final-newline":"warning"}})");
    WriteConfig(child, R"({"rules":{"cpp/no-null":"off"}})");

    const auto loaded = heimdall::LoadRuleConfiguration(directory / heimdall::RuleConfigFileName);
    ASSERT_TRUE(loaded);
    EXPECT_TRUE(loaded->format_options.requireFinalNewline);
    const auto inherited = heimdall::FindFormatOptions(child);
    ASSERT_TRUE(inherited&&*inherited);
    EXPECT_TRUE((* *inherited).requireFinalNewline);

    WriteConfig(child, R"({"rules":{"format/require-final-newline":"off"}})");
    const auto disabled = heimdall::FindFormatOptions(child);
    ASSERT_TRUE(disabled&&*disabled);
    EXPECT_FALSE((* *disabled).requireFinalNewline);

    WriteConfig(child, R"({"rules":{"format/require-final-newline":"error"}})");
    const auto enabled = heimdall::FindFormatOptions(child);
    ASSERT_TRUE(enabled&&*enabled);
    EXPECT_TRUE((* *enabled).requireFinalNewline);
    std::filesystem::remove_all(directory);
}

TEST(RuleConfigSpec, AcceptsNewRuleCodesAndStillRejectsUnknown)
{
    const auto directory = MakeConfigDir("new-rules");
    const auto path = directory / heimdall::RuleConfigFileName;
    WriteConfig(
        directory,
        R"({"rules":{"cpp/no-empty-catch":"error","cpp/no-duplicate-include":"off","cpp/modernize-using":"warning"}})");

    auto loaded = heimdall::LoadRuleConfiguration(path);
    ASSERT_TRUE(loaded) <<(loaded ? "" : loaded.error());
    ASSERT_EQ(loaded->options.overrides.size(), 3);
    EXPECT_EQ(loaded->options.overrides[0].severity, heimdall::Severity::Error);
    EXPECT_FALSE(loaded->options.overrides[1].enabled);
    EXPECT_EQ(loaded->options.overrides[2].severity, heimdall::Severity::Warning);

    WriteConfig(directory, R"({"rules":{"cpp/no-such-rule":"warning"}})");
    EXPECT_FALSE(heimdall::LoadRuleConfiguration(path));
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

TEST(RuleConfigSpec, LoadsIncludeOrderSettings)
{
    const auto directory = MakeConfigDir("include-order");
    const auto path = directory / heimdall::RuleConfigFileName;
    WriteConfig(
        directory,
        R"({"rules":{"cpp/sort-includes":"warning"},"include-order":{"groups":["quote","angle"],"case-insensitive":false}})");

    auto loaded = heimdall::LoadRuleConfiguration(path);
    ASSERT_TRUE(loaded) <<(loaded ? "" : loaded.error());
    ASSERT_TRUE(loaded->has_include_order);
    ASSERT_EQ(loaded->options.include_order.size(), 2);
    EXPECT_EQ(loaded->options.include_order[0], heimdall::IncludeGroup::Quote);
    EXPECT_EQ(loaded->options.include_order[1], heimdall::IncludeGroup::Angle);
    EXPECT_FALSE(loaded->options.include_case_insensitive);

    // The order alone does not enable the rule; 'rules' does.
    heimdall::RuleOptions options = loaded->options;
    options.sort_includes = true;
    constexpr std::string_view source = "#include <vector>\n#include \"app.h\"\n";
    const auto diagnostics = heimdall::RuleEngine(options).Analyze(source);
    ASSERT_EQ(diagnostics.size(), 1);
    EXPECT_EQ(diagnostics[0].code, "cpp/sort-includes");
    EXPECT_EQ(heimdall::RuleEngine::ApplyFixes(source, diagnostics),
              "#include \"app.h\"\n\n#include <vector>\n");
    std::filesystem::remove_all(directory);
}

TEST(RuleConfigSpec, LoadsDocScopeAndAcceptsDocRules)
{
    const auto directory = MakeConfigDir("doc-scope");
    const auto path = directory / heimdall::RuleConfigFileName;
    WriteConfig(
        directory,
        R"({"rules":{"doc/require-comment":"warning","doc/doxygen-style":"error"},"doc":{"scope":"all"}})");

    auto loaded = heimdall::LoadRuleConfiguration(path);
    ASSERT_TRUE(loaded) <<(loaded ? "" : loaded.error());
    EXPECT_TRUE(loaded->has_doc_scope);
    EXPECT_EQ(loaded->options.doc_scope, heimdall::DocScope::All);
    EXPECT_TRUE(heimdall::RuleEngine(loaded->options).OptInEnabled("doc/require-comment"));
    EXPECT_TRUE(heimdall::RuleEngine(loaded->options).OptInEnabled("doc/doxygen-style"));
    EXPECT_FALSE(heimdall::RuleEngine().OptInEnabled("doc/doxygen-style"));

    for (const auto[text, expected] :
        {
            std::pair<const char *, heimdall::DocScope> {"public", heimdall::DocScope::Public},
            {"private", heimdall::DocScope::Private}
    })
    {
        WriteConfig(directory, std::string(R"({"doc":{"scope":")") + text + R"("}})");
        loaded = heimdall::LoadRuleConfiguration(path);
        ASSERT_TRUE(loaded) <<(loaded ? "" : loaded.error());
        EXPECT_EQ(loaded->options.doc_scope, expected);
    }

    std::filesystem::remove_all(directory);
}

TEST(RuleConfigSpec, RejectsInvalidDocScope)
{
    const auto directory = MakeConfigDir("doc-scope-invalid");
    const auto path = directory / heimdall::RuleConfigFileName;
    for (const char* contents :
        {
            R"({"doc":{"scope":"protected"}})", R"({"doc":{"scope":3}})", R"({"doc":true})"
    })
    {
        WriteConfig(directory, contents);
        EXPECT_FALSE(heimdall::LoadRuleConfiguration(path)) << contents;
    }

    std::filesystem::remove_all(directory);
}

TEST(RuleConfigSpec, NearestDocScopeWins)
{
    const auto directory = MakeConfigDir("doc-scope-merge");
    const auto child = directory / "child";
    std::filesystem::create_directories(child);
    WriteConfig(directory, R"({"doc":{"scope":"all"}})");
    WriteConfig(child, R"({"rules":{"doc/doxygen-style":"warning"}})");

    auto loaded = heimdall::FindRuleOptions(child);
    ASSERT_TRUE(loaded) <<(loaded ? "" : loaded.error());
    ASSERT_TRUE(*loaded);
    EXPECT_EQ((* *loaded).doc_scope, heimdall::DocScope::All);
    std::filesystem::remove_all(directory);
}

TEST(RuleConfigSpec, RejectsInvalidIncludeOrderSettings)
{
    const auto directory = MakeConfigDir("include-order-invalid");
    const auto path = directory / heimdall::RuleConfigFileName;
    WriteConfig(directory, R"({"include-order":"angle"})");
    EXPECT_FALSE(heimdall::LoadRuleConfiguration(path));
    WriteConfig(directory, R"({"include-order":{"groups":"angle"}})");
    EXPECT_FALSE(heimdall::LoadRuleConfiguration(path));
    WriteConfig(directory, R"({"include-order":{}})");
    EXPECT_FALSE(heimdall::LoadRuleConfiguration(path));
    WriteConfig(directory, R"({"include-order":{"groups":["angle"]}})");
    EXPECT_FALSE(heimdall::LoadRuleConfiguration(path));
    WriteConfig(directory, R"({"include-order":{"groups":["angle","quote","angle"]}})");
    EXPECT_FALSE(heimdall::LoadRuleConfiguration(path));
    WriteConfig(directory, R"({"include-order":{"groups":["angle","weird"]}})");
    EXPECT_FALSE(heimdall::LoadRuleConfiguration(path));
    WriteConfig(directory,
                R"({"include-order":{"groups":["angle","quote"],"case-insensitive":"yes"}})");
    EXPECT_FALSE(heimdall::LoadRuleConfiguration(path));
    std::filesystem::remove_all(directory);
}

TEST(RuleConfigSpec, NearestIncludeOrderWins)
{
    const auto directory = MakeConfigDir("include-order-merge");
    const auto child = directory / "child";
    std::filesystem::create_directories(child);
    WriteConfig(directory, R"({"include-order":{"groups":["angle","quote"]}})");
    WriteConfig(child, R"({"include-order":{"groups":["quote","angle"]}})");

    auto loaded = heimdall::FindRuleOptions(child);
    ASSERT_TRUE(loaded) <<(loaded ? "" : loaded.error());
    ASSERT_TRUE(*loaded);
    ASSERT_EQ((*loaded)->include_order.size(), 2);
    EXPECT_EQ((*loaded)->include_order[0], heimdall::IncludeGroup::Quote);
    EXPECT_EQ((*loaded)->include_order[1], heimdall::IncludeGroup::Angle);
    EXPECT_TRUE((*loaded)->include_case_insensitive);
    std::filesystem::remove_all(directory);
}

TEST(RuleConfigSpec, LoadsAndMergesDeclarationLayoutSettings)
{
    const auto directory = MakeConfigDir("layout");
    WriteConfig(
        directory,
        R"({"root":true,"format":{"blank-line-between-methods":false}})");
    auto loaded = heimdall::LoadRuleConfiguration(directory / heimdall::RuleConfigFileName);
    ASSERT_TRUE(loaded);
    EXPECT_TRUE(loaded->has_blank_line_between_methods);
    EXPECT_FALSE(loaded->format_options.blank_line_between_methods);
    const auto child = directory / "child";
    std::filesystem::create_directories(child);
    WriteConfig(child, R"({"format":{"blank-line-between-methods":true}})");
    auto merged = heimdall::FindFormatOptions(child);
    ASSERT_TRUE(merged&&*merged);
    EXPECT_TRUE((* *merged).blank_line_between_methods);
    std::filesystem::remove_all(directory);
}

TEST(RuleConfigSpec, RejectsInvalidDeclarationLayoutSettings)
{
    const auto directory = MakeConfigDir("invalid_layout");
    for (const auto json :
        {
            R"({"format":{"blank-line-between-methods":"true"}})",
            R"({"format":{"max-parameters-per-line":3}})",
            R"({"format":{"max-parameters-per-line":0}})"
    })
    {
        SCOPED_TRACE(json);
        WriteConfig(directory, json);
        EXPECT_FALSE(heimdall::LoadRuleConfiguration(directory / heimdall::RuleConfigFileName));
    }

    std::filesystem::remove_all(directory);
}

TEST(RuleConfigSpec, LoadsFormatAlignments)
{
    const auto directory = MakeConfigDir("format");
    const auto path = directory / heimdall::RuleConfigFileName;
    WriteConfig(directory,
                R"({"format":{"pointer-alignment":"left","reference-alignment":"left"}})");

    auto loaded = heimdall::LoadRuleConfiguration(path);
    ASSERT_TRUE(loaded) <<(loaded ? "" : loaded.error());
    EXPECT_TRUE(loaded->has_pointer_alignment);
    EXPECT_TRUE(loaded->has_reference_alignment);
    EXPECT_EQ(loaded->format_options.pointer_alignment, heimdall::PointerAlignment::Left);
    EXPECT_EQ(loaded->format_options.reference_alignment, heimdall::ReferenceAlignment::Left);

    auto found = heimdall::FindFormatOptions(directory);
    ASSERT_TRUE(found) <<(found ? "" : found.error());
    ASSERT_TRUE(*found);
    EXPECT_EQ((*found) -> pointer_alignment, heimdall::PointerAlignment::Left);
    EXPECT_EQ((*found) -> reference_alignment, heimdall::ReferenceAlignment::Left);
    std::filesystem::remove_all(directory);
}

TEST(RuleConfigSpec, RejectsInvalidFormatSettings)
{
    const auto directory = MakeConfigDir("format-invalid");
    const auto path = directory / heimdall::RuleConfigFileName;
    WriteConfig(directory, R"({"format":{"pointer-alignment":"middle"}})");
    EXPECT_FALSE(heimdall::LoadRuleConfiguration(path));
    WriteConfig(directory, R"({"format":{"reference-alignment":true}})");
    EXPECT_FALSE(heimdall::LoadRuleConfiguration(path));
    WriteConfig(directory, R"({"format":{"indent-width":true}})");
    EXPECT_FALSE(heimdall::LoadRuleConfiguration(path));
    WriteConfig(directory, R"({"format":{"align-consecutive-macros":"true"}})");
    EXPECT_FALSE(heimdall::LoadRuleConfiguration(path));
    WriteConfig(directory, R"({"format":{"align-consecutive-assignments":2}})");
    EXPECT_FALSE(heimdall::LoadRuleConfiguration(path));
    WriteConfig(directory, R"({"format":"left"})");
    EXPECT_FALSE(heimdall::LoadRuleConfiguration(path));
    std::filesystem::remove_all(directory);
}

TEST(RuleConfigSpec, LoadsFormatterBehaviorSettings)
{
    const auto directory = MakeConfigDir("format-settings");
    const auto path = directory / heimdall::RuleConfigFileName;
    WriteConfig(
        directory,
        R"({"format":{"indent-width":2,"use-tabs":true,"max-empty-lines":3,"column-limit":80,"sort-includes":true,"blank-line-after-control-block":false,"blank-line-after-type-definition":false,"space-before-inheritance-colon":false,"align-trailing-comments":false}})");

    auto found = heimdall::FindFormatOptions(directory);
    ASSERT_TRUE(found) <<(found ? "" : found.error());
    ASSERT_TRUE(*found);
    EXPECT_EQ((*found) -> indent_width, 2u);
    EXPECT_TRUE((*found)->use_tabs);
    EXPECT_EQ((*found) -> max_empty_lines, 3u);
    EXPECT_EQ((*found) -> column_limit, 80u);
    EXPECT_TRUE((*found)->sort_includes);
    EXPECT_FALSE((*found)->blank_line_after_control_block);
    EXPECT_FALSE((*found)->blank_line_after_type_definition);
    EXPECT_FALSE((*found)->space_before_inheritance_colon);
    EXPECT_FALSE((*found)->align_trailing_comments);
    std::filesystem::remove_all(directory);
}

TEST(RuleConfigSpec, FormatterUsesNearestConfiguredIncludeOrder)
{
    const auto directory = MakeConfigDir("format-include-order");
    const auto child = directory / "child";
    std::filesystem::create_directories(child);
    WriteConfig(directory,
                R"({"format":{"sort-includes":true},"include-order":{"groups":["angle","quote"]}})");
    WriteConfig(child,
                R"({"include-order":{"groups":["quote","angle"],"case-insensitive":false}})");

    const auto found = heimdall::FindFormatOptions(child);

    ASSERT_TRUE(found);
    ASSERT_TRUE(*found);
    EXPECT_TRUE((*found)->sort_includes);
    EXPECT_FALSE((*found)->includeCaseInsensitive);
    EXPECT_EQ(heimdall::Formatter(* *found).Format("#include <apple>\n#include <Zebra>\n#include \"app.hpp\"\n"),
              "#include \"app.hpp\"\n\n#include <Zebra>\n#include <apple>\n");
    std::filesystem::remove_all(directory);
}

TEST(RuleConfigSpec, BareFilenameInWorkingDirectoryResolvesConfig)
{
    const auto directory = MakeConfigDir("bare-filename");
    WriteConfig(directory,
                R"({"root":true,"format":{"sort-includes":true},"include-order":{"groups":["quote","angle"]}})");
    std::ofstream(directory / "file.cpp") << "#include <a>\n";
    const auto saved = std::filesystem::current_path();
    std::filesystem::current_path(directory);
    const auto found = heimdall::FindFormatOptions("file.cpp");
    std::filesystem::current_path(saved);

    ASSERT_TRUE(found);
    ASSERT_TRUE(*found);
    EXPECT_TRUE((*found)->sort_includes);
    ASSERT_EQ((*found)->includeOrder.size(), 2u);
    EXPECT_EQ((*found)->includeOrder[0], heimdall::IncludeGroup::Quote);
    EXPECT_EQ((*found)->includeOrder[1], heimdall::IncludeGroup::Angle);
    std::filesystem::remove_all(directory);
}

TEST(RuleConfigSpec, NearestFormatAlignmentWinsPerKey)
{
    const auto directory = MakeConfigDir("format-merge");
    const auto child = directory / "child";
    std::filesystem::create_directories(child);
    WriteConfig(
        directory,
        R"({"format":{"pointer-alignment":"left","reference-alignment":"left","indent-width":2,"use-tabs":true}})");
    WriteConfig(child, R"({"format":{"pointer-alignment":"right","indent-width":8}})");

    auto found = heimdall::FindFormatOptions(child);
    ASSERT_TRUE(found) <<(found ? "" : found.error());
    ASSERT_TRUE(*found);
    EXPECT_EQ((*found) -> pointer_alignment, heimdall::PointerAlignment::Right);
    EXPECT_EQ((*found) -> reference_alignment, heimdall::ReferenceAlignment::Left);
    EXPECT_EQ((*found) -> indent_width, 8u);
    EXPECT_TRUE((*found)->use_tabs);
    std::filesystem::remove_all(directory);
}

TEST(RuleConfigSpec, MergesFreyrSpacingSettingsPerKey)
{
    const auto directory = MakeConfigDir("freyr-spacing");
    const auto child = directory / "child";
    std::filesystem::create_directories(child);
    WriteConfig(directory,
                R"({"format":{"space-after-c-style-cast":false,"space-after-logical-not":true,"space-before-cpp11-braced-list":false}})");
    WriteConfig(child, R"({"format":{"space-after-c-style-cast":true}})");

    const auto found = heimdall::FindFormatOptions(child);

    ASSERT_TRUE(found) <<(found ? "" : found.error());
    ASSERT_TRUE(*found);
    EXPECT_TRUE((*found)->space_after_c_style_cast);
    EXPECT_TRUE((*found)->space_after_logical_not);
    EXPECT_FALSE((*found)->space_before_cpp11_braced_list);
    std::filesystem::remove_all(directory);
}

TEST(RuleConfigSpec, MergesConsecutiveAlignmentSettingsPerKey)
{
    const auto directory = MakeConfigDir("consecutive-alignment");
    const auto child = directory / "child";
    std::filesystem::create_directories(child);
    WriteConfig(directory,
                R"({"format":{"align-consecutive-macros":true,"align-consecutive-assignments":true}})");
    WriteConfig(child, R"({"format":{"align-consecutive-macros":false}})");

    const auto found = heimdall::FindFormatOptions(child);

    ASSERT_TRUE(found) <<(found ? "" : found.error());
    ASSERT_TRUE(*found);
    EXPECT_FALSE((*found)->align_consecutive_macros);
    EXPECT_TRUE((*found)->align_consecutive_assignments);
    std::filesystem::remove_all(directory);
}

TEST(RuleConfigSpec, StopsAtGitRootDirectoryMarker)
{
    const auto outer = MakeConfigDir("git-stop-outer");
    const auto repo = outer / "repo";
    const auto child = repo / "sub";
    std::filesystem::create_directories(child);
    std::filesystem::create_directories(repo / ".git");
    WriteConfig(outer, R"({"rules":{"cpp/no-null":"off"}})");
    WriteConfig(repo, R"({"rules":{"cpp/no-null":"error"}})");

    auto loaded = heimdall::FindRuleOptions(child);
    ASSERT_TRUE(loaded) <<(loaded ? "" : loaded.error());
    ASSERT_TRUE(*loaded);
    // Only the repo config is seen; the parent outside the git root is ignored.
    ASSERT_EQ((* *loaded).overrides.size(), 1);
    EXPECT_EQ((* *loaded).overrides[0].severity, heimdall::Severity::Error);
    std::filesystem::remove_all(outer);
}

TEST(RuleConfigSpec, StopsAtGitRootFileMarker)
{
    const auto outer = MakeConfigDir("git-stop-file-outer");
    const auto repo = outer / "repo";
    const auto child = repo / "sub";
    std::filesystem::create_directories(child);
    // Worktrees and submodules record the gitdir in a `.git` file.
    {
        std::ofstream gitlink(repo / ".git", std::ios::binary | std::ios::trunc);
        gitlink << "gitdir: /elsewhere/repo.git";
    }
    WriteConfig(outer, R"({"rules":{"cpp/no-null":"off"}})");
    WriteConfig(repo, R"({"rules":{"cpp/no-null":"error"}})");

    auto loaded = heimdall::FindRuleOptions(child);
    ASSERT_TRUE(loaded) <<(loaded ? "" : loaded.error());
    ASSERT_TRUE(*loaded);
    ASSERT_EQ((* *loaded).overrides.size(), 1);
    EXPECT_EQ((* *loaded).overrides[0].severity, heimdall::Severity::Error);
    std::filesystem::remove_all(outer);
}

TEST(RuleConfigSpec, LoadsMacroLimitsAndMergesThemFromTheNearestConfig)
{
    const auto directory = MakeConfigDir("macro_limits");
    std::filesystem::create_directories(directory / "inner");
    WriteConfig(directory, R"({"root":true,"macros":{"max-include-depth":64,"max-macros":1000}})");
    WriteConfig(directory / "inner", R"({"macros":{"max-macros":50}})");

    const auto outer = heimdall::FindRuleOptions(directory);
    const auto inner = heimdall::FindRuleOptions(directory / "inner");

    ASSERT_TRUE(outer&&*outer);
    EXPECT_EQ((*outer) -> macro_max_include_depth, 64u);
    EXPECT_EQ((*outer) -> macro_max_macros, 1000u);
    ASSERT_TRUE(inner&&*inner);
    EXPECT_EQ((*inner) -> macro_max_macros, 50u);
    EXPECT_EQ((*inner) -> macro_max_include_depth, 64u);
    std::filesystem::remove_all(directory);
}

TEST(RuleConfigSpec, DefaultsFollowTheCompilersAndTheStandard)
{
    const heimdall::RuleOptions options;

    EXPECT_EQ(options.macro_max_include_depth, 200u);
    EXPECT_EQ(options.macro_max_macros, 65536u);
}

TEST(RuleConfigSpec, RejectsInvalidMacroLimits)
{
    const auto directory = MakeConfigDir("macro_limits_invalid");
    for (const std::string_view contents :
        {
            R"({"macros":{"max-macros":0}})", R"({"macros":{"max-macros":"many"}})",
            R"({"macros":{"unknown":1}})", R"({"macros":[]})", R"({"macros":{"max-include-depth":2000000}})"
    })
    {
        WriteConfig(directory, contents);

        const auto loaded = heimdall::LoadRuleConfiguration(directory / heimdall::RuleConfigFileName);

        EXPECT_FALSE(loaded) << contents;
    }

    std::filesystem::remove_all(directory);
}
