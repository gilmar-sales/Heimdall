#include <Heimdall/ProjectIndex.hpp>

#include <algorithm>
#include <unordered_set>

namespace heimdall
{

    namespace
    {

        constexpr std::size_t kMaxDepth = 64;

    } // namespace

    ProjectIndex ProjectIndex::FromSummaries(std::vector<std::shared_ptr<const HeaderSummary>> summaries)
    {
        ProjectIndex index;
        index.m_summaries = std::move(summaries);
        for (std::uint32_t s = 0; s < index.m_summaries.size(); ++s)
        {
            const HeaderSummary& summary = *index.m_summaries[s];
            for (std::uint32_t i = 0; i < summary.ExportCount(); ++i)
            {
                index.m_exports[summary.ExportName(i)].push_back({s, i});
            }

            for (std::uint32_t c = 0; c < summary.ClassCount(); ++c)
            {
                index.m_classes[summary.ClassName(c)].push_back({s, c});
                for (std::uint32_t b = 0; b < summary.BaseCount(c); ++b)
                {
                    ++index.m_base_uses[summary.BaseName(c, b)];
                }
            }
        }

        return index;
    }

    ProjectIndex ProjectIndex::Build(const IncludeProfile& profile)
    {
        std::vector<std::shared_ptr<const HeaderSummary>> summaries;
        std::unordered_set<std::string> seen;
        for (const auto& entry : profile.entries)
        {
            // Conditional includes are not walked: only their own file is known.
            const auto add =[&](const std::filesystem::path& file)
            {
                if (file.empty() || profile.IsSystemFile(file) ||
                    !seen.insert(file.lexically_normal().generic_string()).second)
                {
                    return;
                }

                if (auto summary = HeaderSummary::Load(file); summary->Readable())
                {
                    summaries.push_back(std::move(summary));
                }
            };

            if (entry.closure.empty())
            {
                add(entry.header);
            }

            for (const auto& file : entry.closure)
            {
                add(file);
            }
        }

        return FromSummaries(std::move(summaries));
    }

    std::span<const ProjectIndex::Ref> ProjectIndex::ExportsNamed(std::string_view name) const
    {
        const auto found = m_exports.find(name);
        return found == m_exports.end() ? std::span<const Ref> {}: std::span<const Ref>(found->second);
    }

    std::span<const ProjectIndex::Ref> ProjectIndex::ClassesNamed(std::string_view name) const
    {
        const auto found = m_classes.find(name);
        return found == m_classes.end() ? std::span<const Ref> {}: std::span<const Ref>(found->second);
    }

    bool ProjectIndex::HasDerived(std::string_view name) const
    {
        return m_base_uses.contains(name);
    }

    ProjectIndex::Tri ProjectIndex::IsPolymorphic(std::string_view name) const
    {
        std::vector<std::string_view> visiting;
        return IsPolymorphic(name, visiting);
    }

    ProjectIndex::Tri ProjectIndex::IsPolymorphic(std::string_view name,
        std::vector<std::string_view>& visiting) const
    {
        const auto classes = ClassesNamed(name);
        if (classes.size() != 1 || visiting.size() >= kMaxDepth ||
            std::find(visiting.begin(), visiting.end(), name) != visiting.end())
        {
            return Tri::Unknown;
        }

        const HeaderSummary& summary = *m_summaries[classes.front().summary];
        const std::size_t c = classes.front().index;
        if (summary.ClassHasVirtual(c))
        {
            return Tri::Yes;
        }

        visiting.push_back(name);
        Tri result = Tri::No;
        for (std::size_t b = 0; b < summary.BaseCount(c) && result != Tri::Yes; ++b)
        {
            const std::string_view base = summary.BaseName(c, b);
            const Tri base_result = base.empty() ? Tri::Unknown : IsPolymorphic(base, visiting);
            if (base_result == Tri::Yes)
            {
                result = Tri::Yes;
            }
            else if (base_result == Tri::Unknown)
            {
                result = Tri::Unknown;
            }
        }

        visiting.pop_back();
        return result;
    }

} // namespace heimdall
