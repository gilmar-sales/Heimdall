#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>

#include <Heimdall/ModuleUnit.hpp>
#include <Heimdall/ParseTree.hpp>

namespace
{
    using heimdall::GrammarKind;
    using heimdall::ModuleUnitKind;

    std::size_t Count(const heimdall::ParseTree& tree, GrammarKind kind)
    {
        std::size_t count = 0;
        const auto& soa = tree.NodesSoA();
        for (std::size_t node = 0; node < soa.size(); ++node)
        {
            count += soa.Kind(node) == kind ? 1 : 0;
        }

        return count;
    }

    heimdall::ModuleUnitInfo Extract(std::string_view source)
    {
        const auto tree = heimdall::ParseTree::Parse(source);
        return heimdall::ExtractModuleUnit(tree);
    }

    bool HasProblem(const heimdall::ModuleUnitInfo& info, std::string_view fragment)
    {
        for (const auto& problem : info.problems)
        {
            if (problem.message.find(fragment) != std::string::npos)
            {
                return true;
            }
        }

        return false;
    }
}

TEST(ModuleParseSpec, ExportNamespaceIsAnExportedNamespaceDefinition)
{
    const auto tree = heimdall::ParseTree::Parse(
        "export module m;\n"
        "export namespace app { int f(); }\n");

    EXPECT_TRUE(tree.Diagnostics().empty());
    EXPECT_EQ(Count(tree, GrammarKind::ExportDeclaration), 1u);
    EXPECT_EQ(Count(tree, GrammarKind::NamespaceDefinition), 1u);
}

TEST(ModuleParseSpec, ExportBlockOwnsItsDeclarations)
{
    const auto tree = heimdall::ParseTree::Parse(
        "export module m;\n"
        "export { int a; struct S {}; void f(); }\n"
        "int after;\n");

    EXPECT_TRUE(tree.Diagnostics().empty());
    EXPECT_EQ(Count(tree, GrammarKind::ExportDeclaration), 1u);
    EXPECT_EQ(Count(tree, GrammarKind::RecordDefinition), 1u);

    const auto& soa = tree.NodesSoA();
    std::size_t exportNode = 0;
    for (std::size_t node = 0; node < soa.size(); ++node)
    {
        if (soa.Kind(node) == GrammarKind::ExportDeclaration)
        {
            exportNode = node;
        }
    }

    std::size_t children = 0;
    for (std::size_t node = 0; node < soa.size(); ++node)
    {
        children += soa.Parent(node) == exportNode && node != exportNode ? 1 : 0;
    }

    EXPECT_GE(children, 3u);
}

TEST(ModuleParseSpec, UnclosedExportBlockReportsDiagnostic)
{
    const auto tree = heimdall::ParseTree::Parse("export module m;\nexport { int a;\n");

    EXPECT_FALSE(tree.Diagnostics().empty());
}

TEST(ModuleParseSpec, ExportedDeclarationsKeepTheirOwnKinds)
{
    const auto tree = heimdall::ParseTree::Parse(
        "export module m;\n"
        "export int g() { return 1; }\n"
        "export class C {};\n"
        "export using X = int;\n"
        "export enum class E { A };\n");

    EXPECT_TRUE(tree.Diagnostics().empty());
    EXPECT_EQ(Count(tree, GrammarKind::ExportDeclaration), 4u);
    EXPECT_EQ(Count(tree, GrammarKind::FunctionDefinition), 1u);
    EXPECT_EQ(Count(tree, GrammarKind::UsingDeclaration), 1u);
    EXPECT_GE(Count(tree, GrammarKind::RecordDefinition), 2u);
}

TEST(ModuleParseSpec, ModuleAndImportAreOrdinaryNamesWhenNotFollowedByModuleSyntax)
{
    const auto tree = heimdall::ParseTree::Parse(
        "int module = 3;\n"
        "int Use() { int import = module; return import; }\n"
        "module::Type value;\n");

    EXPECT_EQ(Count(tree, GrammarKind::ModuleDeclaration), 0u);
    EXPECT_EQ(Count(tree, GrammarKind::ImportDeclaration), 0u);
}

