#include "Reporting.hpp"

#include <fstream>
#include <iostream>

namespace heimdall::cli
{

    std::string JsonEscape(std::string_view text)
    {
        std::string out;
        for (const unsigned char c: text)
        {
            switch (c)
            {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                constexpr unsigned char kControlCharLimit = ' ';
                constexpr int kHexDigitBits = 4;
                constexpr unsigned char kLowNibbleMask = 0x0f;
                if (c < kControlCharLimit)
                {
                    constexpr char hex[] = "0123456789abcdef";
                    out += "\\u00";
                    out += hex[c >> kHexDigitBits];
                    out += hex[c & kLowNibbleMask];
                }
                else
                {
                    out += static_cast<char>(c);
                }
            }
        }

        return out;
    }

    bool WriteFile(const std::filesystem::path& path, std::string_view data)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            return false;
        }

        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        return static_cast<bool>(out);
    }

    int ReportResults(const std::vector<FileResult>& results, const Options& options)
    {
        constexpr int kExitUsageError = 2;
        bool failed = false;
        bool has_diagnostics = false;
        const auto severity_name =[](heimdall::Severity severity)
        {
            return severity == heimdall::Severity::Error ? "error" : "warning";
        };
        if (options.command == Command::Parse && options.json)
        {
            std::cout << '[';
            bool first_file = true;
            for (const auto & result: results)
            {
                if (!result.error.empty())
                {
                    std::cerr << result.path.string() << ": " << result.error << '\n';
                    failed = true;
                    continue;
                }

                if (!first_file)
                {
                    std::cout << ',';
                }

                first_file = false;
                std::cout << "{\"file\":\"" << JsonEscape(result.path.string()) << "\",\"standard\":\""
                << JsonEscape(result.standard_name) << "\",\"nodes\":[";
                bool first_node = true;
                for (const auto & node: result.nodes)
                {
                    if (!first_node)
                    {
                        std::cout << ',';
                    }

                    first_node = false;
                    std::cout << "{\"kind\":\"" << JsonEscape(node.kind) << "\",\"parent\":" << node.parent
                    << ",\"offset\":" << node.offset << ",\"length\":" << node.length << '}';
                }

                std::cout << "],\"diagnostics\":[";
                bool first_diag = true;
                for (const auto & diagnostic: result.syntax_diagnostics)
                {
                    if (!first_diag)
                    {
                        std::cout << ',';
                    }

                    first_diag = false;
                    std::cout << "{\"line\":" << diagnostic.line << ",\"column\":" << diagnostic.column
                    << ",\"severity\":\"error\",\"code\":\"" << JsonEscape(diagnostic.code)
                    << "\",\"message\":\"" << JsonEscape(diagnostic.message) << "\"}";
                    has_diagnostics = true;
                }

                std::cout << "]}";
            }

            std::cout << "]\n";
        }
        else if (options.json)
        {
            std::cout << '[';
            bool first = true;
            for (const auto & result: results)
            {
                if (!result.error.empty())
                {
                    std::cerr << result.path.string() << ": " << result.error << '\n';
                    failed = true;
                    continue;
                }

                if (options.semantic)
                {
                    if (result.has_semantic_context)
                    {
                        std::cerr << result.path.string() << ": semantic context: " << result.type_count
                        << " types, " << result.declaration_count << " pointer declarations, "
                        << result.ambiguous_count << " unresolved forms\n";
                    }
                    else
                    {
                        std::cerr << result.path.string() << ": no matching compile command; semantic checks skipped\n";
                    }
                }

                for (const auto & diagnostic: result.diagnostics)
                {
                    if (!first)
                    {
                        std::cout << ',';
                    }

                    first = false;
                    std::cout << "{\"file\":\"" << JsonEscape(result.path.string()) << "\",\"line\":" << diagnostic.line
                    << ",\"column\":" << diagnostic.column << ",\"severity\":\""
                    << severity_name(diagnostic.severity) << "\",\"code\":\""
                    << JsonEscape(diagnostic.code) << "\",\"message\":\"" << JsonEscape(diagnostic.message)
                    << "\"}";
                    has_diagnostics = true;
                }

                for (const auto & diagnostic: result.semantic_diagnostics)
                {
                    if (!first)
                    {
                        std::cout << ',';
                    }

                    first = false;
                    std::cout << "{\"file\":\"" << JsonEscape(result.path.string()) << "\",\"line\":" << diagnostic.line
                    << ",\"column\":" << diagnostic.column << ",\"severity\":\"warning\",\"code\":\""
                    << JsonEscape(diagnostic.code) << "\",\"message\":\"" << JsonEscape(diagnostic.message)
                    << "\"}";
                    has_diagnostics = true;
                }

                for (const auto & diagnostic: result.syntax_diagnostics)
                {
                    if (!first)
                    {
                        std::cout << ',';
                    }

                    first = false;
                    std::cout << "{\"file\":\"" << JsonEscape(result.path.string()) << "\",\"line\":" << diagnostic.line
                    << ",\"column\":" << diagnostic.column << ",\"severity\":\"error\",\"code\":\""
                    << JsonEscape(diagnostic.code) << "\",\"message\":\"" << JsonEscape(diagnostic.message)
                    << "\"}";
                    has_diagnostics = true;
                }
            }

            std::cout << "]\n";
        }
        else if (options.command == Command::Parse)
        {
            for (const auto & result: results)
            {
                if (!result.error.empty())
                {
                    std::cerr << result.path.string() << ": " << result.error << '\n';
                    failed = true;
                    continue;
                }

                for (const auto & diagnostic: result.syntax_diagnostics)
                {
                    std::cout << result.path.string() << ':' << diagnostic.line << ':' << diagnostic.column
                    << ": error " << diagnostic.code << ": " << diagnostic.message << '\n';
                }

                has_diagnostics |=!result.syntax_diagnostics.empty();
                std::cout << result.path.string() << ": parsed " << result.nodes.size() << " nodes (standard "
                << result.standard_name << "): " << result.syntax_diagnostics.size() << " syntax errors\n";
            }
        }
        else
        {
            for (const auto & result: results)
            {
                if (!result.error.empty())
                {
                    std::cerr << result.path.string() << ": " << result.error << '\n';
                    failed = true;
                    continue;
                }

                if (options.semantic)
                {
                    if (result.has_semantic_context)
                    {
                        std::cerr << result.path.string() << ": semantic context: " << result.type_count
                        << " known types, " << result.declaration_count << " pointer declarations, "
                        << result.ambiguous_count << " unresolved forms\n";
                    }
                    else
                    {
                        std::cerr << result.path.string() << ": no matching compile command; semantic checks skipped\n";
                    }
                }

                if (options.command == Command::Format)
                {
                    if (options.write)
                    {
                        if (result.changed && !WriteFile(result.path, result.output))
                        {
                            std::cerr << result.path.string() << ": failed to write formatted file\n";
                            failed = true;
                        }
                    }
                    else
                    {
                        std::cout << result.output;
                    }

                    continue;
                }

                has_diagnostics |=!result.diagnostics.empty();
                for (const auto & diagnostic: result.diagnostics)
                {
                    std::cout << result.path.string() << ':' << diagnostic.line << ':' << diagnostic.column << ": "
                    << severity_name(diagnostic.severity) << ' ' << diagnostic.code << ": "
                    << diagnostic.message << '\n';
                }

                for (const auto & diagnostic: result.semantic_diagnostics)
                {
                    std::cout << result.path.string() << ':' << diagnostic.line << ':' << diagnostic.column
                    << ": warning " << diagnostic.code << ": " << diagnostic.message << '\n';
                }

                for (const auto & diagnostic: result.syntax_diagnostics)
                {
                    std::cout << result.path.string() << ':' << diagnostic.line << ':' << diagnostic.column
                    << ": error " << diagnostic.code << ": " << diagnostic.message << '\n';
                }

                has_diagnostics |=!result.semantic_diagnostics.empty() ||!result.syntax_diagnostics.empty();
                if (options.fix && result.changed && !WriteFile(result.path, result.output))
                {
                    std::cerr << result.path.string() << ": failed to write fixes\n";
                    failed = true;
                }
            }
        }

        if (failed)
        {
            return kExitUsageError;
        }

        if ((options.command == Command::Check || options.command == Command::Parse) && has_diagnostics)
        {
            return 1;
        }

        return 0;
    }

} // namespace heimdall::cli
