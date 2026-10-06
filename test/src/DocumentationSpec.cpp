#include <Heimdall/RuleEngine.hpp>
#include <Heimdall/SemanticRules.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <string>

namespace
{

    using heimdall::DocScope;

    std::vector<heimdall::Diagnostic> Comments(const std::string& source,
        DocScope scope = DocScope::Public)
    {
        const auto tree = heimdall::ParseTree::Parse(source);
        const auto model = heimdall::Binder::Bind(tree);
        return heimdall::SemanticRules::AnalyzeRequireDocComment(model, scope);
    }

    std::vector<heimdall::Diagnostic> Style(const std::string& source,
        DocScope scope = DocScope::Public)
    {
        const auto tree = heimdall::ParseTree::Parse(source);
        const auto model = heimdall::Binder::Bind(tree);
        return heimdall::SemanticRules::AnalyzeDoxygenStyle(model, scope);
    }

    bool Mentions(const std::vector<heimdall::Diagnostic>& diagnostics, const std::string& text)
    {
        return std::any_of(diagnostics.begin(), diagnostics.end(),
            [&](const heimdall::Diagnostic& diagnostic)
            {
                return diagnostic.message.find(text) != std::string::npos;
        });
    }

    // A comment that satisfies every check, for the tests that break one thing.
    const std::string kGood =
        "/**\n"
    " * @brief Calculates the area of a circle.\n"
    " *\n"
    " * Longer explanation.\n"
    " *\n"
    " * @param radius Radius in meters, at least 0.\n"
    " * @return The area in square meters.\n"
    " */\n"
    "double area(double radius);\n";

} // namespace

TEST(DocFix, GeneratesACompleteTemplateAndRequiresReview)
{
    const std::string source = "template<class T>\nT convert(T value);\n";
    const auto diagnostics = Comments(source);
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_EQ(heimdall::RuleEngine::ApplyFixes(source, diagnostics), source);
    const auto fixed = heimdall::RuleEngine::ApplyFixes(source, diagnostics, true);
    EXPECT_NE(fixed.find("@brief TODO:"), std::string::npos);
    EXPECT_NE(fixed.find("@tparam T TODO:"), std::string::npos);
    EXPECT_NE(fixed.find("@param value TODO:"), std::string::npos);
    EXPECT_NE(fixed.find("@return TODO:"), std::string::npos);
    EXPECT_TRUE(Comments(fixed).empty());
    EXPECT_TRUE(Style(fixed).empty());
}

TEST(DocFix, RepairsStyleWithoutLosingDocumentation)
{
    for (const std::string source : {
        "//! @brief Runs.\nvoid run();\n",
        "/** @brief Runs. \\note Keep this. */\nvoid run();\n",
        "/** @brief Runs. More details. */\nvoid run();\n",
        "/**\n * @brief Runs.\n * More details.\n */\nvoid run();\n",
        "/// @brief Runs.\n/// More details.\nvoid run();\n",
        "/**\n * @brief Runs\n * with input. More details.\n */\nvoid run();\n",
        "void run(); //!< @brief Runs.\n"})
    {
        SCOPED_TRACE(source);
        const auto diagnostics = Style(source);
        ASSERT_FALSE(diagnostics.empty());
        for (const auto& diagnostic : diagnostics)
        {
            ASSERT_TRUE(diagnostic.has_fix);
            EXPECT_TRUE(diagnostic.fix_is_safe);
        }
        const auto fixed = heimdall::RuleEngine::ApplyFixes(source, diagnostics);
        EXPECT_NE(fixed, source);
        EXPECT_TRUE(Style(fixed).empty()) << fixed;
        EXPECT_NE(fixed.find("Runs"), std::string::npos);
        if (source.find("More details.") != std::string::npos)
            EXPECT_NE(fixed.find("More details."), std::string::npos);
        if (source.find("Keep this.") != std::string::npos)
            EXPECT_NE(fixed.find("Keep this."), std::string::npos);
        EXPECT_EQ(heimdall::RuleEngine::ApplyFixes(fixed, Style(fixed)), fixed);
    }
}

