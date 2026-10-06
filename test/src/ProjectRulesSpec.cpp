#include <gtest/gtest.h>

#include <Heimdall/HeaderSummary.hpp>
#include <Heimdall/IncludeAnalyzer.hpp>
#include <Heimdall/ProjectIndex.hpp>
#include <Heimdall/RuleEngine.hpp>
#include <Heimdall/SemanticRules.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace
{

    // A throwaway project: headers under `inc/` (the only -I directory) and the
    // analyzed file next to it, so quoted and angled includes both resolve.
    class Project
    {
    public:
        Project()
        {
            static std::atomic<int> counter{0};
            m_root = std::filesystem::temp_directory_path() /
                ("heimdall-project-rules-" + std::to_string(counter.fetch_add(1)) + "-" +
                std::to_string(std::filesystem::file_time_type::clock::now().time_since_epoch().count()));
            std::filesystem::create_directories(m_root / "inc");
            m_command.include_directories.push_back(m_root / "inc");
        }

        ~Project()
        {
            std::error_code ec;
            std::filesystem::remove_all(m_root, ec);
        }

        Project(const Project&) = delete;

        Project& operator= (const Project&) = delete;

        void Header(const std::string& name, std::string_view content) const
        {
            const auto path = m_root / "inc" / name;
            std::filesystem::create_directories(path.parent_path());
            std::ofstream(path, std::ios::binary) << content;
        }

        std::filesystem::path File(const std::string& name) const
        {
            return m_root / name;
        }

        const heimdall::CompileCommand* Command() const
        {
            return &m_command;
        }

        std::vector<heimdall::Diagnostic> Iwyu(std::string_view source,
            const std::string& file = "main.cpp") const
        {
            const auto tree = heimdall::ParseTree::Parse(source, {});
            const auto model = heimdall::Binder::Bind(tree);
            const auto profile = heimdall::IncludeAnalyzer::BuildProfile(File(file), tree, &m_command);
            const heimdall::ProjectContext context
            {
                File(file), profile.get(), &m_command
            };
            return heimdall::SemanticRules::AnalyzeIncludeWhatYouUse(model, context);
        }

        std::vector<heimdall::Diagnostic> Final(std::string_view source,
            const std::string& file = "main.cpp",
            bool with_profile = true) const
        {
            const auto tree = heimdall::ParseTree::Parse(source, {});
            const auto model = heimdall::Binder::Bind(tree);
            const auto profile = with_profile ? heimdall::IncludeAnalyzer::BuildProfile(File(file), tree,
                &m_command)
            : nullptr;
            const heimdall::ProjectContext context
            {
                File(file), profile.get(), with_profile ? &m_command : nullptr
            };
            return heimdall::SemanticRules::AnalyzeFinal(model, context);
        }

    private:
        std::filesystem::path m_root;
        heimdall::CompileCommand m_command;
    };

    std::string Apply(std::string_view source, const heimdall::Diagnostic& diagnostic)
    {
        std::string text(source);
        text.replace(diagnostic.fix.offset, diagnostic.fix.length, diagnostic.fix.replacement);
        return text;
    }

    std::vector<std::string> Names(const heimdall::HeaderSummary& summary)
    {
        std::vector<std::string> names;
        for (std::size_t i = 0; i < summary.ExportCount(); ++i)
        {
            std::string name(summary.ExportNamespace(i));
            name += name.empty() ? "" : "::";
            name += summary.ExportName(i);
            names.push_back(std::move(name));
        }

        std::sort(names.begin(), names.end());
        return names;
    }

} // namespace

// ---------------------------------------------------------------------------
// HeaderSummary

TEST(HeaderSummarySpec, ExportsNamespaceAndGlobalDeclarationsWithTheirNamespace)
{
    const auto summary = heimdall::HeaderSummary::FromSource(
        "#pragma once\n"
        "namespace lib { namespace detail {\n"
        "struct Impl { int x; };\n"
        "int helper(int);\n"
        "} enum class Mode { On, Off };\n"
        "enum Color { Red };\n"
        "using Handle = int;\n"
        "extern int counter;\n"
        "}\n"
        "class Global {};\n");
    EXPECT_EQ(Names(*summary),
        (std::vector<std::string>{"Global", "lib::Color", "lib::Handle", "lib::Mode",
            "lib::Red", "lib::counter", "lib::detail::Impl", "lib::detail::helper"}));
}

TEST(HeaderSummarySpec, KeepsMembersLocalsAndInternalNamesOut)
{
    const auto summary = heimdall::HeaderSummary::FromSource(
        "namespace { struct Hidden {}; }\n"
        "namespace lib {\n"
        "namespace { int internal_counter; }\n"
        "struct Widget { int member; void method(); struct Nested {}; };\n"
        "inline int twice(int value) { int local = value; return local * 2; }\n"
        "class Forward;\n"
        "struct Widget2;\n"
        "}\n"
        "void lib_free();\n"
        "struct _Reserved {};\n");
    EXPECT_EQ(Names(*summary), (std::vector<std::string>{"lib::Widget", "lib::twice", "lib_free"}));
}

TEST(HeaderSummarySpec, DoesNotExportOutOfLineMembersOrFriends)
{
    const auto summary = heimdall::HeaderSummary::FromSource(
        "struct Box { friend void swap(Box &, Box &); Box(); ~Box(); Box & operator=(const Box &); };\n"
        "inline Box::Box() {}\n"
        "void Box::reset() {}\n");
    EXPECT_EQ(Names(*summary), (std::vector<std::string>{"Box"}));
}