TEST(ModuleParseSpec, PrivateFragmentAndHeaderUnitsAreModuleDeclarations)
{
    const auto tree = heimdall::ParseTree::Parse(
        "module;\n"
        "#include <cstdio>\n"
        "export module m;\n"
        "import <vector>;\n"
        "import \"local.h\";\n"
        "module :private;\n"
        "int hidden();\n");

    EXPECT_TRUE(tree.Diagnostics().empty());
    EXPECT_EQ(Count(tree, GrammarKind::ModuleDeclaration), 3u);
    EXPECT_EQ(Count(tree, GrammarKind::ImportDeclaration), 2u);
}

TEST(ModuleUnitSpec, ClassifiesTheFourUnitKinds)
{
    EXPECT_EQ(Extract("export module a.b;").kind, ModuleUnitKind::Interface);
    EXPECT_EQ(Extract("module a.b;").kind, ModuleUnitKind::Implementation);
    EXPECT_EQ(Extract("export module a.b:p;").kind, ModuleUnitKind::PartitionInterface);
    EXPECT_EQ(Extract("module a.b:p;").kind, ModuleUnitKind::PartitionImplementation);
    EXPECT_EQ(Extract("int x;").kind, ModuleUnitKind::None);
}

TEST(ModuleUnitSpec, ReadsNamePartitionAndSpan)
{
    const std::string source = "export module app.core:detail;\n";
    const auto info = Extract(source);

    EXPECT_EQ(info.name, "app.core");
    EXPECT_EQ(info.partition, "detail");
    EXPECT_EQ(info.QualifiedName(), "app.core:detail");
    EXPECT_EQ(source.substr(info.nameOffset, info.nameLength), "app.core:detail");
    EXPECT_TRUE(info.problems.empty());
}

TEST(ModuleUnitSpec, ReadsImportsOfEveryForm)
{
    const auto info = Extract(
        "export module app;\n"
        "import std;\n"
        "import app.util;\n"
        "export import :detail;\n"
        "import <vector>;\n"
        "import \"local.h\";\n"
        "export { import app.extra; }\n");

    ASSERT_EQ(info.imports.size(), 6u);
    EXPECT_EQ(info.imports[0].module, "std");
    EXPECT_FALSE(info.imports[0].exported);
    EXPECT_EQ(info.imports[1].module, "app.util");
    EXPECT_EQ(info.imports[2].partition, "detail");
    EXPECT_TRUE(info.imports[2].exported);
    EXPECT_EQ(info.imports[3].header, "<vector>");
    EXPECT_TRUE(info.imports[3].IsHeaderUnit());
    EXPECT_EQ(info.imports[4].header, "\"local.h\"");
    EXPECT_EQ(info.imports[5].module, "app.extra");
    EXPECT_TRUE(info.imports[5].exported);
    EXPECT_TRUE(info.problems.empty());
}

TEST(ModuleUnitSpec, RecordsFragments)
{
    const auto info = Extract(
        "module;\n"
        "#include <cstdio>\n"
        "export module m;\n"
        "export int f();\n"
        "module :private;\n"
        "int hidden();\n");

    EXPECT_TRUE(info.hasGlobalFragment);
    EXPECT_TRUE(info.hasPrivateFragment);
    EXPECT_TRUE(info.problems.empty());
}

TEST(ModuleUnitSpec, RejectsDeclarationsInTheGlobalFragment)
{
    const auto info = Extract(
        "module;\n"
        "int leaked;\n"
        "export module m;\n");

    EXPECT_TRUE(HasProblem(info, "global module fragment"));
}

TEST(ModuleUnitSpec, RejectsDeclarationsBeforeTheModuleDeclaration)
{
    const auto info = Extract(
        "int early;\n"
        "export module m;\n");

    EXPECT_TRUE(HasProblem(info, "first declaration"));
}

