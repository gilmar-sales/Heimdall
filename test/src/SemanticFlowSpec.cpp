#include <Heimdall/FlowModel.hpp>
#include <Heimdall/RuleEngine.hpp>
#include <Heimdall/SemanticRules.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace
{

    using heimdall::EventKind;

    // The layers stacked in the order they are built; they reference each other,
    // so the fixture is never moved.
    struct Stack
    {
        explicit Stack(std::string text) : source(std::move(text)),
            tree(heimdall::ParseTree::Parse(source)),
            model(heimdall::Binder::Bind(tree)), types(heimdall::Typer::Type(model)),
            flow(heimdall::Flow::Build(types)) {}

        Stack(const Stack&) = delete;

        Stack& operator= (const Stack&) = delete;

        // First variable or parameter called `name`.
        heimdall::SymbolId Variable(const std::string& name) const
        {
            // Locals first: a member or global may share the name.
            const auto& symbols = model.Symbols();
            heimdall::SymbolId found = heimdall::kNone;
            for (heimdall::SymbolId symbol = 0; symbol < symbols.Size(); ++symbol)
            {
                if ((symbols.kind[symbol] == heimdall::SymbolKind::Variable ||
                    symbols.kind[symbol] == heimdall::SymbolKind::Parameter) &&
                    model.Names().Text(symbols.name[symbol]) == name)
                {
                    if (flow.OwnerOf(symbol) != heimdall::kNone)
                    {
                        return symbol;
                    }

                    found = found == heimdall::kNone ? symbol : found;
                }
            }

            return found;
        }

        std::vector<EventKind> Events(const std::string& name) const
        {
            std::vector<EventKind> kinds;
            for (const auto index : flow.EventsOf(Variable(name)))
            {
                kinds.push_back(flow.Events().kind[index]);
            }

            return kinds;
        }

        std::string source;
        heimdall::ParseTree tree;
        heimdall::SemanticModel model;
        heimdall::TypeModel types;
        heimdall::FlowModel flow;
    };

    std::vector<heimdall::Diagnostic> Const(const std::string& source)
    {
        const Stack stack(source);
        return heimdall::SemanticRules::AnalyzeConst(stack.flow);
    }

    std::vector<heimdall::Diagnostic> Constexpr(const std::string& source)
    {
        const Stack stack(source);
        return heimdall::SemanticRules::AnalyzeConstexpr(stack.flow);
    }

    std::string ApplyAll(std::string source, std::vector<heimdall::Diagnostic> diagnostics)
    {
        std::sort(diagnostics.begin(), diagnostics.end(),
            [](const heimdall::Diagnostic& a, const heimdall::Diagnostic& b)
            {
                return a.fix.offset > b.fix.offset;
        });
        for (const auto& diagnostic : diagnostics)
        {
            source.replace(diagnostic.fix.offset, diagnostic.fix.length, diagnostic.fix.replacement);
        }

        return source;
    }

    std::string Flagged(const std::string& source, const heimdall::Diagnostic& diagnostic)
    {
        return source.substr(diagnostic.offset, diagnostic.length);
    }

    using Kinds = std::vector<EventKind>;

} // namespace

// ---- control-flow graph ------------------------------------------------------

TEST(FlowCfg, StraightLineFunctionReachesItsExit)
{
    const Stack stack("int f(int a) { int b = a + 1; return b; }\n");
    ASSERT_EQ(stack.flow.Functions().Size(), 1u);
    EXPECT_NE(stack.flow.Functions().complete[0], 0);
    EXPECT_TRUE(stack.flow.ExitReachable(0));
    EXPECT_TRUE(stack.flow.HasReachableReturn(0));
    EXPECT_EQ(stack.flow.OwnerOf(stack.Variable("b")), 0u);
    EXPECT_EQ(stack.flow.OwnerOf(stack.Variable("a")), 0u);
}

TEST(FlowCfg, IfElseJoinsBothBranches)
{
    const Stack stack(
        "int f(int a) {\n"
        "    int r = 0;\n"
        "    if (a > 0) { r = 1; } else { r = 2; }\n"
        "    return r;\n"
        "}\n");
    const auto& events = stack.flow.Events();
    const auto& blocks = stack.flow.Blocks();
    // The two writes of `r` sit in different blocks that both reach the `return`.
    std::vector<heimdall::BlockId> writes;
    for (const auto index : stack.flow.EventsOf(stack.Variable("r")))
    {
        if (events.kind[index] == EventKind::Write)
        {
            writes.push_back(events.block[index]);
        }
    }

    ASSERT_EQ(writes.size(), 2u);
    EXPECT_NE(writes[0], writes[1]);
    EXPECT_GE(stack.flow.Successors(writes[0]).size(), 1u);
    EXPECT_GE(stack.flow.Successors(writes[1]).size(), 1u);
    EXPECT_EQ(stack.flow.Successors(writes[0])[0], stack.flow.Successors(writes[1])[0]);
    EXPECT_GT(blocks.Size(), 4u);
    EXPECT_TRUE(stack.flow.IsReachable(0, writes[0]));
    EXPECT_TRUE(stack.flow.IsReachable(0, writes[1]));
}

