#include <Heimdall/AnalysisFeatures.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <iterator>

namespace
{
    using namespace heimdall;

    RuleOptions DisabledRules()
    {
        RuleOptions options;
        for (const auto& rule : RuleCatalog())
        {
            options.overrides.push_back({std::string(rule.code), false, rule.default_severity});
        }

        return options;
    }

    std::vector<Diagnostic> Legacy(const AnalysisContext& context, const RuleEngine& engine,
        const ProjectContext& project = {})
    {
        auto all = engine.Analyze(context.Syntax());
        auto semantic = SemanticRules::Analyze(context.Semantic(), context.Types(), project);
        auto doc = SemanticRules::AnalyzeDocumentation(context.Semantic(), engine);
        semantic.insert(semantic.end(), std::make_move_iterator(doc.begin()),
            std::make_move_iterator(doc.end()));
        semantic = engine.ApplyPolicy(std::move(semantic), context.Syntax());
        all.insert(all.end(), std::make_move_iterator(semantic.begin()),
            std::make_move_iterator(semantic.end()));
        std::stable_sort(all.begin(), all.end(),
            [](const Diagnostic& a, const Diagnostic& b)
            {
                return a.offset < b.offset;
        });
        return all;
    }

    void ExpectEquivalent(const std::vector<Diagnostic>& actual,
        const std::vector<Diagnostic>& expected)
    {
        ASSERT_EQ(actual.size(), expected.size());
        for (std::size_t i = 0; i < actual.size(); ++i)
        {
            SCOPED_TRACE(i);
            const auto& a = actual[i];
            const auto& b = expected[i];
            EXPECT_EQ(a.rule, b.rule);
            EXPECT_EQ(a.code, b.code);
            EXPECT_EQ(a.message, b.message);
            EXPECT_EQ(a.severity, b.severity);
            EXPECT_EQ(a.offset, b.offset);
            EXPECT_EQ(a.length, b.length);
            EXPECT_EQ(a.line, b.line);
            EXPECT_EQ(a.column, b.column);
            EXPECT_EQ(a.has_fix, b.has_fix);
            EXPECT_EQ(a.fix.offset, b.fix.offset);
            EXPECT_EQ(a.fix.length, b.fix.length);
            EXPECT_EQ(a.fix.replacement, b.fix.replacement);
            EXPECT_EQ(a.fix_is_safe, b.fix_is_safe);
            EXPECT_EQ(a.fix_title, b.fix_title);
        }
    }

    TEST(SemanticDispatchSpec, DisabledRulesDoNotBuildSemanticModels)
    {
        Workspace workspace;
        auto id = workspace.Open("disabled.cpp", std::make_shared<const std::string>("int *p = 0;\n"));
        ASSERT_TRUE(id);
        AnalysisContext context(workspace.Snapshot(), *id);
        EXPECT_TRUE(AnalysisFeatures::Diagnostics(context, RuleEngine(DisabledRules()), true).empty());
        EXPECT_EQ(context.Snapshot().Metrics().parse_count, 1);
        EXPECT_EQ(context.Snapshot().Metrics().bind_count, 0);
        EXPECT_EQ(context.Snapshot().Metrics().type_count, 0);
    }