TEST(DocFix, RepairsMissingAndInvalidTagsTogether)
{
    for (const std::string source : {
        "/** text */\nint f(int x);\n",
        "/** @brief */\nvoid f();\n",
        "/** @brief Runs. @param x */\nvoid f(int x);\n",
        "/** @brief Runs. @param old Keep prose. */\nvoid f(int x);\n",
        "/** @brief Runs. @param x First. @param x Duplicate. */\nvoid f(int x);\n",
        "/** @brief Runs. @param x,x Duplicate. */\nvoid f(int x);\n",
        "/** @brief Runs. @param[in] x @return */\nint f(int x);\n",
        "/** @brief Runs. @param */\nvoid f(int x);\n",
        "/** @brief Runs. @return Keep prose. */\nvoid f();\n",
        "/** @brief Runs. @return */\nint f();\n",
        "/** @brief Runs. */\nvoid f() { throw Error{}; }\n",
        "/** @brief Runs. @throws Error */\nvoid f();\n",
        "/** @brief Runs. @throws */\nvoid f();\n",
        "/** @brief Container. */\ntemplate<class T> struct Container {};\n",
        "int f(int x); ///< @brief Calculates.\n"})
    {
        SCOPED_TRACE(source);
        const auto diagnostics = Style(source);
        ASSERT_FALSE(diagnostics.empty());
        for (const auto& diagnostic : diagnostics)
        {
            EXPECT_TRUE(diagnostic.has_fix);
            EXPECT_FALSE(diagnostic.fix_is_safe);
        }
        EXPECT_EQ(heimdall::RuleEngine::ApplyFixes(source, diagnostics), source);
        const auto fixed = heimdall::RuleEngine::ApplyFixes(source, diagnostics, true);
        EXPECT_NE(fixed, source);
        EXPECT_TRUE(Style(fixed).empty()) << fixed;
        if (source.find("Keep prose.") != std::string::npos)
            EXPECT_NE(fixed.find("Keep prose."), std::string::npos);
        if (source.find("Duplicate.") != std::string::npos)
            EXPECT_NE(fixed.find("Duplicate."), std::string::npos);
    }
}

TEST(DocFix, PreservesCodeIndentationAndInlineCommands)
{
    const std::string source =
        "//! @brief Runs.\n"
        "//!\n"
        "//! @code\n"
        "//! if (ready) {\n"
        "//!     run();\n"
        "//! }\n"
        "//! @endcode\n"
        "//! @note Uses \\p ready.\n"
        "void run();\n";
    const auto fixed = heimdall::RuleEngine::ApplyFixes(source, Style(source));
    EXPECT_NE(fixed.find(" *     run();"), std::string::npos);
    EXPECT_NE(fixed.find("Uses \\p ready."), std::string::npos);
    EXPECT_TRUE(Style(fixed).empty());
}

TEST(DocFix, PreservesIndentationCrlfAndDetachedDocumentation)
{
    const std::string source = "/** @file widget.hpp */\r\nstruct Widget\r\n{\r\n\tint get(int x);\r\n};\r\n";
    const auto diagnostics = Comments(source);
    const auto fixed = heimdall::RuleEngine::ApplyFixes(source, diagnostics, true);
    EXPECT_TRUE(fixed.starts_with("/** @file widget.hpp */\r\n"));
    EXPECT_NE(fixed.find("\t * @param x"), std::string::npos);
    EXPECT_NE(fixed.find("\t */\r\n\tint get"), std::string::npos);
    EXPECT_TRUE(Comments(fixed).empty());
    EXPECT_TRUE(Style(fixed).empty());
    for (std::size_t i = 0; i < fixed.size(); ++i)
        if (fixed[i] == '\n') EXPECT_TRUE(i > 0 && fixed[i - 1] == '\r');
}

// ---- doc/require-comment ---------------------------------------------------