TEST(FlowCfg, CodeAfterReturnIsUnreachable)
{
    const Stack stack(
        "int f(int a) {\n"
        "    return a;\n"
        "    int dead = 1;\n"
        "    return dead;\n"
        "}\n");
    const auto dead = stack.Variable("dead");
    ASSERT_NE(dead, heimdall::kNone);
    const auto events = stack.flow.EventsOf(dead);
    ASSERT_FALSE(events.empty());
    EXPECT_FALSE(stack.flow.IsReachable(0, stack.flow.Events().block[events[0]]));
    EXPECT_TRUE(stack.flow.HasReachableReturn(0));
}

TEST(FlowCfg, InfiniteLoopHasNoReachableExit)
{
    const Stack stack("int spin() { for (;;) { } }\n");
    ASSERT_EQ(stack.flow.Functions().Size(), 1u);
    EXPECT_FALSE(stack.flow.ExitReachable(0));
    EXPECT_FALSE(stack.flow.HasReachableReturn(0));
}

TEST(FlowCfg, BreakLeavesAnInfiniteLoop)
{
    const Stack stack("int f(int n) { for (;;) { if (n > 3) break; n = n + 1; } return n; }\n");
    EXPECT_TRUE(stack.flow.ExitReachable(0));
    EXPECT_TRUE(stack.flow.HasReachableReturn(0));
}

TEST(FlowCfg, LoopsAndSwitchAndTryAreModeled)
{
    const Stack stack(
        "int f(int a) {\n"
        "    int x = 0;\n"
        "    while (a > 0) { a = a - 1; if (a == 5) continue; x = x + a; }\n"
        "    do { a = a + 1; } while (a < 3);\n"
        "    for (int i = 0; i < 4; ++i) { x = x + i; }\n"
        "    switch (a) { case 1: x = 1; break; case 2: x = 2; default: x = 3; }\n"
        "    try { x = x + 1; } catch (...) { x = 0; }\n"
        "    return x;\n"
        "}\n");
    EXPECT_NE(stack.flow.Functions().complete[0], 0);
    EXPECT_TRUE(stack.flow.ExitReachable(0));
    EXPECT_TRUE(stack.flow.HasReachableReturn(0));
    // Every block of the function is reachable except none: no dead code was written.
    const auto first = stack.flow.Functions().first_block[0];
    for (std::uint32_t i = 0; i < stack.flow.Functions().block_count[0]; ++i)
    {
        EXPECT_TRUE(stack.flow.IsReachable(0, first + i)) << "block " << i;
    }
}

TEST(FlowCfg, SwitchWithoutDefaultCanSkipTheBody)
{
    const Stack stack("int f(int a) { int r = 0; switch (a) { case 1: r = 1; break; } return r; }\n");
    EXPECT_TRUE(stack.flow.HasReachableReturn(0));
    const auto writes = stack.Events("r");
    EXPECT_EQ(writes, (Kinds{EventKind::Init, EventKind::Write, EventKind::Read}));
}

TEST(FlowCfg, GotoMakesTheFunctionIncomplete)
{
    const Stack stack("int f(int a) { int x = a; goto end; end: return x; }\n");
    EXPECT_EQ(stack.flow.Functions().complete[0], 0);
    EXPECT_FALSE(stack.flow.IsNeverModified(stack.Variable("x")));
}

TEST(FlowCfg, LambdasAreFunctionsOfTheirOwn)
{
    const Stack stack("int f(int a) { auto g = [](int z) { int w = z + 1; return w; }; return a; }\n");
    ASSERT_EQ(stack.flow.Functions().Size(), 2u);
    // (The grammar does not parse lambda parameters, so `z` has no symbol.)
    EXPECT_EQ(stack.flow.OwnerOf(stack.Variable("w")), 1u);
    EXPECT_EQ(stack.flow.OwnerOf(stack.Variable("a")), 0u);
}

TEST(FlowCfg, GlobalsAndMembersAreNotLocal)
{
    const Stack stack(
        "int global = 1;\n"
        "struct S { int member = 2; int get() { return member; } };\n"
        "int f() { return global; }\n");
    EXPECT_EQ(stack.flow.OwnerOf(stack.Variable("global")), heimdall::kNone);
    EXPECT_EQ(stack.flow.OwnerOf(stack.Variable("member")), heimdall::kNone);
    EXPECT_TRUE(stack.flow.EventsOf(stack.Variable("global")).empty());
}

// ---- def-use events --------------------------------------------------------------

TEST(FlowEvents, ClassifiesPlainUses)
{
    const Stack stack(
        "int f(int a) {\n"
        "    int x = 1;\n"
        "    int y = x + a;\n"
        "    x = 2;\n"
        "    x += 3;\n"
        "    ++x;\n"
        "    x--;\n"
        "    int u;\n"
        "    return y;\n"
        "}\n");
    EXPECT_EQ(stack.Events("x"),
        (Kinds{EventKind::Init, EventKind::Read, EventKind::Write, EventKind::Modify,
            EventKind::Modify, EventKind::Modify}));
    EXPECT_EQ(stack.Events("u"), (Kinds{EventKind::Uninit}));
    EXPECT_EQ(stack.Events("a"), (Kinds{EventKind::Read}));
    EXPECT_FALSE(stack.flow.IsNeverModified(stack.Variable("x")));
    EXPECT_TRUE(stack.flow.IsNeverModified(stack.Variable("y")));
}

