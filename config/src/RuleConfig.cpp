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

        simdjson::dom::element order_element;
        if (!root["include-order"].get(order_element))
        {
            simdjson::dom::object order;
            if (const auto error = order_element.get_object().get(order); error)
            {
                return std::unexpected("'include-order' in Heimdall config must be an object: " + path.string());
            }

            simdjson::dom::element groups_element;
            if (order["groups"].get(groups_element))
            {
                return std::unexpected("'include-order' requires a 'groups' array in Heimdall config: " + path.string());
            }

            simdjson::dom::array groups;
            if (const auto error = groups_element.get_array().get(groups); error)
            {
                return std::unexpected("'groups' in 'include-order' must be an array: " + path.string());
            }

            std::vector<IncludeGroup> parsed_order;
            bool saw_angle = false;
            bool saw_quote = false;
            for (const auto group: groups)
            {
                std::string_view name;
                if (const auto error = group.get_string().get(name); error)
                {
                    return std::unexpected("'groups' in 'include-order' must list 'angle' and 'quote': " + path.string());
                }

                if (name == "angle")
                {
                    if (saw_angle)
                    {
                        return std::unexpected("'angle' appears more than once in 'include-order': " + path.string());
                    }
                    saw_angle = true;
                    parsed_order.push_back(IncludeGroup::Angle);
                }
                else if (name == "quote")
                {
                    if (saw_quote)
                    {
                        return std::unexpected("'quote' appears more than once in 'include-order': " + path.string());
                    }
                    saw_quote = true;
                    parsed_order.push_back(IncludeGroup::Quote);
                }
                else
                {
                    return std::unexpected("unknown include group in 'include-order': " + std::string(name));
                }
            }

            if (!saw_angle || !saw_quote)
            {
                return std::unexpected("'groups' in 'include-order' must list both 'angle' and 'quote': " + path.string());
            }

            configuration.options.include_order = std::move(parsed_order);
            configuration.has_include_order = true;

            bool case_insensitive = true;
            if (!order["case-insensitive"].get_bool().get(case_insensitive))
            {
                configuration.options.include_case_insensitive = case_insensitive;
            }
            else
            {
                simdjson::dom::element case_element;
                if (!order["case-insensitive"].get(case_element) && !case_element.is_null())
                {
                    return std::unexpected("'case-insensitive' in 'include-order' must be a boolean: " + path.string());
                }
            }
        }

        simdjson::dom::element format_element;
        if (!root["format"].get(format_element))
        {
            simdjson::dom::object format;
            if (const auto error = format_element.get_object().get(format); error)
            {
                return std::unexpected("'format' in Heimdall config must be an object: " + path.string());
            }

            for (const auto field: format)
            {
                const std::string_view key = field.key;
                std::string_view setting;
                if (const auto error = field.value.get_string().get(setting); error)
                {
                    return std::unexpected("'" + std::string(key) +
                        "' in 'format' must be 'left' or 'right': " + path.string());
                }

                if (key == "pointer-alignment")
                {
                    if (setting == "left")
                    {
                        configuration.format_options.pointer_alignment = PointerAlignment::Left;
                    }
                    else if (setting == "right")
                    {
                        configuration.format_options.pointer_alignment = PointerAlignment::Right;
                    }
                    else
                    {
                        return std::unexpected("invalid 'pointer-alignment' in Heimdall config '" +
                            path.string() + "': expected 'left' or 'right'");
                    }
                    configuration.has_pointer_alignment = true;
                }
                else if (key == "reference-alignment")
                {
                    if (setting == "left")
                    {
                        configuration.format_options.reference_alignment = ReferenceAlignment::Left;
                    }
                    else if (setting == "right")
                    {
                        configuration.format_options.reference_alignment = ReferenceAlignment::Right;
                    }
                    else
                    {
                        return std::unexpected("invalid 'reference-alignment' in Heimdall config '" +
                            path.string() + "': expected 'left' or 'right'");
                    }
                    configuration.has_reference_alignment = true;
                }
                else
                {
                    return std::unexpected("unknown key in 'format' in Heimdall config: " + std::string(key));
                }
            }
        }

        return configuration;
    }

    std::expected<std::vector<RuleConfiguration>, std::string> FindConfigurations(
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

        return configurations;
    }

    std::expected<std::optional<RuleOptions>, std::string> FindRuleOptions(
        const std::filesystem::path & directory)
    {
        auto found = FindConfigurations(directory);
        if (!found)
        {
            return std::unexpected(found.error());
        }
        auto & configurations = *found;

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
            if (it->has_include_order)
            {
                merged.include_order = it->options.include_order;
                merged.include_case_insensitive = it->options.include_case_insensitive;
            }
        }
        return std::optional<RuleOptions>{std::move(merged)};
    }

    std::expected<std::optional<FormatOptions>, std::string> FindFormatOptions(
        const std::filesystem::path & directory)
    {
        auto found = FindConfigurations(directory);
        if (!found)
        {
            return std::unexpected(found.error());
        }
        auto & configurations = *found;

        if (configurations.empty())
        {
            return std::optional<FormatOptions>{};
        }

        FormatOptions merged;
        for (auto it = configurations.rbegin(); it != configurations.rend(); ++it)
        {
            if (it->has_pointer_alignment)
            {
                merged.pointer_alignment = it->format_options.pointer_alignment;
            }
            if (it->has_reference_alignment)
            {
                merged.reference_alignment = it->format_options.reference_alignment;
            }
        }
        return std::optional<FormatOptions>{std::move(merged)};
    }

} // namespace heimdall