TEST(HeaderSummarySpec, RecordsClassBasesVirtualsAndFinal)
{
    const auto summary = heimdall::HeaderSummary::FromSource(
        "namespace lib {\n"
        "struct Plain { int x; };\n"
        "struct Shape { virtual ~Shape(); virtual void draw() = 0; };\n"
        "class Circle final : public Shape, private Plain { void draw() override; };\n"
        "template <class T> struct Box : Base<T> {};\n"
        "}\n");
    std::vector<std::string> seen;
    for (std::size_t c = 0; c < summary->ClassCount(); ++c)
    {
        std::string line(summary->ClassName(c));
        line += summary->ClassHasVirtual(c) ? " virtual" : "";
        line += summary->ClassIsFinal(c) ? " final" : "";
        line += summary->ClassIsTemplate(c) ? " template" : "";
        for (std::size_t b = 0; b < summary->BaseCount(c); ++b)
        {
            line += " :" + std::string(summary->BaseName(c, b));
        }

        seen.push_back(std::move(line));
    }

    EXPECT_EQ(seen,
        (std::vector<std::string>{"Plain", "Shape virtual", "Circle virtual final :Shape :Plain",
            "Box template :Base"}));
}

TEST(HeaderSummarySpec, RecordsDirectIncludesAndReexports)
{
    const auto summary = heimdall::HeaderSummary::FromSource(
        "#include <vector>\n"
        "#include \"core.hpp\" // IWYU pragma: export\n"
        "#  include <map>\n"
        "#define NOT_AN_INCLUDE\n");
    ASSERT_EQ(summary->IncludeCount(), 3);
    EXPECT_EQ(summary->IncludeTarget(0), "<vector>");
    EXPECT_FALSE(summary->IncludeReexported(0));
    EXPECT_EQ(summary->IncludeTarget(1), "\"core.hpp\"");
    EXPECT_TRUE(summary->IncludeReexported(1));
    EXPECT_EQ(summary->IncludeTarget(2), "<map>");
}

TEST(HeaderSummarySpec, FlagsPrivateAndTextualHeaders)
{
    EXPECT_TRUE(heimdall::HeaderSummary::FromSource("// IWYU pragma: private\nstruct A {};\n")->IsPrivate());
    EXPECT_FALSE(heimdall::HeaderSummary::FromSource("struct A {};\n")->IsPrivate());
    EXPECT_TRUE(heimdall::HeaderSummary::FromSource("int x;\n", "impl.INL")->IsTextual());
    EXPECT_FALSE(heimdall::HeaderSummary::FromSource("int x;\n", "impl.hpp")->IsTextual());
}

TEST(HeaderSummarySpec, ToleratesBrokenHeaders)
{
    const auto summary = heimdall::HeaderSummary::FromSource(
        "namespace lib {\n"
        "struct Good {};\n"
        "struct Half : \n");
    EXPECT_TRUE(summary->Readable());
    EXPECT_FALSE(Names(*summary).empty());
}

TEST(HeaderSummarySpec, LoadIsSharedUntilTheFileChanges)
{
    Project project;
    project.Header("a.hpp", "struct Alpha {};\n");
    const auto path = project.File("inc/a.hpp");
    const auto first = heimdall::HeaderSummary::Load(path);
    EXPECT_EQ(first, heimdall::HeaderSummary::Load(path));
    EXPECT_EQ(Names(*first), (std::vector<std::string>{"Alpha"}));

    project.Header("a.hpp", "struct Alpha {};\nstruct Beta {};\n");
    const auto second = heimdall::HeaderSummary::Load(path);
    EXPECT_NE(first, second);
    EXPECT_EQ(Names(*second), (std::vector<std::string>{"Alpha", "Beta"}));
}

TEST(HeaderSummarySpec, MissingFileIsNotReadable)
{
    Project project;
    EXPECT_FALSE(heimdall::HeaderSummary::Load(project.File("inc/missing.hpp"))->Readable());
}

// ---------------------------------------------------------------------------
// ProjectIndex

TEST(ProjectIndexSpec, PolymorphismFollowsBasesAcrossHeaders)
{
    using heimdall::ProjectIndex;
    const auto index = ProjectIndex::FromSummaries({
            heimdall::HeaderSummary::FromSource("struct Base { virtual void f(); };\n"),
            heimdall::HeaderSummary::FromSource("struct Mid : Base {};\nstruct Plain {};\nstruct Odd : Missing {};\n"),
            heimdall::HeaderSummary::FromSource("struct Dup {};\n"),
            heimdall::HeaderSummary::FromSource("struct Dup { virtual void g(); };\n"),
    });
    EXPECT_EQ(index.IsPolymorphic("Base"), ProjectIndex::Tri::Yes);
    EXPECT_EQ(index.IsPolymorphic("Mid"), ProjectIndex::Tri::Yes);
    EXPECT_EQ(index.IsPolymorphic("Plain"), ProjectIndex::Tri::No);
    EXPECT_EQ(index.IsPolymorphic("Odd"), ProjectIndex::Tri::Unknown);
    EXPECT_EQ(index.IsPolymorphic("Nope"), ProjectIndex::Tri::Unknown);
    EXPECT_EQ(index.IsPolymorphic("Dup"), ProjectIndex::Tri::Unknown);
    EXPECT_TRUE(index.HasDerived("Base"));
    EXPECT_FALSE(index.HasDerived("Mid"));
}