TEST(FlowEvents, AddressOfAndReferencesEscape)
{
    const Stack stack(
        "void f() {\n"
        "    int a = 1;\n"
        "    int* p = &a;\n"
        "    int b = 2;\n"
        "    int& r = b;\n"
        "    int c = 3;\n"
        "    const int& k = c;\n"
        "    int d = 4;\n"
        "    int e = d;\n"
        "}\n");
    EXPECT_EQ(stack.Events("a"), (Kinds{EventKind::Init, EventKind::Escape}));
    EXPECT_EQ(stack.Events("b"), (Kinds{EventKind::Init, EventKind::Escape}));
    EXPECT_EQ(stack.Events("c"), (Kinds{EventKind::Init, EventKind::Read}));
    EXPECT_EQ(stack.Events("d"), (Kinds{EventKind::Init, EventKind::Read}));
}

TEST(FlowEvents, ArgumentsDependOnTheParameterOfTheCallee)
{
    const Stack stack(
        "void by_value(int v) {}\n"
        "void by_const_ref(const int& v) {}\n"
        "void by_ref(int& v) {}\n"
        "void by_pointer(int* v) {}\n"
        "void f() {\n"
        "    int a = 1; by_value(a);\n"
        "    int b = 2; by_const_ref(b);\n"
        "    int c = 3; by_ref(c);\n"
        "    int d = 4; unknown(d);\n"
        "    int e = 5; printf(\"%d\", e);\n"
        "    int arr[2] = {1, 2}; by_pointer(arr);\n"
        "}\n");
    EXPECT_EQ(stack.Events("a"), (Kinds{EventKind::Init, EventKind::Read}));
    EXPECT_EQ(stack.Events("b"), (Kinds{EventKind::Init, EventKind::Read}));
    EXPECT_EQ(stack.Events("c"), (Kinds{EventKind::Init, EventKind::Escape}));
    EXPECT_EQ(stack.Events("d"), (Kinds{EventKind::Init, EventKind::Escape}));
    EXPECT_EQ(stack.Events("e"), (Kinds{EventKind::Init, EventKind::Read}));
    EXPECT_EQ(stack.Events("arr").back(), EventKind::Escape);
}

TEST(FlowEvents, ElementsAndMembersAreWrittenThroughTheObject)
{
    const Stack stack(
        "struct S { int m; int get() const { return m; } void set(int v) { m = v; } };\n"
        "void f(int* p) {\n"
        "    int a[3] = {1, 2, 3};\n"
        "    a[1] = 5;\n"
        "    int b[3] = {1, 2, 3};\n"
        "    int t = b[1];\n"
        "    S s = make();\n"
        "    s.m = 1;\n"
        "    S q = make();\n"
        "    int g = q.get();\n"
        "    S r = make();\n"
        "    r.set(1);\n"
        "    *p = 3;\n"
        "}\n");
    EXPECT_EQ(stack.Events("a"), (Kinds{EventKind::Init, EventKind::Modify}));
    EXPECT_EQ(stack.Events("b"), (Kinds{EventKind::Init, EventKind::Read}));
    EXPECT_EQ(stack.Events("s"), (Kinds{EventKind::Init, EventKind::Modify}));
    EXPECT_EQ(stack.Events("q"), (Kinds{EventKind::Init, EventKind::Read}));
    EXPECT_EQ(stack.Events("r"), (Kinds{EventKind::Init, EventKind::Modify}));
    EXPECT_EQ(stack.Events("p"), (Kinds{EventKind::Read}));
}

TEST(FlowEvents, StreamsAndLambdas)
{
    const Stack stack(
        "void f(std::istream& in) {\n"
        "    int a = 1; std::cout << a;\n"
        "    int b = 2; in >> b;\n"
        "    int c = 3; auto g = [c]() { return c; };\n"
        "    int d = 4; auto h = [&]() { d = 5; };\n"
        "}\n");
    EXPECT_EQ(stack.Events("a"), (Kinds{EventKind::Init, EventKind::Read}));
    EXPECT_EQ(stack.Events("b"), (Kinds{EventKind::Init, EventKind::Modify}));
    EXPECT_EQ(stack.Events("c").back(), EventKind::Escape);
    EXPECT_EQ(stack.Events("d").back(), EventKind::Escape);
}

TEST(FlowEvents, ReturnAndRangeForAndDecltype)
{
    const Stack stack(
        "int by_value() { int a = 1; return a; }\n"
        "std::string by_name() { std::string s = make(); return s; }\n"
        "int& by_ref(int& r) { int b = 1; return r; }\n"
        "void loop() { std::vector<int> v = make(); for (auto e : v) {} }\n"
        "void type() { int x = 1; decltype(x) y = 2; }\n");
    EXPECT_EQ(stack.Events("a"), (Kinds{EventKind::Init, EventKind::Read}));
    EXPECT_EQ(stack.Events("s"), (Kinds{EventKind::Init, EventKind::Escape}));
    EXPECT_EQ(stack.Events("v").back(), EventKind::Escape);
    EXPECT_EQ(stack.Events("x").back(), EventKind::Escape);
}

TEST(FlowEvents, NamesTheGrammarDidNotModelEscape)
{
    // `S s(q);` reads as a function declarator: `q` is no expression node, and a
    // brace list swallows its names. Both must still count against the variable.
    const Stack stack(
        "void f() {\n"
        "    int q = 1;\n"
        "    S s(q);\n"
        "    int r = 2;\n"
        "    int a[2] = {r, 0};\n"
        "}\n");
    EXPECT_EQ(stack.Events("q").back(), EventKind::Escape);
    EXPECT_EQ(stack.Events("r").back(), EventKind::Escape);
    EXPECT_FALSE(stack.flow.IsNeverModified(stack.Variable("q")));
}

