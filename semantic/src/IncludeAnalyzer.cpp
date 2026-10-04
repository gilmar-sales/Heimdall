#include <Heimdall/IncludeAnalyzer.hpp>

#include <Heimdall/Lexer.hpp>
#include <Heimdall/LineTable.hpp>
#include <Heimdall/Preprocessor.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <functional>
#include <fstream>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace heimdall
{

    struct NameHash
    {
        using is_transparent = void;
        std::size_t operator()(std::string_view name) const noexcept
        {
            return std::hash<std::string_view>{}
            (name);
        }
    };

    struct HeaderSymbols
    {
        std::unordered_set<std::string, NameHash, std::equal_to<>> names;
        // The file declares something that is used without being named:
        // a namespace-scope operator or an explicit template specialization.
        bool implicit_use = false;
        bool readable = false;
    };

    namespace
    {

        const std::unordered_set<std::string_view> & Keywords()
        {
            static const std::unordered_set<std::string_view> keywords = {
                "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor", "bool", "break",
                "case", "catch", "char", "char8_t", "char16_t", "char32_t", "class", "compl", "concept",
                "const", "consteval", "constexpr", "constinit", "const_cast", "continue", "co_await",
                "co_return", "co_yield", "decltype", "default", "delete", "do", "double", "dynamic_cast",
                "else", "enum", "explicit", "export", "extern", "false", "final", "float", "for", "friend",
                "goto", "if", "import", "inline", "int", "long", "module", "mutable", "namespace", "new",
                "noexcept", "not", "not_eq", "nullptr", "operator", "or", "or_eq", "override", "private",
                "protected", "public", "register", "reinterpret_cast", "requires", "return", "short",
                "signed", "sizeof", "static", "static_assert", "static_cast", "struct", "switch",
                "template", "this", "thread_local", "throw", "true", "try", "typedef", "typeid",
                "typename", "union", "unsigned", "using", "virtual", "void", "volatile", "wchar_t", "while",
                "xor", "xor_eq",
                // Standard attribute names: written in attributes, never declared.
                "nodiscard", "maybe_unused", "deprecated", "fallthrough", "noreturn", "likely", "unlikely",
                "carries_dependency", "no_unique_address", "assume", "defined", "__has_include",
                "__has_cpp_attribute",
            };
            return keywords;
        }

        bool IsTrivia(TokenKind kind)
        {
            return kind == TokenKind::Whitespace || kind == TokenKind::LineComment ||
                kind == TokenKind::BlockComment;
        }

        bool IsAllCaps(std::string_view word)
        {
            if (word.size() < 2)
            {
                return false;
            }

            return std::all_of(word.begin(), word.end(),
                [](char c)
                {
                    return (c >= 'A' && c <= 'Z') ||(c >= '0' && c <= '9') || c == '_';
            });
        }

        std::size_t NextSignificant(const std::vector<Token> & tokens, std::size_t index)
        {
            while (index < tokens.size() && IsTrivia(tokens[index].kind))
            {
                ++index;
            }

            return index;
        }

        std::size_t PreviousSignificant(const std::vector<Token> & tokens, std::size_t index)
        {
            while (index > 0)
            {
                --index;
                if (!IsTrivia(tokens[index].kind))
                {
                    return index;
                }
            }

            return tokens.size();
        }

        // End (exclusive) of the preprocessor line that starts at `begin`,
        // honoring backslash continuations.
        std::size_t DirectiveEnd(std::string_view source, std::size_t begin)
        {
            std::size_t pos = begin;
            while (pos < source.size())
            {
                const std::size_t newline = source.find('\n', pos);
                if (newline == std::string_view::npos)
                {
                    return source.size();
                }

                std::size_t content_end = newline;
                if (content_end > pos && source[content_end - 1] == '\r')
                {
                    --content_end;
                }

                if (content_end > pos && source[content_end - 1] == '\\')
                {
                    pos = newline + 1;
                    continue;
                }

                return newline;
            }

            return source.size();
        }

        bool StartsLine(std::string_view source, std::size_t offset)
        {
            while (offset > 0)
            {
                const char c = source[offset - 1];
                if (c == '\n')
                {
                    return true;
                }

                if (c != ' ' && c != '\t' && c != '\r')
                {
                    return false;
                }

                --offset;
            }

            return true;
        }

        // Names a header file declares for its includers. Token-level and
        // approximate by design; every approximation errs towards listing
        // more names, which can only make the rule report less.
        //
        // Loose mode (project headers) lists every identifier of a
        // namespace-scope declaration, including the types it mentions.
        // Strict mode (system headers, whose layout is regular) lists only
        // names that are being declared, so that `uint32_t` appearing in the
        // declaration of something else does not tie unrelated headers together.
        HeaderSymbols ExtractSymbols(std::string_view source, bool strict)
        {
            HeaderSymbols symbols;
            symbols.readable = true;
            const auto &keywords = Keywords();
            const std::vector<Token> tokens = Lexer(source).Lex();
            auto text =[&](std::size_t i)
            {
                return source.substr(tokens[i].offset, tokens[i].length);
            };
            auto add =[&](std::string_view name)
            {
                if (!name.empty() && name.front() != '_' && name != "std" && !keywords.contains(name))
                {
                    symbols.names.emplace(name);
                }
            };

            // Scope stack: 'N' namespace/extern "C", 'R' record, 'E' enum, 'X' anything else
            // (function bodies, initializers). Empty means global scope.
            std::vector<char> scopes;
            std::vector<bool> paren_collects;
            char pending = 'X';
            bool saw_typedef = false;
            bool in_namespace_name = false;
            bool expect_enumerator = false;
            bool expect_record_name = false;
            std::size_t bracket_depth = 0;

            auto at_declaration_level =[&]
            {
                return scopes.empty() || scopes.back() == 'N';
            };

            for (std::size_t i = 0; i < tokens.size(); ++i)
            {
                const Token &token = tokens[i];
                if (IsTrivia(token.kind))
                {
                    continue;
                }

                if (token.kind == TokenKind::Punctuation && text(i) == "#" && StartsLine(source, token.offset))
                {
                    const std::size_t end = DirectiveEnd(source, token.offset);
                    const std::size_t word = NextSignificant(tokens, i + 1);
                    if (word < tokens.size() && tokens[word].offset < end &&
                        tokens[word].kind == TokenKind::Identifier && text(word) == "define")
                    {
                        const std::size_t name = NextSignificant(tokens, word + 1);
                        if (name < tokens.size() && tokens[name].offset < end &&
                            tokens[name].kind == TokenKind::Identifier)
                        {
                            add(text(name));
                        }
                    }

                    while (i + 1 < tokens.size() && tokens[i + 1].offset < end)
                    {
                        ++i;
                    }

                    continue;
                }

                if (token.kind == TokenKind::Punctuation)
                {
                    const std::string_view punctuation = text(i);
                    for (const char c: punctuation)
                    {
                        if (c == '[')
                        {
                            ++bracket_depth;
                        }
                        else if (c == ']' && bracket_depth > 0)
                        {
                            --bracket_depth;
                        }
                    }

                    if (punctuation == "{" || punctuation == ";" || punctuation == ":" || punctuation == "=" ||
                        punctuation == "(" || punctuation == ")" || punctuation == ",")
                    {
                        expect_record_name = false;
                    }

                    if (punctuation == "{")
                    {
                        if (at_declaration_level())
                        {
                            scopes.push_back(pending);
                            expect_enumerator = pending == 'E';
                            pending = 'X';
                        }
                        else
                        {
                            scopes.push_back('X');
                        }

                        in_namespace_name = false;
                    }
                    else if (punctuation == "}")
                    {
                        if (!scopes.empty())
                        {
                            const char closed = scopes.back();
                            scopes.pop_back();
                            if (closed == 'N' || closed == 'X')
                            {
                                saw_typedef = false;
                                pending = 'X';
                            }
                        }

                        expect_enumerator = false;
                    }
                    else if (!scopes.empty() && scopes.back() == 'E' && punctuation == ",")
                    {
                        expect_enumerator = true;
                    }
                    else if (at_declaration_level())
                    {
                        if (punctuation == "(")
                        {
                            const std::size_t previous = PreviousSignificant(tokens, i);
                            const bool macro_call = previous < tokens.size() &&
                                tokens[previous].kind == TokenKind::Identifier && IsAllCaps(text(previous));
                            paren_collects.push_back(saw_typedef || macro_call);
                        }
                        else if (punctuation == ")")
                        {
                            if (!paren_collects.empty())
                            {
                                paren_collects.pop_back();
                            }
                        }
                        else if (punctuation == ";")
                        {
                            saw_typedef = false;
                            in_namespace_name = false;
                            pending = 'X';
                            paren_collects.clear();
                        }
                        else if (punctuation == "=")
                        {
                            in_namespace_name = false;
                        }
                    }

                    continue;
                }

                if (token.kind != TokenKind::Identifier)
                {
                    continue;
                }

                const std::string_view word = text(i);
                if (!scopes.empty() && scopes.back() == 'E')
                {
                    if (expect_enumerator && bracket_depth == 0)
                    {
                        add(word);
                        expect_enumerator = false;
                    }

                    continue;
                }

                if (!at_declaration_level())
                {
                    continue;
                }

                if (word == "namespace")
                {
                    pending = 'N';
                    in_namespace_name = true;
                    continue;
                }

                if (word == "extern")
                {
                    const std::size_t next = NextSignificant(tokens, i + 1);
                    if (next < tokens.size() && tokens[next].kind == TokenKind::StringLiteral)
                    {
                        pending = 'N';
                    }

                    continue;
                }

                if (word == "enum")
                {
                    pending = 'E';
                    expect_record_name = true;
                    continue;
                }

                if ((word == "class" || word == "struct" || word == "union") && pending != 'E' && pending != 'N')
                {
                    pending = 'R';
                    expect_record_name = true;
                    continue;
                }

                if (word == "typedef")
                {
                    saw_typedef = true;
                    continue;
                }

                if (word == "operator")
                {
                    if (paren_collects.empty())
                    {
                        symbols.implicit_use = true;
                    }

                    continue;
                }

                if (word == "template")
                {
                    // Skip the parameter list: its names are not declarations.
                    const std::size_t open = NextSignificant(tokens, i + 1);
                    if (open < tokens.size() && tokens[open].kind == TokenKind::Punctuation &&
                        text(open).starts_with("<"))
                    {
                        std::size_t depth = 0;
                        std::size_t j = open;
                        for (; j < tokens.size(); ++j)
                        {
                            if (tokens[j].kind != TokenKind::Punctuation)
                            {
                                continue;
                            }

                            for (const char c: text(j))
                            {
                                if (c == '<')
                                {
                                    ++depth;
                                }
                                else if (c == '>' && depth > 0)
                                {
                                    --depth;
                                }
                            }

                            if (depth == 0)
                            {
                                break;
                            }
                        }

                        const std::size_t first = NextSignificant(tokens, open + 1);
                        if (first < tokens.size() && text(first) == ">")
                        {
                            symbols.implicit_use = true;
                        }

                        i = j;
                    }

                    continue;
                }

                if (bracket_depth > 0 || in_namespace_name)
                {
                    continue;
                }

                const std::size_t next = NextSignificant(tokens, i + 1);
                const std::string_view next_text = next < tokens.size() ? text(next) : std::string_view();
                if (next_text == "::")
                {
                    continue;
                }

                if (expect_record_name)
                {
                    // `class EXPORT_MACRO Name final : Base {` lists the macro too.
                    if (word != "final")
                    {
                        add(word);
                    }

                    continue;
                }

                const bool in_parens = !paren_collects.empty();
                const bool collecting = in_parens && paren_collects.back();
                if (!strict)
                {
                    if (!in_parens || collecting)
                    {
                        add(word);
                    }

                    continue;
                }

                if (collecting)
                {
                    add(word);
                }
                else if (in_parens)
                {
                    // Declarator nested in parentheses: `void (*signal(int, ...))(int);`
                    if (next_text == "(")
                    {
                        add(word);
                    }
                }
                else if (next_text == "(" || next_text == "=" || next_text == ";" || next_text == "{" ||
                    next_text == "[" || next_text == "," || next_text == ")" || next_text == ":" ||
                    next_text == "final" || next_text == "asm" || next_text == "alignas" ||
                    next_text.starts_with("__"))
                {
                    add(word);
                }
            }

            return symbols;
        }

        std::string ReadWholeFile(const std::filesystem::path & path, std::size_t max_bytes)
        {
            std::error_code ec;
            const auto size = std::filesystem::file_size(path, ec);
            if (ec || size > max_bytes)
            {
                return {};
            }

            std::ifstream in(path, std::ios::binary);
            if (!in)
            {
                return {};
            }

            std::string content(static_cast<std::size_t>(size), '\0');
            in.read(content.data(), static_cast<std::streamsize>(content.size()));
            content.resize(static_cast<std::size_t>(in.gcount()));
            return content;
        }

        std::int64_t MTimeOf(const std::filesystem::path & path)
        {
            std::error_code ec;
            const auto time = std::filesystem::last_write_time(path, ec);
            return ec ? 0 : static_cast<std::int64_t>(time.time_since_epoch().count());
        }

        std::uintmax_t SizeOf(const std::filesystem::path & path)
        {
            std::error_code ec;
            const auto size = std::filesystem::file_size(path, ec);
            return ec ? 0 : size;
        }

        // Process-wide: the same <vector> is extracted once across documents.
        std::shared_ptr<const HeaderSymbols> SymbolsFor(const std::filesystem::path & path, bool strict)
        {
            struct Cached
            {
                std::uintmax_t size = 0;
                std::int64_t mtime = 0;
                std::shared_ptr<const HeaderSymbols> symbols;
            };

            static std::mutex mutex;
            static std::unordered_map<std::string, Cached> cache;
            const std::string key = (strict ? "S|" : "L|") + path.string();
            const auto size = SizeOf(path);
            const auto mtime = MTimeOf(path);
            {
                const std::lock_guard<std::mutex> lock(mutex);
                if (const auto found = cache.find(key);
                    found != cache.end() && found->second.size == size && found->second.mtime == mtime)
                {
                    return found->second.symbols;
                }
            }

            constexpr std::size_t max_bytes = 4u << 20;
            std::shared_ptr<const HeaderSymbols> symbols;
            const std::string content = ReadWholeFile(path, max_bytes);
            if (content.empty() && size != 0)
            {
                symbols = std::make_shared<const HeaderSymbols>();
            }
            else
            {
                symbols = std::make_shared<const HeaderSymbols>(ExtractSymbols(content, strict));
            }

            const std::lock_guard<std::mutex> lock(mutex);
            if (cache.size() > 8192)
            {
                cache.clear();
            }

            cache[key] = {size, mtime, symbols};
            return symbols;
        }

        // `#ifndef X` / `#define X` ... `#endif` wrapping the whole file.
        bool HasIncludeGuard(const std::vector<PreprocessorDirective> & directives)
        {
            if (directives.size() < 3 || directives.front().kind != DirectiveKind::Ifndef ||
                directives[1].kind != DirectiveKind::Define || directives.back().kind != DirectiveKind::Endif)
            {
                return false;
            }

            std::size_t depth = 0;
            for (std::size_t i = 0; i < directives.size(); ++i)
            {
                switch (directives[i].kind)
                {
                case DirectiveKind::If:
                case DirectiveKind::Ifdef:
                case DirectiveKind::Ifndef:
                    ++depth;
                    break;
                case DirectiveKind::Endif:
                    depth = depth == 0 ? 0 : depth - 1;
                    if (depth == 0 && i + 1 != directives.size())
                    {
                        return false;
                    }

                    break;
                default:
                    break;
                }
            }

            return true;
        }

        struct IncludeDirective
        {
            std::size_t directive_offset = 0;
            std::size_t directive_length = 0;
            std::size_t target_offset = 0;
            std::size_t target_length = 0;
            std::string target;
            bool conditional = false;
            bool keep = false;
        };

        // Direct `#include "x"` / `#include <x>` directives, in source order.
        // Macro includes and `#include_next` have no literal target to judge.
        std::vector<IncludeDirective> DirectIncludes(const ParseTree &tree)
        {
            const std::string_view source = tree.Source();
            const auto &directives = tree.Directives();

            const std::size_t base_depth = HasIncludeGuard(directives) ? 1 : 0;

            std::vector<IncludeDirective> result;
            std::size_t depth = 0;
            for (const auto & directive: directives)
            {
                switch (directive.kind)
                {
                case DirectiveKind::If:
                case DirectiveKind::Ifdef:
                case DirectiveKind::Ifndef:
                    ++depth;
                    continue;
                case DirectiveKind::Endif:
                    depth = depth == 0 ? 0 : depth - 1;
                    continue;
                case DirectiveKind::Include:
                    break;
                default:
                    continue;
                }

                const std::string_view body = source.substr(directive.offset, directive.length);
                std::size_t pos = body.find('#');
                if (pos == std::string_view::npos)
                {
                    continue;
                }

                ++pos;
                while (pos < body.size() && (body[pos] == ' ' || body[pos] == '\t'))
                {
                    ++pos;
                }

                constexpr std::string_view keyword = "include";
                if (body.substr(pos, keyword.size()) != keyword)
                {
                    continue;
                }

                pos += keyword.size();
                if (pos < body.size() && (std::isalnum(static_cast<unsigned char>(body[pos])) || body[pos] == '_'))
                {
                    continue; // include_next and friends
                }

                const std::size_t open = body.find_first_of("<\"", pos);
                if (open == std::string_view::npos)
                {
                    continue;
                }

                // Only whitespace may sit between the keyword and the delimiter.
                if (body.substr(pos, open - pos).find_first_not_of(" \t") != std::string_view::npos)
                {
                    continue;
                }

                const char close_char = body[open] == '<' ? '>' : '"';
                const std::size_t close = body.find(close_char, open + 1);
                if (close == std::string_view::npos)
                {
                    continue;
                }

                IncludeDirective include;
                include.directive_offset = directive.offset;
                include.directive_length = directive.length;
                include.target_offset = directive.offset + open;
                include.target_length = close - open + 1;
                include.target = std::string(body.substr(open, close - open + 1));
                include.conditional = depth > base_depth;
                const std::string_view trailing = body.substr(close + 1);
                include.keep = trailing.find("IWYU pragma: keep") != std::string_view::npos ||
                    trailing.find("IWYU pragma: export") != std::string_view::npos;
                result.push_back(std::move(include));
            }

            return result;
        }

        std::string Lowercase(std::string text)
        {
            std::transform(text.begin(), text.end(), text.begin(),
                [](unsigned char c)
                {
                    return static_cast<char>(std::tolower(c));
            });
            return text;
        }

        // Headers whose job is not to be named: they are used implicitly by
        // language features (braced lists, <=>, typeid, new, coroutines,
        // structured bindings) or textually included.
        bool IsImplicitUseHeader(std::string_view target, const std::filesystem::path & header)
        {
            static const std::unordered_set<std::string_view> implicit = {
                "<initializer_list>", "<compare>", "<typeinfo>", "<new>", "<coroutine>", "<tuple>",
                "<version>", "<exception>", "<cstdarg>", "<stdarg.h>", "<csetjmp>", "<setjmp.h>",
            };
            if (implicit.contains(target))
            {
                return true;
            }

            const std::string extension = Lowercase(header.extension().string());
            return extension == ".inc" || extension == ".def" || extension == ".inl" || extension == ".tpp" ||
                extension == ".ipp" || extension == ".tcc";
        }

        // Configuration headers every standard header drags in: their names say
        // nothing about which include is needed.
        bool IsAmbientConfigHeader(const std::filesystem::path & file)
        {
            const std::string name = file.filename().string();
            return name == "c++config.h" || name == "os_defines.h" || name == "cpu_defines.h";
        }

        bool IsSystemFile(const std::filesystem::path & file, const std::vector<std::filesystem::path> & system_dirs)
        {
            const auto normalized = file.lexically_normal();
            return std::any_of(system_dirs.begin(), system_dirs.end(), [&](const std::filesystem::path &dir)
                {
                    const auto relative = normalized.lexically_relative(dir);
                    return !relative.empty() && *relative.begin() != "..";
            });
        }

        std::string PathKey(const std::filesystem::path & path)
        {
            return path.lexically_normal().generic_string();
        }

    } // namespace

    std::string IncludeAnalyzer::Fingerprint(const std::filesystem::path & file, const ParseTree &tree,
        const CompileCommand *command)
    {
        const std::filesystem::path base_dir = file.has_parent_path() ? file.parent_path() : std::filesystem::path();
        return IncludeIndex::IncludeFingerprint(base_dir, tree.Source(), command);
    }

    std::shared_ptr<const IncludeProfile> IncludeAnalyzer::BuildProfile(const std::filesystem::path & file,
        const ParseTree &tree, const CompileCommand *command)
    {
        auto profile = std::make_shared<IncludeProfile>();
        profile->fingerprint = Fingerprint(file, tree, command);
        const std::filesystem::path base_dir = file.has_parent_path() ? file.parent_path() : std::filesystem::path();
        const auto includes = DirectIncludes(tree);
        profile->entries.resize(includes.size());

        // Standard-library operators and specializations always belong to a type
        // the file has to name anyway; only project headers can be "used" through
        // an operator alone.
        std::vector<std::filesystem::path> system_dirs;
        for (const auto & dir: IncludeIndex::SystemIncludes(
            command != nullptr && !command->arguments.empty() ? std::string_view(command->arguments.front())
            : std::string_view()))
        {
            system_dirs.push_back(dir.lexically_normal());
        }


        IncludeIndex::Limits limits;
        limits.max_headers = 1024;
        limits.follow_include_next = true;

        struct Closure
        {
            std::vector<std::filesystem::path> files;
            std::unordered_set<std::string> keys;
            bool complete = false;
        };

        std::vector<Closure> closures(includes.size());
        for (std::size_t i = 0; i < includes.size(); ++i)
        {
            profile->entries[i].target = includes[i].target;
            if (includes[i].conditional)
            {
                continue;
            }

            ResolveReport report;
            closures[i].files = IncludeIndex::ResolveHeaders(base_dir, "#include " + includes[i].target + "\n",
                command, limits, &report);
            closures[i].complete = report.complete && !closures[i].files.empty();
            for (const auto & path: closures[i].files)
            {
                closures[i].keys.insert(PathKey(path));
            }
        }

        const std::string primary_stem = Lowercase(file.stem().string());
        for (std::size_t i = 0; i < includes.size(); ++i)
        {
            auto &entry = profile->entries[i];
            const Closure &closure = closures[i];
            if (!closure.complete)
            {
                continue;
            }

            const std::filesystem::path & header = closure.files.front();
            const std::string header_key = PathKey(header);
            if (Lowercase(header.stem().string()) == primary_stem || IsImplicitUseHeader(entry.target, header))
            {
                continue;
            }

            // Names reachable through another direct include belong to that
            // include: <iostream> must not count as used because of <string>.
            std::unordered_set<std::string> covered;
            for (std::size_t j = 0; j < includes.size(); ++j)
            {
                if (j == i || closures[j].files.empty())
                {
                    continue;
                }

                const std::string other_key = PathKey(closures[j].files.front());
                if (other_key == header_key ||!closure.keys.contains(other_key) ||
                    closures[j].keys.contains(header_key))
                {
                    continue;
                }

                covered.insert(closures[j].keys.begin(), closures[j].keys.end());
            }

            bool usable = true;
            for (const auto & path: closure.files)
            {
                const std::string key = PathKey(path);
                const bool direct = key == header_key;
                if (!direct && (covered.contains(key) || IsAmbientConfigHeader(path)))
                {
                    continue;
                }

                auto symbols = SymbolsFor(path, IsSystemFile(path, system_dirs));
                if (!symbols->readable || (direct && symbols->implicit_use && !IsSystemFile(header, system_dirs)))
                {
                    usable = false;
                    break;
                }

                entry.providers.push_back(std::move(symbols));
            }

            if (!usable)
            {
                entry.providers.clear();
                continue;
            }

            entry.eligible = true;
        }

        std::unordered_set<std::string> stamped;
        for (const auto & closure: closures)
        {
            for (const auto & path: closure.files)
            {
                if (stamped.insert(PathKey(path)).second)
                {
                    profile->stamps.push_back({path, SizeOf(path), MTimeOf(path)});
                }
            }
        }

        return profile;
    }

    bool IncludeAnalyzer::IsFresh(const IncludeProfile &profile)
    {
        return std::all_of(profile.stamps.begin(), profile.stamps.end(),
            [](const IncludeProfile::FileStamp & stamp)
            {
                return SizeOf(stamp.path) == stamp.size && MTimeOf(stamp.path) == stamp.mtime;
        });
    }

    std::vector<Diagnostic> IncludeAnalyzer::Analyze(const ParseTree &tree,
        const IncludeProfile &profile)
    {
        std::vector<Diagnostic> diagnostics;
        const auto includes = DirectIncludes(tree);
        if (includes.size() != profile.entries.size())
        {
            return diagnostics;
        }

        const std::string_view source = tree.Source();
        const auto &tokens = tree.Tokens();
        const auto &directives = tree.Directives();
        const auto &keywords = Keywords();

        // Every identifier of the file outside #include lines. Comments and
        // literals are separate token kinds, so they never count.
        std::unordered_set<std::string_view> used;
        const bool guarded = HasIncludeGuard(directives);
        std::size_t cursor = 0;
        bool after_member_access = false;
        for (const auto & token: tokens)
        {
            if (token.kind == TokenKind::Whitespace || token.kind == TokenKind::LineComment ||
                token.kind == TokenKind::BlockComment)
            {
                continue;
            }

            // `object.size()` names a member, which no header declares at
            // namespace scope; counting it would tie every container header
            // to std::size.
            const bool member_name = after_member_access;
            after_member_access = false;
            if (token.kind == TokenKind::Punctuation)
            {
                const std::string_view punctuation = source.substr(token.offset, token.length);
                after_member_access = punctuation == "." || punctuation == "->";
                continue;
            }

            if (token.kind != TokenKind::Identifier || member_name)
            {
                continue;
            }

            while (cursor < directives.size() && directives[cursor].offset + directives[cursor].length <= token.offset)
            {
                ++cursor;
            }

            if (cursor < directives.size() && directives[cursor].offset <= token.offset)
            {
                const bool guard_line = guarded && cursor < 2;
                if (directives[cursor].kind == DirectiveKind::Include ||
                    directives[cursor].kind == DirectiveKind::Pragma || guard_line)
                {
                    continue;
                }
            }

            const std::string_view word = source.substr(token.offset, token.length);
            if (!keywords.contains(word))
            {
                used.insert(word);
            }
        }

        // A file with nothing but includes (an umbrella header) re-exports them.
        if (used.empty())
        {
            return diagnostics;
        }

        LineTable lines;
        lines.Build(source);
        for (std::size_t i = 0; i < includes.size(); ++i)
        {
            const auto &include = includes[i];
            const auto &entry = profile.entries[i];
            if (!entry.eligible || include.conditional || include.keep || entry.target != include.target)
            {
                continue;
            }

            bool is_used = false;
            for (const auto & provider: entry.providers)
            {
                for (const std::string_view word: used)
                {
                    if (provider->names.contains(word))
                    {
                        is_used = true;
                        break;
                    }
                }

                if (is_used)
                {
                    break;
                }
            }

            if (is_used)
            {
                continue;
            }

            Diagnostic diagnostic{RuleId::UnusedInclude, Severity::Warning, "cpp/no-unused-include",
                "included header " + include.target + " is not used directly",
                include.target_offset, include.target_length, 0, 0, true,
                RemoveDirectiveLine(source, include.directive_offset, include.directive_length)};
            const auto position = lines.Lookup(include.target_offset);
            diagnostic.line = position.line;
            diagnostic.column = position.column;
            diagnostic.fix_is_safe = false;
            diagnostic.fix_title = "Remove unused include " + include.target;
            diagnostics.push_back(std::move(diagnostic));
        }

        return diagnostics;
    }

} // namespace heimdall
