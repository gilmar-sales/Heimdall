#include <cstddef>
#include <string>

struct Node
{
	Node* next;
	int value;
};

auto sizeOfNode  = sizeof(Node);
auto alignOfNode = alignof(Node);

/**
 * @brief Describe test_method.
 *
 * @param a Describe this parameter.
 * @param b Describe this parameter.
 * @param desc Describe this parameter.
 */
void test_method(int a, int b, std::string desc)
{
	auto value = a + b;
}

void unreacheable_code() {
	return;

	auto unreacheable_variable = 10;
	unreacheable_variable++;
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