    TEST(SemanticDispatchSpec, EachNativeRuleRequestsOnlyItsModelDomain)
    {
        for (const auto& rule : RuleCatalog())
        {
            // IncludeAnalyzer's rules are dispatched separately by CLI/LSP.
            if (rule.id < RuleId::ModernizeOverride)
            {
                continue;
            }

            // Purely syntactic rules run in RuleEngine::Analyze and never
            // request semantic models.
            const bool syntax_only = rule.id == RuleId::ModernizeEmplace ||
                rule.id == RuleId::ModernizeMakeUnique || rule.id == RuleId::ModernizeMakeShared ||
                rule.id == RuleId::ModernizeSmartPtr || rule.id == RuleId::NoNewDelete ||
                rule.id == RuleId::ModernizeAlgorithms || rule.id == RuleId::ModernizeStructuredBindings;
            if (syntax_only)
            {
                SCOPED_TRACE(rule.code);
                Workspace workspace;
                auto id = workspace.Open("requirements.cpp",
                    std::make_shared<const std::string>("int f(int x) { return x; }\n"));
                ASSERT_TRUE(id);
                auto options = DisabledRules();
                options.overrides.push_back({std::string(rule.code), true, rule.default_severity});
                AnalysisContext context(workspace.Snapshot(), *id);
                (void) AnalysisFeatures::Diagnostics(context, RuleEngine(options), true);
                EXPECT_EQ(context.Snapshot().Metrics().bind_count, 0);
                EXPECT_EQ(context.Snapshot().Metrics().type_count, 0);
                EXPECT_EQ(context.Snapshot().Metrics().project_index_count, 0);
                continue;
            }

            SCOPED_TRACE(rule.code);
            Workspace workspace;
            auto id = workspace.Open("requirements.cpp",
                std::make_shared<const std::string>("int f(int x) { return x; }\n"));
            ASSERT_TRUE(id);
            auto options = DisabledRules();
            options.overrides.push_back({std::string(rule.code), true, rule.default_severity});
            AnalysisContext context(workspace.Snapshot(), *id);
            (void) AnalysisFeatures::Diagnostics(context, RuleEngine(options), true);
            const bool typed = rule.id == RuleId::NoImplicitBoolConversion ||
                rule.id == RuleId::ModernizeRangeLoop || rule.id == RuleId::ModernizeLoopConvert ||
                rule.id == RuleId::ModernizeConst || rule.id == RuleId::ModernizeConstexpr ||
                rule.id == RuleId::ModernizeSpan || rule.id == RuleId::ModernizeAttributes ||
                rule.id == RuleId::ApiMissingNodiscard || rule.id == RuleId::ApiPassByValue ||
                rule.id == RuleId::ApiPassByConstReference || rule.id == RuleId::ApiConstCorrectness ||
                rule.id == RuleId::ApiUnsafeDowncast || rule.id == RuleId::ApiSlicing;
            EXPECT_EQ(context.Snapshot().Metrics().bind_count, 1);
            EXPECT_EQ(context.Snapshot().Metrics().type_count, typed ? 1 : 0);
            EXPECT_EQ(context.Snapshot().Metrics().project_index_count, 0);
        }
    }

    TEST(SemanticDispatchSpec, SyntaxOnlyModeNeverRequestsSemanticModels)
    {
        Workspace workspace;
        auto id = workspace.Open("syntax.cpp", std::make_shared<const std::string>("int *p = NULL;\n"));
        ASSERT_TRUE(id);
        AnalysisContext context(workspace.Snapshot(), *id);
        RuleEngine engine;
        ExpectEquivalent(AnalysisFeatures::Diagnostics(context, engine, false),
            engine.Analyze(context.Syntax()));
        EXPECT_EQ(context.Snapshot().Metrics().bind_count, 0);
        EXPECT_EQ(context.Snapshot().Metrics().type_count, 0);
    }