TEST(FlowEvents, InactiveCodeAndSameNamedMembersAreIgnored)
{
    const Stack stack(
        "struct S { int mem; };\n"
        "int f(S s) {\n"
        "    int mem = 1;\n"
        "    return s.mem + mem;\n"
        "}\n");
    EXPECT_EQ(stack.Events("mem").size(), 2u);
    EXPECT_TRUE(stack.flow.IsNeverModified(stack.Variable("mem")));
}

// ---- cpp/modernize-const ----------------------------------------------------------

TEST(ModernizeConst, SuggestsConstForValuesNeverModified)
{
    const std::string source =
        "int f(int a) {\n"
    "    int x = a * 2;\n"
    "    double d = a / 3.0;\n"
    "    return x + static_cast<int>(d);\n"
    "}\n";
    const auto diagnostics = Const(source);
    ASSERT_EQ(diagnostics.size(), 2u);
    EXPECT_EQ(diagnostics[0].code, "cpp/modernize-const");
    EXPECT_EQ(diagnostics[0].rule, heimdall::RuleId::ModernizeConst);
    EXPECT_EQ(Flagged(source, diagnostics[0]), "x");
    EXPECT_EQ(Flagged(source, diagnostics[1]), "d");
    EXPECT_EQ(diagnostics[0].line, 2u);
    EXPECT_EQ(ApplyAll(source, diagnostics),
        "int f(int a) {\n"
        "    const int x = a * 2;\n"
        "    const double d = a / 3.0;\n"
        "    return x + static_cast<int>(d);\n"
        "}\n");
}

TEST(ModernizeConst, FixIsOnlyAQuickFix)
{
    const std::string source = "int f(int a) { int x = a + 1; return x; }\n";
    const auto diagnostics = Const(source);
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_FALSE(diagnostics[0].fix_is_safe);
    EXPECT_FALSE(diagnostics[0].fix_title.empty());
    EXPECT_EQ(heimdall::RuleEngine::ApplyFixes(source, diagnostics), source);
}

TEST(ModernizeConst, InsertsBeforeQualifiedAndStaticTypes)
{
    const std::string source =
        "int f() {\n"
    "    std::string s = make();\n"
    "    static unsigned int u = seed();\n"
    "    return s.size() + u;\n"
    "}\n";
    const auto diagnostics = Const(source);
    ASSERT_EQ(diagnostics.size(), 2u);
    EXPECT_EQ(ApplyAll(source, diagnostics),
        "int f() {\n"
        "    const std::string s = make();\n"
        "    static const unsigned int u = seed();\n"
        "    return s.size() + u;\n"
        "}\n");
}

TEST(ModernizeConst, ClassObjectsOnlyWhenEveryUseIsConst)
{
    const std::string source =
        "struct P { int x; int get() const { return x; } void bump() { ++x; } };\n"
    "int f() {\n"
    "    P a = make();\n"
    "    int g = a.get() + a.x;\n"
    "    P b = make();\n"
    "    b.bump();\n"
    "    P c = make();\n"
    "    return g + c.get();\n"
    "}\n";
    const auto diagnostics = Const(source);
    std::vector<std::string> names;
    for (const auto& diagnostic : diagnostics)
    {
        names.push_back(Flagged(source, diagnostic));
    }

    EXPECT_NE(std::find(names.begin(), names.end(), "a"), names.end());
    EXPECT_EQ(std::find(names.begin(), names.end(), "b"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), "c"), names.end());
}

TEST(ModernizeConst, SilentWhenTheVariableChanges)
{
    EXPECT_TRUE(Const("int f(int a) { int x = a; x = 2; return x; }\n").empty());
    EXPECT_TRUE(Const("int f(int a) { int x = a; x += 2; return x; }\n").empty());
    EXPECT_TRUE(Const("int f(int a) { int x = a; ++x; return x; }\n").empty());
    EXPECT_TRUE(Const("int f(int a) { int x = a; x--; return x; }\n").empty());
    EXPECT_TRUE(Const("int f(int a) { int x = a; int a2[2] = {0, 0}; a2[0] = x; return a2[0]; }\n").size() <= 1u);
    EXPECT_TRUE(Const("int f(std::istream& in) { int x = 0; in >> x; return x; }\n").empty());
}

TEST(ModernizeConst, SilentWhenTheVariableEscapes)
{
    EXPECT_TRUE(Const("int* f(int a) { int x = a; int* p = &x; return p; }\n").size() <= 1u);
    EXPECT_TRUE(Const("void use(int&);\nvoid f(int a) { int x = a; use(x); }\n").empty());
    EXPECT_TRUE(Const("void f(int a) { int x = a; unknown(x); }\n").empty());
    EXPECT_TRUE(Const("int f(int a) { int x = a; auto g = [&]() { x = 1; }; g(); return x; }\n").empty());
    EXPECT_TRUE(Const("void f(int a) { int x = a; int& r = x; r = 1; }\n").empty());
    EXPECT_TRUE(Const("void f(int a) { int x = a; decltype(x) y = 2; }\n").empty());
    EXPECT_TRUE(Const("void f(int a) { int x = a; S s(x); }\n").empty());
    for (const auto& d : Const("void f(int a) { int x = a; int arr[1] = {x}; }\n"))
    {
        EXPECT_NE(d.message.find("arr"), std::string::npos); // x itself must not be reported
    }

}

