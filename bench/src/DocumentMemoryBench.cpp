// Per-document memory of the parse result (architecture review, section 7, item 2).
//
// For stb-based documents of growing size, reports the bytes held by a ParseTree
// (vector capacities, i.e. what the allocator really reserved), the derived
// bytes/line, bytes/token and nodes/token ratios used to calibrate section 1 of the
// review, and the process working-set growth while the tree is alive.
// ParseTree does not use Arena today, so Arena::Used() would read zero; capacities
// are the honest number. The end-to-end server footprint is reported by
// LspLatencyBench ("doc MiB" / "peak MiB").
//
//   DocumentMemoryBench [--stb-dir DIR] [--lines 1000,5000,20000]
#include "StbCorpus.hpp"

#include <Heimdall/ParseTree.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
    #include <psapi.h>
#endif

namespace
{

    double WorkingSetMb()
    {
#if defined(_WIN32)
        PROCESS_MEMORY_COUNTERS counters {};
        counters.cb = sizeof(counters);
        if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
        {
            return static_cast<double>(counters.WorkingSetSize) / (1024.0 * 1024.0);
        }
#else
        if (std::FILE* status = std::fopen("/proc/self/status", "r"))
        {
            char   line[256];
            double kb  = 0;
            double rss = 0;
            while (std::fgets(line, sizeof(line), status))
            {
                if (std::sscanf(line, "VmRSS: %lf kB", &kb) == 1)
                {
                    rss = kb / 1024.0;
                }
            }

            std::fclose(status);
            return rss;
        }
#endif
        return 0;
    }

    std::size_t TreeBytes(const heimdall::ParseTree& tree)
    {
        std::size_t bytes = tree.Tokens().capacity() * sizeof(heimdall::Token);
        const auto& soa   = tree.NodesSoA();
        bytes += soa.kind.capacity() * sizeof(std::uint8_t);
        bytes += (soa.first_token.capacity() + soa.token_count.capacity() + soa.parent.capacity() +
                  soa.subtree_end.capacity()) *
                 sizeof(std::uint32_t);
        bytes += tree.Directives().capacity() * sizeof(heimdall::PreprocessorDirective);
        bytes += tree.Diagnostics().capacity() * sizeof(heimdall::GrammarDiagnostic);
        for (const auto& diagnostic : tree.Diagnostics())
        {
            bytes += diagnostic.message.capacity();
        }

        return bytes;
    }

} // namespace

int main(int argc, char** argv)
{
    std::string              stb_dir = HEIMDALL_STB_DIR;
    std::vector<std::size_t> sizes   = { 1000, 5000, 20000 };
    for (int i = 1; i + 1 < argc; i += 2)
    {
        const std::string_view flag = argv[i];
        if (flag == "--stb-dir")
        {
            stb_dir = argv[i + 1];
        }
        else if (flag == "--lines")
        {
            sizes.clear();
            for (const char* p = argv[i + 1]; *p;)
            {
                char* end = nullptr;
                sizes.push_back(std::strtoull(p, &end, 10));
                if (end == p)
                {
                    return 2;
                }

                p = *end == ',' ? end + 1 : end;
            }
        }
        else
        {
            return 2;
        }
    }

    std::printf("%8s %9s %9s %9s %10s %10s %10s %11s\n", "lines", "KiB", "tokens", "nodes",
                "tree MiB", "B/line", "B/token", "RSS +MiB");
    for (const std::size_t target : sizes)
    {
        auto document = heimdall::bench::BuildDocument(stb_dir, target);
        if (!document)
        {
            std::fprintf(stderr, "cannot read stb headers from %s\n", stb_dir.c_str());
            return 1;
        }

        const std::size_t lines  = heimdall::bench::CountLines(*document);
        const double      before = WorkingSetMb();
        const auto        tree   = heimdall::ParseTree::Parse(*document);
        const double      growth = WorkingSetMb() - before;
        const std::size_t bytes  = TreeBytes(tree);
        std::printf("%8zu %9.1f %9zu %9zu %10.2f %10.0f %10.1f %11.1f\n", lines,
                    static_cast<double>(document->size()) / 1024.0, tree.Tokens().size(),
                    tree.NodesSoA().size(), static_cast<double>(bytes) / (1024.0 * 1024.0),
                    static_cast<double>(bytes) / static_cast<double>(lines),
                    static_cast<double>(bytes) / static_cast<double>(tree.Tokens().size()), growth);
    }

    return 0;
}
