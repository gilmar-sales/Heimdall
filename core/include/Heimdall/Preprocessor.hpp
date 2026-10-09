#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace heimdall
{

    enum class DirectiveKind
    {
        Include,
        Define,
        Undef,
        If,
        Ifdef,
        Ifndef,
        Elif,
        Else,
        Endif,
        Pragma,
        Other
    };

    struct PreprocessorDirective
    {
        DirectiveKind kind;
        std::size_t   offset;
        std::size_t   length;
    };

    struct PreprocessorDiagnostic
    {
        std::size_t offset;
        std::string message;
    };

    struct ActiveSourceRange
    {
        std::size_t offset;
        std::size_t length;
    };

    struct PreprocessorResult
    {
        std::string                         active_source;
        std::vector<PreprocessorDirective>  directives;
        std::vector<ActiveSourceRange>      active_ranges;
        std::vector<PreprocessorDiagnostic> diagnostics;
        // Object-like macros the file itself #defines (final state, #undef applied).
        std::unordered_map<std::string, std::string> local_macros;
    };

    // Transparent hash so macro lookups take string_view without allocating a
    // temporary std::string key per identifier.
    struct TransparentStringHash
    {
        using is_transparent = void;

        std::size_t operator()(std::string_view text) const noexcept
        {
            return std::hash<std::string_view> {}(text);
        }
    };

    class Preprocessor
    {
      public:
        using MacroMap =
            std::unordered_map<std::string, std::string, TransparentStringHash, std::equal_to<>>;
        using ErasedSet = std::unordered_set<std::string, TransparentStringHash, std::equal_to<>>;

        // The empty preprocessor owns its (empty) map. All other constructors
        // are non-owning views or shared ownership: predefined macros are never
        // copied per file. A view is valid only while the referenced map lives;
        // every current call site uses the preprocessor immediately within the
        // same full expression as Process(), which is safe.
        Preprocessor() : m_owned(std::make_shared<const MacroMap>()), m_predefined(m_owned.get()) {}

        // Non-owning view: zero copies.
        explicit Preprocessor(const MacroMap& predefined) : m_predefined(&predefined) {}

        explicit Preprocessor(std::shared_ptr<const MacroMap> predefined) :
            m_owned(std::move(predefined)), m_predefined(m_owned.get())
        {
            if (m_predefined == nullptr)
            {
                m_owned      = std::make_shared<const MacroMap>();
                m_predefined = m_owned.get();
            }
        }

        // Production call sites use the default (no active_source): the grammar,
        // linter and formatter only need ranges/directives/diagnostics. Tests and
        // benchmarks that assert on expansion pass build_active_source=true.
        PreprocessorResult Process(std::string_view source, bool build_active_source = false) const;

      private:
        std::shared_ptr<const MacroMap> m_owned;
        const MacroMap*                 m_predefined = nullptr;
    };

} // namespace heimdall