TEST(ModernizeConst, SilentForWhatCannotBeConst)
{
    // already const / constexpr
    EXPECT_TRUE(Const("int f(int a) { const int x = a; return x; }\n").empty());
    EXPECT_TRUE(Const("int f(int a) { int const x = a; return x; }\n").empty());
    EXPECT_TRUE(Const("int f(int a) { constexpr int x = 3; return x + a; }\n").empty());
    // references, pointers, parameters
    EXPECT_TRUE(Const("int f(int& a) { int& r = a; return r; }\n").empty());
    EXPECT_TRUE(Const("int f(int* a) { int* p = a; return *p; }\n").empty());
    EXPECT_TRUE(Const("int f(int a) { return a; }\n").empty());
    // no initializer
    EXPECT_TRUE(Const("int f(int a) { int x; x = a; return x; }\n").empty());
    // several declarators share one declaration
    EXPECT_TRUE(Const("int f(int a) { int x = a, y = a + 1; return x + y; }\n").empty());
    // loop headers
    EXPECT_TRUE(Const("int f(int n) { int s = 0; for (int i = 0; i < n; ) { s = s + 1; break; } return s; }\n").size() == 0u);
    // globals and members belong to no function
    EXPECT_TRUE(Const("int g = f();\nint h() { return g; }\n").empty());
    EXPECT_TRUE(Const("struct S { int m = 1; };\n").empty());
}

TEST(ModernizeConst, SilentForUnknownTypes)
{
    EXPECT_TRUE(Const("int f() { auto x = unknown(); return x; }\n").empty());
    EXPECT_TRUE(Const("int f() { T x = make(); return x.v; }\n").empty());
    EXPECT_TRUE(Const("template <class T> T f(T a) { T x = a; return x; }\n").empty());
    EXPECT_TRUE(Const("int f() { vector<int> v = make(); return v.size(); }\n").empty());
}

TEST(ModernizeConst, ReturningAClassObjectByNameIsLeftAlone)
{
    // `const` would turn the implicit move into a copy.
    EXPECT_TRUE(Const("std::string f() { std::string s = make(); return s; }\n").empty());
    EXPECT_TRUE(Const("std::string f() { std::string s = make(); return std::move(s); }\n").empty());
}

TEST(ModernizeConst, IncompleteFunctionsStaySilent)
{
    EXPECT_TRUE(Const("int f(int a) { int x = a; goto out; out: return x; }\n").empty());
}

TEST(ModernizeConst, ConstantInitializersBelongToConstexpr)
{
    // Reported by cpp/modernize-constexpr instead: it says more.
    EXPECT_TRUE(Const("int f() { int x = 3; return x; }\n").empty());
    EXPECT_EQ(Constexpr("int f() { int x = 3; return x; }\n").size(), 1u);
}

TEST(ModernizeConst, ReportsOnceAcrossLambdasAndFunctions)
{
    const std::string source =
        "int f(int a) {\n"
    "    int outer = a + 1;\n"
    "    auto g = [](int z) { int inner = z * 2; return inner; };\n"
    "    return outer + g(a);\n"
    "}\n";
    const auto diagnostics = Const(source);
    std::vector<std::string> names;
    for (const auto& diagnostic : diagnostics)
    {
        names.push_back(Flagged(source, diagnostic));
    }

    EXPECT_NE(std::find(names.begin(), names.end(), "outer"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), "inner"), names.end());
}

// ---- cpp/modernize-constexpr: variables ---------------------------------------------

TEST(ModernizeConstexpr, ReplacesConstWhenTheInitializerIsConstant)
{
    const std::string source =
        "const int kSize = 16;\n"
    "const int kMask = kSize * 2 - 1;\n"
    "const double kPi = 3.14159;\n"
    "const bool kOn = true && !false;\n"
    "const unsigned kBits = 1u << 4;\n";
    const auto diagnostics = Constexpr(source);
    ASSERT_EQ(diagnostics.size(), 5u);
    EXPECT_EQ(diagnostics[0].code, "cpp/modernize-constexpr");
    EXPECT_EQ(diagnostics[0].rule, heimdall::RuleId::ModernizeConstexpr);
    EXPECT_EQ(Flagged(source, diagnostics[0]), "kSize");
    EXPECT_EQ(ApplyAll(source, diagnostics),
        "constexpr int kSize = 16;\n"
        "constexpr int kMask = kSize * 2 - 1;\n"
        "constexpr double kPi = 3.14159;\n"
        "constexpr bool kOn = true && !false;\n"
        "constexpr unsigned kBits = 1u << 4;\n");
}

TEST(ModernizeConstexpr, FixIsOnlyAQuickFix)
{
    const std::string source = "const int kSize = 16;\n";
    const auto diagnostics = Constexpr(source);
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_FALSE(diagnostics[0].fix_is_safe);
    EXPECT_EQ(heimdall::RuleEngine::ApplyFixes(source, diagnostics), source);
}

