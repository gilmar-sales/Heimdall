// End-to-end LSP latency benchmark (architecture review, section 7, items 1 and 2).
//
// Spawns the real heimdall-lsp binary and types into a stb-based document one
// character at a time: each iteration sends `didChange` (1 inserted character) and
// immediately `completion` at the cursor, and records the time until the completion
// response arrives. That is exactly the user-visible keystroke -> popup latency, so
// it includes JSON parsing, the debounced diagnostics worker competing for the same
// parse, the thread pool and the response serialization.
//
// It also samples the server's working set: the delta between the idle server and
// the server holding the document is the per-document memory cost, and the peak is
// the worst case reached while parsing/typing.
//
//   LspLatencyBench [--server <heimdall-lsp>] [--stb-dir <dir>] [--lines 1000,5000,20000]
//                   [--iterations N] [--warmup N] [--budget-ms 50] [--enforce]
//
// Exit code: 0, or 1 with --enforce when p95 of any size exceeds the budget.
#include "StbCorpus.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#else
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace
{

    using Clock = std::chrono::steady_clock;

    struct MemorySample
    {
        double working_set_mb = 0;
        double peak_working_set_mb = 0;
    };

    // Child process with piped stdin/stdout (stderr discarded).
    class Child
    {
    public:
        bool Start(const std::string& executable);

        bool Write(std::string_view bytes);

        std::size_t Read(char* buffer, std::size_t size); // 0 on EOF

        MemorySample Memory() const;

        void CloseStdin();

        int Wait();

        ~Child();

    private:
#if defined(_WIN32)
        HANDLE m_process = nullptr;
        HANDLE m_stdin = nullptr;
        HANDLE m_stdout = nullptr;
#else
        pid_t m_pid = -1;
        int m_stdin = -1;
        int m_stdout = -1;
#endif
    };

#if defined(_WIN32)

    bool Child::Start(const std::string& executable)
    {
        SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        HANDLE child_stdin_read = nullptr;
        HANDLE child_stdout_write = nullptr;
        if (!CreatePipe(&child_stdin_read, &m_stdin, &inheritable, 1 << 20))
        {
            return false;
        }

        if (!CreatePipe(&m_stdout, &child_stdout_write, &inheritable, 1 << 20))
        {
            return false;
        }

        SetHandleInformation(m_stdin, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(m_stdout, HANDLE_FLAG_INHERIT, 0);

        HANDLE null_device = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &inheritable,
            OPEN_EXISTING, 0, nullptr);
        STARTUPINFOA startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = child_stdin_read;
        startup.hStdOutput = child_stdout_write;
        startup.hStdError = null_device;

        PROCESS_INFORMATION info{};
        std::string command = "\"" + executable + "\"";
        const BOOL started = CreateProcessA(nullptr, command.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW, nullptr,
            nullptr, &startup, &info);
        CloseHandle(child_stdin_read);
        CloseHandle(child_stdout_write);
        CloseHandle(null_device);
        if (!started)
        {
            return false;
        }

        CloseHandle(info.hThread);
        m_process = info.hProcess;
        return true;
    }

    bool Child::Write(std::string_view bytes)
    {
        while (!bytes.empty())
        {
            DWORD written = 0;
            if (!WriteFile(m_stdin, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr))
            {
                return false;
            }

            bytes.remove_prefix(written);
        }

        return true;
    }

    std::size_t Child::Read(char* buffer, std::size_t size)
    {
        DWORD read = 0;
        if (!ReadFile(m_stdout, buffer, static_cast<DWORD>(size), &read, nullptr))
        {
            return 0;
        }

        return read;
    }

    MemorySample Child::Memory() const
    {
        PROCESS_MEMORY_COUNTERS counters{};
        counters.cb = sizeof(counters);
        MemorySample sample;
        if (GetProcessMemoryInfo(m_process, &counters, sizeof(counters)))
        {
            sample.working_set_mb = static_cast<double>(counters.WorkingSetSize) /(1024.0 * 1024.0);
            sample.peak_working_set_mb = static_cast<double>(counters.PeakWorkingSetSize) /(1024.0 * 1024.0);
        }

        return sample;
    }

    void Child::CloseStdin()
    {
        if (m_stdin)
        {
            CloseHandle(m_stdin);
        }

        m_stdin = nullptr;
    }

    int Child::Wait()
    {
        CloseStdin();
        if (!m_process)
        {
            return -1;
        }

        if (WaitForSingleObject(m_process, 10000) == WAIT_TIMEOUT)
        {
            TerminateProcess(m_process, 1);
        }

        DWORD code = 1;
        GetExitCodeProcess(m_process, &code);
        CloseHandle(m_process);
        m_process = nullptr;
        return static_cast<int>(code);
    }

    Child::~Child()
    {
        if (m_process)
        {
            TerminateProcess(m_process, 1);
        }

        if (m_process)
        {
            CloseHandle(m_process);
        }

        if (m_stdin)
        {
            CloseHandle(m_stdin);
        }

        if (m_stdout)
        {
            CloseHandle(m_stdout);
        }
    }

#else

    bool Child::Start(const std::string& executable)
    {
        int to_child[2];
        int from_child[2];
        if (pipe(to_child) != 0 || pipe(from_child) != 0)
        {
            return false;
        }

        m_pid = fork();
        if (m_pid < 0)
        {
            return false;
        }

        if (m_pid == 0)
        {
            dup2(to_child[0], STDIN_FILENO);
            dup2(from_child[1], STDOUT_FILENO);
            freopen("/dev/null", "w", stderr);
            close(to_child[0]);
            close(to_child[1]);
            close(from_child[0]);
            close(from_child[1]);
            execl(executable.c_str(), executable.c_str(), static_cast<char*>(nullptr));
            _exit(127);
        }

        close(to_child[0]);
        close(from_child[1]);
        m_stdin = to_child[1];
        m_stdout = from_child[0];
        signal(SIGPIPE, SIG_IGN);
        return true;
    }

    bool Child::Write(std::string_view bytes)
    {
        while (!bytes.empty())
        {
            const ssize_t written = write(m_stdin, bytes.data(), bytes.size());
            if (written <= 0)
            {
                return false;
            }

            bytes.remove_prefix(static_cast<std::size_t>(written));
        }

        return true;
    }

    std::size_t Child::Read(char* buffer, std::size_t size)
    {
        const ssize_t read_bytes = read(m_stdout, buffer, size);
        return read_bytes > 0 ? static_cast<std::size_t>(read_bytes) : 0;
    }

    MemorySample Child::Memory() const
    {
        // Linux only (/proc); other POSIX systems report zeros.
        MemorySample sample;
        std::FILE* status = std::fopen(("/proc/" + std::to_string(m_pid) + "/status").c_str(), "r");
        if (!status)
        {
            return sample;
        }

        char line[256];
        while (std::fgets(line, sizeof(line), status))
        {
            double kb = 0;
            if (std::sscanf(line, "VmRSS: %lf kB", &kb) == 1)
            {
                sample.working_set_mb = kb / 1024.0;
            }

            if (std::sscanf(line, "VmHWM: %lf kB", &kb) == 1)
            {
                sample.peak_working_set_mb = kb / 1024.0;
            }
        }

        std::fclose(status);
        return sample;
    }

    void Child::CloseStdin()
    {
        if (m_stdin >= 0)
        {
            close(m_stdin);
        }

        m_stdin = -1;
    }

    int Child::Wait()
    {
        CloseStdin();
        if (m_pid <= 0)
        {
            return -1;
        }

        int status = 0;
        waitpid(m_pid, &status, 0);
        m_pid = -1;
        return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    }

    Child::~Child()
    {
        if (m_pid > 0)
        {
            kill(m_pid, SIGKILL);
            waitpid(m_pid, nullptr, 0);
        }

        if (m_stdin >= 0)
        {
            close(m_stdin);
        }

        if (m_stdout >= 0)
        {
            close(m_stdout);
        }
    }

#endif

    // Parses `Content-Length` frames off the child's stdout and records the arrival
    // time of every response (messages carrying an `"id"` and a `"result"`).
    class ResponseReader
    {
    public:
        explicit ResponseReader(Child& child) : m_thread([this, &child]
            {
                Loop(child);
            }) {}

        ~ResponseReader()
        {
            if (m_thread.joinable())
            {
                m_thread.join();
            }
        }

        // Blocks until response `id` arrives; returns its arrival time.
        bool Await(std::int64_t id, std::chrono::milliseconds timeout, Clock::time_point& arrived)
        {
            std::unique_lock lock(m_mu);
            const bool ok = m_cv.wait_for(lock, timeout,[&]
                {
                    return m_closed || m_arrivals.contains(id);
            });
            if (!ok ||!m_arrivals.contains(id))
            {
                return false;
            }

            arrived = m_arrivals[id];
            m_arrivals.erase(id);
            return true;
        }

    private:
        void Loop(Child& child)
        {
            std::string pending;
            char buffer[1 << 16];
            for (;;)
            {
                const std::size_t got = child.Read(buffer, sizeof(buffer));
                if (got == 0)
                {
                    break;
                }

                pending.append(buffer, got);
                const auto now = Clock::now();
                for (;;)
                {
                    const std::size_t header_end = pending.find("\r\n\r\n");
                    if (header_end == std::string::npos)
                    {
                        break;
                    }

                    const std::size_t length_at = pending.find("Content-Length:");
                    if (length_at == std::string::npos || length_at > header_end)
                    {
                        break;
                    }

                    const std::size_t length = std::strtoull(pending.c_str() + length_at + 15, nullptr, 10);
                    if (pending.size() < header_end + 4 + length)
                    {
                        break;
                    }

                    Record(std::string_view(pending).substr(header_end + 4, length), now);
                    pending.erase(0, header_end + 4 + length);
                }
            }

            {
                const std::lock_guard lock(m_mu);
                m_closed = true;
            }
            m_cv.notify_all();
        }

        void Record(std::string_view body, Clock::time_point now)
        {
            // Responses are `{"jsonrpc":"2.0","id":N,"result":...`; notifications
            // (publishDiagnostics) have no id.
            constexpr std::string_view prefix = "{\"jsonrpc\":\"2.0\",\"id\":";
            if (!body.starts_with(prefix))
            {
                return;
            }

            std::int64_t id = 0;
            std::from_chars(body.data() + prefix.size(), body.data() + body.size(), id);
            {
                const std::lock_guard lock(m_mu);
                m_arrivals[id] = now;
            }
            m_cv.notify_all();
        }

        std::mutex m_mu;
        std::condition_variable m_cv;
        std::unordered_map<std::int64_t, Clock::time_point> m_arrivals;
        bool m_closed = false;
        std::thread m_thread;
    };

    std::string JsonEscape(std::string_view text)
    {
        std::string out;
        out.reserve(text.size() + text.size() / 16);
        for (const unsigned char c : text)
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
                if (c < 0x20)
                {
                    char escaped[8];
                    std::snprintf(escaped, sizeof(escaped), "\\u%04x", c);
                    out += escaped;
                }
                else
                {
                    out += static_cast<char>(c);
                }
            }
        }

        return out;
    }

    std::string Frame(const std::string& body)
    {
        return "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    }

    double Percentile(std::vector<double> sorted, double fraction)
    {
        if (sorted.empty())
        {
            return 0;
        }

        const double rank = fraction * static_cast<double>(sorted.size() - 1);
        const auto low = static_cast<std::size_t>(rank);
        const std::size_t high = std::min(low + 1, sorted.size() - 1);
        return sorted[low] +(sorted[high] - sorted[low]) * (rank - static_cast<double>(low));
    }

    struct Options
    {
        std::string server = HEIMDALL_LSP_EXE;
        std::string stb_dir = HEIMDALL_STB_DIR;
        std::vector<std::size_t> lines = {1000, 5000, 20000};
        int iterations = 200;
        int warmup = 10;
        double budget_ms = 50.0;
        bool enforce = false;
    };

    bool ParseArguments(int argc, char**argv, Options& options)
    {
        for (int i = 1; i < argc; ++i)
        {
            const std::string_view flag = argv[i];
            auto value =[&]()->const char *
            {
                return i + 1 < argc ? argv[++i] : nullptr;
            };
            if (flag == "--enforce")
            {
                options.enforce = true;
            }
            else if (flag == "--server")
            {
                if (auto v = value())
                {
                    options.server = v;
                }
                else
                {
                    return false;
                }
            }
            else if (flag == "--stb-dir")
            {
                if (auto v = value())
                {
                    options.stb_dir = v;
                }
                else
                {
                    return false;
                }
            }
            else if (flag == "--iterations")
            {
                if (auto v = value())
                {
                    options.iterations = std::atoi(v);
                }
                else
                {
                    return false;
                }
            }
            else if (flag == "--warmup")
            {
                if (auto v = value())
                {
                    options.warmup = std::atoi(v);
                }
                else
                {
                    return false;
                }
            }
            else if (flag == "--budget-ms")
            {
                if (auto v = value())
                {
                    options.budget_ms = std::atof(v);
                }
                else
                {
                    return false;
                }
            }
            else if (flag == "--lines")
            {
                auto v = value();
                if (!v)
                {
                    return false;
                }

                options.lines.clear();
                for (const char * p = v; *p;)
                {
                    char* end = nullptr;
                    options.lines.push_back(std::strtoull(p, &end, 10));
                    if (end == p)
                    {
                        return false;
                    }

                    p = *end == ',' ? end + 1 : end;
                }
            }
            else
            {
                return false;
            }
        }

        return!options.server.empty() && options.iterations > 0;
    }

    struct Result
    {
        std::size_t lines = 0;
        std::size_t bytes = 0;
        double open_ms = 0; // didOpen -> first completion answered (cold parse)
        std::vector<double> samples_ms;
        double document_mb = 0; // working set after the run minus the idle server
        double peak_mb = 0;
        bool ok = false;
    };

    // Finds an indented line inside a function body: typing there gives the
    // completion engine a real scope to walk. Returns (line, column).
    bool PickTypingSite(const std::string& text, std::size_t& line, std::size_t& column)
    {
        const std::size_t middle = text.size() / 2;
        std::size_t line_start = text.find('\n', middle);
        std::size_t current_line = static_cast<std::size_t>(std::count(text.begin(), text.begin() + middle,
            '\n')) + 1;
        while (line_start != std::string::npos)
        {
            ++line_start;
            std::size_t indent = 0;
            while (line_start + indent < text.size() && text[line_start + indent] == ' ')
            {
                ++indent;
            }

            if (indent >= 4 && line_start + indent < text.size() && text[line_start + indent] != '\n' &&
                text[line_start + indent] != '#' && text[line_start + indent] != '/')
            {
                line = current_line;
                column = indent;
                return true;
            }

            line_start = text.find('\n', line_start);
            ++current_line;
        }

        return false;
    }

    Result RunSize(const Options& options, std::size_t target_lines)
    {
        Result result;
        auto document = heimdall::bench::BuildDocument(options.stb_dir, target_lines);
        if (!document)
        {
            std::fprintf(stderr, "cannot read stb headers from %s\n", options.stb_dir.c_str());
            return result;
        }

        result.lines = heimdall::bench::CountLines(*document);
        result.bytes = document->size();

        std::size_t line = 0;
        std::size_t column = 0;
        if (!PickTypingSite(*document, line, column))
        {
            std::fprintf(stderr, "no typing site found in %zu-line document\n", result.lines);
            return result;
        }

        Child child;
        if (!child.Start(options.server))
        {
            std::fprintf(stderr, "cannot start %s\n", options.server.c_str());
            return result;
        }

        ResponseReader reader(child);
        const std::string uri = "file:///heimdall-bench/stb.cpp";
        std::int64_t next_id = 1;
        std::int64_t version = 1;
        const auto timeout = std::chrono::seconds(60);
        Clock::time_point arrived;

        child.Write(Frame("{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(next_id) +
            ",\"method\":\"initialize\",\"params\":{\"initializationOptions\":{}}}"));
        if (!reader.Await(next_id++, timeout, arrived))
        {
            return result;
        }

        child.Write(Frame("{\"jsonrpc\":\"2.0\",\"method\":\"initialized\",\"params\":{}}"));
        const MemorySample idle = child.Memory();

        auto complete =[&](std::size_t at_line, std::size_t at_column, Clock::time_point start,
            double& elapsed_ms)
        {
            const std::int64_t id = next_id++;
            child.Write(Frame("{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) +
                ",\"method\":\"textDocument/completion\",\"params\":{\"textDocument\":{\"uri\":\"" + uri +
                "\"},\"position\":{\"line\":" + std::to_string(at_line) + ",\"character\":" +
                std::to_string(at_column) + "}}}"));
            if (!reader.Await(id, timeout, arrived))
            {
                return false;
            }

            elapsed_ms = std::chrono::duration<double, std::milli>(arrived - start).count();
            return true;
        };

        // LSP lines are 0-based; PickTypingSite returned a 1-based line.
        const std::size_t lsp_line = line - 1;

        const auto open_start = Clock::now();
        child.Write(Frame("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{\"textDocument\":{\"uri\":\"" +
            uri + "\",\"languageId\":\"cpp\",\"version\":1,\"text\":\"" + JsonEscape(*document) + "\"}}}"));
        if (!complete(lsp_line, column, open_start, result.open_ms))
        {
            return result;
        }

        std::size_t typed = 0;
        for (int i = 0; i < options.warmup + options.iterations; ++i)
        {
            const std::size_t at = column + typed;
            const auto start = Clock::now();
            child.Write(Frame("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didChange\",\"params\":{\"textDocument\":{\"uri\":\"" +
                uri + "\",\"version\":" + std::to_string(++version) + "},\"contentChanges\":[{\"range\":{\"start\":{\"line\":" +
                std::to_string(lsp_line) + ",\"character\":" + std::to_string(at) + "},\"end\":{\"line\":" +
                std::to_string(lsp_line) + ",\"character\":" + std::to_string(at) + "}},\"text\":\"a\"}]}}"));
            ++typed;
            double elapsed = 0;
            if (!complete(lsp_line, column + typed, start, elapsed))
            {
                return result;
            }

            if (i >= options.warmup)
            {
                result.samples_ms.push_back(elapsed);
            }

            // A typist does not send keys back to back; give the debounced
            // diagnostics worker the same chance to compete that real typing does.
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }

        const MemorySample loaded = child.Memory();
        result.document_mb = std::max(0.0, loaded.working_set_mb - idle.working_set_mb);
        result.peak_mb = std::max(0.0, loaded.peak_working_set_mb - idle.working_set_mb);

        child.Write(Frame("{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(next_id) + ",\"method\":\"shutdown\"}"));
        reader.Await(next_id++, timeout, arrived);
        child.Write(Frame("{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}"));
        child.Wait();
        result.ok = true;
        return result;
    }

} // namespace

