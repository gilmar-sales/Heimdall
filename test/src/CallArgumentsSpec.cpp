#include <algorithm>
#include <gtest/gtest.h>
#include <Heimdall/AnalysisFeatures.hpp>
#include <memory>
#include <string>
#include <vector>

namespace
{
    std::vector<heimdall::Diagnostic> AnalyzeCalls(const std::string& source)
    {
        heimdall::Workspace workspace;
        const auto file = workspace.Open("calls.cpp", std::make_shared<const std::string>(source));
        heimdall::AnalysisContext context(workspace.Snapshot(), *file);
        heimdall::RuleOptions options;
        options.overrides.push_back({"cpp/call-argument-count", true, heimdall::Severity::Error});
        options.overrides.push_back({"cpp/call-argument-type", true, heimdall::Severity::Error});
        auto diagnostics = heimdall::AnalysisFeatures::Diagnostics(
            context, heimdall::RuleEngine(std::move(options)), true);
        std::erase_if(diagnostics,[](const auto& diagnostic)
            {
                return diagnostic.code != "cpp/call-argument-count" &&
                diagnostic.code != "cpp/call-argument-type";
            });
        return diagnostics;
    }
}

TEST(CallArgumentsSpec, ReportsMissingRequiredArgumentAtTheCall)
{
    const std::string source =
        "struct Node { Node* next; int value; };\n"
        "void test_method(int a, int b, std::string desc) { auto value = a + b; }\n"
        "int main() { Node* root; test_method(root->value, root->next->value); }\n";

    const auto diagnostics = AnalyzeCalls(source);

    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_EQ(diagnostics[0].severity, heimdall::Severity::Error);
    EXPECT_EQ(diagnostics[0].offset, source.rfind("test_method"));
    EXPECT_EQ(diagnostics[0].length, std::string("test_method").size());
    EXPECT_NE(diagnostics[0].message.find("3"), std::string::npos);
    EXPECT_NE(diagnostics[0].message.find("2"), std::string::npos);
    EXPECT_FALSE(diagnostics[0].has_fix);
}

TEST(CallArgumentsSpec, ReportsExtraArgumentsAndEmptyCalls)
{
    EXPECT_EQ(AnalyzeCalls("void f(int a) {} void g() { f(1, 2); }").size(), 1u);
    EXPECT_EQ(AnalyzeCalls("void f(int a) {} void g() { f(); }").size(), 1u);
    EXPECT_EQ(AnalyzeCalls("void f(void) {} void g() { f(1); }").size(), 1u);
}

TEST(CallArgumentsSpec, AcceptsExactCountsDefaultsAndVariadicArguments)
{
    EXPECT_TRUE(AnalyzeCalls("void f(int a, int b) {} void g() { f(1, 2); }").empty());
    EXPECT_TRUE(AnalyzeCalls("void f(int a, int b = 2) {} void g() { f(1); }").empty());
    EXPECT_TRUE(AnalyzeCalls("void f() {} void g() { f(); }").empty());
    EXPECT_TRUE(AnalyzeCalls("void f(void) {} void g() { f(); }").empty());
    EXPECT_TRUE(AnalyzeCalls("void f(int a, ...) {} void g() { f(1, 2, 3); }").empty());
    EXPECT_EQ(AnalyzeCalls("void f(int a, ...) {} void g() { f(); }").size(), 1u);
}

TEST(CallArgumentsSpec, AcceptsAnyOverloadWithAViableCount)
{
    EXPECT_TRUE(AnalyzeCalls("void f(int); void f(int, int); void g() { f(1, 2); }").empty());
    EXPECT_EQ(AnalyzeCalls("void f(int); void f(int, int); void g() { f(); }").size(), 1u);
    EXPECT_TRUE(AnalyzeCalls("void f(int, int = 2); void f(int, int) {} "
        "void g() { f(1); }").empty());
}

TEST(CallArgumentsSpec, CountsNestedExpressionsAsSingleArguments)
{
    EXPECT_TRUE(AnalyzeCalls("int h(int, int); void f(int, int); "
        "void g() { f(h(1, 2), (1, 2)); }").empty());
    EXPECT_TRUE(AnalyzeCalls("struct P { int a; int b; }; void f(P, int); "
        "void g() { f(P{1, 2}, 3); }").empty());
}