TEST(ModernizeConstexpr, LocalsThatNothingModifiesGetConstexpr)
{
    const std::string source =
        "int f(int a) {\n"
    "    int base = 10;\n"
    "    static int step = 2 * 3;\n"
    "    int changed = 1;\n"
    "    changed = 2;\n"
    "    return a + base + step + changed;\n"
    "}\n";
    const auto diagnostics = Constexpr(source);
    ASSERT_EQ(diagnostics.size(), 2u);
    EXPECT_EQ(ApplyAll(source, diagnostics),
        "int f(int a) {\n"
        "    constexpr int base = 10;\n"
        "    static constexpr int step = 2 * 3;\n"
        "    int changed = 1;\n"
        "    changed = 2;\n"
        "    return a + base + step + changed;\n"
        "}\n");
}

TEST(ModernizeConstexpr, ReadsOtherConstantVariables)
{
    const std::string source =
        "const int kA = 4;\n"
    "int g() {\n"
    "    const int kB = kA + 1;\n"
    "    int c = kB * 2;\n"
    "    return c;\n"
    "}\n";
    const auto diagnostics = Constexpr(source);
    std::vector<std::string> names;
    for (const auto& diagnostic : diagnostics)
    {
        names.push_back(Flagged(source, diagnostic));
    }

    EXPECT_EQ(names, (std::vector<std::string>{"kA", "kB", "c"}));
}

TEST(ModernizeConstexpr, StaticConstMembersOfClasses)
{
    const std::string source =
        "struct S {\n"
    "    static const int kMax = 8;\n"
    "    const int kNotStatic = 9;\n"
    "};\n";
    const auto diagnostics = Constexpr(source);
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_EQ(Flagged(source, diagnostics[0]), "kMax");
    EXPECT_EQ(ApplyAll(source, diagnostics),
        "struct S {\n"
        "    static constexpr int kMax = 8;\n"
        "    const int kNotStatic = 9;\n"
        "};\n");
}

TEST(ModernizeConstexpr, SilentWhenTheInitializerIsNotConstant)
{
    EXPECT_TRUE(Constexpr("int f(int a) { const int x = a * 2; return x; }\n").empty());
    EXPECT_TRUE(Constexpr("int g();\nconst int kV = g();\n").empty());
    EXPECT_TRUE(Constexpr("int v = 3;\nconst int kW = v + 1;\n").empty());
    EXPECT_TRUE(Constexpr("const int kC = (int)3.5;\n").empty());
    EXPECT_TRUE(Constexpr("const int kS = sizeof(int);\n").empty());
    EXPECT_TRUE(Constexpr("const char* const kP = \"text\";\n").empty());
    EXPECT_TRUE(Constexpr("const int kU = unknown + 1;\n").empty());
    EXPECT_TRUE(Constexpr("const int kQ = ns::kValue;\n").empty());
    EXPECT_TRUE(Constexpr("const std::string kName = \"x\";\n").empty());
    EXPECT_TRUE(Constexpr("const int kSelf = kSelf + 1;\n").empty());
}

TEST(ModernizeConstexpr, SilentWhenEvaluationWouldFail)
{
    EXPECT_TRUE(Constexpr("const int kDiv = 1 / 0;\n").empty());
    EXPECT_TRUE(Constexpr("const int kMod = 5 % 0;\n").empty());
    EXPECT_TRUE(Constexpr("const int kOver = 2147483647 + 1;\n").empty());
    EXPECT_TRUE(Constexpr("const int kMul = 65536 * 65536;\n").empty());
    EXPECT_TRUE(Constexpr("const int kShift = 1 << 40;\n").empty());
    EXPECT_TRUE(Constexpr("const int kNeg = -(-2147483647 - 1);\n").empty());
    EXPECT_TRUE(Constexpr("const double kInf = 1.0 / 0.0;\n").empty());
    // unsigned arithmetic wraps and is fine
    EXPECT_EQ(Constexpr("const unsigned kWrap = 0u - 1u;\n").size(), 1u);
}

TEST(ModernizeConstexpr, SilentForQualifiersItDoesNotTouch)
{
    EXPECT_TRUE(Constexpr("constexpr int kA = 1;\n").empty());
    EXPECT_TRUE(Constexpr("extern const int kB;\n").empty());
    EXPECT_TRUE(Constexpr("const volatile int kC = 1;\n").empty());
    EXPECT_TRUE(Constexpr("thread_local const int kD = 1;\n").empty());
    EXPECT_TRUE(Constexpr("const int kE = 1, kF = 2;\n").empty());
    EXPECT_TRUE(Constexpr("constinit int kG = 1;\n").empty());
}

TEST(ModernizeConstexpr, LocalsThatEscapeOrChangeAreLeftAlone)
{
    EXPECT_TRUE(Constexpr("int f() { int x = 3; ++x; return x; }\n").empty());
    EXPECT_TRUE(Constexpr("int* f() { int x = 3; return &x; }\n").empty());
    EXPECT_TRUE(Constexpr("void f() { int x = 3; unknown(x); }\n").empty());
    EXPECT_TRUE(Constexpr("int f() { int x; x = 3; return x; }\n").empty());
    EXPECT_TRUE(Constexpr("void f() { for (int i = 0; i < 3;) { break; } }\n").empty());
    EXPECT_TRUE(Constexpr("int f() { int x = 3; goto out; out: return x; }\n").empty());
}