    TEST(SemanticDispatchSpec, DiagnosticsAndFixesMatchLegacyWithConfigurationAndSuppressions)
    {
        const std::string sources[] = {
            R"cpp(struct Base { virtual void run(int); };
struct Derived : Base { Derived(int); void run(int) {} void run(double); };
struct Values { int a; int b; int *ptr; };
Values values{.b = 1, .a = 2, .ptr = 20};
int *make() { return 0; }
int f(int value) {
    int *p = 0;
    int *q = (int*)0;
    int x = static_cast<int>(value);
    if (p && value) return x;
    int items[3];
    for (int i = 0; i < items.size(); ++i) { value += items[i]; }
    return value;
}
)cpp",
            R"cpp(// heimdall-disable-next-line cpp/no-zero-as-null
int *hidden = 0;
int *visible = 0;
int *line = 0; // heimdall-disable-line cpp/no-zero-as-null
/// @brief Example.
/// @param nonexistent invalid
int documented(int argument) { return argument; }
int undocumented(int argument) { return argument; }
)cpp",
            "#define NIL 0\nint *p = NIL;\nint incomplete(int x) { if (x)\n"
        };
        for (const auto& source : sources)
        {
            Workspace workspace;
            auto id = workspace.Open("equivalence.cpp", std::make_shared<const std::string>(source));
            ASSERT_TRUE(id);
            AnalysisContext context(workspace.Snapshot(), *id);
            ProjectContext project;
            project.file = "equivalence.cpp";
            RuleEngine defaults;
            ExpectEquivalent(AnalysisFeatures::Diagnostics(context, defaults, true, project),
                Legacy(context, defaults, project));
            for (const auto& rule : RuleCatalog())
            {
                if (rule.id < RuleId::ModernizeOverride)
                {
                    continue;
                }

                SCOPED_TRACE(rule.code);
                auto options = DisabledRules();
                options.doc_scope = DocScope::All;
                options.overrides.push_back({std::string(rule.code), true, Severity::Error});
                RuleEngine engine(options);
                auto actual = AnalysisFeatures::Diagnostics(context, engine, true, project);
                auto expected = Legacy(context, engine, project);
                ExpectEquivalent(actual, expected);
                EXPECT_EQ(RuleEngine::ApplyFixes(source, actual), RuleEngine::ApplyFixes(source, expected));
                EXPECT_EQ(RuleEngine::ApplyFixes(source, actual, true),
                    RuleEngine::ApplyFixes(source, expected, true));
            }
        }
    }

    TEST(SemanticDispatchSpec, LastOverrideWinsAndDocumentationRemainsOptIn)
    {
        Workspace workspace;
        auto id = workspace.Open("options.cpp", std::make_shared<const std::string>("int *p = 0;\n"));
        ASSERT_TRUE(id);
        AnalysisContext context(workspace.Snapshot(), *id);
        auto options = DisabledRules();
        options.overrides.push_back({"cpp/no-zero-as-null", true, Severity::Error});
        auto diagnostics = AnalysisFeatures::Diagnostics(context, RuleEngine(options), true);
        ASSERT_EQ(diagnostics.size(), 1);
        EXPECT_EQ(diagnostics[0].severity, Severity::Error);
        EXPECT_EQ(context.Snapshot().Metrics().type_count, 0);
        options.overrides.push_back({"cpp/no-zero-as-null", false, Severity::Warning});
        EXPECT_TRUE(AnalysisFeatures::Diagnostics(context, RuleEngine(options), true).empty());
    }

    TEST(SemanticDispatchSpec, PreservesDiagnosticOrderWhenManyRulesShareOffsets)
    {
        std::string source;
        for (int i = 0; i < 30; ++i)
        {
            source += "struct Type" + std::to_string(i) +
                " { Type" + std::to_string(i) + "(int); virtual void run(); };\n"
            "/// @brief Example.\n/// @param unknown invalid\n"
            "int function" + std::to_string(i) + "(int value) { return value; }\n";
        }

        Workspace workspace;
        auto id = workspace.Open("ties.cpp", std::make_shared<const std::string>(source));
        ASSERT_TRUE(id);
        AnalysisContext context(workspace.Snapshot(), *id);
        RuleOptions options;
        options.doc_scope = DocScope::All;
        options.overrides.push_back({"doc/require-comment", true, Severity::Warning});
        options.overrides.push_back({"doc/doxygen-style", true, Severity::Warning});
        RuleEngine engine(options);
        auto expected = Legacy(context, engine);
        ASSERT_GT(expected.size(), 30);
        ASSERT_TRUE(std::adjacent_find(expected.begin(), expected.end(),
            [](const Diagnostic& a, const Diagnostic& b)
            {
                return a.offset == b.offset;
            }) != expected.end());
        ExpectEquivalent(AnalysisFeatures::Diagnostics(context, engine, true), expected);
    }
}