TEST(ProjectIndexSpec, InheritanceCycleEndsAsUnknown)
{
    using heimdall::ProjectIndex;
    const auto index = ProjectIndex::FromSummaries({
            heimdall::HeaderSummary::FromSource("struct A : B {};\nstruct B : A {};\n"),
    });
    EXPECT_EQ(index.IsPolymorphic("A"), ProjectIndex::Tri::Unknown);
}

TEST(ProjectIndexSpec, SystemHeadersStayOutOfTheIndex)
{
    Project project;
    project.Header("a.hpp", "struct Alpha {};\n");
    heimdall::CompileCommand command = *project.Command();
    const auto tree = heimdall::ParseTree::Parse("#include <a.hpp>\n", {});
    auto profile = *heimdall::IncludeAnalyzer::BuildProfile(project.File("main.cpp"), tree, &command);
    EXPECT_EQ(heimdall::ProjectIndex::Build(profile).ExportsNamed("Alpha").size(), 1);
    profile.system_dirs.push_back(project.File("inc").lexically_normal());
    EXPECT_TRUE(heimdall::ProjectIndex::Build(profile).ExportsNamed("Alpha").empty());
}

// ---------------------------------------------------------------------------
// cpp/include-what-you-use

TEST(IncludeWhatYouUseSpec, ReportsProjectHeaderReachedOnlyThroughAnotherInclude)
{
    Project project;
    project.Header("core.hpp", "#pragma once\nstruct Core { int x; };\n");
    project.Header("wrapper.hpp",
        "#pragma once\n#include <core.hpp>\nstruct Wrapper { Core core; };\n");
    constexpr std::string_view source =
        "#include <wrapper.hpp>\n"
    "Core value;\n";
    const auto diagnostics = project.Iwyu(source);
    ASSERT_EQ(diagnostics.size(), 1);
    EXPECT_EQ(diagnostics[0].code, "cpp/include-what-you-use");
    EXPECT_EQ(diagnostics[0].rule, heimdall::RuleId::IncludeWhatYouUse);
    EXPECT_EQ(diagnostics[0].line, 2);
    EXPECT_EQ(source.substr(diagnostics[0].offset, diagnostics[0].length), "Core");
    EXPECT_NE(diagnostics[0].message.find("core.hpp"), std::string::npos);
    ASSERT_TRUE(diagnostics[0].has_fix);
    EXPECT_FALSE(diagnostics[0].fix_is_safe);
    EXPECT_EQ(Apply(source, diagnostics[0]),
        "#include <wrapper.hpp>\n#include <core.hpp>\n"
        "Core value;\n");
}

TEST(IncludeWhatYouUseSpec, SilentWhenTheHeaderIsIncludedDirectly)
{
    Project project;
    project.Header("core.hpp", "#pragma once\nstruct Core { int x; };\n");
    project.Header("wrapper.hpp", "#pragma once\n#include <core.hpp>\n");
    EXPECT_TRUE(project.Iwyu("#include <wrapper.hpp>\n#include <core.hpp>\nCore value;\n").empty());
    EXPECT_TRUE(project.Iwyu("#include <wrapper.hpp>\n#include \"core.hpp\"\nCore value;\n").empty());
}

TEST(IncludeWhatYouUseSpec, SilentWhenTheDirectIncludeIsConditional)
{
    Project project;
    project.Header("core.hpp", "#pragma once\nstruct Core { int x; };\n");
    project.Header("wrapper.hpp", "#pragma once\n#include <core.hpp>\n");
    EXPECT_TRUE(project.Iwyu("#include <wrapper.hpp>\n#ifdef FEATURE\n#include <core.hpp>\n#endif\nCore value;\n").empty());
}

TEST(IncludeWhatYouUseSpec, ReportsEachMissingHeaderOnceAtItsFirstUse)
{
    Project project;
    project.Header("a.hpp", "#pragma once\nstruct A {};\nstruct A2 {};\n");
    project.Header("b.hpp", "#pragma once\nstruct B {};\n");
    project.Header("all.hpp", "#pragma once\n#include <a.hpp>\n#include <b.hpp>\n");
    constexpr std::string_view source =
        "#include <all.hpp>\n"
    "A first;\n"
    "A2 second;\n"
    "B third;\n"
    "A again;\n";
    const auto diagnostics = project.Iwyu(source);
    ASSERT_EQ(diagnostics.size(), 2);
    EXPECT_EQ(diagnostics[0].line, 2);
    EXPECT_EQ(diagnostics[1].line, 4);
}

TEST(IncludeWhatYouUseSpec, UsesTheNamespaceToTellNamesApart)
{
    Project project;
    project.Header("one.hpp", "#pragma once\nnamespace one { struct Thing {}; int make(); }\n");
    project.Header("two.hpp", "#pragma once\nnamespace two { struct Thing {}; }\n");
    project.Header("both.hpp", "#pragma once\n#include <one.hpp>\n#include <two.hpp>\n");

    auto diagnostics = project.Iwyu("#include <both.hpp>\none::Thing a;\n");
    ASSERT_EQ(diagnostics.size(), 1);
    EXPECT_NE(diagnostics[0].message.find("one.hpp"), std::string::npos);

    diagnostics = project.Iwyu("#include <both.hpp>\ntwo::Thing a;\n");
    ASSERT_EQ(diagnostics.size(), 1);
    EXPECT_NE(diagnostics[0].message.find("two.hpp"), std::string::npos);

    // Unqualified at global scope: neither header declares a global `Thing`.
    EXPECT_TRUE(project.Iwyu("#include <both.hpp>\nThing a;\n").empty());
}