TEST(CallArgumentsSpec, SkipsUnknownDependentAndShadowedCallees)
{
    EXPECT_TRUE(AnalyzeCalls("void g() { unknown(1, 2); }").empty());
    EXPECT_TRUE(AnalyzeCalls("template<class... T> void f(T... args) {} "
        "void g() { f(1, 2); }").empty());
    EXPECT_TRUE(AnalyzeCalls("void f(int); void g() { auto f = [](int, int) {}; f(1, 2); }")
        .empty());
    EXPECT_TRUE(AnalyzeCalls("void f(int); template<class... T> void g(T... args) "
        "{ f(args...); }").empty());
}

TEST(CallArgumentsSpec, DoesNotUseOverloadsOrDefaultsDeclaredAfterTheCall)
{
    EXPECT_EQ(AnalyzeCalls("void f(int); void g() { f(1, 2); } void f(int, int);").size(), 1u);
    EXPECT_EQ(AnalyzeCalls("void f(int, int); void g() { f(1); } "
        "void f(int, int = 2);").size(), 1u);
}

TEST(CallArgumentsSpec, CountsCastToUnknownTypeAsSingleArgument)
{
    // `Mode` is unknown without its header (as `SDL_Keymod` is without SDL):
    // the parenthesized type must not split its operand into a second argument.
    EXPECT_TRUE(AnalyzeCalls("struct Event { int mod; }; void Update(int); "
        "void Test(Event* event) { Update((Mode)event->mod); }").empty());
    EXPECT_TRUE(AnalyzeCalls("struct Event { int mod; }; void Update(int); "
        "void Test(Event* event) { Update(event->mod); }").empty());
    EXPECT_TRUE(AnalyzeCalls("void Update(int); void Test() { Update((int)1); }").empty());
    // Control: a genuine second argument is still reported.
    EXPECT_EQ(AnalyzeCalls("void Update(int); void Test() { Update(1, 2); }").size(), 1u);
}

TEST(CallArgumentsSpec, SeesClassMemberOverloadsDeclaredAfterTheCall)
{
    EXPECT_TRUE(AnalyzeCalls("class S { void Apply(int, int); void Run() { Apply(1, 2, 3); } "
        "void Apply(int, int, int); };")
        .empty());
    EXPECT_TRUE(AnalyzeCalls("class S { public: void Apply(int, int); void Run() "
        "{ Apply(1, 2, 3); } private: void Apply(int, int, int); };")
        .empty());
    EXPECT_TRUE(AnalyzeCalls("class S { void Apply(int, std::string); void Run() "
        "{ Apply(1, 2); } void Apply(int, int); };")
        .empty());
    EXPECT_EQ(AnalyzeCalls("class S { void Apply(int, int); void Run() { Apply(1); } "
        "void Apply(int, int, int); };")
        .size(),
        1u);
}

TEST(CallArgumentsSpec, CountsFunctionPointerAndTemplateParametersCorrectly)
{
    EXPECT_TRUE(AnalyzeCalls("void f(void (*p)(int, int), std::pair<int, int> v); "
        "void g() { f(nullptr, std::pair<int, int>{1, 2}); }").empty());
    EXPECT_EQ(AnalyzeCalls("void f(void (*p)(int, int), std::pair<int, int> v); "
        "void g() { f(nullptr); }").size(), 1u);
    EXPECT_TRUE(AnalyzeCalls("void f(int a, int b = [] { return 2; }()); "
        "void g() { f(1); }").empty());
}

TEST(CallArgumentsSpec, DoesNotAnalyzeInactiveCallsOrUnfinishedInput)
{
    EXPECT_TRUE(AnalyzeCalls("void f(int);\n#if 0\nvoid g() { f(); }\n#endif\n").empty());
    EXPECT_TRUE(AnalyzeCalls("void f(int); void g() { f(").empty());
    EXPECT_TRUE(AnalyzeCalls("void f(int); void g() { f").empty());
}