TEST(DocRequireComment, ReportsAnUndocumentedFunctionClassAndEnum)
{
    const std::string source =
        "void run();\n"
    "struct Widget { int x; };\n"
    "enum class Color { Red };\n";
    const auto diagnostics = Comments(source);
    ASSERT_EQ(diagnostics.size(), 3u);
    for (const auto& diagnostic : diagnostics)
    {
        EXPECT_EQ(diagnostic.code, "doc/require-comment");
        EXPECT_EQ(diagnostic.rule, heimdall::RuleId::DocRequireComment);
        EXPECT_TRUE(diagnostic.has_fix);
        EXPECT_FALSE(diagnostic.fix_is_safe);
    }

    EXPECT_EQ(source.substr(diagnostics[0].offset, diagnostics[0].length), "run");
    EXPECT_EQ(source.substr(diagnostics[1].offset, diagnostics[1].length), "Widget");
    EXPECT_EQ(source.substr(diagnostics[2].offset, diagnostics[2].length), "Color");
    EXPECT_EQ(diagnostics[1].line, 2u);
}

TEST(DocRequireComment, AcceptsEveryDoxygenCommentForm)
{
    EXPECT_TRUE(Comments("/** @brief x */\nvoid a();\n").empty());
    EXPECT_TRUE(Comments("/*! @brief x */\nvoid a();\n").empty());
    EXPECT_TRUE(Comments("/// @brief x\nvoid a();\n").empty());
    EXPECT_TRUE(Comments("//! @brief x\nvoid a();\n").empty());
    EXPECT_TRUE(Comments("/// first\n/// second\nvoid a();\n").empty());
    EXPECT_TRUE(Comments("void a(); ///< trailing\n").empty());
    EXPECT_TRUE(Comments("void a(); /**< trailing */\n").empty());
}

TEST(DocRequireComment, PlainCommentsAndBannersAreNotDocumentation)
{
    EXPECT_EQ(Comments("// just a note\nvoid a();\n").size(), 1u);
    EXPECT_EQ(Comments("/* just a note */\nvoid a();\n").size(), 1u);
    EXPECT_EQ(Comments("//// banner\nvoid a();\n").size(), 1u);
    EXPECT_EQ(Comments("/**/\nvoid a();\n").size(), 1u);
    EXPECT_EQ(Comments("/***** banner *****/\nvoid a();\n").size(), 1u);
}

TEST(DocRequireComment, ABlankLineSeparatesTheCommentFromTheDeclaration)
{
    EXPECT_EQ(Comments("/// @brief x\n\nvoid a();\n").size(), 1u);
}

TEST(DocRequireComment, ATrailingCommentDoesNotDocumentTheNextDeclaration)
{
    const auto diagnostics = Comments("void a(); ///< about a\nvoid b();\n");
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_TRUE(Mentions(diagnostics, "'b'"));
}

TEST(DocRequireComment, ACommentBeforeATemplateHeaderCountsAndAFileBlockDoesNot)
{
    EXPECT_TRUE(Comments("/// @brief x\ntemplate <class T>\nvoid a(T t);\n").empty());
    EXPECT_EQ(Comments("/** @file\n * Describes the file. */\nvoid a();\n").size(), 1u);
}

TEST(DocRequireComment, MembersFollowTheScope)
{
    const std::string source =
        "/// @brief A class.\n"
    "class A\n"
    "{\n"
    "public:\n"
    "    void open();\n"
    "protected:\n"
    "    void hook();\n"
    "private:\n"
    "    void secret();\n"
    "};\n";
    const auto in_public = Comments(source, DocScope::Public);
    ASSERT_EQ(in_public.size(), 2u);
    EXPECT_TRUE(Mentions(in_public, "'open'"));
    EXPECT_TRUE(Mentions(in_public, "'hook'"));

    const auto in_private = Comments(source, DocScope::Private);
    ASSERT_EQ(in_private.size(), 1u);
    EXPECT_TRUE(Mentions(in_private, "'secret'"));

    EXPECT_EQ(Comments(source, DocScope::All).size(), 3u);
}

TEST(DocRequireComment, StructMembersArePublicAndClassMembersArePrivateByDefault)
{
    EXPECT_EQ(Comments("/// x\nstruct S { void f(); };\n").size(), 1u);
    EXPECT_TRUE(Comments("/// x\nclass C { void f(); };\n").empty());
    EXPECT_EQ(Comments("/// x\nclass C { void f(); };\n", DocScope::Private).size(), 1u);
}