TEST(IncludeWhatYouUseSpec, UnqualifiedNamesNeedTheEnclosingNamespaceOrAUsingDirective)
{
    Project project;
    project.Header("lib.hpp", "#pragma once\nnamespace lib { struct Widget {}; }\n");
    project.Header("wrap.hpp", "#pragma once\n#include <lib.hpp>\n");

    EXPECT_EQ(project.Iwyu("#include <wrap.hpp>\nnamespace lib { Widget w; }\n").size(), 1);
    EXPECT_EQ(project.Iwyu("#include <wrap.hpp>\nnamespace lib { namespace inner { Widget w; } }\n").size(),
        1);
    EXPECT_EQ(project.Iwyu("#include <wrap.hpp>\nusing namespace lib;\nWidget w;\n").size(), 1);
    EXPECT_TRUE(project.Iwyu("#include <wrap.hpp>\nnamespace other { Widget w; }\n").empty());
    EXPECT_TRUE(project.Iwyu("#include <wrap.hpp>\nWidget w;\n").empty());
}

TEST(IncludeWhatYouUseSpec, FollowsPartialQualificationAndGlobalQualifier)
{
    Project project;
    project.Header("lib.hpp",
        "#pragma once\nnamespace outer { namespace inner { struct Widget {}; } }\nstruct Top {};\n");
    project.Header("wrap.hpp", "#pragma once\n#include <lib.hpp>\n");
    EXPECT_EQ(project.Iwyu("#include <wrap.hpp>\nouter::inner::Widget w;\n").size(), 1);
    EXPECT_EQ(project.Iwyu("#include <wrap.hpp>\ninner::Widget w;\n").size(), 1);
    EXPECT_EQ(project.Iwyu("#include <wrap.hpp>\n::Top t;\n").size(), 1);
    EXPECT_TRUE(project.Iwyu("#include <wrap.hpp>\nwrong::Widget w;\n").empty());
    EXPECT_TRUE(project.Iwyu("#include <wrap.hpp>\n::inner::Widget w;\n").empty());
}

TEST(IncludeWhatYouUseSpec, StaysSilentWhenTwoHeadersDeclareTheName)
{
    Project project;
    project.Header("a.hpp", "#pragma once\nstruct Twin {};\n");
    project.Header("b.hpp", "#pragma once\nstruct Twin {};\n");
    project.Header("both.hpp", "#pragma once\n#include <a.hpp>\n#include <b.hpp>\n");
    EXPECT_TRUE(project.Iwyu("#include <both.hpp>\nTwin t;\n").empty());
}

TEST(IncludeWhatYouUseSpec, IgnoresNamesTheFileDeclaresItself)
{
    Project project;
    project.Header("core.hpp", "#pragma once\nstruct Core {};\nint helper();\nstruct Token {};\n");
    project.Header("wrap.hpp", "#pragma once\n#include <core.hpp>\n");
    // A class of the same name, a parameter, a local and a template parameter.
    EXPECT_TRUE(project.Iwyu("#include <wrap.hpp>\nstruct Core {};\nCore value;\n").empty());
    EXPECT_TRUE(project.Iwyu("#include <wrap.hpp>\nvoid f(int helper) { helper = 1; }\n").empty());
    EXPECT_TRUE(project.Iwyu("#include <wrap.hpp>\nvoid g() { int Token = 1; Token++; }\n").empty());
    EXPECT_TRUE(project.Iwyu("#include <wrap.hpp>\ntemplate <typename Core> Core id(Core c) { return c; }\n").empty());
}

TEST(IncludeWhatYouUseSpec, ForwardDeclarationNeedsNoInclude)
{
    Project project;
    project.Header("core.hpp", "#pragma once\nstruct Core {};\n");
    project.Header("wrap.hpp", "#pragma once\n#include <core.hpp>\n");
    EXPECT_TRUE(project.Iwyu("#include <wrap.hpp>\nstruct Core;\nCore * pointer;\n").empty());
    EXPECT_TRUE(project.Iwyu("#include <wrap.hpp>\nclass Core;\nvoid take(Core &);\n").empty());
}

TEST(IncludeWhatYouUseSpec, MemberNamesAreNotUses)
{
    Project project;
    project.Header("core.hpp", "#pragma once\nstruct Core {};\nint size();\n");
    project.Header("wrap.hpp", "#pragma once\n#include <core.hpp>\n");
    EXPECT_TRUE(project.Iwyu("#include <wrap.hpp>\nstruct S { int size; };\nint f(S s) { return s.size; }\n").empty());
    EXPECT_TRUE(project.Iwyu("#include <wrap.hpp>\nint f(S * s) { return s->size; }\n").empty());
}

TEST(IncludeWhatYouUseSpec, BareFunctionNamesAreNotTrustedInsideClassesWithOutsideBases)
{
    Project project;
    project.Header("core.hpp", "#pragma once\nint helper();\n");
    project.Header("wrap.hpp", "#pragma once\n#include <core.hpp>\n");
    // `helper()` may be a member inherited from `Outside`.
    EXPECT_TRUE(project.Iwyu("#include <wrap.hpp>\nstruct S : Outside { int f() { return helper(); } };\n").empty());
    EXPECT_EQ(project.Iwyu("#include <wrap.hpp>\nint f() { return helper(); }\n").size(), 1);
}

