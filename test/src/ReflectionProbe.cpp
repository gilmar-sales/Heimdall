#include <gtest/gtest.h>

#include <Heimdall/Cpp26Features.hpp>
#include <meta>

#include <concepts>
#include <string_view>

#if !HEIMDALL_HAS_CPP26_REFLECTION
    #error Reflection probe requires __cpp_impl_reflection >= 202506L
#endif

namespace
{
    struct Sample
    {
        int value;
    };

    static_assert(std::meta::identifier_of(^^Sample) == std::string_view {"Sample"});

    static_assert(std::meta::identifier_of(^^Sample::value) == std::string_view {"value"});

    static_assert(std::meta::is_type(^^Sample));

    static_assert(std::same_as<decltype(^^Sample), std::meta::info>);

    static_assert(std::same_as<decltype(^^int), std::meta::info>);

    static_assert(std::meta::nonstatic_data_members_of(
        ^^Sample, std::meta::access_context::current()).size() == 1);

    using ReflectedType = [: ^^Sample :];
    static_assert(std::same_as<ReflectedType, Sample>);

    int Function(int value);
    enum class Color
    {
        Red, Blue
    };
} // namespace

TEST(Cpp26ReflectionSpec, ReportsCompilerReflectionAndReflectsMembers)
{
    EXPECT_GE(__cpp_impl_reflection, 202506L);
    EXPECT_EQ(std::meta::identifier_of(^^Sample::value), "value");
    EXPECT_EQ(std::meta::size_of(^^Sample), sizeof(Sample));
}

TEST(Cpp26ReflectionSpec, ReflectsFunctionParametersAndEnumerators)
{
    constexpr bool matches =[]() consteval
    {
        const auto parameters = std::meta::parameters_of(^^Function);
        const auto enumerators = std::meta::enumerators_of(^^Color);
        return parameters.size() == 1 &&
            std::meta::identifier_of(parameters.front()) == "value" &&
            enumerators.size() == 2 &&
            std::meta::identifier_of(enumerators.front()) == "Red";
    }
    ();

    EXPECT_TRUE(matches);
}
