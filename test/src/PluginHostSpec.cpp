#include <Heimdall/PluginHost.hpp>
#include <Heimdall/AnalysisEngine.hpp>

#include <gtest/gtest.h>

#include <stdexcept>

namespace
{
    using namespace heimdall;

    Plugin Returns(std::string name, std::string code)
    {
        Plugin plugin;
        plugin.name = std::move(name);
        PluginRule rule;
        rule.code = std::move(code);
        rule.interests = {PluginNodeKind::Return};
        rule.on_node = [](const PluginContext &context, NodeId node, std::vector<PluginDiagnostic> &out)
        {
            out.push_back({context.NodeRange(node), "experimental return check"});
        };
        plugin.rules.push_back(std::move(rule));
        return plugin;
    }

    AnalysisContext Context()
    {
        auto text = std::make_shared<const std::string>("int value; int f() { return value; }\n");
        auto tree = std::make_shared<const ParseTree>(ParseTree::ParseSnapshot(text, {}));
        return AnalysisContext(AnalysisSnapshot::FromSyntax(tree), 0);
    }

    TEST(PluginHostSpec, VersionDuplicateAndNativeShadowValidationIsTransactional)
    {
        PluginHost host;
        auto wrong = Returns("wrong", "test/wrong");
        wrong.required_api = PluginApiVersion + 1;
        EXPECT_FALSE(host.Register(std::move(wrong)));
        EXPECT_EQ(host.Size(), 0);
        ASSERT_TRUE(host.Register(Returns("valid", "test/valid")));
        EXPECT_FALSE(host.Register(Returns("valid", "test/other")));
        EXPECT_FALSE(host.Register(Returns("other", "test/valid")));
        EXPECT_FALSE(host.Register(Returns("native", "cpp/modernize-override")));
        auto partial = Returns("partial", "test/partial");
        partial.rules.push_back(Returns("ignored", "test/valid").rules[0]);
        EXPECT_FALSE(host.Register(std::move(partial)));
        ASSERT_TRUE(host.Register(Returns("complete", "test/partial")));
        EXPECT_EQ(host.Size(), 2);
    }

    TEST(PluginHostSpec, HandlesBatchQueriesAndDiagnosticsUseOneSnapshot)
    {
        PluginHost host;
        Plugin plugin;
        plugin.name = "query-test";
        PluginRule rule;
        rule.code = "test/queries";
        rule.interests = {PluginNodeKind::Return};
        rule.on_node = [](const PluginContext &context, NodeId node, std::vector<PluginDiagnostic> &out)
        {
            EXPECT_EQ(context.NodeKind(node), PluginNodeKind::Return);
            EXPECT_EQ(context.Document(), 0);
            EXPECT_FALSE(context.Children(node).empty());
            EXPECT_FALSE(context.Descendants(node).empty());
            EXPECT_TRUE(context.Children(InvalidNode).empty());
            EXPECT_TRUE(context.Descendants(InvalidNode).empty());
            EXPECT_EQ(context.NodeRange(InvalidNode).length, 0);
            const auto range = context.NodeRange(node);
            const auto symbol = context.ResolveSymbol(range.offset + 7);
            EXPECT_NE(symbol, InvalidHandle);
            EXPECT_EQ(context.String(context.SymbolName(symbol)), "value");
            EXPECT_NE(context.SymbolType(symbol), 0);
            EXPECT_FALSE(context.References(symbol).empty());
            EXPECT_FALSE(context.SymbolsInScope(range.offset).empty());
            EXPECT_EQ(context.SymbolName(InvalidHandle), InvalidHandle);
            EXPECT_TRUE(context.String(InvalidHandle).empty());
            out.push_back({range, "valid query diagnostic"});
            out.push_back({{9999, 1}, "invalid diagnostic"});
        };
        plugin.rules.push_back(std::move(rule));
        ASSERT_TRUE(host.Register(std::move(plugin)));
        auto result = host.Analyze(Context(), RuleEngine(), true);
        ASSERT_EQ(result.diagnostics.size(), 1);
        EXPECT_EQ(result.diagnostics[0].rule, RuleId::External);
        EXPECT_EQ(result.diagnostics[0].code, "test/queries");
        EXPECT_EQ(result.metrics[0].rejected_diagnostics, 1);
    }

    TEST(PluginHostSpec, ExceptionIsContainedAndOtherRulesContinue)
    {
        PluginHost host;
        auto broken = Returns("broken", "test/broken");
        broken.rules[0].on_node = [](const PluginContext &, NodeId, std::vector<PluginDiagnostic> &)
        { throw std::runtime_error("plugin failure"); };
        ASSERT_TRUE(host.Register(std::move(broken)));
        ASSERT_TRUE(host.Register(Returns("valid", "test/valid")));
        auto result = host.Analyze(Context(), RuleEngine());
        ASSERT_EQ(result.diagnostics.size(), 1);
        EXPECT_EQ(result.diagnostics[0].code, "test/valid");
        EXPECT_TRUE(result.metrics[0].failed);
        EXPECT_FALSE(result.metrics[1].failed);
    }

    TEST(PluginHostSpec, AnalysisEngineCoordinatesNativeAndPluginRules)
    {
        AnalysisEngine engine;
        ScheduledRule native;
        native.metadata.code = "test/native";
        native.interests = {GrammarKind::ReturnStatement};
        native.on_node = [](const AnalysisContext &context, NodeId node, DiagnosticSink &sink)
        { sink.Emit(context.NodeRange(node), "native direct SoA view"); };
        ASSERT_TRUE(engine.RegisterNative(std::move(native)));
        ASSERT_TRUE(engine.RegisterPlugin(Returns("plugin", "test/plugin")));
        auto result = engine.Analyze(Context(), false, {}, true);
        ASSERT_EQ(result.rules.size(), 2);
        EXPECT_EQ(result.rules[0].execution_count, 1);
        EXPECT_EQ(result.rules[1].execution_count, 1);
        EXPECT_EQ(std::count_if(result.diagnostics.begin(), result.diagnostics.end(),
            [](const Diagnostic &d) { return d.code == "test/plugin" || d.code == "test/native"; }), 2);
    }
}