TEST(IncludeWhatYouUseSpec, PrimaryHeaderIncludesCountAsIncluded)
{
    Project project;
    project.Header("core.hpp", "#pragma once\nstruct Core {};\n");
    project.Header("widget.hpp", "#pragma once\n#include <core.hpp>\nstruct Widget { Core core; };\n");
    EXPECT_TRUE(project.Iwyu("#include <widget.hpp>\nCore value;\n", "widget.cpp").empty());
    EXPECT_EQ(project.Iwyu("#include <widget.hpp>\nCore value;\n", "other.cpp").size(), 1);
}

TEST(IncludeWhatYouUseSpec, HonorsExportAndPrivatePragmas)
{
    Project project;
    project.Header("core.hpp", "#pragma once\nstruct Core {};\n");
    project.Header("umbrella.hpp", "#pragma once\n#include <core.hpp> // IWYU pragma: export\n");
    project.Header("plain.hpp", "#pragma once\n#include <core.hpp>\n");
    EXPECT_TRUE(project.Iwyu("#include <umbrella.hpp>\nCore value;\n").empty());
    EXPECT_EQ(project.Iwyu("#include <plain.hpp>\nCore value;\n").size(), 1);

    project.Header("hidden.hpp", "#pragma once\n// IWYU pragma: private\nstruct Hidden {};\n");
    project.Header("front.hpp", "#pragma once\n#include <hidden.hpp>\n");
    EXPECT_TRUE(project.Iwyu("#include <front.hpp>\nHidden value;\n").empty());
}

TEST(IncludeWhatYouUseSpec, NeverSuggestsTextualIncludes)
{
    Project project;
    project.Header("body.inc", "struct FromInc {};\n");
    project.Header("front.hpp", "#pragma once\n#include <body.inc>\n");
    EXPECT_TRUE(project.Iwyu("#include <front.hpp>\nFromInc value;\n").empty());
}

TEST(IncludeWhatYouUseSpec, StaysSilentWhenSomeIncludeIsUnknown)
{
    Project project;
    project.Header("core.hpp", "#pragma once\nstruct Core {};\n");
    project.Header("wrap.hpp", "#pragma once\n#include <core.hpp>\n");
    EXPECT_TRUE(project.Iwyu("#include <wrap.hpp>\n#include \"generated.hpp\"\nCore value;\n").empty());
    EXPECT_TRUE(project.Iwyu("#include <wrap.hpp>\n#include <missing.hpp>\nCore value;\n").empty());
    // A platform header that is only included under #if is not a gap.
    EXPECT_EQ(project.Iwyu("#include <wrap.hpp>\n#ifdef _WIN32\n#include <windows.h>\n#endif\nCore value;\n").size(),
        1);
}

TEST(IncludeWhatYouUseSpec, StaysSilentWithoutAProfile)
{
    const auto tree = heimdall::ParseTree::Parse("#include <wrap.hpp>\nCore value;\n", {});
    const auto model = heimdall::Binder::Bind(tree);
    const heimdall::ProjectContext context
    {
        "main.cpp", nullptr, nullptr
    };
    EXPECT_TRUE(heimdall::SemanticRules::AnalyzeIncludeWhatYouUse(model, context).empty());
}

TEST(IncludeWhatYouUseSpec, IgnoresInactiveBranchesAndCommentsAndStrings)
{
    Project project;
    project.Header("core.hpp", "#pragma once\nstruct Core {};\n");
    project.Header("wrap.hpp", "#pragma once\n#include <core.hpp>\n");
    EXPECT_TRUE(project.Iwyu("#include <wrap.hpp>\n// Core is documented\nconst char * s = \"Core\";\n").empty());
}

TEST(IncludeWhatYouUseSpec, FixKeepsTheFileLineEndingsAndGoesAfterTheLastInclude)
{
    Project project;
    project.Header("core.hpp", "#pragma once\nstruct Core {};\n");
    project.Header("wrap.hpp", "#pragma once\n#include <core.hpp>\n");
    constexpr std::string_view source =
        "#include <wrap.hpp>\r\n"
    "#ifdef X\r\n"
    "#include <extra.hpp>\r\n"
    "#endif\r\n"
    "\r\n"
    "Core value;\r\n";
    const auto diagnostics = project.Iwyu(source);
    ASSERT_EQ(diagnostics.size(), 1);
    EXPECT_EQ(Apply(source, diagnostics[0]),
        "#include <wrap.hpp>\r\n#include <core.hpp>\r\n"
        "#ifdef X\r\n"
        "#include <extra.hpp>\r\n"
        "#endif\r\n"
        "\r\n"
        "Core value;\r\n");
}

TEST(IncludeWhatYouUseSpec, FixAddsALineBreakWhenTheLastIncludeEndsTheFile)
{
    Project project;
    project.Header("core.hpp", "#pragma once\nstruct Core {};\n");
    project.Header("wrap.hpp", "#pragma once\n#include <core.hpp>\n");
    constexpr std::string_view source = "Core value;\n#include <wrap.hpp>";
    const auto diagnostics = project.Iwyu(source);
    ASSERT_EQ(diagnostics.size(), 1);
    EXPECT_EQ(Apply(source, diagnostics[0]), "Core value;\n#include <wrap.hpp>\n#include <core.hpp>");
}