TEST(DocRequireComment, InternalLinkageIsPrivateScope)
{
    const std::string source =
        "static void hidden();\n"
    "namespace { void anonymous(); }\n"
    "namespace api { void visible(); }\n";
    const auto in_public = Comments(source, DocScope::Public);
    ASSERT_EQ(in_public.size(), 1u);
    EXPECT_TRUE(Mentions(in_public, "'visible'"));
    EXPECT_EQ(Comments(source, DocScope::Private).size(), 2u);
    EXPECT_EQ(Comments(source, DocScope::All).size(), 3u);
}

TEST(DocRequireComment, AStaticMemberFunctionIsStillPartOfTheClass)
{
    EXPECT_EQ(Comments("/// x\nstruct S { static void make(); };\n").size(), 1u);
}

TEST(DocRequireComment, MembersOfAPrivateNestedClassArePrivate)
{
    const std::string source =
        "/// x\n"
    "class Outer\n"
    "{\n"
    "    struct Inner { void f(); };\n"
    "};\n";
    EXPECT_TRUE(Comments(source, DocScope::Public).empty());
    EXPECT_EQ(Comments(source, DocScope::Private).size(), 2u);
}

TEST(DocRequireComment, SkipsWhatIsDocumentedElsewhere)
{
    EXPECT_TRUE(Comments("/// x\nstruct A { A() = default; A(const A &) = delete; };\n").empty());
    // Only B::f, the declaration that introduces the function, needs the comment.
    EXPECT_EQ(Comments("/// x\nstruct B { virtual void f(); };\n"
        "/// y\nstruct D : B { void f() override; };\n").size(), 1u);
    // And only the declaration in the class, not the definition outside it.
    EXPECT_EQ(Comments("/// x\nstruct S { void f(); };\nvoid S::f() {}\n").size(), 1u);
    EXPECT_TRUE(Comments("int main() { return 0; }\n").empty());
    EXPECT_TRUE(Comments("struct F;\nenum class E : int;\n").empty());
    EXPECT_TRUE(Comments("/// x\ntemplate <class T> void f(T);\ntemplate <> void f<int>(int);\n").empty());
}

TEST(DocRequireComment, ADefinitionAfterADocumentedDeclarationIsFine)
{
    EXPECT_TRUE(Comments("/// @brief x\nvoid f(int a);\nvoid f(int a) {}\n").empty());
}

TEST(DocRequireComment, LocalDeclarationsAreSkipped)
{
    EXPECT_TRUE(Comments("/// x\nvoid f() { struct Local { void g(); }; }\n", DocScope::All).empty());
}

TEST(DocRequireComment, MacroDecoratedDeclarationsKeepTheirComment)
{
    EXPECT_TRUE(Comments("/// @brief x\nclass __declspec(dllexport) A {};\n").empty());
}

TEST(DocRequireComment, SurvivesBrokenInput)
{
    for (const char* source :
        {
            "/** unterminated", "struct A { void f(", "template <class T", "enum E {", "/// x\n",
            "void f(int, , );\n", ""
    })
    {
        EXPECT_NO_THROW((void) Comments(source, DocScope::All)) << source;
        EXPECT_NO_THROW((void) Style(source, DocScope::All)) << source;
    }
}

// ---- doc/doxygen-style -----------------------------------------------------

TEST(DocDoxygenStyle, AcceptsAWellFormedComment)
{
    EXPECT_TRUE(Style(kGood).empty());
}

TEST(DocDoxygenStyle, UndocumentedDeclarationsAreLeftToRequireComment)
{
    EXPECT_TRUE(Style("double area(double radius);\n").empty());
}

TEST(DocDoxygenStyle, RequiresABrief)
{
    const auto diagnostics = Style("/** Calculates the area. */\nvoid f();\n");
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_EQ(diagnostics[0].code, "doc/doxygen-style");
    EXPECT_EQ(diagnostics[0].rule, heimdall::RuleId::DocDoxygenStyle);
    EXPECT_TRUE(Mentions(diagnostics, "@brief"));
    EXPECT_EQ(diagnostics[0].line, 1u);
}

