#pragma once

#include <array>
#include <string_view>
#include <version>

#if defined(__cpp_pp_embed) && __cpp_pp_embed >= 202502L
    #define HEIMDALL_HAS_CPP26_EMBED 1
#else
    #define HEIMDALL_HAS_CPP26_EMBED 0
#endif

#if defined(__cpp_impl_reflection) && __cpp_impl_reflection >= 202506L
    #define HEIMDALL_HAS_CPP26_REFLECTION 1
#else
    #define HEIMDALL_HAS_CPP26_REFLECTION 0
#endif

namespace heimdall
{

    /** Feature-test macro value observed in the compiler or standard library. */
    struct Cpp26Feature
    {
        std::string_view name;
        long value;
        long required;

        /** Whether this implementation advertises the C++26 revision. */
        [[nodiscard]] constexpr bool Available() const noexcept
        {
            return value >= required;
        }
    };

    /** C++26 feature revisions used to survey the active build toolchain. */
    [[nodiscard]] constexpr auto Cpp26Features() noexcept
    {
        return std::array {
            Cpp26Feature { "__cpp_pp_embed",
#ifdef __cpp_pp_embed
                           __cpp_pp_embed,
#else
                           0L,
#endif
                           202502L },
            Cpp26Feature { "__cpp_pack_indexing",
#ifdef __cpp_pack_indexing
                           __cpp_pack_indexing,
#else
                           0L,
#endif
                           202311L },
            Cpp26Feature { "__cpp_variadic_friend",
#ifdef __cpp_variadic_friend
                           __cpp_variadic_friend,
#else
                           0L,
#endif
                           202403L },
            Cpp26Feature { "__cpp_template_parameters",
#ifdef __cpp_template_parameters
                           __cpp_template_parameters,
#else
                           0L,
#endif
                           202502L },
            Cpp26Feature { "__cpp_impl_reflection",
#ifdef __cpp_impl_reflection
                           __cpp_impl_reflection,
#else
                           0L,
#endif
                           202506L },
            Cpp26Feature { "__cpp_constexpr_exceptions",
#ifdef __cpp_constexpr_exceptions
                           __cpp_constexpr_exceptions,
#else
                           0L,
#endif
                           202411L },
            Cpp26Feature { "__cpp_lib_inplace_vector",
#ifdef __cpp_lib_inplace_vector
                           __cpp_lib_inplace_vector,
#else
                           0L,
#endif
                           202406L },
        };
    }

} // namespace heimdall