TEST(IncludeWhatYouUseSpec, FixSpellsTheIncludeTheWayTheProjectDoes)
{
    Project project;
    project.Header("mylib/core.hpp", "#pragma once\nstruct Core {};\n");
    project.Header("mylib/wrap.hpp", "#pragma once\n#include <mylib/core.hpp>\n");

    // Angled includes of project headers elsewhere in the file: stay angled.
    auto diagnostics = project.Iwyu("#include <mylib/wrap.hpp>\nCore value;\n");
    ASSERT_EQ(diagnostics.size(), 1);
    EXPECT_EQ(diagnostics[0].fix.replacement, "#include <mylib/core.hpp>\n");

    // Only quoted project includes in the file: quoted.
    diagnostics = project.Iwyu("#include \"mylib/wrap.hpp\"\nCore value;\n");
    ASSERT_EQ(diagnostics.size(), 1);
    EXPECT_EQ(diagnostics[0].fix.replacement, "#include \"mylib/core.hpp\"\n");
}

TEST(IncludeWhatYouUseSpec, ReportsStandardNamesTakenFromAProjectHeader)
{
    Project project;
    project.Header("vector", "#pragma once\nnamespace std { template <class T> struct vector {}; }\n");
    project.Header("wrapper.hpp", "#pragma once\n#include <vector>\n");
    constexpr std::string_view source =
        "#include <wrapper.hpp>\n"
    "std::vector<int> values;\n";
    const auto diagnostics = project.Iwyu(source);
    ASSERT_EQ(diagnostics.size(), 1);
    EXPECT_EQ(source.substr(diagnostics[0].offset, diagnostics[0].length), "vector");
    EXPECT_NE(diagnostics[0].message.find("std::vector"), std::string::npos);
    EXPECT_EQ(Apply(source, diagnostics[0]),
        "#include <wrapper.hpp>\n#include <vector>\n"
        "std::vector<int> values;\n");

    EXPECT_TRUE(project.Iwyu("#include <wrapper.hpp>\n#include <vector>\nstd::vector<int> values;\n").empty());
}

TEST(IncludeWhatYouUseSpec, StandardNamesNeedTheHeaderSomewhereInTheClosure)
{
    Project project;
    project.Header("wrapper.hpp", "#pragma once\n");
    // `std::vector` with no <vector> anywhere is a compile error, not a missing-include hint.
    EXPECT_TRUE(project.Iwyu("#include <wrapper.hpp>\nstd::vector<int> values;\n").empty());
}

TEST(IncludeWhatYouUseSpec, StandardNamesAcceptAnyOfTheirHeaders)
{
    Project project;
    project.Header("cstdlib", "#pragma once\n");
    project.Header("cmath", "#pragma once\n");
    project.Header("wrapper.hpp", "#pragma once\n#include <cstdlib>\n#include <cmath>\n");
    EXPECT_TRUE(project.Iwyu("#include <wrapper.hpp>\n#include <cmath>\nauto x = std::abs(-1);\n").empty());
    EXPECT_EQ(project.Iwyu("#include <wrapper.hpp>\nauto x = std::abs(-1);\n").size(), 1);
}

TEST(IncludeWhatYouUseSpec, StandardChronoIsJudgedAtTheFirstNamespace)
{
    Project project;
    project.Header("chrono", "#pragma once\n");
    project.Header("wrapper.hpp", "#pragma once\n#include <chrono>\n");
    constexpr std::string_view source = "#include <wrapper.hpp>\nauto d = std::chrono::seconds(1);\n";
    const auto diagnostics = project.Iwyu(source);
    ASSERT_EQ(diagnostics.size(), 1);
    EXPECT_EQ(source.substr(diagnostics[0].offset, diagnostics[0].length), "chrono");
}

TEST(IncludeWhatYouUseSpec, LeavesNestedStdLookalikesAlone)
{
    Project project;
    project.Header("vector", "#pragma once\n");
    project.Header("wrapper.hpp", "#pragma once\n#include <vector>\n");
    EXPECT_TRUE(project.Iwyu("#include <wrapper.hpp>\nmine::std::vector<int> values;\n").empty());
    EXPECT_TRUE(project.Iwyu("#include <wrapper.hpp>\nauto n = values.std;\n").empty());
}

// ---------------------------------------------------------------------------
// cpp/modernize-final

TEST(ModernizeFinalSpec, ReportsLeafPolymorphicClassInASourceFile)
{
    Project project;
    constexpr std::string_view source =
        "struct Base { virtual void f(); virtual ~Base(); };\n"
    "struct Leaf : Base { void f() override; };\n";
    const auto diagnostics = project.Final(source);
    ASSERT_EQ(diagnostics.size(), 1);
    EXPECT_EQ(diagnostics[0].code, "cpp/modernize-final");
    EXPECT_EQ(diagnostics[0].rule, heimdall::RuleId::ModernizeFinal);
    EXPECT_EQ(diagnostics[0].line, 2);
    EXPECT_EQ(source.substr(diagnostics[0].offset, diagnostics[0].length), "Leaf");
    ASSERT_TRUE(diagnostics[0].has_fix);
    EXPECT_FALSE(diagnostics[0].fix_is_safe);
    EXPECT_EQ(Apply(source, diagnostics[0]),
        "struct Base { virtual void f(); virtual ~Base(); };\n"
        "struct Leaf final : Base { void f() override; };\n");
}

TEST(ModernizeFinalSpec, ReportsAClassWithItsOwnVirtualFunctions)
{
    Project project;
    const auto diagnostics = project.Final("class Solo { public: virtual void run(); };\n");
    ASSERT_EQ(diagnostics.size(), 1);
    EXPECT_NE(diagnostics[0].message.find("'Solo'"), std::string::npos);
}

