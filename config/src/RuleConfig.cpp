#include <Heimdall/RuleConfig.hpp>

#include <simdjson.h>

#include <algorithm>
#include <limits>
#include <vector>

namespace heimdall
{

    namespace
    {

        bool IsKnownRule(std::string_view code)
        {
            return heimdall::IsKnownRuleCode(code);
        }

        bool IsGitRoot(const std::filesystem::path& directory)
        {
            // A `.git` entry marks the repository root: a directory for a
            // normal checkout, or a file (gitfile) for worktrees/submodules.
            std::error_code ec;
            return std::filesystem::exists(directory / ".git", ec) && !ec;
        }

        std::string ErrorFor(const std::filesystem::path& path, simdjson::error_code error)
        {
            return "cannot parse Heimdall config '" + path.string() +
                "': " + std::string(simdjson::error_message(error));
        }

    } // namespace

    std::expected<RuleConfiguration, std::string> LoadRuleConfiguration(
        const std::filesystem::path& path)
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
                return std::unexpected(
                    "'rules' in Heimdall config must be an object: " + path.string());
            }

            for (const auto field : rules)
            {
                const std::string_view code = field.key;
                if (!IsKnownRule(code))
                {
                    return std::unexpected(
                        "unknown rule code in Heimdall config: " + std::string(code));
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
                return std::unexpected(
                    "'suppressions' in Heimdall config must be a boolean: " + path.string());
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
                return std::unexpected(
                    "'root' in Heimdall config must be a boolean: " + path.string());
            }
        }

        simdjson::dom::element order_element;
        if (!root["include-order"].get(order_element))
        {
            simdjson::dom::object order;
            if (const auto error = order_element.get_object().get(order); error)
            {
                return std::unexpected(
                    "'include-order' in Heimdall config must be an object: " + path.string());
            }

            simdjson::dom::element groups_element;
            if (order["groups"].get(groups_element))
            {
                return std::unexpected(
                    "'include-order' requires a 'groups' array in Heimdall config: " +
                    path.string());
            }

            simdjson::dom::array groups;
            if (const auto error = groups_element.get_array().get(groups); error)
            {
                return std::unexpected(
                    "'groups' in 'include-order' must be an array: " + path.string());
            }

            std::vector<IncludeGroup> parsed_order;
            bool saw_angle = false;
            bool saw_quote = false;
            for (const auto group : groups)
            {
                std::string_view name;
                if (const auto error = group.get_string().get(name); error)
                {
                    return std::unexpected(
                        "'groups' in 'include-order' must list 'angle' and 'quote': " +
                        path.string());
                }

                if (name == "angle")
                {
                    if (saw_angle)
                    {
                        return std::unexpected(
                            "'angle' appears more than once in 'include-order': " + path.string());
                    }

                    saw_angle = true;
                    parsed_order.push_back(IncludeGroup::Angle);
                }
                else if (name == "quote")
                {
                    if (saw_quote)
                    {
                        return std::unexpected(
                            "'quote' appears more than once in 'include-order': " + path.string());
                    }

                    saw_quote = true;
                    parsed_order.push_back(IncludeGroup::Quote);
                }
                else
                {
                    return std::unexpected(
                        "unknown include group in 'include-order': " + std::string(name));
                }
            }

            if (!saw_angle ||!saw_quote)
            {
                return std::unexpected(
                    "'groups' in 'include-order' must list both 'angle' and 'quote': " +
                    path.string());
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
                    return std::unexpected(
                        "'case-insensitive' in 'include-order' must be a boolean: " +
                        path.string());
                }
            }
        }

        simdjson::dom::element doc_element;
        if (!root["doc"].get(doc_element))
        {
            simdjson::dom::object doc;
            if (const auto error = doc_element.get_object().get(doc); error)
            {
                return std::unexpected(
                    "'doc' in Heimdall config must be an object: " + path.string());
            }

            simdjson::dom::element scope_element;
            if (!doc["scope"].get(scope_element))
            {
                std::string_view scope;
                if (const auto error = scope_element.get_string().get(scope);
                    error ||(scope != "public" && scope != "private" && scope != "all"))
                {
                    return std::unexpected(
                        "'scope' in 'doc' must be 'public', 'private' or 'all': " + path.string());
                }

                configuration.options.doc_scope = scope == "public"
                ? DocScope::Public
                : scope == "private"
                ? DocScope::Private
                : DocScope::All;
                configuration.has_doc_scope = true;
            }
        }

        simdjson::dom::element format_element;
        if (!root["format"].get(format_element))
        {
            simdjson::dom::object format;
            if (const auto error = format_element.get_object().get(format); error)
            {
                return std::unexpected(
                    "'format' in Heimdall config must be an object: " + path.string());
            }

            for (const auto field : format)
            {
                const std::string_view key = field.key;
                const auto mark_override =[&]
                {
                    configuration.format_overrides.emplace_back(key);
                };
                const auto read_boolean =[&](bool& setting)->std::expected<void, std::string> {
                    if (field.value.get_bool().get(setting))
                    {
                        return std::unexpected("'" + std::string(key) +
                            "' in 'format' must be a boolean: " + path.string());
                    }

                    mark_override();
                    return {};
                };
                const auto read_size =
                    [&](std::size_t& setting)->std::expected<void, std::string> {
                    std::uint64_t value;
                    if (field.value.get_uint64().get(value) ||
                        value > std::numeric_limits<std::size_t>::max())
                    {
                        return std::unexpected(
                            "'" + std::string(key) +
                            "' in 'format' must be a non-negative integer: " + path.string());
                    }

                    setting = static_cast<std::size_t>(value);
                    mark_override();
                    return {};
                };

                if (key == "indent-width")
                {
                    if (auto result = read_size(configuration.format_options.indent_width); !result)
                    {
                        return std::unexpected(result.error());
                    }

                    continue;
                }

                if (key == "use-tabs")
                {
                    if (auto result = read_boolean(configuration.format_options.use_tabs); !result)
                    {
                        return std::unexpected(result.error());
                    }

                    continue;
                }

                if (key == "max-empty-lines")
                {
                    if (auto result = read_size(configuration.format_options.max_empty_lines);
                        !result)
                    {
                        return std::unexpected(result.error());
                    }

                    continue;
                }

                if (key == "column-limit")
                {
                    if (auto result = read_size(configuration.format_options.column_limit); !result)
                    {
                        return std::unexpected(result.error());
                    }

                    continue;
                }

                if (key == "sort-includes")
                {
                    if (auto result = read_boolean(configuration.format_options.sort_includes);
                        !result)
                    {
                        return std::unexpected(result.error());
                    }

                    continue;
                }

                if (key == "blank-line-after-control-block")
                {
                    if (auto result = read_boolean(
                        configuration.format_options.blank_line_after_control_block);
                        !result)
                    {
                        return std::unexpected(result.error());
                    }

                    continue;
                }

                if (key == "blank-line-after-type-definition")
                {
                    if (auto result = read_boolean(
                        configuration.format_options.blank_line_after_type_definition);
                        !result)
                    {
                        return std::unexpected(result.error());
                    }

                    continue;
                }

                if (key == "space-before-inheritance-colon")
                {
                    if (auto result = read_boolean(
                        configuration.format_options.space_before_inheritance_colon);
                        !result)
                    {
                        return std::unexpected(result.error());
                    }

                    continue;
                }

                if (key == "align-trailing-comments")
                {
                    if (auto result =
                        read_boolean(configuration.format_options.align_trailing_comments);
                        !result)
                    {
                        return std::unexpected(result.error());
                    }

                    continue;
                }

                if (key == "align-consecutive-macros")
                {
                    if (auto result =
                        read_boolean(configuration.format_options.align_consecutive_macros);
                        !result)
                    {
                        return std::unexpected(result.error());
                    }

                    continue;
                }

                if (key == "align-consecutive-assignments")
                {
                    if (auto result = read_boolean(
                        configuration.format_options.align_consecutive_assignments);
                        !result)
                    {
                        return std::unexpected(result.error());
                    }

                    continue;
                }

                if (key == "space-after-c-style-cast")
                {
                    if (auto result =
                        read_boolean(configuration.format_options.space_after_c_style_cast);
                        !result)
                    {
                        return std::unexpected(result.error());
                    }

                    continue;
                }

                if (key == "space-after-logical-not")
                {
                    if (auto result =
                        read_boolean(configuration.format_options.space_after_logical_not);
                        !result)
                    {
                        return std::unexpected(result.error());
                    }

                    continue;
                }

                if (key == "space-before-cpp11-braced-list")
                {
                    if (auto result = read_boolean(
                        configuration.format_options.space_before_cpp11_braced_list);
                        !result)
                    {
                        return std::unexpected(result.error());
                    }

                    continue;
                }

                if (key == "blank-line-between-methods")
                {
                    bool setting;
                    if (field.value.get_bool().get(setting))
                    {
                        return std::unexpected(
                            "'blank-line-between-methods' in 'format' must be a boolean: " +
                            path.string());
                    }

                    configuration.format_options.blank_line_between_methods = setting;
                    configuration.has_blank_line_between_methods = true;
                    mark_override();
                    continue;
                }

                if (key == "max-parameters-per-line")
                {
                    std::uint64_t setting;
                    if (field.value.get_uint64().get(setting) ||
                        setting > std::numeric_limits<std::size_t>::max())
                    {
                        return std::unexpected("'max-parameters-per-line' in 'format' must be a "
                            "non-negative integer: " +
                            path.string());
                    }

                    configuration.format_options.max_parameters_per_line =
                        static_cast<std::size_t>(setting);
                    configuration.has_max_parameters_per_line = true;
                    mark_override();
                    continue;
                }

                std::string_view setting;
                if (const auto error = field.value.get_string().get(setting); error)
                {
                    return std::unexpected(
                        "'" + std::string(key) +
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
                    mark_override();
                }
                else if (key == "reference-alignment")
                {
                    if (setting == "left")
                    {
                        configuration.format_options.reference_alignment = ReferenceAlignment::Left;
                    }
                    else if (setting == "right")
                    {
                        configuration.format_options.reference_alignment =
                            ReferenceAlignment::Right;
                    }
                    else
                    {
                        return std::unexpected(
                            "invalid 'reference-alignment' in Heimdall config '" + path.string() +
                            "': expected 'left' or 'right'");
                    }

                    configuration.has_reference_alignment = true;
                    mark_override();
                }
                else
                {
                    return std::unexpected(
                        "unknown key in 'format' in Heimdall config: " + std::string(key));
                }
            }
        }

        return configuration;
    }

    std::expected<std::vector<RuleConfiguration>, std::string> FindConfigurations(
        const std::filesystem::path& directory)
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

            // Never cross the repository boundary: a `.git` entry marks the
            // top of the project, so configs in parent directories (home,
            // /tmp, other checkouts) are ignored.
            if (IsGitRoot(current))
            {
                break;
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
        const std::filesystem::path& directory)
    {
        auto found = FindConfigurations(directory);
        if (!found)
        {
            return std::unexpected(found.error());
        }

        auto& configurations = *found;

        if (configurations.empty())
        {
            return std::optional<RuleOptions> {};
        }

        RuleOptions merged;
        for (auto it = configurations.rbegin(); it != configurations.rend(); ++it)
        {
            merged.overrides.insert(merged.overrides.end(),
                it->options.overrides.begin(),
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

            if (it->has_doc_scope)
            {
                merged.doc_scope = it->options.doc_scope;
            }
        }

        return std::optional<RuleOptions> {std::move(merged)};
    }

    std::expected<std::optional<FormatOptions>, std::string> FindFormatOptions(
        const std::filesystem::path& directory)
    {
        auto found = FindConfigurations(directory);
        if (!found)
        {
            return std::unexpected(found.error());
        }

        auto& configurations = *found;

        if (configurations.empty())
        {
            return std::optional<FormatOptions> {};
        }

        FormatOptions merged;
        for (auto it = configurations.rbegin(); it != configurations.rend(); ++it)
        {
            for (const auto& key : it->format_overrides)
            {
                const auto& value = it->format_options;
                if (key == "pointer-alignment")
                {
                    merged.pointer_alignment = value.pointer_alignment;
                }
                else if (key == "reference-alignment")
                {
                    merged.reference_alignment = value.reference_alignment;
                }
                else if (key == "blank-line-between-methods")
                {
                    merged.blank_line_between_methods = value.blank_line_between_methods;
                }
                else if (key == "max-parameters-per-line")
                {
                    merged.max_parameters_per_line = value.max_parameters_per_line;
                }
                else if (key == "indent-width")
                {
                    merged.indent_width = value.indent_width;
                }
                else if (key == "use-tabs")
                {
                    merged.use_tabs = value.use_tabs;
                }
                else if (key == "max-empty-lines")
                {
                    merged.max_empty_lines = value.max_empty_lines;
                }
                else if (key == "column-limit")
                {
                    merged.column_limit = value.column_limit;
                }
                else if (key == "sort-includes")
                {
                    merged.sort_includes = value.sort_includes;
                }
                else if (key == "blank-line-after-control-block")
                {
                    merged.blank_line_after_control_block = value.blank_line_after_control_block;
                }
                else if (key == "blank-line-after-type-definition")
                {
                    merged.blank_line_after_type_definition =
                        value.blank_line_after_type_definition;
                }
                else if (key == "space-before-inheritance-colon")
                {
                    merged.space_before_inheritance_colon = value.space_before_inheritance_colon;
                }
                else if (key == "align-trailing-comments")
                {
                    merged.align_trailing_comments = value.align_trailing_comments;
                }
                else if (key == "align-consecutive-macros")
                {
                    merged.align_consecutive_macros = value.align_consecutive_macros;
                }
                else if (key == "align-consecutive-assignments")
                {
                    merged.align_consecutive_assignments = value.align_consecutive_assignments;
                }
                else if (key == "space-after-c-style-cast")
                {
                    merged.space_after_c_style_cast = value.space_after_c_style_cast;
                }
                else if (key == "space-after-logical-not")
                {
                    merged.space_after_logical_not = value.space_after_logical_not;
                }
                else if (key == "space-before-cpp11-braced-list")
                {
                    merged.space_before_cpp11_braced_list = value.space_before_cpp11_braced_list;
                }
            }
        }

        return std::optional<FormatOptions> {std::move(merged)};
    }

} // namespace heimdall