TEST(DocDoxygenStyle, AcceptsBriefAndShortAndBackslashSpelling)
{
    EXPECT_TRUE(Style("/// @brief Does it.\nvoid f();\n").empty());
    EXPECT_TRUE(Style("/// @short Does it.\nvoid f();\n").empty());
    EXPECT_TRUE(Style("/// \\brief Does it.\nvoid f();\n").empty());
}

TEST(DocDoxygenStyle, ABriefHasOneSentence)
{
    EXPECT_TRUE(Mentions(Style("/// @brief Does it. Then does more.\nvoid f();\n"), "single sentence"));
    EXPECT_TRUE(Style("/// @brief Uses e.g. Foo and i.e. Bar.\nvoid f();\n").empty());
    EXPECT_TRUE(Style("/// @brief Does it with\n/// a wrapped line.\nvoid f();\n").empty());
}

TEST(DocDoxygenStyle, TheBriefIsSeparatedFromTheDetailsByABlankLine)
{
    EXPECT_TRUE(Mentions(Style("/// @brief Does it.\n/// More about it.\nvoid f();\n"), "blank line"));
    EXPECT_TRUE(Style("/// @brief Does it.\n///\n/// More about it.\nvoid f();\n").empty());
}

TEST(DocDoxygenStyle, AnEmptyBriefIsReported)
{
    EXPECT_TRUE(Mentions(Style("/// @brief\nvoid f();\n"), "no text"));
}

TEST(DocDoxygenStyle, EveryNamedParameterNeedsAParam)
{
    const std::string source = "/// @brief Adds.\n/// @param a First.\n/// @return Sum.\nint add(int a, int b);\n";
    const auto diagnostics = Style(source);
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_TRUE(Mentions(diagnostics, "'b' is not documented"));
    EXPECT_EQ(source.substr(diagnostics[0].offset, diagnostics[0].length), "b");
}

TEST(DocDoxygenStyle, UnnamedAndVoidParametersNeedNothing)
{
    EXPECT_TRUE(Style("/// @brief x.\nvoid f(void);\n").empty());
    EXPECT_TRUE(Style("/// @brief x.\nvoid f(int, const std::string &);\n").empty());
    EXPECT_TRUE(Style("/// @brief x.\nvoid f(const Foo);\n").empty());
}

TEST(DocDoxygenStyle, FindsTheNameOfComplexParameters)
{
    const std::string source =
        "/// @brief x.\n"
    "void f(int values[4], void (*callback)(int), std::function<void(int)> handler, const char *text = \"a,b\",\n"
    "       std::pair<int, int> pair = {1, 2});\n";
    const auto diagnostics = Style(source);
    ASSERT_EQ(diagnostics.size(), 5u);
    for (const char* name :
        {
            "'values'", "'callback'", "'handler'", "'text'", "'pair'"
    })
    {
        EXPECT_TRUE(Mentions(diagnostics, name)) << name;
    }
}

TEST(DocDoxygenStyle, ParamAcceptsDirectionsListsAndBackslash)
{
    EXPECT_TRUE(Style("/// @brief x.\n/// @param[in] a In.\n/// @param[out] b Out.\nvoid f(int a, int *b);\n").empty());
    EXPECT_TRUE(Style("/// @brief x.\n/// @param a,b Both.\nvoid f(int a, int b);\n").empty());
    EXPECT_TRUE(Style("/// \\brief x.\n/// \\param a Value.\nvoid f(int a);\n").empty());
}

TEST(DocDoxygenStyle, ReportsAParamThatMatchesNothingOrRepeats)
{
    const std::string unknown = "/// @brief x.\n/// @param a A.\n/// @param z Z.\nvoid f(int a);\n";
    const auto stale = Style(unknown);
    ASSERT_EQ(stale.size(), 1u);
    EXPECT_TRUE(Mentions(stale, "'z' does not match"));
    EXPECT_EQ(stale[0].line, 3u);

    EXPECT_TRUE(Mentions(Style("/// @brief x.\n/// @param a A.\n/// @param a Again.\nvoid f(int a);\n"),
        "more than once"));
}