TEST(CallArgumentsSpec, ReportsIntegerPassedToStringAtTheArgument)
{
    const std::string source =
        "struct Node { Node* next; int value; };\n"
        "void test_method(int a, int b, std::string desc) {}\n"
        "void g(Node* root) { test_method(root->value, root->next->value, 1); }\n";

    const auto diagnostics = AnalyzeCalls(source);

    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_EQ(diagnostics[0].code, "cpp/call-argument-type");
    EXPECT_EQ(diagnostics[0].severity, heimdall::Severity::Error);
    EXPECT_EQ(diagnostics[0].offset, source.rfind("1)"));
    EXPECT_EQ(diagnostics[0].length, 1u);
    EXPECT_NE(diagnostics[0].message.find("argument 3"), std::string::npos);
    EXPECT_NE(diagnostics[0].message.find("int"), std::string::npos);
    EXPECT_NE(diagnostics[0].message.find("std::string"), std::string::npos);
    EXPECT_FALSE(diagnostics[0].has_fix);
}

TEST(CallArgumentsSpec, AcceptsStandardImplicitConversions)
{
    EXPECT_TRUE(AnalyzeCalls("void f(double, bool, const int*); "
        "void g(int* p) { f(1, p, p); }").empty());
    EXPECT_TRUE(AnalyzeCalls("void f(std::string); void g() { f(\"description\"); }").empty());
    EXPECT_TRUE(AnalyzeCalls("void f(const int*); void g() { f(nullptr); f(0); }").empty());
    EXPECT_TRUE(AnalyzeCalls("void f(const int&); void g() { f(1); }").empty());
}

TEST(CallArgumentsSpec, RejectsProvenInvalidStandardConversions)
{
    EXPECT_EQ(AnalyzeCalls("void f(int*); void g() { f(1); }").size(), 1u);
    EXPECT_EQ(AnalyzeCalls("void f(int*); void g(const int* p) { f(p); }").size(), 1u);
    EXPECT_EQ(AnalyzeCalls("void f(int); void g() { f(\"text\"); }").size(), 1u);
    EXPECT_EQ(AnalyzeCalls("void f(std::string); void g() { f(1); }").size(), 1u);
    EXPECT_EQ(AnalyzeCalls("void f(const std::string&); void g() { f(1); }").size(), 1u);
}

TEST(CallArgumentsSpec, ValidatesArgumentsAgainstInstantiatedClassTemplateMethods)
{
    const std::string source =
        "template<class TValue> class LinkedList { public: void Push(const TValue& value); };\n"
        "void f() { auto list = LinkedList<int>(); list.Push(\"\"); }\n";

    const auto diagnostics = AnalyzeCalls(source);

    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_EQ(diagnostics[0].code, "cpp/call-argument-type");
    EXPECT_EQ(diagnostics[0].severity, heimdall::Severity::Error);
    EXPECT_EQ(source.substr(diagnostics[0].offset, diagnostics[0].length), "\"\"");
    EXPECT_NE(diagnostics[0].message.find("int"), std::string::npos);

    EXPECT_TRUE(AnalyzeCalls(
        "template<class TValue> class LinkedList { public: void Push(const TValue& value); };\n"
        "void f() { auto list = LinkedList<int>(); list.Push(20); }\n")
        .empty());
}

TEST(CallArgumentsSpec, AcceptsAnyOverloadWithCompatibleTypes)
{
    EXPECT_TRUE(AnalyzeCalls("void f(int); void f(std::string); void g() { f(1); }").empty());
    EXPECT_TRUE(AnalyzeCalls("void f(int); void f(std::string); void g() { f(\"text\"); }")
        .empty());
    EXPECT_EQ(AnalyzeCalls("void f(int*); void f(std::string); void g() { f(1); }").size(), 1u);
}

TEST(CallArgumentsSpec, LeavesUnknownAndUserDefinedConversionsUndiagnosed)
{
    EXPECT_TRUE(AnalyzeCalls("void f(Unknown); void g() { f(1); }").empty());
    EXPECT_TRUE(AnalyzeCalls("void f(std::string); void g() { f(unknown()); }").empty());
    EXPECT_TRUE(AnalyzeCalls("struct S { operator int() const; }; "
        "void f(int); void g(S s) { f(s); }").empty());
    EXPECT_TRUE(AnalyzeCalls("struct S { S(int); }; void f(S); void g() { f(1); }").empty());
    EXPECT_TRUE(AnalyzeCalls("void f(std::string); void g() { f(0); }").empty());
}

