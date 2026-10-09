#include "Support.hpp"
#include <Heimdall/SourceOverlay.hpp>

#include <Heimdall/ParseTree.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <thread>

#ifdef _WIN32
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#else
    #include <csignal>
    #include <fcntl.h>
    #include <sys/wait.h>
    #include <unistd.h>
#endif

namespace heimdall::refactor_detail
{
    namespace
    {
        RefactoringError Failed(std::string message)
        {
            return { RefactoringErrorCode::MacroContext, std::move(message) };
        }

        struct Scratch
        {
            std::filesystem::path directory;

            ~Scratch()
            {
                if (directory.empty())
                {
                    return;
                }

                std::error_code error;
                for (const auto name : { "source.cpp", "source.ii", "errors.txt" })
                {
                    std::filesystem::remove(directory / name, error);
                }

                std::filesystem::remove(directory, error);
            }
        };

        bool Trivia(const Token& token)
        {
            return token.kind == TokenKind::Whitespace || token.kind == TokenKind::LineComment ||
                   token.kind == TokenKind::BlockComment;
        }

#ifdef _WIN32
        std::wstring Wide(std::string_view text)
        {
            const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                                  static_cast<int>(text.size()), nullptr, 0);
            if (!size)
                return {};
            std::wstring result(static_cast<std::size_t>(size), L'\0');
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                static_cast<int>(text.size()), result.data(), size);
            return result;
        }

        std::wstring Quote(std::wstring_view argument)
        {
            std::wstring result  = L"\"";
            std::size_t  slashes = 0;
            for (auto character : argument)
            {
                if (character == L'\\')
                {
                    ++slashes;
                    continue;
                }

                result.append(character == L'"' ? slashes * 2 + 1 : slashes, L'\\');
                result += character;
                slashes = 0;
            }

            result.append(slashes * 2, L'\\');
            result += L'"';
            return result;
        }
