#include <Heimdall/CompileDatabase.hpp>

#include <simdjson.h>

#include <system_error>
#include <algorithm>
#include <cctype>

namespace heimdall
{

    namespace
    {

        std::filesystem::path AbsoluteNormalized(const std::filesystem::path &path)
        {
            std::error_code ec;
            auto absolute = std::filesystem::absolute(path, ec);
            return (ec ? path : absolute).lexically_normal();
        }

        // Comparison form of a path: the Windows file system ignores case, and
        // editors disagree with compile databases on drive-letter case (`c:` vs `C:`).
        std::filesystem::path ComparableKey(const std::filesystem::path &path)
        {
            auto text = AbsoluteNormalized(path).generic_string();
#if defined(_WIN32)
            std::transform(text.begin(), text.end(), text.begin(),
                [](unsigned char c)
                {
                    return static_cast<char>(std::tolower(c));
            });
#endif
            return std::filesystem::path(text);
        }

        void ParseOption(CompileCommand &command, std::string_view arg, std::string_view next,
            bool &consume_next)
        {
            consume_next = false;
            auto parse_define =[&command](std::string_view value)
            {
                const auto equal = value.find('=');
                const std::string name(value.substr(0, equal));
                if (!name.empty())
                {
                    command.undefines.erase(std::remove(command.undefines.begin(), command.undefines.end(), name),
                        command.undefines.end());
                    command.defines[name] = equal == std::string_view::npos ? "1" : std::string(value.substr(equal + 1));
                }
            };
            auto parse_undef =[&command](std::string_view value)
            {
                if (!value.empty())
                {
                    const std::string name(value);
                    command.defines.erase(name);
                    command.undefines.push_back(name);
                }
            };
            auto parse_include =[&command](std::string_view value)
            {
                if (value.empty())
                {
                    return;
                }

                std::filesystem::path include(value);
                if (include.is_relative())
                {
                    include = command.directory / include;
                }

                command.include_directories.push_back(include.lexically_normal());
            };
            auto parse_quote =[&command](std::string_view value)
            {
                if (value.empty())
                {
                    return;
                }

                std::filesystem::path quote(value);
                if (quote.is_relative())
                {
                    quote = command.directory / quote;
                }

                command.quote_directories.push_back(quote.lexically_normal());
            };

            auto parse_standard =[&command](std::string_view value)
            {
                if (value == "c++23" || value == "gnu++23" || value == "c++2b" || value == "gnu++2b")
                {
                    command.standard = CppStandard::Cpp23;
                }
                else if (value == "c++26" || value == "gnu++26" || value == "c++2c" || value == "gnu++2c" ||
                    value == "c++latest")
                {
                    command.standard = CppStandard::Cpp26;
                }
                else if (value == "c++20" || value == "gnu++20" || value == "c++2a" || value == "gnu++2a")
                {
                    command.standard = CppStandard::Cpp20;
                }
            };

            if (arg == "-std" && !next.empty())
            {
                parse_standard(next);
                consume_next = true;
            }
            else if (arg.starts_with("-std="))
            {
                parse_standard(arg.substr(5));
            }
            else if (arg.starts_with("/std:"))
            {
                parse_standard(arg.substr(5));
            }
            else if (arg == "-D" || arg == "/D")
            {
                if (!next.empty())
                {
                    parse_define(next);
                    consume_next = true;
                }
            }
            else if (arg.starts_with("-D") || arg.starts_with("/D"))
            {
                parse_define(arg.substr(2));
            }
            else if (arg == "-U" || arg == "/U")
            {
                if (!next.empty())
                {
                    parse_undef(next);
                    consume_next = true;
                }
            }
            else if (arg.starts_with("-U") || arg.starts_with("/U"))
            {
                parse_undef(arg.substr(2));
            }
            else if (arg == "-I" || arg == "/I")
            {
                if (!next.empty())
                {
                    parse_include(next);
                    consume_next = true;
                }
            }
            else if (arg.starts_with("-I") || arg.starts_with("/I"))
            {
                parse_include(arg.substr(2));
            }
            else if (arg == "-iquote")
            {
                if (!next.empty())
                {
                    parse_quote(next);
                    consume_next = true;
                }
            }
            else if (arg.starts_with("-iquote"))
            {
                parse_quote(arg.substr(7));
            }
            else if (arg == "-isystem" || arg == "-idirafter")
            {
                if (!next.empty())
                {
                    parse_include(next);
                    consume_next = true;
                }
            }
            else if (arg.starts_with("-isystem") || arg.starts_with("-idirafter"))
            {
                const auto value = arg.starts_with("-isystem") ? arg.substr(8) : arg.substr(10);
                parse_include(value);
            }
        }