TEST(ModernizeFinalSpec, SilentWhenAnotherClassDerives)
{
    Project project;
    const auto diagnostics = project.Final(
        "struct Base { virtual void f(); };\n"
        "struct Mid : Base { void f() override; };\n"
        "struct Leaf : Mid { void f() override; };\n");
    ASSERT_EQ(diagnostics.size(), 1);
    EXPECT_EQ(diagnostics[0].line, 3);
}

TEST(ModernizeFinalSpec, SilentWhenTheClassIsNotPolymorphic)
{
    Project project;
    EXPECT_TRUE(project.Final("struct Plain { int x; void f(); };\nstruct Next : Plain {};\n").empty());
    EXPECT_TRUE(project.Final("struct Alone {};\n").empty());
}

TEST(ModernizeFinalSpec, SilentForAbstractTemplateUnionAndAlreadyFinalClasses)
{
    Project project;
    EXPECT_TRUE(project.Final("struct Abstract { virtual void f() = 0; };\n").empty());
    EXPECT_TRUE(project.Final("struct Base { virtual void f(); };\n"
        "struct Sealed final : Base { void f() override; };\n").size() == 0);
    EXPECT_TRUE(project.Final("template <class T> struct Box { virtual void f(); };\n").empty());
}

TEST(ModernizeFinalSpec, SilentWhenSomeBaseOfAnotherClassIsUnresolvedAndSharesTheName)
{
    Project project;
    // `Mid<int>` may well be `Leaf`'s derived class through a template-id.
    EXPECT_TRUE(project.Final(
        "struct Leaf { virtual void f(); };\n"
        "template <class T> struct Mid : Leaf {};\n"
        "struct Other : ns::Leaf {};\n").empty());
}

TEST(ModernizeFinalSpec, HeaderClassesAreOnlyReportedInAnAnonymousNamespace)
{
    Project project;
    constexpr std::string_view source =
        "struct Exported { virtual void f(); };\n"
    "namespace { struct Hidden { virtual void g(); }; }\n"
    "namespace lib { namespace { struct Inner { virtual void h(); }; } }\n";
    const auto diagnostics = project.Final(source, "types.hpp");
    ASSERT_EQ(diagnostics.size(), 2);
    EXPECT_EQ(source.substr(diagnostics[0].offset, diagnostics[0].length), "Hidden");
    EXPECT_EQ(source.substr(diagnostics[1].offset, diagnostics[1].length), "Inner");
}

TEST(ModernizeFinalSpec, SourceFileClassesCountEvenInNamedNamespaces)
{
    Project project;
    EXPECT_EQ(project.Final("namespace lib { struct A { virtual void f(); }; }\n", "a.cpp").size(), 1);
    EXPECT_EQ(project.Final("namespace lib { struct A { virtual void f(); }; }\n", "a.cc").size(), 1);
    EXPECT_TRUE(project.Final("namespace lib { struct A { virtual void f(); }; }\n", "a.hpp").empty());
    EXPECT_TRUE(project.Final("namespace lib { struct A { virtual void f(); }; }\n", "a.inl").empty());
    EXPECT_TRUE(project.Final("namespace lib { struct A { virtual void f(); }; }\n", "noext").empty());
}

TEST(ModernizeFinalSpec, BaseFromAProjectHeaderMakesTheClassPolymorphic)
{
    Project project;
    project.Header("shape.hpp", "#pragma once\nstruct Shape { virtual ~Shape(); };\n");
    project.Header("plain.hpp", "#pragma once\nstruct Plain { int x; };\n");
    const std::string_view with_shape = "#include <shape.hpp>\nstruct Circle : Shape {};\n";
    EXPECT_EQ(project.Final(with_shape).size(), 1);
    // Without the include context the base is unknown: stay quiet.
    EXPECT_TRUE(project.Final(with_shape, "main.cpp", false).empty());
    EXPECT_TRUE(project.Final("#include <plain.hpp>\nstruct Box : Plain {};\n").empty());
}

TEST(ModernizeFinalSpec, BaseVirtualnessIsFollowedThroughHeaderChains)
{
    Project project;
    project.Header("a.hpp", "#pragma once\nstruct A { virtual void f(); };\n");
    project.Header("b.hpp", "#pragma once\n#include <a.hpp>\nstruct B : A {};\n");
    EXPECT_EQ(project.Final("#include <b.hpp>\nstruct C : B {};\n").size(), 1);
}

TEST(ModernizeFinalSpec, AHeaderClassDerivingFromTheNameBlocksTheReport)
{
    Project project;
    project.Header("derived.hpp", "#pragma once\nstruct Leaf2;\nstruct User : Leaf {};\n");
    EXPECT_TRUE(project.Final("#include <derived.hpp>\nstruct Leaf { virtual void f(); };\n").empty());
}

TEST(ModernizeFinalSpec, UnknownBaseInTheHeaderKeepsTheClassQuiet)
{
    Project project;
    project.Header("odd.hpp", "#pragma once\nstruct Odd : Elsewhere {};\n");
    EXPECT_TRUE(project.Final("#include <odd.hpp>\nstruct C : Odd {};\n").empty());
}

