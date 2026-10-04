#include <Heimdall/RuleConfig.hpp>

#include <simdjson.h>

#include <algorithm>
#include <vector>

namespace heimdall
{

    namespace
    {

        bool IsKnownRule(std::string_view code)
        {
            return heimdall::IsKnownRuleCode(code);
        }

        std::string ErrorFor(const std::filesystem::path & path, simdjson::error_code error)
        {
            return "cannot parse Heimdall config '" + path.string() + "': " +
                std::string(simdjson::error_message(error));
        }

    } // namespace

    std::expected<RuleConfiguration, std::string> LoadRuleConfiguration(const std::filesystem::path & path)
    {
        simdjson::dom::parser parser;
        simdjson::dom::element document;
        if (const auto error = parser.load(path.string()).get(document); error)
        {
            return std::unexpected(ErrorFor(path, error));
        }

        simdjson::dom::object root;
        if (const auto error = document.get_object().get(root); error)
        {
            return std::unexpected("Heimdall config root must be a JSON object: " + path.string());
        }

        RuleConfiguration configuration;
        simdjson::dom::element rules_element;
        if (!root["rules"].get(rules_element))
        {
            simdjson::dom::object rules;
            if (const auto error = rules_element.get_object().get(rules); error)
            {
                return std::unexpected("'rules' in Heimdall config must be an object: " + path.string());
            }

            for (const auto field: rules)
            {
                const std::string_view code = field.key;
                if (!IsKnownRule(code))
                {
                    return std::unexpected("unknown rule code in Heimdall config: " + std::string(code));
                }

                std::string_view setting;
                if (const auto error = field.value.get_string().get(setting); error)
                {
                    return std::unexpected("rule setting for '" + std::string(code) +
                        "' must be 'off', 'warning' or 'error'");
                }

                RuleOverride override;
                override.code = code;
                if (setting == "off")
                {
                    override.enabled = false;
                }
                else if (setting == "warning")
                {
                    override.severity = Severity::Warning;
                }
                else if (setting == "error")
                {
                    override.severity = Severity::Error;
                }
                else
                {
                    return std::unexpected("invalid setting for rule '" + std::string(code) +
                        "': expected 'off', 'warning' or 'error'");
                }
                configuration.options.overrides.push_back(std::move(override));
            }
        }

        bool suppressions = true;
        if (!root["suppressions"].get_bool().get(suppressions))
        {
            configuration.options.honor_suppressions = suppressions;
            configuration.has_suppressions = true;
        }
        else
        {
            simdjson::dom::element suppressions_element;
            if (!root["suppressions"].get(suppressions_element) && !suppressions_element.is_null())
            {
                return std::unexpected("'suppressions' in Heimdall config must be a boolean: " + path.string());
            }
        }

        bool is_root = false;
        if (!root["root"].get_bool().get(is_root))
        {
            configuration.root = is_root;
        }
        else
        {
            simdjson::dom::element root_element;
            if (!root["root"].get(root_element) && !root_element.is_null())
            {
                return std::unexpected("'root' in Heimdall config must be a boolean: " + path.string());
            }
        }

        return configuration;
    }

    std::expected<std::optional<RuleOptions>, std::string> FindRuleOptions(
        const std::filesystem::path & directory)
    {
        std::vector<RuleConfiguration> configurations;
        auto current = directory;
        if (current.empty())
        {
            current = std::filesystem::current_path();
        }
        if (std::filesystem::is_regular_file(current))
        {
            current = current.parent_path();
        }
        current = std::filesystem::absolute(current).lexically_normal();

        while (!current.empty())
        {
            const auto config_path = current / RuleConfigFileName;
            std::error_code ec;
            if (std::filesystem::exists(config_path, ec) && !ec)
            {
                auto loaded = LoadRuleConfiguration(config_path);
                if (!loaded)
                {
                    return std::unexpected(loaded.error());
                }
                const bool reached_root = loaded->root;
                configurations.push_back(std::move(*loaded));
                if (reached_root)
                {
                    break;
                }
            }

            const auto parent = current.parent_path();
            if (parent == current)
            {
                break;
            }
            current = parent;
        }

        if (configurations.empty())
        {
            return std::optional<RuleOptions>{};
        }

        RuleOptions merged;
        for (auto it = configurations.rbegin(); it != configurations.rend(); ++it)
        {
            merged.overrides.insert(merged.overrides.end(), it->options.overrides.begin(),
                it->options.overrides.end());
            if (it->has_suppressions)
            {
                merged.honor_suppressions = it->options.honor_suppressions;
            }
        }
        return std::optional<RuleOptions>{std::move(merged)};
    }

} // namespace heimdall