        std::vector<std::string> SplitCommand(std::string_view command)
        {
            std::vector<std::string> args;
            std::string current;
            char quote = '\0';
            bool escaped = false;
            for (std::size_t i = 0; i < command.size(); ++i)
            {
                const char c = command[i];
                if (escaped)
                {
                    current += c;
                    escaped = false;
                }
                else if (c == '\\' && quote != '\'')
                {
                    // Only quotes, backslashes and blanks are escapable; any other
                    // backslash is a Windows path separator (`C:\mingw64\bin\g++`).
                    const char next = i + 1 < command.size() ? command[i + 1] : '\0';
                    if (next == '"' || next == '\'' || next == '\\' || next == ' ' || next == '\t')
                    {
                        escaped = true;
                    }
                    else
                    {
                        current += c;
                    }
                }
                else if (quote != '\0')
                {
                    if (c == quote)
                    {
                        quote = '\0';
                    }
                    else
                    {
                        current += c;
                    }
                }
                else if (c == '"' || c == '\'')
                {
                    quote = c;
                }
                else if (c == ' ' || c == '\t')
                {
                    if (!current.empty())
                    {
                        args.push_back(std::move(current));
                        current.clear();
                    }
                }
                else
                {
                    current += c;
                }
            }

            if (escaped)
            {
                current += '\\';
            }

            if (!current.empty())
            {
                args.push_back(std::move(current));
            }

            return args;
        }

        bool GetString(simdjson::dom::object object, const char *key, std::string &output)
        {
            std::string_view value;
            if (object[key].get_string().get(value))
            {
                return false;
            }

            output.assign(value);
            return true;
        }

    } // namespace

    std::expected<CompileDatabase,
        std::string> CompileDatabase::Load(const std::filesystem::path &path)
    {
        simdjson::dom::parser parser;
        simdjson::dom::element document;
        const auto parse_error = parser.load(path.string()).get(document);
        if (parse_error)
        {
            return std::unexpected("cannot parse compile database '" + path.string() + "': " +
                std::string(simdjson::error_message(parse_error)));
        }

        simdjson::dom::array entries;
        if (document.get_array().get(entries))
        {
            return std::unexpected("compile database root must be a JSON array");
        }

        CompileDatabase database;
        for (simdjson::dom::element entry_element: entries)
        {
            simdjson::dom::object entry;
            if (entry_element.get_object().get(entry))
            {
                continue;
            }

            std::string directory_text;
            std::string file_text;
            if (!GetString(entry, "directory", directory_text) ||!GetString(entry, "file", file_text))
            {
                continue;
            }

            CompileCommand command;
            std::filesystem::path directory(directory_text);
            if (directory.is_relative())
            {
                directory = path.parent_path() / directory;
            }

            command.directory = AbsoluteNormalized(directory);
            command.file = file_text;
            if (command.file.is_relative())
            {
                command.file = command.directory / command.file;
            }

            command.file = command.file.lexically_normal();

            simdjson::dom::element args_element;
            if (!entry["arguments"].get(args_element))
            {
                simdjson::dom::array args;
                if (!args_element.get_array().get(args))
                {
                    for (simdjson::dom::element arg: args)
                    {
                        std::string_view value;
                        if (!arg.get_string().get(value))
                        {
                            command.arguments.emplace_back(value);
                        }
                    }
                }
            }

            if (command.arguments.empty())
            {
                std::string command_text;
                if (GetString(entry, "command", command_text))
                {
                    command.arguments = SplitCommand(command_text);
                }
            }

            for (std::size_t i = 0; i < command.arguments.size(); ++i)
            {
                bool consume_next = false;
                const std::string_view next = i + 1 < command.arguments.size() ? command.arguments[i + 1] : std::string_view {};
                ParseOption(command, command.arguments[i], next, consume_next);
                if (consume_next)
                {
                    ++i;
                }
            }

            database.m_commands.push_back(std::move(command));
        }

        return database;
    }

    const CompileCommand * CompileDatabase::Find(std::filesystem::path file) const
    {
        file = ComparableKey(file);
        for (const auto & command: m_commands)
        {
            if (ComparableKey(command.file) == file)
            {
                return &command;
            }
        }

        return nullptr;
    }

    const CompileCommand * CompileDatabase::FindOrNearest(std::filesystem::path file) const
    {
        if (const auto * exact = Find(file))
        {
            return exact;
        }

        file = ComparableKey(file);
        const CompileCommand *best = nullptr;
        std::size_t best_score = 0;
        for (const auto & command: m_commands)
        {
            const auto other = ComparableKey(command.file);
            std::size_t score = 0;
            for (auto a = file.begin(), b = other.begin(); a != file.end() && b != other.end() && *a == *b; ++a,
                ++b)
            {
                ++score;
            }

            if (best == nullptr || score > best_score)
            {
                best = &command;
                best_score = score;
            }
        }

        return best;
    }

} // namespace heimdall