TEST(ModuleUnitSpec, RejectsGlobalFragmentWithoutModuleDeclaration)
{
    EXPECT_TRUE(HasProblem(Extract("module;\n#include <x>\n"), "must be followed"));
}

TEST(ModuleUnitSpec, RejectsSecondModuleDeclaration)
{
    EXPECT_TRUE(HasProblem(Extract("export module a;\nexport module b;\n"), "only one module"));
}

TEST(ModuleUnitSpec, RejectsPrivateFragmentOutsidePrimaryInterface)
{
    EXPECT_TRUE(HasProblem(Extract("module a;\nmodule :private;\n"), "private module fragment"));
    EXPECT_TRUE(HasProblem(
        Extract("export module a:p;\nmodule :private;\n"), "private module fragment"));
    EXPECT_TRUE(Extract("export module a;\nmodule :private;\n").problems.empty());
}

TEST(ModuleUnitSpec, RejectsExportOutsideInterfaceUnits)
{
    EXPECT_TRUE(HasProblem(Extract("module a;\nexport int f();\n"), "module interface unit"));
    EXPECT_TRUE(HasProblem(Extract("int g;\nexport int f();\n"), "module interface unit"));
    EXPECT_TRUE(HasProblem(Extract("module a;\nexport import b;\n"), "module interface unit"));
    EXPECT_TRUE(Extract("export module a;\nexport int f();\n").problems.empty());
}

TEST(ModuleUnitSpec, RejectsExportInsidePrivateFragment)
{
    EXPECT_TRUE(HasProblem(
        Extract("export module a;\nmodule :private;\nexport int f();\n"), "private module"));
}

TEST(ModuleUnitSpec, RejectsSelfImport)
{
    EXPECT_TRUE(HasProblem(Extract("export module a;\nimport a;\n"), "import itself"));
    EXPECT_TRUE(Extract("export module a:p;\nimport a;\n").problems.empty());
}

TEST(ModuleUnitSpec, OrdinaryCodeProducesNoModuleFacts)
{
    const auto info = Extract("int module = 1;\nint f() { return module; }\n");

    EXPECT_FALSE(info.IsModuleUnit());
    EXPECT_TRUE(info.imports.empty());
    EXPECT_TRUE(info.problems.empty());
}

TEST(ModuleUnitSpec, ImportAtFindsTheImportUnderTheCursor)
{
    const std::string source =
        "export module app;\n"
        "import app.util;\n"
        "import <vector>;\n";
    const auto info = Extract(source);

    const auto* named = info.ImportAt(source.find("util"));
    const auto* header = info.ImportAt(source.find("vector"));

    ASSERT_NE(named, nullptr);
    EXPECT_EQ(named->module, "app.util");
    ASSERT_NE(header, nullptr);
    EXPECT_TRUE(header->IsHeaderUnit());
    EXPECT_EQ(info.ImportAt(source.find("export")), nullptr);
}

TEST(ModuleUnitSpec, ImportTargetQualifiesPartitionsWithTheOwningModule)
{
    const auto info = Extract(
        "export module app.core;\n"
        "import app.util;\n"
        "import :detail;\n"
        "import <vector>;\n");

    ASSERT_EQ(info.imports.size(), 3u);
    EXPECT_EQ(info.ImportTarget(info.imports[0]), "app.util");
    EXPECT_EQ(info.ImportTarget(info.imports[1]), "app.core:detail");
    EXPECT_TRUE(info.ImportTarget(info.imports[2]).empty());
}

TEST(ModuleUnitSpec, PartitionImportSpanStartsAtTheColon)
{
    const std::string source = "export module app;\nimport :detail;\n";
    const auto info = Extract(source);

    ASSERT_EQ(info.imports.size(), 1u);
    EXPECT_EQ(source.substr(info.imports[0].offset, info.imports[0].length), ":detail");
    EXPECT_NE(info.ImportAt(source.find(':', 19)), nullptr);
}