TEST(DocDoxygenStyle, ATagNeedsADescription)
{
    EXPECT_TRUE(Mentions(Style("/// @brief x.\n/// @param a\nvoid f(int a);\n"), "no description"));
    EXPECT_TRUE(Style("/// @brief x.\n/// @param a Value that\n/// continues.\nvoid f(int a);\n").empty());
}

TEST(DocDoxygenStyle, AFunctionThatReturnsAValueNeedsReturn)
{
    const auto diagnostics = Style("/// @brief x.\nint f();\n");
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_TRUE(Mentions(diagnostics, "no @return"));
    EXPECT_TRUE(Style("/// @brief x.\n/// @returns Value.\nint f();\n").empty());
    EXPECT_TRUE(Style("/// @brief x.\n/// @result Value.\nint f();\n").empty());
    EXPECT_TRUE(Style("/// @brief x.\n/// @retval 0 Done.\nint f();\n").empty());
    EXPECT_TRUE(Mentions(Style("/// @brief x.\n/// @return\nint f();\n"), "no description"));
}

TEST(DocDoxygenStyle, VoidConstructorsAndDestructorsNeedNoReturn)
{
    EXPECT_TRUE(Style("/// @brief x.\nvoid f();\n").empty());
    EXPECT_TRUE(Style("/// @brief x.\nauto f() -> void;\n").empty());
    EXPECT_TRUE(Style("/// @brief x.\n[[noreturn]] static inline void f();\n").empty());
    EXPECT_TRUE(Style("/// @brief x.\nstruct S\n{\n    /// @brief Builds.\n    S();\n    /// @brief Destroys.\n    ~S();\n};\n").empty());
}

TEST(DocDoxygenStyle, PointerAndTemplateReturnsCount)
{
    EXPECT_EQ(Style("/// @brief x.\nvoid *f();\n").size(), 1u);
    EXPECT_EQ(Style("/// @brief x.\nstd::vector<int> f();\n").size(), 1u);
    EXPECT_EQ(Style("/// @brief x.\nauto f() -> int;\n").size(), 1u);
}

TEST(DocDoxygenStyle, ADeducedReturnTypeIsNotGuessed)
{
    EXPECT_TRUE(Style("/// @brief x.\nauto f() { return 1; }\n").empty());
}

TEST(DocDoxygenStyle, ReturnOnAVoidFunctionIsReported)
{
    const auto diagnostics = Style("/// @brief x.\n/// @return Nothing.\nvoid f();\n");
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_TRUE(Mentions(diagnostics, "returns nothing"));
    EXPECT_EQ(diagnostics[0].line, 2u);
}

TEST(DocDoxygenStyle, ThrowRequiresThrows)
{
    const std::string body = "{ if (a < 0) { throw 1; } }\n";
    EXPECT_TRUE(Mentions(Style("/// @brief x.\n/// @param a A.\nvoid f(int a) " + body), "can throw"));
    EXPECT_TRUE(Style("/// @brief x.\n/// @param a A.\n/// @throws int When a is negative.\nvoid f(int a) " + body).empty());
    EXPECT_TRUE(Style("/// @brief x.\n/// @param a A.\n/// @exception int When negative.\nvoid f(int a) " + body).empty());
}

TEST(DocDoxygenStyle, ThrowsInsideATryOrANoexceptFunctionAreNotReported)
{
    EXPECT_TRUE(Style("/// @brief x.\nvoid f() { try { throw 1; } catch (...) {} }\n").empty());
    EXPECT_TRUE(Style("/// @brief x.\nvoid f() noexcept { throw 1; }\n").empty());
}

TEST(DocDoxygenStyle, AThrowsNeedsTheTypeAndTheCondition)
{
    EXPECT_TRUE(Mentions(Style("/// @brief x.\n/// @throws\nvoid f();\n"), "exception type"));
    EXPECT_TRUE(Mentions(Style("/// @brief x.\n/// @throws std::runtime_error\nvoid f();\n"),
        "exception type"));
}

