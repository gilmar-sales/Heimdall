#include <gtest/gtest.h>

#include <Heimdall/Cpp26Features.hpp>

#include <algorithm>
#include <iostream>
#include <iterator>
#include <string_view>

TEST(Cpp26FeaturesSpec, ReportsAvailableRevisions)
{
    const auto features = heimdall::Cpp26Features();

    for (const auto& feature : features)
    {
        std::cout << feature.name << ": " << feature.value << " (minimum " << feature.required
                  << ") " << (feature.Available() ? "available" : "unavailable") << '\n';
        EXPECT_EQ(feature.Available(), feature.value >= feature.required);
    }

    const auto embed = std::ranges::find(features, std::string_view { "__cpp_pp_embed" },
                                         &heimdall::Cpp26Feature::name);
    ASSERT_NE(embed, features.end());
    EXPECT_EQ(embed->Available(), HEIMDALL_HAS_CPP26_EMBED != 0);

    const auto reflection = std::ranges::find(features, std::string_view { "__cpp_impl_reflection" },
                                              &heimdall::Cpp26Feature::name);
    ASSERT_NE(reflection, features.end());
    EXPECT_EQ(reflection->Available(), HEIMDALL_HAS_CPP26_REFLECTION != 0);
}

TEST(Cpp26FeaturesSpec, EmbedsFixtureWhenSupported)
{
#if HEIMDALL_HAS_CPP26_EMBED && defined(__has_embed)
    #if __has_embed("../fixtures/embed_payload.bin") == __STDC_EMBED_FOUND__
    constexpr unsigned char payload[] = {
        #embed "../fixtures/embed_payload.bin"
    };

    ASSERT_EQ(std::size(payload), 4);
    EXPECT_EQ(payload[0], 0);
    EXPECT_EQ(payload[1], 127);
    EXPECT_EQ(payload[2], 128);
    EXPECT_EQ(payload[3], 255);
    #else
    GTEST_SKIP() << "Compiler cannot embed the test fixture";
    #endif
#else
    GTEST_SKIP() << "C++26 #embed is not advertised by this compiler";
#endif
}
