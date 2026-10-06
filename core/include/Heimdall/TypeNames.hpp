#pragma once

#include <cstdint>
#include <string_view>

namespace heimdall
{

    // What the grammar parser needs to know to tell `a * b;` (declaration) from
    // `a * b;` (product), and `(T)x` (cast) from `(a)x`: whether a name denotes a
    // type. Names declared in the file itself are tracked by the parser as it goes;
    // this covers the ones it cannot see, the headers the file includes.
    //
    // Names are matched by their last component (`string` for `std::string`).
    // Implementations are immutable and shared between threads. An unknown name is
    // answered `false`, and the parser then falls back to its token-shape heuristic,
    // so an incomplete oracle never makes a parse worse than having none.
    class TypeNameOracle
    {
    public:
        virtual~TypeNameOracle() = default;

        virtual bool IsType(std::string_view name) const noexcept = 0;

        // Changes whenever the answers may change: incremental reuse of a previous
        // parse is only valid under the same fingerprint.
        virtual std::uint64_t Fingerprint() const noexcept = 0;
    };

} // namespace heimdall