TEST(DocDoxygenStyle, TemplateParametersNeedTparam)
{
    const std::string source =
        "/// @brief x.\n"
    "/// @tparam T Element.\n"
    "template <typename T, typename U, int N = 3, template <class> class C = std::vector>\n"
    "void f();\n";
    const auto diagnostics = Style(source);
    ASSERT_EQ(diagnostics.size(), 3u);
    EXPECT_TRUE(Mentions(diagnostics, "'U'"));
    EXPECT_TRUE(Mentions(diagnostics, "'N'"));
    EXPECT_TRUE(Mentions(diagnostics, "'C'"));
}

TEST(DocDoxygenStyle, UnnamedTemplateParametersNeedNothingAndClassesCount)
{
    EXPECT_TRUE(Style("/// @brief x.\ntemplate <typename = void>\nvoid f();\n").empty());
    EXPECT_TRUE(Mentions(Style("/// @brief x.\ntemplate <class T>\nstruct S {};\n"), "'T'"));
    EXPECT_TRUE(Style("/// @brief x.\n/// @tparam T Element.\ntemplate <class T>\nstruct S {};\n").empty());
    EXPECT_TRUE(Mentions(Style("/// @brief x.\n/// @tparam Q None.\ntemplate <class T>\nstruct S {};\n"),
        "'Q' does not match"));
}

TEST(DocDoxygenStyle, ClassesAndEnumsOnlyNeedABrief)
{
    EXPECT_TRUE(Style("/// @brief A thing.\nstruct S {};\n").empty());
    EXPECT_TRUE(Style("/// @brief Colors.\nenum class Color { Red };\n").empty());
    EXPECT_TRUE(Mentions(Style("/// A thing.\nstruct S {};\n"), "@brief"));
}

TEST(DocDoxygenStyle, QtStyleAndMixedSpellingsAreReported)
{
    EXPECT_TRUE(Mentions(Style("//! @brief x.\nvoid f();\n"), "Javadoc style"));
    EXPECT_TRUE(Mentions(Style("/*! @brief x. */\nvoid f();\n"), "Javadoc style"));
    EXPECT_TRUE(Style("/** @brief x. */\nvoid f();\n").empty());
    EXPECT_TRUE(Mentions(Style("/// @brief x.\n/// \\param a A.\nvoid f(int a);\n"), "mixes"));
}

TEST(DocDoxygenStyle, CopyingAndInheritingCommentsAreSkipped)
{
    EXPECT_TRUE(Style("/// @copydoc other\nint f(int a);\n").empty());
    EXPECT_TRUE(Style("/// @inheritdoc\nint f(int a);\n").empty());
    EXPECT_TRUE(Style("/// @overload\nint f(int a);\n").empty());
}

TEST(DocDoxygenStyle, FileBlocksAreNotDeclarationDocs)
{
    EXPECT_TRUE(Style("/** @file\n * @brief Thing. */\nint f(int a);\n").empty());
}

TEST(DocDoxygenStyle, InlineFormulasDoNotSwallowTheTagDescription)
{
    EXPECT_TRUE(Style("/**\n * @brief Area of \\f$ \\pi r^2 \\f$.\n *\n * @param r Radius \\f$ r \\f$ in meters.\n * @return Area.\n */\ndouble f(double r);\n").empty());
}

TEST(DocDoxygenStyle, MultiLineBlockCommentsOfDoxygenAreParsed)
{
    const std::string source =
        "/**\n"
    " * @brief Calculates the area of a circle.\n"
    " *\n"
    " * @param radius The radius.\n"
    " * @return The area.\n"
    " * @throws std::invalid_argument If the radius is negative.\n"
    " *\n"
    " * @see other\n"
    " * @note Thread-safe.\n"
    " */\n"
    "double f(double radius)\n"
    "{\n"
    "    if (radius < 0.0) { throw std::invalid_argument(\"x\"); }\n"
    "    return radius;\n"
    "}\n";
    EXPECT_TRUE(Style(source).empty());
}