TEST(ModernizeFinalSpec, ReportsOverridesNothingOverrides)
{
    Project project;
    constexpr std::string_view source =
        "struct Base { virtual void a(); virtual void b(); };\n"
    "struct Mid : Base { void a() override; void b() override; };\n"
    "struct Leaf : Mid { void a() override; };\n";
    const auto diagnostics = project.Final(source);
    // `Leaf` itself becomes final; of Mid's overrides only `b` is never overridden below.
    ASSERT_EQ(diagnostics.size(), 2);
    EXPECT_EQ(source.substr(diagnostics[0].offset, diagnostics[0].length), "b");
    EXPECT_EQ(diagnostics[0].line, 2);
    EXPECT_EQ(Apply(source, diagnostics[0]),
        "struct Base { virtual void a(); virtual void b(); };\n"
        "struct Mid : Base { void a() override; void b() override final; };\n"
        "struct Leaf : Mid { void a() override; };\n");
    EXPECT_EQ(source.substr(diagnostics[1].offset, diagnostics[1].length), "Leaf");
}

TEST(ModernizeFinalSpec, AnOverrideWithTheNameInADerivedClassIsNeverReported)
{
    Project project;
    const auto diagnostics = project.Final(
        "struct Base { virtual void a(); };\n"
        "struct Mid : Base { void a() override; };\n"
        "struct Leaf : Mid { void a(int) ; };\n");
    for (const auto& diagnostic : diagnostics)
    {
        EXPECT_EQ(diagnostic.message.find("'a' is never overridden"),
            std::string::npos) << diagnostic.message;
    }
}

TEST(ModernizeFinalSpec, MethodsAreSilentWhenTheHierarchyMayBeOpen)
{
    Project project;
    constexpr std::string_view source =
        "struct Base { virtual void a(); };\n"
    "struct Mid : Base { void a() override; };\n"
    "struct Leaf : Mid { };\n";
    // A header: Mid may be derived from by files we do not see.
    EXPECT_TRUE(project.Final(source, "types.hpp").empty());
    // A name that something unresolved derives from.
    EXPECT_EQ(project.Final(std::string(source) + "struct Odd : ns::Mid {};\n", "main.cpp").size(), 1);
}

TEST(ModernizeFinalSpec, ReportsDeclarationOrderAndIgnoresAlreadyFinalMethods)
{
    Project project;
    const auto diagnostics = project.Final(
        "struct Base { virtual void a(); virtual void b(); };\n"
        "struct Mid : Base { void a() final; void b() override; };\n"
        "struct Leaf : Mid {};\n");
    ASSERT_EQ(diagnostics.size(), 2);
    EXPECT_EQ(diagnostics[0].line, 2);
    EXPECT_NE(diagnostics[0].message.find("'b'"), std::string::npos);
}

TEST(ModernizeFinalSpec, SurvivesCyclesAndBrokenInput)
{
    Project project;
    EXPECT_NO_THROW(project.Final("struct A : B { virtual void f(); };\nstruct B : A {};\n"));
    EXPECT_NO_THROW(project.Final("struct A : public { virtual\nclass\n"));
}

// ---------------------------------------------------------------------------
// Wiring

TEST(ProjectRulesSpec, AnalyzeWithContextAddsTheProjectRulesInOffsetOrder)
{
    Project project;
    project.Header("core.hpp", "#pragma once\nstruct Core {};\n");
    project.Header("wrap.hpp", "#pragma once\n#include <core.hpp>\n");
    constexpr std::string_view source =
        "#include <wrap.hpp>\n"
    "Core value;\n"
    "struct Solo { virtual void f(); };\n";
    const auto tree = heimdall::ParseTree::Parse(source, {});
    const auto model = heimdall::Binder::Bind(tree);
    const auto profile = heimdall::IncludeAnalyzer::BuildProfile(project.File("main.cpp"), tree,
        project.Command());
    const heimdall::ProjectContext context
    {
        project.File("main.cpp"), profile.get(), project.Command()
    };
    const auto types = heimdall::Typer::Type(model);

    const auto without = heimdall::SemanticRules::Analyze(model, types);
    const auto with = heimdall::SemanticRules::Analyze(model, types, context);
    EXPECT_EQ(with.size(), without.size() + 2);
    EXPECT_TRUE(std::is_sorted(with.begin(), with.end(),
        [](const auto& a, const auto& b)
        {
            return a.offset < b.offset;
    }));
    EXPECT_TRUE(std::any_of(with.begin(), with.end(),[](const auto& d)
        {
            return d.code == "cpp/include-what-you-use";
    }));
    EXPECT_TRUE(std::any_of(with.begin(), with.end(),[](const auto& d)
        {
            return d.code == "cpp/modernize-final";
    }));
}

TEST(ProjectRulesSpec, RulesAreInTheCatalogAndCanBeSwitchedOff)
{
    for (const std::string_view code :
        {
            "cpp/include-what-you-use", "cpp/modernize-final"
    })
    {
        EXPECT_TRUE(heimdall::IsKnownRuleCode(code)) << code;
        const auto* info = heimdall::FindRuleByCode(code);
        ASSERT_NE(info, nullptr);
        EXPECT_EQ(info->category, "cpp");
        EXPECT_EQ(info->layer, "semântica");
    }

    Project project;
    constexpr std::string_view source = "struct Solo { virtual void f(); };\n";
    const auto tree = heimdall::ParseTree::Parse(source, {});
    const auto model = heimdall::Binder::Bind(tree);
    const heimdall::ProjectContext context
    {
        project.File("main.cpp"), nullptr, nullptr
    };
    auto diagnostics = heimdall::SemanticRules::AnalyzeFinal(model, context);
    ASSERT_EQ(diagnostics.size(), 1);

    heimdall::RuleOptions options;
    options.overrides.push_back({"cpp/modernize-final", false, heimdall::Severity::Warning});
    EXPECT_TRUE(heimdall::RuleEngine(options).ApplyPolicy(diagnostics, tree).empty());
}
