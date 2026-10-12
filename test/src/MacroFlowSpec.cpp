#include <Heimdall/FlowModel.hpp>
#include <Heimdall/MacroEnvironment.hpp>
#include <Heimdall/SemanticRules.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace
{

    // The analysis layers for `source`, parsed with the macros the text itself defines.
    struct MacroStack
    {
        explicit MacroStack(std::string text, bool withMacros) :
            source(std::move(text)),
            tree(heimdall::ParseTree::Parse(source, Options(source, withMacros))),
            model(heimdall::Binder::Bind(tree)), types(heimdall::Typer::Type(model)),
            flow(heimdall::Flow::Build(types)) {}

        MacroStack(const MacroStack&) = delete;

        MacroStack& operator= (const MacroStack&) = delete;

        static heimdall::ParserOptions Options(const std::string& text, bool withMacros)
        {
            heimdall::ParserOptions options;
            if (withMacros)
            {
                heimdall::MacroEnvironmentOptions environmentOptions;
                environmentOptions.probeCompiler = false;
                options.macro_resolver = std::make_shared<heimdall::MacroEnvironmentResolver>(
                    std::make_shared<const heimdall::MacroEnvironment>(heimdall::MacroEnvironment::Build(
                    "/nonexistent/flow.cpp", text, nullptr, environmentOptions)));
            }

            return options;
        }

        std::string source;
        heimdall::ParseTree tree;
        heimdall::SemanticModel model;
        heimdall::TypeModel types;
        heimdall::FlowModel flow;
    };

    std::vector<std::string> Flagged(const std::string& source, bool withMacros)
    {
        const MacroStack stack(source, withMacros);
        std::vector<std::string> names;
        for (const auto& diagnostic : heimdall::SemanticRules::AnalyzeConst(stack.flow))
        {
            names.push_back(source.substr(diagnostic.offset, diagnostic.length));
        }

        return names;
    }

    const std::string kPrelude =
        "#define MIN(a, b) ((a) < (b) ? (a) : (b))\n"
        "#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))\n"
        "#define CONSUME(x) consume(x)\n"
        "#define BUMP(x) ((x)++)\n"
        "#define TAKE(x) take(&(x))\n";

} // namespace

TEST(MacroFlowSpec, AnArgumentOnlyComparedByAMacroStillCanBeConst)
{
    const std::string source = kPrelude + "int f(int a, int b) { int lo = a * 2; int hi = MIN(lo, b); return hi; }\n";

    EXPECT_EQ(Flagged(source, true), (std::vector<std::string> {"lo", "hi"}));
    const auto without = Flagged(source, false);
    EXPECT_EQ(std::ranges::find(without, std::string("lo")), without.end());
}

TEST(MacroFlowSpec, AnArgumentInsideSizeofIsNeverEvaluated)
{
    const std::string source = kPrelude + "int g() { int table[4] = {1, 2, 3, 4}; int n = ARRAY_SIZE(table); return n; }\n";

    const auto flagged = Flagged(source, true);

    EXPECT_NE(std::ranges::find(flagged, std::string("n")), flagged.end());
}

TEST(MacroFlowSpec, AnArgumentHandedToAFunctionOrWrittenByAMacroStaysAnEscape)
{
    const std::string source = kPrelude +
        "void consume(int);\nvoid take(int*);\n"
        "int h(int a) { int c = a; int b = a; int t = a; CONSUME(c); BUMP(b); TAKE(t); return 0; }\n";

    const auto flagged = Flagged(source, true);

    EXPECT_EQ(std::ranges::find(flagged, std::string("c")), flagged.end());
    EXPECT_EQ(std::ranges::find(flagged, std::string("b")), flagged.end());
    EXPECT_EQ(std::ranges::find(flagged, std::string("t")), flagged.end());
}