// ---- cpp/modernize-constexpr: functions ---------------------------------------------

TEST(ModernizeConstexpr, MarksInternalFunctionsThatOnlyCompute)
{
    const std::string source =
        "static int square(int x) { return x * x; }\n"
    "namespace {\n"
    "int twice(int x) { return 2 * x; }\n"
    "}\n"
    "inline int plus_one(int x) { return x + 1; }\n";
    const auto diagnostics = Constexpr(source);
    ASSERT_EQ(diagnostics.size(), 3u);
    EXPECT_EQ(Flagged(source, diagnostics[0]), "square");
    EXPECT_EQ(Flagged(source, diagnostics[1]), "twice");
    EXPECT_EQ(Flagged(source, diagnostics[2]), "plus_one");
    EXPECT_EQ(ApplyAll(source, diagnostics),
        "static constexpr int square(int x) { return x * x; }\n"
        "namespace {\n"
        "constexpr int twice(int x) { return 2 * x; }\n"
        "}\n"
        "inline constexpr int plus_one(int x) { return x + 1; }\n");
}

TEST(ModernizeConstexpr, DeducedAutoReturnTypesAreLiteralTypes)
{
    const std::string source = "static auto square(int x) { return x * x; }\n";
    const auto diagnostics = Constexpr(source);
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_EQ(Flagged(source, diagnostics[0]), "square");
}

TEST(ModernizeConstexpr, FunctionFixIsOnlyAQuickFix)
{
    const std::string source = "static int square(int x) { return x * x; }\n";
    const auto diagnostics = Constexpr(source);
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_FALSE(diagnostics[0].fix_is_safe);
    EXPECT_EQ(heimdall::RuleEngine::ApplyFixes(source, diagnostics), source);
}

TEST(ModernizeConstexpr, LoopsLocalsAndRecursionAreFine)
{
    const std::string source =
        "static int sum(int n) {\n"
    "    int total = 0;\n"
    "    for (int i = 0; i < n; ++i) { total += i; }\n"
    "    return total;\n"
    "}\n"
    "static int fact(int n) { return n <= 1 ? 1 : n * fact(n - 1); }\n"
    "static unsigned clamp(unsigned v, unsigned lo, unsigned hi) {\n"
    "    if (v < lo) return lo;\n"
    "    if (v > hi) return hi;\n"
    "    return v;\n"
    "}\n";
    EXPECT_EQ(Constexpr(source).size(), 3u);
}

TEST(ModernizeConstexpr, CallsToConstexprFunctionsChain)
{
    const std::string source =
        "static int a(int x) { return x + 1; }\n"
    "static int b(int x) { return a(x) * 2; }\n"
    "constexpr int c(int x) { return x; }\n"
    "static int d(int x) { return c(x) + b(x); }\n";
    const auto diagnostics = Constexpr(source);
    std::vector<std::string> names;
    for (const auto& diagnostic : diagnostics)
    {
        names.push_back(Flagged(source, diagnostic));
    }

    EXPECT_EQ(names, (std::vector<std::string>{"a", "b", "d"}));
}

TEST(ModernizeConstexpr, StaticMembersDefinedInTheClass)
{
    const std::string source =
        "struct S {\n"
    "    static int twice(int x) { return 2 * x; }\n"
    "    int member(int x) { return x; }\n"
    "    virtual int virt(int x) { return x; }\n"
    "};\n";
    const auto diagnostics = Constexpr(source);
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_EQ(Flagged(source, diagnostics[0]), "twice");
}

TEST(ModernizeConstexpr, FunctionsWithExternalLinkageAreLeftAlone)
{
    // `constexpr` implies `inline`: callers in other files would stop linking.
    EXPECT_TRUE(Constexpr("int square(int x) { return x * x; }\n").empty());
}

TEST(ModernizeConstexpr, SilentWhenTheBodyCannotBeEvaluated)
{
    EXPECT_TRUE(Constexpr("static int f(int x) { return unknown(x); }\n").empty());
    EXPECT_TRUE(Constexpr("static int f(int x) { return puts(\"hi\"); }\n").empty());
    for (const auto& d : Constexpr("static int f(int x) { static int calls = 0; return x + calls; }\n"))
    {
        EXPECT_EQ(d.message.find("function"), std::string::npos); // only the variable may be reported
    }

    EXPECT_TRUE(Constexpr("static int f(int x) { int* p = new int(x); return *p; }\n").empty());
    EXPECT_TRUE(Constexpr("static int f(int x) { if (x < 0) throw 1; return x; }\n").empty());
    EXPECT_TRUE(Constexpr("static int f(int x) { try { return x; } catch (...) { return 0; } }\n").empty());
    EXPECT_TRUE(Constexpr("static int f(int x) { auto g = [x]() { return x; }; return g(); }\n").empty());
    EXPECT_TRUE(Constexpr("static int f(int x) { goto out; out: return x; }\n").empty());
    EXPECT_TRUE(Constexpr("static int f(int x) { return reinterpret_cast<int>(&x); }\n").empty());
    EXPECT_TRUE(Constexpr("static int f(int x) { return x; }\nstatic int f(int x);\n").empty());
}

