#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include <Heimdall/ModuleGraph.hpp>
#include <Heimdall/ParseTree.hpp>

namespace
{
    using heimdall::ModuleIssueKind;

    void Add(heimdall::ModuleGraph& graph, std::string file, std::string_view source)
    {
        const auto tree = heimdall::ParseTree::Parse(source);
        graph.Set(std::move(file), heimdall::ExtractModuleUnit(tree));
    }

    std::size_t CountKind(const std::vector<heimdall::ModuleIssue>& issues, ModuleIssueKind kind)
    {
        return static_cast<std::size_t>(std::ranges::count_if(
            issues,[kind](const heimdall::ModuleIssue& issue)
            {
                return issue.kind == kind;
            }));
    }
}

TEST(ModuleGraphSpec, ResolvesInterfacesPartitionsAndImplementations)
{
    heimdall::ModuleGraph graph;
    Add(graph, "app.cppm", "export module app;\nexport import :detail;\n");
    Add(graph, "detail.cppm", "export module app:detail;\n");
    Add(graph, "app_impl.cpp", "module app;\n");

    EXPECT_EQ(graph.InterfaceOf("app"), "app.cppm");
    EXPECT_EQ(graph.PartitionOf("app", "detail"), "detail.cppm");
    EXPECT_EQ(graph.ImplementationsOf("app"), (std::vector<std::string> {"app_impl.cpp"}));
    EXPECT_TRUE(graph.InterfaceOf("missing").empty());
    EXPECT_TRUE(graph.PartitionOf("app", "missing").empty());
}

TEST(ModuleGraphSpec, DependenciesFollowImportsAndTheImplicitInterfaceImport)
{
    heimdall::ModuleGraph graph;
    Add(graph, "util.cppm", "export module util;\n");
    Add(graph, "app.cppm", "export module app;\nimport util;\nimport <vector>;\nimport std;\n");
    Add(graph, "app_impl.cpp", "module app;\n");

    EXPECT_EQ(graph.Dependencies("app.cppm"), (std::vector<std::string> {"util.cppm"}));
    EXPECT_EQ(graph.Dependencies("app_impl.cpp"), (std::vector<std::string> {"app.cppm"}));
    EXPECT_TRUE(graph.Dependencies("util.cppm").empty());
    EXPECT_TRUE(graph.Dependencies("unknown.cpp").empty());
}

TEST(ModuleGraphSpec, BuildOrderPutsDependenciesFirstAndIsDeterministic)
{
    heimdall::ModuleGraph graph;
    Add(graph, "c_main.cpp", "import app;\nint main() {}\n");
    Add(graph, "b_app.cppm", "export module app;\nimport util;\n");
    Add(graph, "a_util.cppm", "export module util;\n");

    const auto result = graph.BuildOrder();

    EXPECT_EQ(result.order,
              (std::vector<std::string> {"a_util.cppm", "b_app.cppm", "c_main.cpp"}));
    EXPECT_TRUE(result.cyclic.empty());
}

TEST(ModuleGraphSpec, ReportsEveryImportThatClosesACycle)
{
    heimdall::ModuleGraph graph;
    Add(graph, "a.cppm", "export module a;\nimport b;\n");
    Add(graph, "b.cppm", "export module b;\nimport c;\n");
    Add(graph, "c.cppm", "export module c;\nimport a;\n");
    Add(graph, "d.cppm", "export module d;\nimport a;\n");

    const auto issues = graph.Check();
    const auto order = graph.BuildOrder();

    EXPECT_EQ(CountKind(issues, ModuleIssueKind::ImportCycle), 3u);
    for (const auto& issue : issues)
    {
        EXPECT_NE(issue.file, "d.cppm");
    }

    EXPECT_TRUE(order.order.empty());
    EXPECT_EQ(order.cyclic.size(), 4u);
}

TEST(ModuleGraphSpec, AcyclicDiamondHasNoCycleIssue)
{
    heimdall::ModuleGraph graph;
    Add(graph, "base.cppm", "export module base;\n");
    Add(graph, "left.cppm", "export module left;\nimport base;\n");
    Add(graph, "right.cppm", "export module right;\nimport base;\n");
    Add(graph, "top.cppm", "export module top;\nimport left;\nimport right;\n");

    EXPECT_TRUE(graph.Check().empty());
    EXPECT_EQ(graph.BuildOrder().order.front(), "base.cppm");
    EXPECT_EQ(graph.BuildOrder().order.back(), "top.cppm");
}

TEST(ModuleGraphSpec, ReportsADuplicatePrimaryInterfaceOnTheLaterFile)
{
    heimdall::ModuleGraph graph;
    Add(graph, "a.cppm", "export module m;\n");
    Add(graph, "b.cppm", "export module m;\n");

    const auto issues = graph.Check();

    ASSERT_EQ(issues.size(), 1u);
    EXPECT_EQ(issues[0].kind, ModuleIssueKind::DuplicateInterface);
    EXPECT_EQ(issues[0].file, "b.cppm");
    EXPECT_NE(issues[0].message.find("a.cppm"), std::string::npos);
}

TEST(ModuleGraphSpec, ReportsAnInterfacePartitionThePrimaryInterfaceDoesNotExport)
{
    heimdall::ModuleGraph graph;
    Add(graph, "m.cppm", "export module m;\nimport :hidden;\nexport import :shown;\n");
    Add(graph, "hidden.cppm", "export module m:hidden;\n");
    Add(graph, "shown.cppm", "export module m:shown;\n");

    const auto issues = graph.Check();

    ASSERT_EQ(issues.size(), 1u);
    EXPECT_EQ(issues[0].kind, ModuleIssueKind::PartitionNotExported);
    EXPECT_EQ(issues[0].file, "hidden.cppm");
}

TEST(ModuleGraphSpec, UnresolvedImportsAreOptInAndIgnoreToolchainModules)
{
    heimdall::ModuleGraph graph;
    Add(graph, "app.cppm", "export module app;\nimport std;\nimport missing.mod;\nimport <vector>;\n");

    EXPECT_TRUE(graph.Check().empty());

    heimdall::ModuleCheckOptions options;
    options.reportUnresolved = true;
    const auto issues = graph.Check(options);

    ASSERT_EQ(issues.size(), 1u);
    EXPECT_EQ(issues[0].kind, ModuleIssueKind::UnresolvedImport);
    EXPECT_NE(issues[0].message.find("missing.mod"), std::string::npos);
}

TEST(ModuleGraphSpec, SetReplacesAndRemoveForgetsAFile)
{
    heimdall::ModuleGraph graph;
    Add(graph, "a.cppm", "export module a;\n");
    Add(graph, "a.cppm", "export module renamed;\n");

    EXPECT_EQ(graph.Size(), 1u);
    EXPECT_TRUE(graph.InterfaceOf("a").empty());
    EXPECT_EQ(graph.InterfaceOf("renamed"), "a.cppm");

    graph.Remove("a.cppm");
    graph.Remove("never-added.cppm");

    EXPECT_EQ(graph.Size(), 0u);
}
