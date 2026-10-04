#include <Heimdall/IncludeIndex.hpp>

#include <Heimdall/Lexer.hpp>
#include <Heimdall/ParseTree.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <deque>
#include <cstdio>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace heimdall
{

    namespace
    {

        struct IncludeRef
        {
            std::string name;
            bool angled = false;
        };

        std::vector<IncludeRef> ScanIncludes(std::string_view text)
        {
            std::vector<IncludeRef> refs;
            const std::vector<Token> tokens = Lexer(text).Lex();
            auto significant =[&](std::size_t i)->std::size_t
            {
                while (i < tokens.size() && (tokens[i].kind == TokenKind::Whitespace ||
                    tokens[i].kind == TokenKind::LineComment ||
                    tokens[i].kind == TokenKind::BlockComment))
                {
                    ++i;
                }

                return i;
            };
            auto text_of =[&](std::size_t i)->std::string_view
            {
                return text.substr(tokens[i].offset, tokens[i].length);
            };
            for (std::size_t i = 0; i < tokens.size(); ++i)
            {
                if (tokens[i].kind != TokenKind::Punctuation || text_of(i) != "#")
                {
                    continue;
                }

                i = significant(i + 1);
                if (i >= tokens.size() || tokens[i].kind != TokenKind::Identifier || text_of(i) != "include")
                {
                    continue;
                }

                i = significant(i + 1);
                if (i >= tokens.size())
                {
                    break;
                }

                if (tokens[i].kind == TokenKind::StringLiteral)
                {
                    std::string_view quoted = text_of(i);
                    const std::size_t open = quoted.find('"');
                    const std::size_t close = quoted.rfind('"');
                    if (open != std::string_view::npos && close != std::string_view::npos && close > open)
                    {
                        refs.push_back({std::string(quoted.substr(open + 1, close - open - 1)), false});
                    }
                }
                else if (tokens[i].kind == TokenKind::Punctuation && text_of(i) == "<")
                {
                    // Header names contain no `>`; slice raw source up to it.
                    const std::size_t start = tokens[i].offset + tokens[i].length;
                    std::size_t stop = start;
                    while (stop < text.size() && text[stop] != '>' && text[stop] != '\n')
                    {
                        ++stop;
                    }

                    if (stop < text.size() && text[stop] == '>')
                    {
                        std::string_view name = text.substr(start, stop - start);
                        while (!name.empty() && (name.front() == ' ' || name.front() == '\t'))
                        {
                            name.remove_prefix(1);
                        }

                        while (!name.empty() && (name.back() == ' ' || name.back() == '\t'))
                        {
                            name.remove_suffix(1);
                        }

                        if (!name.empty())
                        {
                            refs.push_back({std::string(name), true});
                        }
                    }
                }
            }

            return refs;
        }

        std::filesystem::path NormalizedAbsolute(const std::filesystem::path & path)
        {
            std::error_code ec;
            auto absolute = std::filesystem::absolute(path, ec);
            return (ec ? path : absolute).lexically_normal();
        }

        std::filesystem::path TryResolve(const std::string & name, bool angled,
            const std::filesystem::path & including_dir,
            const CompileCommand *command,
            const std::vector<std::filesystem::path> & system_dirs)
        {
            std::vector<std::filesystem::path> dirs;
            if (!angled)
            {
                if (!including_dir.empty())
                {
                    dirs.push_back(including_dir);
                }

                if (command != nullptr)
                {
                    dirs.insert(dirs.end(), command->quote_directories.begin(),
                        command->quote_directories.end());
                }
            }

            if (command != nullptr)
            {
                dirs.insert(dirs.end(), command->include_directories.begin(),
                    command->include_directories.end());
            }

            dirs.insert(dirs.end(), system_dirs.begin(), system_dirs.end());
            std::error_code ec;
            for (const auto & dir: dirs)
            {
                if (dir.empty())
                {
                    continue;
                }

                const auto candidate = NormalizedAbsolute(dir / name);
                if (std::filesystem::exists(candidate, ec) && !ec &&
                    std::filesystem::is_regular_file(candidate, ec))
                {
                    return candidate;
                }
            }

            return {};
        }

        std::string DriverOf(const CompileCommand *command)
        {
            if (command != nullptr && !command->arguments.empty() && !command->arguments.front().empty())
            {
                return command->arguments.front();
            }

            return "c++";
        }

        std::string ReadFile(const std::filesystem::path & path, std::size_t max_bytes)
        {
            std::error_code ec;
            const auto size = std::filesystem::file_size(path, ec);
            if (ec || size > max_bytes) return {};
            std::ifstream in(path, std::ios::binary);
            if (!in) return {};
            std::string content;
            content.resize(static_cast<std::size_t>(size));
            in.read(content.data(), static_cast<std::streamsize>(content.size()));
            content.resize(static_cast<std::size_t>(in.gcount()));
            return content;
        }

        // Name `X` of a leading `#ifndef X` / `#define X` include guard, or empty.
        std::string IncludeGuardOf(std::string_view text)
        {
            auto directive_name =[](std::string_view line, std::string_view keyword)->std::string
            {
                std::size_t i = 0;
                while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
                {
                    ++i;
                }

                if (i >= line.size() || line[i] != '#')
                {
                    return {};
                }

                ++i;
                while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
                {
                    ++i;
                }

                if (line.substr(i, keyword.size()) != keyword)
                {
                    return {};
                }

                i += keyword.size();
                while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
                {
                    ++i;
                }

                std::size_t end = i;
                while (end < line.size() && (std::isalnum(static_cast<unsigned char>(line[end])) || line[end] == '_'))
                {
                    ++end;
                }

                return std::string(line.substr(i, end - i));
            };
            std::string guard;
            bool in_block_comment = false;
            std::size_t pos = 0;
            while (pos < text.size())
            {
                std::size_t end = text.find('\n', pos);
                if (end == std::string_view::npos)
                {
                    end = text.size();
                }

                std::string_view line = text.substr(pos, end - pos);
                pos = end + 1;
                if (in_block_comment)
                {
                    in_block_comment = line.find("*/") == std::string_view::npos;
                    continue;
                }

                std::size_t first = line.find_first_not_of(" \t\r");
                if (first == std::string_view::npos || line.substr(first, 2) == "//")
                {
                    continue;
                }

                if (line.substr(first, 2) == "/*")
                {
                    in_block_comment = line.find("*/", first + 2) == std::string_view::npos;
                    continue;
                }

                if (guard.empty())
                {
                    guard = directive_name(line, "ifndef");
                    if (guard.empty())
                    {
                        return {};
                    }

                    continue;
                }

                return directive_name(line, "define") == guard ? guard : std::string {};
            }

            return {};
        }

        // Macros the compiler itself would have defined after preprocessing the
        // given headers (`__cplusplus`, `_GLIBCXX_USE_CXX11_ABI`, feature-test
        // macros...). Headers are parsed one by one with no `#include` expansion,
        // so without this every `#if __cplusplus >= ...` guard in the standard
        // library selects nothing and the class bodies never reach the index.
        // Object-like macros only; empty when the compiler cannot be run.
        Preprocessor::MacroMap CompilerMacros(const CompileCommand *command,
            const std::vector<std::filesystem::path> & headers)
        {
            Preprocessor::MacroMap macros;
            static std::atomic<unsigned> counter{0};
            std::error_code ec;
            const std::filesystem::path scratch = std::filesystem::temp_directory_path(ec) /
                ("heimdall-macros-" + std::to_string(counter.fetch_add(1)) + "-" +
                std::to_string(std::hash<std::thread::id>{}
                (std::this_thread::get_id()) % 100000) + ".cpp");
            if (ec)
            {
                return macros;
            }

            {
                std::ofstream out(scratch, std::ios::binary);
                if (!out)
                {
                    return macros;
                }

                for (const auto & header: headers)
                {
                    out << "#include \"" << header.generic_string() << "\"\n";
                }
            }

            std::string flags;
            bool has_standard = false;
            if (command != nullptr)
            {
                for (std::size_t i = 1; i < command->arguments.size(); ++i)
                {
                    const std::string & argument = command->arguments[i];
                    const bool takes_next = argument == "-isystem" || argument == "-iquote" ||
                        argument == "-idirafter" || argument == "-I" || argument == "-D" || argument == "-U";
                    if (takes_next && i + 1 < command->arguments.size())
                    {
                        flags += " " + argument + " \"" + command->arguments[++i] + "\"";
                    }
                    else if (argument.starts_with("-std=") || argument.starts_with("-stdlib=") ||
                        argument.starts_with("-D") || argument.starts_with("-U") ||
                        argument.starts_with("-I") || argument.starts_with("-isystem") ||
                        argument.starts_with("-nostd") || argument.starts_with("--target") ||
                        argument.starts_with("-m"))
                    {
                        has_standard = has_standard || argument.starts_with("-std=");
                        flags += " \"" + argument + "\"";
                    }
                }
            }

            if (!has_standard)
            {
                const CppStandard standard = command != nullptr ? command->standard : CppStandard::Cpp20;
                flags += standard == CppStandard::Cpp26 ? " -std=c++26"
                : standard == CppStandard::Cpp23 ? " -std=c++23" : " -std=c++20";
            }

            const std::string driver = DriverOf(command);
#if defined(_WIN32)
            const std::string line = "\"\"" + driver + "\" -x c++ -dM -E" + flags + " \"" +
                scratch.string() + "\" 2>nul\"";
            std::unique_ptr<FILE, decltype(&_pclose) > pipe(_popen(line.c_str(), "r"), _pclose);
#else
            const std::string line = "\"" + driver + "\" -x c++ -dM -E" + flags + " \"" +
                scratch.string() + "\" 2>/dev/null";
            std::unique_ptr<FILE, decltype(&pclose) > pipe(popen(line.c_str(), "r"), pclose);
#endif
            if (pipe)
            {
                std::string text;
                char buffer[4096];
                while (std::fgets(buffer, sizeof(buffer), pipe.get()) != nullptr)
                {
                    text += buffer;
                }

                std::size_t pos = 0;
                while (pos < text.size())
                {
                    std::size_t end = text.find('\n', pos);
                    if (end == std::string::npos)
                    {
                        end = text.size();
                    }

                    std::string_view row(text.data() + pos, end - pos);
                    pos = end + 1;
                    while (!row.empty() && row.back() == '\r')
                    {
                        row.remove_suffix(1);
                    }

                    constexpr std::string_view define = "#define ";
                    if (!row.starts_with(define))
                    {
                        continue;
                    }

                    row.remove_prefix(define.size());
                    const std::size_t name_end = row.find_first_of(" (");
                    if (name_end == std::string_view::npos || row[name_end] == '(')
                    {
                        if (name_end == std::string_view::npos)
                        {
                            macros.emplace(std::string(row), std::string());
                        }

                        continue; // function-like macros stay unexpanded
                    }

                    macros.emplace(std::string(row.substr(0, name_end)), std::string(row.substr(name_end + 1)));
                }
            }

            std::filesystem::remove(scratch, ec);
            // The probe included every header, so every include guard is now
            // defined; each header is parsed on its own and must still see its
            // body. Drop the guards (`#ifndef X` / `#define X` at the top).
            for (const auto & header: headers)
            {
                const std::string guard = IncludeGuardOf(ReadFile(header, 1 << 20));
                if (!guard.empty())
                {
                    macros.erase(guard);
                }
            }

            return macros;
        }

        bool IsReservedName(std::string_view name)
        {
            return!name.empty() && name.front() == '_';
        }

        std::string ScopePathKey(const std::vector<std::string> & path)
        {
            std::string key;
            for (const auto & element: path)
            {
                key += element;
                key += '\0';
            }

            return key;
        }

    } // namespace

    std::vector<std::filesystem::path> IncludeIndex::SystemIncludes(std::string_view compiler)
    {
        // Thread-safe memoization: previously a bare static map (data race as
        // soon as workers arrive) populated on the I/O thread via popen.
        static std::mutex mutex;
        static std::unordered_map<std::string, std::vector<std::filesystem::path>> cache;
        const std::string key(compiler.empty() ? "c++" : compiler);
        {
            const std::lock_guard<std::mutex> lock(mutex);
            if (const auto found = cache.find(key); found != cache.end())
            {
                return found->second;
            }
        }

        std::vector<std::filesystem::path> dirs;
#if defined(_WIN32)
        const std::string command = "\"" + key + "\" -xc++ -E -v - <nul 2>&1";
        std::unique_ptr<FILE, decltype(&_pclose) > pipe(_popen(command.c_str(), "r"), _pclose);
#else
        const std::string command = "\"" + key + "\" -xc++ -E -v - </dev/null 2>&1";
        std::unique_ptr<FILE, decltype(&pclose) > pipe(popen(command.c_str(), "r"), pclose);
#endif
        if (pipe)
        {
            bool listing = false;
            char buffer[512];
            while (std::fgets(buffer, sizeof(buffer), pipe.get()) != nullptr)
            {
                std::string_view line(buffer);
                while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
                {
                    line.remove_suffix(1);
                }

                while (!line.empty() && line.front() == ' ')
                {
                    line.remove_prefix(1);
                }

                if (line == "#include <...> search starts here:")
                {
                    listing = true;
                }
                else if (line == "End of search list.")
                {
                    listing = false;
                }
                else if (listing && !line.empty())
                {
                    constexpr std::string_view framework = " (framework directory)";
                    if (line.ends_with(framework))
                    {
                        line.remove_suffix(framework.size());
                    }

                    if (!line.empty())
                    {
                        dirs.push_back(NormalizedAbsolute(std::filesystem::path(line)));
                    }
                }
            }
        }

        {
            const std::lock_guard<std::mutex> lock(mutex);
            // Another thread may have populated while popen ran outside the lock.
            if (const auto found = cache.find(key); found != cache.end())
            {
                return found->second;
            }

            cache.emplace(key, dirs);
        }
        return dirs;
    }

    std::vector<std::filesystem::path> IncludeIndex::ResolveHeaders(const std::filesystem::path & base_dir,
        std::string_view text,
        const CompileCommand *command,
        const Limits &limits)
    {
        const std::vector<std::filesystem::path> system_dirs = SystemIncludes(DriverOf(command));
        std::vector<std::filesystem::path> ordered;
        std::unordered_set<std::string> visited;
        // Worklist of (including directory, include name, angled, depth).
        struct Work
        {
            std::filesystem::path dir;
            std::string name;
            bool angled = false;
            int depth = 0;
        };

        // Breadth-first, project (quoted) includes before system (angled) ones:
        // with the header cap a depth-first walk spent the whole budget inside
        // the last <standard header> and never reached the project's own headers.
        std::deque<Work> stack;
        auto enqueue =[&](const std::filesystem::path & dir, int depth, std::string_view source)
        {
            const std::vector<IncludeRef> refs = ScanIncludes(source);
            for (const bool angled:
                {
                    false, true
            })
            {
                for (const auto & ref: refs)
                {
                    if (ref.angled == angled)
                    {
                        stack.push_back({dir, ref.name, ref.angled, depth});
                    }
                }
            }
        };
        enqueue(base_dir, 0, text);

        while (!stack.empty() && ordered.size() < limits.max_headers)
        {
            Work work = std::move(stack.front());
            stack.pop_front();
            if (work.depth > limits.max_depth)
            {
                continue;
            }

            const auto resolved = TryResolve(work.name, work.angled, work.dir, command, system_dirs);
            if (resolved.empty())
            {
                continue;
            }

            const std::string key = resolved.string();
            if (!visited.insert(key).second)
            {
                continue;
            }

            ordered.push_back(resolved);
            const std::string nested = ReadFile(resolved, limits.max_file_bytes);
            if (nested.empty())
            {
                continue;
            }

            enqueue(resolved.parent_path(), work.depth + 1, nested);
        }

        return ordered;
    }

    std::filesystem::path IncludeIndex::ResolveIncludeAt(const std::filesystem::path & base_dir,
        std::string_view text, std::size_t offset, const CompileCommand *command)
    {
        offset = std::min(offset, text.size());
        const std::size_t begin = offset == 0 ? 0 : text.rfind('\n', offset - 1) + 1;
        std::size_t end = text.find('\n', offset);
        if (end == std::string_view::npos)
        {
            end = text.size();
        }

        // begin wraps to 0 when no newline precedes the offset (npos + 1).
        const std::vector<IncludeRef> refs = ScanIncludes(text.substr(begin, end - begin));
        if (refs.empty())
        {
            return {};
        }

        return TryResolve(refs.front().name, refs.front().angled, base_dir, command,
            SystemIncludes(DriverOf(command)));
    }

    std::string IncludeIndex::IncludeFingerprint(const std::filesystem::path & base_dir,
        std::string_view text, const CompileCommand *command)
    {
        // Raw line scan: no lexer, no filesystem access. Only the file's own
        // include lines matter; nested headers are covered by CacheKey's mtimes.
        std::string fingerprint;
        fingerprint += base_dir.string();
        fingerprint += '\0';
        std::size_t pos = 0;
        while (pos < text.size())
        {
            std::size_t end = text.find('\n', pos);
            if (end == std::string_view::npos)
            {
                end = text.size();
            }

            std::string_view line = text.substr(pos, end - pos);
            std::size_t first = 0;
            while (first < line.size() && (line[first] == ' ' || line[first] == '\t' || line[first] == '\r'))
            {
                ++first;
            }

            if (first < line.size() && line[first] == '#')
            {
                std::size_t word = first + 1;
                while (word < line.size() && (line[word] == ' ' || line[word] == '\t'))
                {
                    ++word;
                }

                const std::size_t word_end = line.find_first_of(" \t\r(", word);
                const std::string_view directive = line.substr(word, word_end == std::string_view::npos
                    ? word_end
                    : word_end - word);
                if (directive == "include" || directive == "include_next")
                {
                    fingerprint.append(line);
                    fingerprint += '\n';
                }
            }

            pos = end + 1;
        }

        fingerprint += '\0';
        if (command != nullptr)
        {
            fingerprint += std::to_string(static_cast<int>(command->standard));
            fingerprint += '\0';
            for (const auto & dir: command->include_directories)
            {
                fingerprint += dir.string();
                fingerprint += '\0';
            }

            for (const auto & dir: command->quote_directories)
            {
                fingerprint += dir.string();
                fingerprint += '\0';
            }

            std::vector<std::string> defines;
            for (const auto &[name, value]: command->defines)
            {
                defines.push_back(name + "=" + value);
            }

            std::sort(defines.begin(), defines.end());
            for (const auto & define: defines)
            {
                fingerprint += define;
                fingerprint += '\0';
            }
        }

        return fingerprint;
    }

    std::string IncludeIndex::CacheKey(const std::vector<std::filesystem::path> & headers,
        const CompileCommand *command)
    {
        std::string key;
        for (const auto & header: headers)
        {
            std::error_code ec;
            const auto size = std::filesystem::file_size(header, ec);
            const auto time = std::filesystem::last_write_time(header, ec);
            key += header.string();
            key += '\0';
            key += std::to_string(ec ? 0 : size);
            key += '\0';
            key += std::to_string(ec ? 0 : time.time_since_epoch().count());
            key += '\0';
        }

        key += '\1';
        if (command != nullptr)
        {
            key += std::to_string(static_cast<int>(command->standard));
            key += '\0';
            std::vector<std::string> defines;
            for (const auto &[name, value]: command->defines)
            {
                defines.push_back(name + "=" + value);
            }

            std::sort(defines.begin(), defines.end());
            for (const auto & define: defines)
            {
                key += define;
                key += '\0';
            }
        }

        return key;
    }

    IncludeIndex IncludeIndex::Build(const std::vector<std::filesystem::path> & headers,
        const CompileCommand *command, const Limits &limits)
    {
        IncludeIndex index;
        // Path -> position in index.m_scopes: merging is O(1) per scope instead of
        // the previous linear MergeScope scan (O(S^2) over hundreds of scopes for
        // <vector>+<string>+<algorithm>).
        std::unordered_map<std::string, std::size_t> positions;
        ParserOptions options;
        if (command != nullptr)
        {
            options.standard = command->standard;
            options.shared_macros = std::make_shared<const Preprocessor::MacroMap>(command->defines);
            if (!command->undefines.empty())
            {
                auto filtered = std::make_shared<Preprocessor::MacroMap>(command->defines);
                for (const auto & name: command->undefines)
                {
                    filtered->erase(name);
                }

                options.shared_macros = std::move(filtered);
            }
        }

        if (!headers.empty())
        {
            Preprocessor::MacroMap probed = CompilerMacros(command, headers);
            if (!probed.contains("__cplusplus"))
            {
                // No compiler to ask: at least select the right language branch.
                probed["__cplusplus"] = options.standard == CppStandard::Cpp26 ? "202400L"
                : options.standard == CppStandard::Cpp23 ? "202302L" : "202002L";
            }

            // The project's own -D/-U win over what the compiler reports.
            if (command != nullptr)
            {
                for (const auto &[name, value]: command->defines)
                {
                    probed[name] = value;
                }

                for (const auto & name: command->undefines)
                {
                    probed.erase(name);
                }
            }

            options.shared_macros = std::make_shared<const Preprocessor::MacroMap>(std::move(probed));
        }

        // Headers are independent: read + parse them on a worker fan-out, then
        // merge serially. IndexScopes is a pure function of (content, options),
        // so concurrent calls only share read-only state.
        std::vector<ScopeIndex> per_header(headers.size());
        if (!headers.empty())
        {
            if (headers.size() <= 2)
            {
                for (std::size_t h = 0; h < headers.size(); ++h)
                {
                    const std::string content = ReadFile(headers[h], limits.max_file_bytes);
                    if (content.empty())
                    {
                        continue;
                    }

                    per_header[h] = CompletionEngine::IndexScopes(content, options);
                }
            }
            else
            {
                unsigned workers = std::thread::hardware_concurrency();
                if (workers == 0)
                {
                    workers = 4;
                }

                if (workers > 2)
                {
                    workers -= 2;
                } // reserve I/O + diagnostics

                workers = std::max<unsigned>(workers, 1);
                workers = std::min<unsigned>(workers, static_cast<unsigned>(headers.size()));
                std::atomic<std::size_t> next = 0;
                std::vector<std::jthread> pool;
                pool.reserve(workers);
                for (unsigned w = 0; w < workers; ++w)
                {
                    pool.emplace_back([&]
                        {
                            while (true)
                            {
                                const std::size_t h = next.fetch_add(1, std::memory_order_relaxed);
                                if (h >= headers.size())
                                {
                                    return;
                            }

                                const std::string content = ReadFile(headers[h], limits.max_file_bytes);
                                if (content.empty())
                                {
                                    continue;
                            }

                                per_header[h] = CompletionEngine::IndexScopes(content, options);
                        }
                    });
                }
            }
        }

        index.m_files = headers;
        for (std::size_t header = 0; header < per_header.size(); ++header)
        {
            auto &scopes = per_header[header];
            for (auto & scope: scopes)
            {
                for (auto & member: scope.members)
                {
                    if (member.has_location)
                    {
                        member.file = static_cast<std::int32_t>(header);
                    }
                }
            }

            for (auto & scope: scopes)
            {
                if (!scope.path.empty())
                {
                    bool reserved = false;
                    for (const auto & element: scope.path)
                    {
                        if (IsReservedName(element))
                        {
                            reserved = true;
                            break;
                        }
                    }

                    if (reserved)
                    {
                        continue;
                    }
                }

                IndexedScope filtered;
                filtered.path = scope.path;
                filtered.kind = scope.kind;
                filtered.members = std::move(scope.members);
                filtered.bases = std::move(scope.bases);
                std::erase_if(filtered.members,
                    [](const CompletionItem &member)
                    {
                        return IsReservedName(member.label);
                });
                if (!filtered.members.empty() ||!filtered.bases.empty())
                {
                    const std::string key = ScopePathKey(filtered.path);
                    if (const auto found = positions.find(key); found != positions.end())
                    {
                        auto &entry = index.m_scopes[found->second];
                        entry.members.insert(entry.members.end(), filtered.members.begin(),
                            filtered.members.end());
                        for (auto & base: filtered.bases)
                        {
                            if (std::find(entry.bases.begin(), entry.bases.end(), base) == entry.bases.end())
                            {
                                entry.bases.push_back(std::move(base));
                            }
                        }
                    }
                    else
                    {
                        positions.emplace(key, index.m_scopes.size());
                        index.m_scopes.push_back(std::move(filtered));
                    }
                }
            }
        }

        return index;
    }

    IncludeIndex IncludeIndex::Build(const std::filesystem::path & base_dir, std::string_view text,
        const CompileCommand *command, const Limits &limits)
    {
        return Build(ResolveHeaders(base_dir, text, command, limits), command, limits);
    }

} // namespace heimdall
