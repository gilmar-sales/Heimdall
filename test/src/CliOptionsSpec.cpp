#include <gtest/gtest.h>

#include "CliOptions.hpp"

#include <string>
#include <vector>

namespace
{

    bool ParseArgs(std::vector<std::string> args, heimdall::cli::Options & options)
    {
        std::vector<char *> argv;
        argv.push_back(const_cast<char *>("heimdall"));
        for (auto & arg: args)
        {
            argv.push_back(arg.data());
        }

        return heimdall::cli::ParseOptions(static_cast<int>(argv.size()), argv.data(), options);
    }

} // namespace

TEST(CliOptionsSpec, ParsesPointerAndReferenceAlignment)
{
    heimdall::cli::Options options{};
    EXPECT_TRUE(ParseArgs({"format", "--pointer-alignment", "left", "--reference-alignment", "right", "a.cpp"},
        options));
    EXPECT_TRUE(options.pointer_alignment_override);
    EXPECT_EQ(options.pointer_alignment, heimdall::PointerAlignment::Left);
    EXPECT_TRUE(options.reference_alignment_override);
    EXPECT_EQ(options.reference_alignment, heimdall::ReferenceAlignment::Right);
}

TEST(CliOptionsSpec, ParsesAlignmentEqualsForm)
{
    heimdall::cli::Options options{};
    EXPECT_TRUE(ParseArgs({"format", "--pointer-alignment=right", "--reference-alignment=left", "a.cpp"},
        options));
    EXPECT_EQ(options.pointer_alignment, heimdall::PointerAlignment::Right);
    EXPECT_EQ(options.reference_alignment, heimdall::ReferenceAlignment::Left);
}

TEST(CliOptionsSpec, RejectsInvalidAlignmentValue)
{
    heimdall::cli::Options options{};
    EXPECT_FALSE(ParseArgs({"format", "--pointer-alignment", "middle", "a.cpp"}, options));
    EXPECT_FALSE(ParseArgs({"format", "--reference-alignment", "center", "a.cpp"}, options));
}

TEST(CliOptionsSpec, RejectsAlignmentFlagsOnOtherCommands)
{
    heimdall::cli::Options options{};
    EXPECT_FALSE(ParseArgs({"lint", "--pointer-alignment", "left", "a.cpp"}, options));
    EXPECT_FALSE(ParseArgs({"check", "--reference-alignment", "right", "a.cpp"}, options));
}

TEST(CliOptionsSpec, AlignmentDefaultsToLeftWithoutFlags)
{
    heimdall::cli::Options options{};
    EXPECT_TRUE(ParseArgs({"format", "a.cpp"}, options));
    EXPECT_FALSE(options.pointer_alignment_override);
    EXPECT_FALSE(options.reference_alignment_override);
    EXPECT_EQ(options.pointer_alignment, heimdall::PointerAlignment::Left);
    EXPECT_EQ(options.reference_alignment, heimdall::ReferenceAlignment::Left);
}

TEST(CliOptionsSpec, FixUnsafeImpliesFix)
{
    heimdall::cli::Options options{};
    EXPECT_TRUE(ParseArgs({"lint", "--fix-unsafe", "a.cpp"}, options));
    EXPECT_TRUE(options.fix);
    EXPECT_TRUE(options.fix_unsafe);

    heimdall::cli::Options safe_only{};
    EXPECT_TRUE(ParseArgs({"lint", "--fix", "a.cpp"}, safe_only));
    EXPECT_TRUE(safe_only.fix);
    EXPECT_FALSE(safe_only.fix_unsafe);
}

TEST(CliOptionsSpec, FixUnsafeIsRejectedOutsideLintAndWithJson)
{
    heimdall::cli::Options check{};
    EXPECT_FALSE(ParseArgs({"check", "--fix-unsafe", "a.cpp"}, check));

    heimdall::cli::Options json{};
    EXPECT_FALSE(ParseArgs({"lint", "--json", "--fix-unsafe", "a.cpp"}, json));
}