#endif

        std::expected<void, RefactoringError> Run(const std::vector<std::string>& arguments,
                                                  const std::filesystem::path&    cwd,
                                                  const std::filesystem::path&    errors,
                                                  std::stop_token                 stop)
        {
            if (stop.stop_requested())
            {
                return std::unexpected(
                    RefactoringError { RefactoringErrorCode::Cancelled, "Request cancelled" });
            }

            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
#ifdef _WIN32
            std::wstring command;
            for (const auto& argument : arguments)
            {
                const auto wide = Wide(argument);
                if (!argument.empty() && wide.empty())
                {
                    return std::unexpected(Failed("Invalid compiler argument encoding"));
                }

                if (!command.empty())
                {
                    command += L' ';
                }

                command += Quote(wide);
            }

            SECURITY_ATTRIBUTES security { sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
            const auto          output =
                CreateFileW(errors.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (output == INVALID_HANDLE_VALUE)
            {
                return std::unexpected(Failed("Cannot create compiler diagnostics file"));
            }

            STARTUPINFOW startup {};
            startup.cb      = sizeof(startup);
            startup.dwFlags = STARTF_USESTDHANDLES;
            const auto input =
                CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            startup.hStdInput  = input;
            startup.hStdOutput = output;
            startup.hStdError  = output;
            PROCESS_INFORMATION process {};
            const bool          started =
                CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                               nullptr, cwd.empty() ? nullptr : cwd.c_str(), &startup, &process);
            CloseHandle(output);
            if (input != INVALID_HANDLE_VALUE)
            {
                CloseHandle(input);
            }

            if (!started)
            {
                return std::unexpected(
                    Failed("Cannot start the compiler from compile_commands.json"));
            }

            CloseHandle(process.hThread);
            bool cancelled = false;
            bool timed_out = false;
            while (WaitForSingleObject(process.hProcess, 10) == WAIT_TIMEOUT)
            {
                cancelled = stop.stop_requested();
                timed_out = std::chrono::steady_clock::now() >= deadline;
                if (cancelled || timed_out)
                {
                    TerminateProcess(process.hProcess, 1);
                    WaitForSingleObject(process.hProcess, INFINITE);
                    break;
                }
            }

            DWORD status = 1;
            GetExitCodeProcess(process.hProcess, &status);
            CloseHandle(process.hProcess);
#else
            std::vector<char*> argv;
            for (const auto& argument : arguments)
            {
                argv.push_back(const_cast<char*>(argument.c_str()));
            }

            argv.push_back(nullptr);
            const auto working    = cwd.string();
            const auto error_path = errors.string();
            const auto process    = fork();
            if (process < 0)
            {
                return std::unexpected(Failed("Cannot start the compiler"));
            }

            if (process == 0)
            {
                if (!working.empty() && chdir(working.c_str()) != 0)
                {
                    _exit(127);
                }

                const auto output = open(error_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
                if (output < 0)
                {
                    _exit(127);
                }

                dup2(output, STDOUT_FILENO);
                dup2(output, STDERR_FILENO);
                close(output);
                execvp(argv[0], argv.data());
                _exit(127);
            }

            bool cancelled    = false;
            bool timed_out    = false;
            int  child_status = 0;
            while (waitpid(process, &child_status, WNOHANG) == 0)
            {
                cancelled = stop.stop_requested();
                timed_out = std::chrono::steady_clock::now() >= deadline;
                if (cancelled || timed_out)
                {
                    kill(process, SIGKILL);
                    waitpid(process, &child_status, 0);
                    break;
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }

            const auto status = WIFEXITED(child_status) ? WEXITSTATUS(child_status) : 1;
#endif
            if (cancelled)
            {
                return std::unexpected(
                    RefactoringError { RefactoringErrorCode::Cancelled, "Request cancelled" });
            }

            if (timed_out)
            {
                return std::unexpected(Failed("Compiler preprocessing timed out"));
            }

            if (status != 0)
            {
                return std::unexpected(Failed("Compiler could not preprocess this document; check "
                                              "compile_commands.json and include paths"));
            }

            return {};
        }

        bool SamePath(const std::filesystem::path& a, const std::filesystem::path& b)
        {
            SourceOverlay overlay;
            overlay.Add(a, std::make_shared<const std::string>());
            return overlay.Find(b) != nullptr;
        }
    } // namespace

    std::expected<void, RefactoringError> VerifyPreprocessing(
        const AnalysisSnapshot& snapshot,
        DocumentId              document,
        std::string_view        source,
        std::stop_token         stop)
    {
        const auto* command = snapshot.Command(document);
        if (!command || command->arguments.empty())
        {
            return std::unexpected(
                Failed("Rename in files with includes requires compile_commands.json"));
        }

        if (std::filesystem::path(command->arguments.front()).stem() == "cl")
        {
            return std::unexpected(Failed("Macro verification currently requires GCC or Clang"));
        }

        // Disk preprocessing must never certify a different version of an open
        // header. Check every header named by compiler line markers below.
        Scratch         scratch;
        std::error_code error;
        const auto      base = std::filesystem::temp_directory_path(error);
        if (error)
        {
            return std::unexpected(
                Failed("Cannot access a temporary directory for macro verification"));
        }

        static std::atomic<std::uint64_t> sequence { 0 };
        for (unsigned attempt = 0; attempt < 10; ++attempt)
        {
            auto candidate =
                base /
                ("heimdall-rename-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                 std::to_string(sequence.fetch_add(1)));
            if (std::filesystem::create_directory(candidate, error))
            {
                scratch.directory = std::move(candidate);
                break;
            }

            if (error)
            {
                return std::unexpected(
                    Failed("Cannot create a temporary directory for macro verification"));
            }
        }

        if (scratch.directory.empty())
        {
            return std::unexpected(Failed("Cannot allocate a scratch directory"));
        }

        const auto input  = scratch.directory / "source.cpp";
        const auto output = scratch.directory / "source.ii";
        {
            std::ofstream file(input, std::ios::binary);
            file.write(source.data(), static_cast<std::streamsize>(source.size()));
            if (!file)
            {
                return std::unexpected(Failed("Cannot write compiler input"));
            }
        }
        std::vector<std::string> arguments {
            command->arguments.front(), "-iquote",
            snapshot.Path(document).parent_path().generic_string()
        };
        for (std::size_t i = 1; i < command->arguments.size(); ++i)
        {
            const auto& argument = command->arguments[i];
            if (argument == "-o" || argument == "-MF" || argument == "-MT" || argument == "-MQ")
            {
                if (i + 1 < command->arguments.size())
                {
                    ++i;
                }

                continue;
            }

            if (argument == "-c" || argument == "-S" || argument == "-E" || argument == "-M" ||
                argument == "-MM" || argument == "-MD" || argument == "-MMD" || argument == "-MP" ||
                argument == "-MG" || argument.starts_with("-MF") || argument.starts_with("-MT") ||
                argument.starts_with("-MQ") || argument.starts_with("-o") ||
                argument.starts_with("-std=") || argument.starts_with("-fdeps-"))
            {
                continue;
            }

            const bool takes_value =
                argument == "-I" || argument == "-D" || argument == "-U" ||
                argument == "-include" || argument == "-imacros" || argument == "-isystem" ||
                argument == "-iquote" || argument == "-idirafter" || argument == "-isysroot" ||
                argument == "--sysroot" || argument == "-x";
            if (takes_value)
            {
                arguments.push_back(argument);
                if (++i >= command->arguments.size())
                {
                    return std::unexpected(Failed("Incomplete compiler option"));
                }

                arguments.push_back(command->arguments[i]);
                continue;
            }

            if (!argument.starts_with('-'))
            {
                const auto path = std::filesystem::path(argument);
                if (SamePath(path.is_absolute() ? path : command->directory / path, command->file))
                {
                    continue;
                }

                return std::unexpected(Failed("Unsupported multi-input compiler command"));
            }

            arguments.push_back(argument);
        }

        const auto& options = snapshot.Options(document);
        arguments.push_back(options.standard == CppStandard::Cpp26   ? "-std=c++26"
                            : options.standard == CppStandard::Cpp23 ? "-std=c++23"
                                                                     : "-std=c++20");
        arguments.insert(arguments.end(), { "-E", "-x", "c++", input.generic_string(), "-o",
                                            output.generic_string() });
        const auto ran = Run(arguments, command->directory, scratch.directory / "errors.txt", stop);
        if (!ran)
        {
            return ran;
        }

        if (std::filesystem::file_size(output, error) > (64u << 20) || error)
        {
            return std::unexpected(
                Failed("Preprocessed output is unavailable or exceeds the verification limit"));
        }

        std::ifstream file(output, std::ios::binary);
        std::string   line, main_source;
        bool          in_main = false;
        while (std::getline(file, line))
        {
            if (stop.stop_requested())
            {
                return std::unexpected(
                    RefactoringError { RefactoringErrorCode::Cancelled, "Request cancelled" });
            }

            if (line.starts_with("# "))
            {
                std::istringstream marker(line.substr(2));
                unsigned           number = 0;
                std::string        name;
                marker >> number >> std::quoted(name);
                if (!marker)
                {
                    return std::unexpected(Failed("Unsupported compiler line marker"));
                }

                auto path = std::filesystem::path(name);
                if (!name.starts_with('<') && path.is_relative())
                {
                    path = command->directory / path;
                }

                in_main = SamePath(path, input);
                if (!in_main && !name.starts_with('<'))
                {
                    const auto id = snapshot.Find(path);
                    if (id != InvalidDocument && id != document)
                    {
                        std::ifstream     disk(path, std::ios::binary);
                        const std::string text { std::istreambuf_iterator<char>(disk),
                                                 std::istreambuf_iterator<char>() };
                        const auto        buffer = snapshot.Source(id);
                        if (!disk || !buffer || text != *buffer)
                        {
                            return std::unexpected(Failed(
                                "An included header has unsaved changes; save it before renaming"));
                        }
                    }
                }

                continue;
            }

            if (in_main)
            {
                main_source += line;
                main_source += '\n';
            }
        }

        const auto raw_tree = ParseTree::Parse(source, options, stop);
        if (raw_tree.Cancelled())
        {
            return std::unexpected(
                RefactoringError { RefactoringErrorCode::Cancelled, "Request cancelled" });
        }

        std::vector<std::string_view> original, expanded;
        for (const auto& token : raw_tree.Tokens())
        {
            if (Trivia(token))
            {
                continue;
            }

            const bool directive =
                std::ranges::any_of(raw_tree.Directives(), [&](const auto& range) {
                    return token.offset >= range.offset &&
                           token.offset - range.offset < range.length;
                });
            if (!directive)
            {
                original.push_back(source.substr(token.offset, token.length));
            }
        }

        for (const auto& token : Lexer(main_source).Lex())
        {
            if (!Trivia(token))
            {
                expanded.push_back(
                    std::string_view(main_source).substr(token.offset, token.length));
            }
        }

        if (original != expanded)
        {
            return std::unexpected(Failed("Macros or conditional compilation change the document's "
                                          "written tokens; rename is blocked"));
        }

        return {};
    }
} // namespace heimdall::refactor_detail
