#include <Heimdall/RuleScheduler.hpp>

#include <gtest/gtest.h>
#include <stdexcept>

namespace
{
    using namespace heimdall;
    AnalysisContext Context(std::string source)
    {
        auto text = std::make_shared<const std::string>(std::move(source));
        auto tree = std::make_shared<const ParseTree>(ParseTree::ParseSnapshot(text, {}));
        return AnalysisContext(AnalysisSnapshot::FromSyntax(std::move(tree)), 0);
    }

    ScheduledRule Returns(std::string code)
    {
        ScheduledRule rule;
        rule.metadata.code = std::move(code);
        rule.interests = {GrammarKind::ReturnStatement, GrammarKind::ReturnStatement};
        rule.on_node =[](const AnalysisContext& context, NodeId node, DiagnosticSink& sink)
        {
            sink.Emit(context.NodeRange(node), "return observed");
        };
        return rule;
    }

    TEST(RuleSchedulerSpec, DispatchesOnlyInterestedNodesAndProfilesEachRule)
    {
        auto context = Context("int f() { return 1; } int g() { return 2; }");
        RuleScheduler scheduler;
        ASSERT_TRUE(scheduler.Register(Returns("test/returns")));
        ASSERT_TRUE(scheduler.Register(Returns("test/other")));
        auto result = scheduler.Analyze(context, RuleEngine(), true);
        ASSERT_EQ(result.diagnostics.size(), 4);
        ASSERT_EQ(result.metrics.size(), 2);
        for (const auto& metrics : result.metrics)
        {
            EXPECT_EQ(metrics.execution_count, 2);
            EXPECT_EQ(metrics.diagnostics_emitted, 2);
            EXPECT_GT(metrics.total_ns, 0);
            EXPECT_GT(metrics.p95_ns, 0);
            EXPECT_GT(metrics.AverageNs(), 0);
        }

        EXPECT_FALSE(scheduler.Register(Returns("test/returns")));
    }

    TEST(RuleSchedulerSpec, SharesSeverityDisableAndSuppressionPolicy)
    {
        auto context = Context("int f() {\n// heimdall-disable-next-line test/returns\nreturn 1;\n}\n");
        RuleScheduler scheduler;
        ASSERT_TRUE(scheduler.Register(Returns("test/returns")));
        RuleOptions options;
        options.overrides.push_back({"test/returns", true, Severity::Error});
        auto result = scheduler.Analyze(context, RuleEngine(options));
        EXPECT_TRUE(result.diagnostics.empty());
        EXPECT_EQ(result.metrics[0].execution_count, 1);
        options.honor_suppressions = false;
        result = scheduler.Analyze(context, RuleEngine(options));
        ASSERT_EQ(result.diagnostics.size(), 1);
        EXPECT_EQ(result.diagnostics[0].severity, Severity::Error);
        options.overrides.push_back({"test/returns", false, Severity::Warning});
        result = scheduler.Analyze(context, RuleEngine(options));
        EXPECT_TRUE(result.diagnostics.empty());
        EXPECT_EQ(result.metrics[0].execution_count, 0);
    }

    TEST(RuleSchedulerSpec, RejectsInvalidRangesAndFixes)
    {
        auto context = Context("int x;");
        ScheduledRule rule;
        rule.metadata.code = "test/bounds";
        rule.on_document =[](const AnalysisContext&, DiagnosticSink& sink)
        {
            sink.Emit({999, 1}, "out of bounds");
            Diagnostic bad{};
            bad.has_fix = true;
            bad.fix = {1, 999, "bad"};
            sink.Emit(std::move(bad));
            sink.Emit({0, 3}, "valid");
        };
        RuleScheduler scheduler;
        ASSERT_TRUE(scheduler.Register(std::move(rule)));
        auto result = scheduler.Analyze(context, RuleEngine());
        ASSERT_EQ(result.diagnostics.size(), 1);
        EXPECT_EQ(result.metrics[0].rejected_diagnostics, 2);
        EXPECT_EQ(result.diagnostics[0].line, 1);
        EXPECT_EQ(result.diagnostics[0].column, 1);
    }

    TEST(RuleSchedulerSpec, CancellationDiscardsPartialDiagnostics)
    {
        auto context = Context("int f() { return 1; }");
        RuleScheduler scheduler;
        ASSERT_TRUE(scheduler.Register(Returns("test/returns")));
        std::stop_source stop;
        stop.request_stop();
        auto result = scheduler.Analyze(context, RuleEngine(), false, stop.get_token());
        EXPECT_TRUE(result.cancelled);
        EXPECT_TRUE(result.diagnostics.empty());
        EXPECT_EQ(result.metrics[0].execution_count, 0);
    }

    TEST(RuleSchedulerSpec, ExternalFailureDiscardsEarlierEmissionsOfThatRule)
    {
        auto context = Context("int f() { return 1; } int g() { return 2; }");
        RuleScheduler scheduler;
        auto rule = Returns("test/failing");
        rule.external = true;
        rule.on_node =[count = 0](const AnalysisContext& ctx, NodeId node, DiagnosticSink& sink) mutable
        {
            sink.Emit(ctx.NodeRange(node), "partial result");
            if (++count == 2)
            {
                throw std::runtime_error("failed later");
            }
        };
        ASSERT_TRUE(scheduler.Register(std::move(rule)));
        auto result = scheduler.Analyze(context, RuleEngine());
        EXPECT_TRUE(result.diagnostics.empty());
        EXPECT_TRUE(result.metrics[0].failed);
        EXPECT_EQ(result.metrics[0].diagnostics_emitted, 0);
    }
}
