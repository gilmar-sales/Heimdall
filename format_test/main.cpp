#include <cstdint>
#include <string>
#include <string_view>

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

/**
 *
 * @brief An linked list node.
 *
 *
 * @tparam TValue value type
 */
template <typename TValue>
struct Node
{
	Node* next;
	TValue value;
};

template <typename TValue>
class LinkedList
{

public:
	void Push(const TValue& value);

private:
	Node<TValue>* mRoot;
};

LinkedList<int> CreateList()
{
	return {};
}

auto list = CreateList();

int main()
{
	auto list = LinkedList<int>();

	list.Push("");

	return 0;
}