TEST(DocDoxygenStyle, OperatorsAndOutOfClassDefinitionsAreChecked)
{
    EXPECT_TRUE(Mentions(Style("/// @brief x.\nbool operator==(const A &lhs, const A &rhs);\n"),
        "'lhs'"));
    EXPECT_TRUE(Mentions(Style("struct S { int f(int a); };\n/// @brief x.\nint S::f(int a) { return a; }\n",
        DocScope::All),
        "'a' is not documented"));
}

TEST(DocDoxygenStyle, ScopeLimitsWhichDeclarationsAreChecked)
{
    const std::string source =
        "/// @brief x.\n"
    "static int hidden();\n"
    "/// @brief y.\n"
    "int shown();\n";
    const auto in_public = Style(source, DocScope::Public);
    ASSERT_EQ(in_public.size(), 1u);
    EXPECT_TRUE(Mentions(in_public, "'shown'"));
    const auto in_private = Style(source, DocScope::Private);
    ASSERT_EQ(in_private.size(), 1u);
    EXPECT_TRUE(Mentions(in_private, "'hidden'"));
    EXPECT_EQ(Style(source, DocScope::All).size(), 2u);
}

TEST(DocDoxygenStyle, TrailingDocumentationIsChecked)
{
    EXPECT_TRUE(Mentions(Style("int f(int a); ///< Does it.\n"), "@brief"));
}

// ---- wiring ----------------------------------------------------------------

TEST(DocRules, AreInTheCatalogAndOffByDefault)
{
    for (const auto code :
        {
            "doc/require-comment", "doc/doxygen-style"
    })
    {
        ASSERT_TRUE(heimdall::IsKnownRuleCode(code)) << code;
        EXPECT_EQ(heimdall::FindRuleByCode(code) -> category, "doc");
        EXPECT_TRUE(heimdall::FindRuleByCode(code)->autofix);
    }

    const std::string source = "void f(int a);\n";
    const auto tree = heimdall::ParseTree::Parse(source);
    const auto model = heimdall::Binder::Bind(tree);
    EXPECT_TRUE(heimdall::SemanticRules::AnalyzeDocumentation(model, heimdall::RuleEngine()).empty());
    EXPECT_TRUE(heimdall::SemanticRules::Analyze(model).empty());
}

TEST(DocRules, RunWhenEnabledAndFollowTheEngineScope)
{
    const std::string source = "void f(int a);\nstatic void g();\n";
    const auto tree = heimdall::ParseTree::Parse(source);
    const auto model = heimdall::Binder::Bind(tree);

    heimdall::RuleOptions options;
    options.overrides.push_back({"doc/require-comment", true, heimdall::Severity::Error});
    EXPECT_EQ(heimdall::SemanticRules::AnalyzeDocumentation(model,
        heimdall::RuleEngine(options)).size(),
        1u);

    options.doc_scope = DocScope::All;
    auto all = heimdall::SemanticRules::AnalyzeDocumentation(model, heimdall::RuleEngine(options));
    ASSERT_EQ(all.size(), 2u);
    EXPECT_LT(all[0].offset, all[1].offset);

    // Severity and disabling go through ApplyPolicy like any other rule.
    const auto policed = heimdall::RuleEngine(options).ApplyPolicy(std::move(all), tree);
    ASSERT_EQ(policed.size(), 2u);
    EXPECT_EQ(policed[0].severity, heimdall::Severity::Error);

    options.overrides.push_back({"doc/doxygen-style", true, heimdall::Severity::Warning});
    EXPECT_EQ(heimdall::SemanticRules::AnalyzeDocumentation(model,
        heimdall::RuleEngine(options)).size(),
        2u);
}

TEST(DocRules, ASuppressionCommentSilencesAFinding)
{
    const std::string source = "void f(); // heimdall-disable-line doc/require-comment\n";
    const auto tree = heimdall::ParseTree::Parse(source);
    const auto model = heimdall::Binder::Bind(tree);
    heimdall::RuleOptions options;
    options.overrides.push_back({"doc/require-comment", true, heimdall::Severity::Warning});
    const heimdall::RuleEngine engine(options);
    EXPECT_TRUE(engine.ApplyPolicy(heimdall::SemanticRules::AnalyzeDocumentation(model, engine),
        tree).empty());
}