TEST(ModernizeConstexpr, SilentForNonConstantGlobalsAndNonLiteralTypes)
{
    EXPECT_TRUE(Constexpr("int g = 1;\nstatic int f(int x) { return x + g; }\n").empty());
    EXPECT_TRUE(Constexpr("static int f(int x) { g = x; return x; }\n").empty());
    EXPECT_TRUE(Constexpr("static std::string f(int x) { return {}; }\n").empty());
    EXPECT_TRUE(Constexpr("static int f(std::string s) { return 1; }\n").empty());
    EXPECT_TRUE(Constexpr("static int f(int x) { std::string s; return x; }\n").empty());
    EXPECT_TRUE(Constexpr("static auto f(int x) { return std::string(); }\n").empty()); // return type not deduced
    EXPECT_TRUE(Constexpr("static int f(int) { return 1; }\n").empty());
}

TEST(ModernizeConstexpr, SilentForFunctionsThatNeverReturnAValue)
{
    EXPECT_TRUE(Constexpr("static int spin(int x) { for (;;) { } }\n").empty());
    EXPECT_TRUE(Constexpr("static int none(int x) { }\n").empty());
    EXPECT_TRUE(Constexpr("static void empty() { }\n").empty());
}

TEST(ModernizeConstexpr, SilentForTemplatesAndMutualRecursion)
{
    EXPECT_TRUE(Constexpr("template <class T> static T f(T x) { return x; }\n").empty());
    EXPECT_TRUE(Constexpr("template <class T> struct S { static int f(int x) { return x; } };\n").empty());
    EXPECT_TRUE(Constexpr(
        "static int odd(int n);\n"
        "static int even(int n) { return n == 0 ? 1 : odd(n - 1); }\n"
        "static int odd(int n) { return n == 0 ? 0 : even(n - 1); }\n").empty());
}

TEST(ModernizeConstexpr, UsesEnumsAndConstantsOfTheFile)
{
    const std::string source =
        "enum Color { Red, Green };\n"
    "enum class Mode { Fast, Slow };\n"
    "const int kLimit = 8;\n"
    "static int pick(Color c) { return c == Red ? 1 : kLimit; }\n"
    "static int mode(int x) { return x > 0 ? static_cast<int>(Mode::Fast) : 2; }\n";
    const auto diagnostics = Constexpr(source);
    std::vector<std::string> names;
    for (const auto& diagnostic : diagnostics)
    {
        names.push_back(Flagged(source, diagnostic));
    }

    EXPECT_EQ(names, (std::vector<std::string>{"kLimit", "pick", "mode"}));
}

// ---- integration -----------------------------------------------------------------------

TEST(FlowRules, AnalyzeIncludesBothRules)
{
    const std::string source =
        "static int square(int x) { return x * x; }\n"
    "int f(int a) { int x = a * 2; return x; }\n"
    "const int kSize = 4;\n";
    const auto tree = heimdall::ParseTree::Parse(source);
    const auto model = heimdall::Binder::Bind(tree);
    const auto diagnostics = heimdall::SemanticRules::Analyze(model);
    std::vector<std::string> codes;
    for (const auto& diagnostic : diagnostics)
    {
        codes.push_back(diagnostic.code);
    }

    EXPECT_EQ(std::count(codes.begin(), codes.end(), "cpp/modernize-const"), 1);
    EXPECT_EQ(std::count(codes.begin(), codes.end(), "cpp/modernize-constexpr"), 2);
    EXPECT_TRUE(std::is_sorted(diagnostics.begin(), diagnostics.end(),
        [](const heimdall::Diagnostic& a, const heimdall::Diagnostic& b)
        {
            return a.offset < b.offset;
    }));
}

TEST(FlowRules, RulesAreInTheCatalogAndDefaultToWarnings)
{
    bool found_const = false;
    bool found_constexpr = false;
    for (const auto& info : heimdall::RuleCatalog())
    {
        found_const = found_const || info.code == "cpp/modernize-const";
        found_constexpr = found_constexpr || info.code == "cpp/modernize-constexpr";
    }

    EXPECT_TRUE(found_const);
    EXPECT_TRUE(found_constexpr);
}

TEST(FlowRules, BrokenCodeNeverCrashes)
{
    const std::string sources[] = {
        "int f( { int x = ; return x }\n",
        "static int f(int x) { return x +; }\n",
        "int f() { if (",
        "int f() { for (int i = 0; i < ; ++i) {} }\n",
        "int f() { switch (1) { case",
        "int f() { do { } while",
        "static constexpr int f(int x) { return x; }\nconst int k = f(",
        "int f() { int x = 1; { { { x = 2; } } } return x; }\n",
    };
    for (const auto& source : sources)
    {
        const Stack stack(source);
        (void) heimdall::SemanticRules::AnalyzeConst(stack.flow);
        (void) heimdall::SemanticRules::AnalyzeConstexpr(stack.flow);
    }
}

TEST(FlowRules, DeeplyNestedStatementsMakeTheFunctionIncomplete)
{
    std::string source = "int f() { int x = 1;\n";
    for (int i = 0; i < 200; ++i)
    {
        source += "{ ";
    }

    source += "x = 2;";
    for (int i = 0; i < 200; ++i)
    {
        source += " }";
    }

    source += " return x; }\n";
    const Stack stack(source);
    ASSERT_GE(stack.flow.Functions().Size(), 1u);
    EXPECT_EQ(stack.flow.Functions().complete[0], 0);
    EXPECT_TRUE(heimdall::SemanticRules::AnalyzeConst(stack.flow).empty());
}