TEST(CallArgumentsSpec, ValidatesKnownReferenceBindings)
{
    EXPECT_EQ(AnalyzeCalls("void f(int&); void g() { f(1); }").size(), 1u);
    EXPECT_EQ(AnalyzeCalls("void f(int&); void g(const int n) { f(n); }").size(), 1u);
    EXPECT_EQ(AnalyzeCalls("void f(int&); void g(double n) { f(n); }").size(), 1u);
    EXPECT_EQ(AnalyzeCalls("void f(int&&); void g(int n) { f(n); }").size(), 1u);
    EXPECT_TRUE(AnalyzeCalls("void f(int&); void g(int n) { f(n); }").empty());
    EXPECT_TRUE(AnalyzeCalls("void f(const int&); void g(double n) { f(n); }").empty());
    EXPECT_TRUE(AnalyzeCalls("void f(int&&); void g() { f(1); }").empty());
}

TEST(CallArgumentsSpec, RejectsIndexedWrapperPassedToRangeForFunctionReference)
{
    const auto index = heimdall::CompletionEngine::IndexScopes(
        "namespace skr { template<class T> struct Arc { T& operator*(); }; "
        "struct Provider { template<class T> Arc<T> GetService(); }; } "
        "namespace app { struct ComponentManager {}; "
        "template<class T> using Action = std::function<void(T&)>; "
        "struct Extension { std::vector<Action<ComponentManager>> callbacks; "
        "void Use(skr::Provider&); }; }", {});
    const std::string source =
        "namespace app { void Extension::Use(skr::Provider& provider) { "
        "const auto component = provider.GetService<ComponentManager>(); "
        "for (auto& func : callbacks) { func(component); func(*component); } } }";
    const auto tree = heimdall::ParseTree::Parse(source);
    const auto model = heimdall::Binder::Bind(tree);
    const auto types = heimdall::Typer::Type(model);

    const auto diagnostics = heimdall::SemanticRules::AnalyzeIndexedCallArgumentTypes(types, index);
    for (std::size_t token = 0; token < tree.Tokens().size(); ++token)
    {
        if (tree.Tokens()[token].offset == source.find("func(component)"))
        {
            EXPECT_EQ(heimdall::CompletionEngine::VariableType(tree, token, &index),
                      "std::function<void(ComponentManager&)>");
        }

        if (tree.Tokens()[token].offset == source.find("component);"))
        {
            EXPECT_EQ(heimdall::CompletionEngine::VariableType(tree, token, &index),
                      "skr::Arc<ComponentManager>");
        }
    }

    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_EQ(diagnostics[0].code, "cpp/call-argument-type");
    EXPECT_EQ(diagnostics[0].severity, heimdall::Severity::Error);
    EXPECT_EQ(diagnostics[0].offset, source.find("component);"));
}

TEST(CallArgumentsSpec, IndexedWrapperCheckKeepsUnknownBasesAndReferenceConversionsSilent)
{
    for (const std::string& declaration :
        {
            "template<class T> struct Arc { operator T&(); };",
            "template<class T> struct Arc : Base {};",
            "template<class T> struct Arc;"
    })
    {
        const auto index = heimdall::CompletionEngine::IndexScopes(
            "namespace skr { " + declaration +
            " struct Provider { template<class T> Arc<T> GetService(); }; }", {});
        const std::string source =
            "struct ComponentManager {}; void F(skr::Provider& provider, "
            "std::function<void(ComponentManager&)>& func) { "
            "auto component = provider.GetService<ComponentManager>(); func(component); }";
        const auto tree = heimdall::ParseTree::Parse(source);
        const auto model = heimdall::Binder::Bind(tree);
        const auto types = heimdall::Typer::Type(model);

        EXPECT_TRUE(heimdall::SemanticRules::AnalyzeIndexedCallArgumentTypes(types, index).empty())
        << declaration;
    }
}
