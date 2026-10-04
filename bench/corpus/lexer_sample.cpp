// Corpus sample: exercises comments, strings, raw strings, macros,
// conditional ranges and templates for the lexer/CST benches.
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

/* Block comment spanning
   multiple lines with "quotes" and 'chars'. */

#define MAX_ITEMS 1024
#define CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : ((v) > (hi) ? (hi) : (v)))

namespace heimdall::samples
{

constexpr std::uint32_t kSeed = 0x9E3779B9u;

template <typename T> T Lerp(const T& a, const T& b, double t)
{
    // Linear interpolation: a + (b - a) * t
    return static_cast<T>(a + (b - a) * t);
}

struct Token
{
    int kind = 0;
    std::size_t offset = 0;
    std::size_t length = 0;
    std::string text; // intentionally owning here
};

#if defined(_WIN32)
constexpr char kPathSep = '\\';
#else
constexpr char kPathSep = '/';
#endif

#ifdef HEIMDALL_ENABLE_EXTRA
int ExtraFeature();
#endif

std::vector<Token> Tokenize(std::string_view source)
{
    std::vector<Token> out;
    const char* raw = R"(raw string with \n no escapes and "quotes")";
    (void)raw;
    std::size_t i = 0;
    while (i < source.size())
    {
        const char c = source[i];
        if (c == ' ' || c == '\t' || c == '\n')
        {
            ++i; // trivia
            continue;
        }
        out.push_back(Token { .kind = 1, .offset = i, .length = 1 });
        ++i;
    }
    std::sort(out.begin(), out.end(), [](const Token& a, const Token& b) { return a.offset < b.offset; });
    return out;
}

} // namespace heimdall::samples
