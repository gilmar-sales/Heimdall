#include <cstdint>
#include <string>
#include <string_view>

/**
 * @brief An linked list node.
 *
 */
struct Node
{
	Node* next;
	int value;
};

auto sizeOfNode  = sizeof(Node);
auto alignOfNode = alignof(Node);

/**
 * @brief Calculates the sum of a and b, returning 100 if the sum exceeds 10, and 1 otherwise.
 *
 * @param first Describe this parameter.
 * @param b Describe this parameter.
 * @param desc Describe this parameter.
 *
 * @return void
 */
int test_method(std::uint32_t first, int b, const std::string& desc)
{
	auto value = first + b;

	if (value)
	{
		return 100;
	}

	return 1;
}

/**
 * @brief Describe unreacheable_code.
 *
 * @return Describe the returned value.
 */
[[nodiscard]] std::string_view unreacheable_code()
{
	return "";

	constexpr auto test = 0;
}

int main()
{
	auto next = new Node
	{
		.value = 20
	};
	auto root = new Node
	{
		.next = next,.value = 20
	};

	return 0;
}