int main(int argc, char**argv)
{
    Options options;
    if (!ParseArguments(argc, argv, options))
    {
        std::fprintf(stderr,
            "usage: LspLatencyBench [--server <heimdall-lsp>] [--stb-dir DIR] [--lines 1000,5000,20000]\n"
            "                       [--iterations N] [--warmup N] [--budget-ms 50] [--enforce]\n");
        return 2;
    }

    std::printf("didChange(1 char) + completion, %d samples after %d warmup, budget %.0f ms\n\n",
        options.iterations,
        options.warmup, options.budget_ms);
    std::printf("%8s %9s %10s %8s %8s %8s %8s %8s %11s %9s\n", "lines", "KiB", "open(ms)", "p50", "p95",
        "p99", "max",
        "budget", "doc MiB", "peak MiB");

    bool over_budget = false;
    bool failed = false;
    for (const std::size_t target : options.lines)
    {
        const Result result = RunSize(options, target);
        if (!result.ok)
        {
            std::printf("%8zu  FAILED (server did not answer)\n", target);
            failed = true;
            continue;
        }

        auto sorted = result.samples_ms;
        std::sort(sorted.begin(), sorted.end());
        const double p95 = Percentile(sorted, 0.95);
        const bool over = p95 > options.budget_ms;
        over_budget = over_budget || over;
        std::printf("%8zu %9.1f %10.1f %8.2f %8.2f %8.2f %8.2f %8s %11.1f %9.1f\n", result.lines,
            static_cast<double>(result.bytes) / 1024.0, result.open_ms, Percentile(sorted, 0.50), p95,
            Percentile(sorted, 0.99), sorted.empty() ? 0.0 : sorted.back(), over ? "OVER" : "ok",
            result.document_mb,
            result.peak_mb);
        std::fflush(stdout);
    }

    return failed ||(options.enforce && over_budget) ? 1 : 0;
}
