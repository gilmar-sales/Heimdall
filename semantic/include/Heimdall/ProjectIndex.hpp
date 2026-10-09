#pragma once

#include <Heimdall/HeaderSummary.hpp>
#include <Heimdall/IncludeAnalyzer.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace heimdall
{

    // Symbol-to-header map for one file: the HeaderSummary of every project
    // header (outside the compiler's system directories) in the include
    // closures of an IncludeProfile, indexed by exported name and by class
    // name. Built from the shared summaries, so it costs a few hash inserts, not
    // a parse; it holds them alive and does not outlive them.
    class ProjectIndex
    {
      public:
        // `index` addresses the export (or class) inside `Summaries()[summary]`.
        struct Ref
        {
            std::uint32_t summary = 0;
            std::uint32_t index   = 0;
        };

        enum class Tri : std::uint8_t
        {
            No,
            Yes,
            Unknown
        }

        ;

        static ProjectIndex Build(const IncludeProfile& profile);

        static ProjectIndex FromSummaries(
            std::vector<std::shared_ptr<const HeaderSummary>> summaries);

        const std::vector<std::shared_ptr<const HeaderSummary>>& Summaries() const noexcept
        {
            return m_summaries;
        }

        // Exports called `name`, in no particular order.
        std::span<const Ref> ExportsNamed(std::string_view name) const;

        std::span<const Ref> ClassesNamed(std::string_view name) const;

        // Whether the class called `name` has a virtual function of its own or
        // through its bases, following bases through the indexed headers. Unknown
        // when the name is ambiguous or a base is not in the index.
        Tri IsPolymorphic(std::string_view name) const;

        // Some indexed class lists `name` among its bases.
        bool HasDerived(std::string_view name) const;

      private:
        Tri IsPolymorphic(std::string_view name, std::vector<std::string_view>& visiting) const;

        std::vector<std::shared_ptr<const HeaderSummary>>      m_summaries;
        std::unordered_map<std::string_view, std::vector<Ref>> m_exports;
        std::unordered_map<std::string_view, std::vector<Ref>> m_classes;
        std::unordered_map<std::string_view, std::uint32_t>    m_base_uses;
    };

} // namespace heimdall
