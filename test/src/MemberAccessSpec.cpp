#include <algorithm>
#include <gtest/gtest.h>
#include <Heimdall/AnalysisFeatures.hpp>
#include <memory>
#include <string>
#include <vector>

namespace
{
    std::vector<heimdall::Diagnostic> AnalyzeMembers(const std::string& source)
    {
        heimdall::Workspace workspace;
        const auto file = workspace.Open("members.cpp", std::make_shared<const std::string>(source));
        heimdall::AnalysisContext context(workspace.Snapshot(), *file);
        auto diagnostics = heimdall::AnalysisFeatures::Diagnostics(
            context, heimdall::RuleEngine {}, true);
        std::erase_if(diagnostics,[](const auto& diagnostic)
            {
                return diagnostic.code != "cpp/dot-on-pointer";
            });
        return diagnostics;
    }
}

TEST(MemberAccessSpec, ReportsDotOnPointerAtTheMember)
{
    const std::string source =
        "struct Node { Node* next; int value; };\n"
        "void f(Node* next) { next.value = 11; }\n";

    const auto diagnostics = AnalyzeMembers(source);

    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_EQ(diagnostics[0].code, "cpp/dot-on-pointer");
    EXPECT_EQ(diagnostics[0].severity, heimdall::Severity::Error);
    EXPECT_EQ(diagnostics[0].offset, source.rfind("value = 11"));
    EXPECT_EQ(diagnostics[0].length, std::string("value").size());
    EXPECT_NE(diagnostics[0].message.find("value"), std::string::npos);
    EXPECT_NE(diagnostics[0].message.find("next"), std::string::npos);
    EXPECT_NE(diagnostics[0].message.find("Node*"), std::string::npos);
    EXPECT_NE(diagnostics[0].message.find("->"), std::string::npos);
    EXPECT_TRUE(diagnostics[0].has_fix);
    EXPECT_TRUE(diagnostics[0].fix_is_safe);
    EXPECT_EQ(diagnostics[0].fix.offset, source.find(".value", source.find("next.value")));
    EXPECT_EQ(diagnostics[0].fix.length, 1u);
    EXPECT_EQ(diagnostics[0].fix.replacement, "->");
    EXPECT_EQ(diagnostics[0].fix_title, "Replace '.' with '->'");
}

TEST(MemberAccessSpec, AcceptsArrowOnPointersAndDotOnObjects)
{
    EXPECT_TRUE(AnalyzeMembers("struct Node { Node* next; int value; };\n"
        "void f(Node* next, Node node) { next->value = 1; node.value = 2; }")
        .empty());
    EXPECT_TRUE(AnalyzeMembers("struct S { int v; };\n"
        "void f(S s, S* p) { s.v = 1; p->v = 2; }")
        .empty());
}

TEST(MemberAccessSpec, ReportsThisDotAndNestedPointerMembers)
{
    EXPECT_EQ(AnalyzeMembers("struct S { int v; void m() { this.v = 1; } };").size(), 1u);
    // The recovered type of `n.next` proves that the outer dot is wrong too.
    EXPECT_EQ(AnalyzeMembers("struct Node { Node* next; int value; };\n"
        "void f(Node* n) { n.next.value = 1; }")
        .size(),
        2u);
}

TEST(MemberAccessSpec, LeavesUnknownNonPointerAndArrowMisuseSilent)
{
    EXPECT_TRUE(AnalyzeMembers("void f() { missing.m = 2; }").empty());
    EXPECT_TRUE(AnalyzeMembers("struct Opaque;\nvoid f(Opaque o) { o.m = 1; }").empty());
    EXPECT_TRUE(AnalyzeMembers("struct Ptr { int* p; };\nvoid f(Ptr w) { w.p = nullptr; }")
        .empty());
    // `->` on a non-pointer is out of scope for this rule.
    EXPECT_TRUE(AnalyzeMembers("struct S { int v; };\nvoid f(S s) { s->v = 1; }").empty());
}

TEST(MemberAccessSpec, ReportsDotOnPointerToIncompleteType)
{
    EXPECT_EQ(AnalyzeMembers("struct Opaque;\nvoid f(Opaque* p) { p.m = 1; }").size(), 1u);
}

TEST(MemberAccessSpec, ReportsDotOnAutoDeducedNewPointer)
{
    const std::string source =
        "struct Node { Node* next; int value; };\n"
        "int main() {\n"
        "auto next = new Node { .value = 20 };\n"
        "next.value = 11;\n"
        "return 0;\n"
        "}\n";

    const auto diagnostics = AnalyzeMembers(source);

    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_EQ(diagnostics[0].code, "cpp/dot-on-pointer");
    EXPECT_EQ(diagnostics[0].severity, heimdall::Severity::Error);
    EXPECT_EQ(diagnostics[0].offset, source.rfind("value = 11"));
    EXPECT_NE(diagnostics[0].message.find("Node*"), std::string::npos);
    EXPECT_NE(diagnostics[0].message.find("->"), std::string::npos);
}

TEST(MemberAccessSpec, ReportsEachDotOnAutoDeducedNewPointer)
{
    const std::string source =
        "struct Node { Node* next; int value; };\n"
        "int main() {\n"
        "auto next = new Node { .value = 20 };\n"
        "auto root = new Node { .next = next, .value = 20 };\n"
        "root.value = 39;\n"
        "next.value = 33;\n"
        "return 0;\n"
        "}\n";

    EXPECT_EQ(AnalyzeMembers(source).size(), 2u);
    EXPECT_TRUE(AnalyzeMembers("struct Node { Node* next; int value; };\n"
        "int main() {\n"
        "auto next = new Node { .value = 20 };\n"
        "next->value = 11;\n"
        "return 0;\n"
        "}\n")
        .empty());
}

TEST(MemberAccessSpec, ReportsEveryPointerInAChainedMemberAccess)
{
    const std::string source =
        "struct Node { Node* next; int value; };\n"
        "void f(Node* next) { next.next.next.next.next = 30; }\n";

    const auto diagnostics = AnalyzeMembers(source);

    ASSERT_EQ(diagnostics.size(), 4u);
    for (const auto& diagnostic : diagnostics)
    {
        ASSERT_TRUE(diagnostic.has_fix);
        EXPECT_EQ(diagnostic.fix.length, 1u);
        EXPECT_EQ(diagnostic.fix.replacement, "->");
        EXPECT_EQ(source.substr(diagnostic.fix.offset, diagnostic.fix.length), ".");
    }
}
