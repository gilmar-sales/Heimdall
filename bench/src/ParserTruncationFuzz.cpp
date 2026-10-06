// Truncation fuzzing of the parser (architecture review, section 7, item 3).
//
// Takes a valid document and parses every prefix of it (every byte offset by
// default, or every --stride-th), which is what the parser sees while the user types
// at the end of a file. For each prefix it checks:
//   * no hang       : a watchdog aborts the process if one parse exceeds --hang-ms
//   * bounded time  : prefix parse time <= --time-factor x full-document parse time
//   * bounded errors: Error/ErrorExpression nodes <= --max-errors
// and prints the worst offender of each metric. Exit code is 0 only if every prefix
// passes.
//
//   ParserTruncationFuzz [--file PATH | --stb-dir DIR --lines N] [--stride N]
//                        [--time-factor 3] [--max-errors 8] [--hang-ms 5000]
#include "StbCorpus.hpp"

#include <Heimdall/ParseTree.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <thread>

namespace
{

    using Clock = std::chrono::steady_clock;

    std::size_t CountErrors(const heimdall::ParseTree& tree)
    {
        std::size_t errors = 0;
        for (const auto& node : tree.Nodes())
        {
            if (node.kind == heimdall::GrammarKind::Error || node.kind == heimdall::GrammarKind::ErrorExpression)
            {
                ++errors;
            }
        }

        return errors;
    }

    double ParseMs(std::string_view source, std::size_t& errors)
    {
        const auto start = Clock::now();
        const auto tree = heimdall::ParseTree::Parse(source);
        const auto stop = Clock::now();
        errors = CountErrors(tree);
        return std::chrono::duration<double, std::milli>(stop - start).count();
    }

} // namespace

int main(int argc, char**argv)
{
    std::string file;
    std::string stb_dir = HEIMDALL_STB_DIR;
    std::size_t lines = 1000;
    std::size_t stride = 1;
    double time_factor = 3.0;
    std::size_t max_errors = 8;
    int hang_ms = 5000;
    for (int i = 1; i + 1 < argc; i += 2)
    {
        const std::string_view flag = argv[i];
        const char* value = argv[i + 1];
        if (flag == "--file")
        {
            file = value;
        }
        else if (flag == "--stb-dir")
        {
            stb_dir = value;
        }
        else if (flag == "--lines")
        {
            lines = std::strtoull(value, nullptr, 10);
        }
        else if (flag == "--stride")
        {
            stride = std::max<std::size_t>(1, std::strtoull(value, nullptr, 10));
        }
        else if (flag == "--time-factor")
        {
            time_factor = std::atof(value);
        }
        else if (flag == "--max-errors")
        {
            max_errors = std::strtoull(value, nullptr, 10);
        }
        else if (flag == "--hang-ms")
        {
            hang_ms = std::atoi(value);
        }
        else
        {
            std::fprintf(stderr, "unknown flag %s\n", argv[i]);
            return 2;
        }
    }

    std::string source;
    if (!file.empty())
    {
        auto loaded = heimdall::bench::ReadFile(file);
        if (!loaded)
        {
            std::fprintf(stderr, "cannot read %s\n", file.c_str());
            return 2;
        }

        source = std::move(*loaded);
    }
    else
    {
        auto built = heimdall::bench::BuildDocument(stb_dir, lines);
        if (!built)
        {
            std::fprintf(stderr, "cannot read stb headers from %s\n", stb_dir.c_str());
            return 2;
        }

        source = std::move(*built);
    }

    // Watchdog: the grammar pass only polls its stop token between top-level
    // items, so a hang inside one item cannot be cancelled; abort with the offset.
    std::atomic<std::size_t> current_offset = 0;
    std::atomic<std::int64_t> deadline_ticks = 0;
    std::atomic<bool> finished = false;
    std::thread watchdog([&]
        {
            while (!finished.load())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                const auto deadline = deadline_ticks.load();
                if (deadline != 0 && Clock::now().time_since_epoch().count() > deadline)
                {
                    std::fprintf(stderr, "HANG: parsing the prefix of %zu bytes exceeded %d ms\n",
                    current_offset.load(), hang_ms);
                    std::fflush(stderr);
                    std::_Exit(3);
            }
        }
    });

    // Baseline: best of three full parses (first one warms caches).
    std::size_t full_errors = 0;
    double full_ms = ParseMs(source, full_errors);
    for (int i = 0; i < 2; ++i)
    {
        std::size_t ignored = 0;
        full_ms = std::min(full_ms, ParseMs(source, ignored));
    }

    std::printf("document: %zu lines, %zu bytes; full parse %.3f ms, %zu error nodes\n",
        heimdall::bench::CountLines(source), source.size(), full_ms, full_errors);
    std::printf("limits: time <= %.1f x full (%.3f ms), error nodes <= %zu, hang > %d ms\n\n",
        time_factor,
        time_factor* full_ms, max_errors, hang_ms);

    const double time_limit = time_factor * full_ms;
    const auto hang_window = std::chrono::milliseconds(hang_ms);
    double worst_ms = 0;
    std::size_t worst_ms_at = 0;
    std::size_t worst_errors = 0;
    std::size_t worst_errors_at = 0;
    std::size_t checked = 0;
    std::size_t slow = 0;
    std::size_t noisy = 0;
    double total_ms = 0;

    for (std::size_t length = 0; length <= source.size(); length += stride)
    {
        current_offset = length;
        deadline_ticks = (Clock::now() + hang_window).time_since_epoch().count();
        std::size_t errors = 0;
        const double ms = ParseMs(std::string_view(source).substr(0, length), errors);
        deadline_ticks = 0;

        ++checked;
        total_ms += ms;
        if (ms > worst_ms)
        {
            worst_ms = ms;
            worst_ms_at = length;
        }

        if (errors > worst_errors)
        {
            worst_errors = errors;
            worst_errors_at = length;
        }

        if (ms > time_limit)
        {
            if (slow++ < 10)
            {
                std::printf("SLOW  prefix %zu: %.3f ms (limit %.3f)\n", length, ms, time_limit);
            }
        }

        if (errors > max_errors)
        {
            if (noisy++ < 10)
            {
                std::printf("NOISY prefix %zu: %zu error nodes (limit %zu)\n", length, errors, max_errors);
            }
        }
    }

    finished = true;
    watchdog.join();

    std::printf("\nprefixes checked : %zu (stride %zu)\n", checked, stride);
    std::printf("mean parse time  : %.3f ms\n", total_ms / static_cast<double>(checked));
    std::printf("worst parse time : %.3f ms at prefix %zu (%.2f x full)\n", worst_ms, worst_ms_at,
        full_ms > 0 ? worst_ms / full_ms : 0.0);
    std::printf("worst error nodes: %zu at prefix %zu\n", worst_errors, worst_errors_at);
    std::printf("violations       : %zu slow, %zu noisy\n", slow, noisy);
    return slow == 0 && noisy == 0 ? 0 : 1;
}
