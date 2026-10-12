#include <cstdint>
#include <string>
#include <string_view>

/**
 * @brief Calculates the sum of a and b, returning 100 if the sum exceeds 10, and 1 otherwise.
 *
 * @param first Describe this parameter.
 * @param second Describe this parameter.
 * @param desc Describe this parameter.
 *
 * @return void
 */
int MetodoTeste(std::uint32_t first, int second, const std::string& desc);

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
	Node* next   = nullptr;
	TValue value = {};
};

template <typename TValue>
class List
{

public:
	List() = default;

	~List() = default;

	void Push(const TValue& value)
	{
		const auto node = new Node<TValue>
		{
			.value = value
		};

		if (mRoot == nullptr)
		{
			mRoot = node;
			return;
		}

		auto current = mRoot;

		while (current->next != nullptr)
		{
			current = current->next;
		}

		current->next = node;
	}

private:
	Node<TValue>* mRoot;
};

List<int> CreateList()
{
	return {};
}

auto list = CreateList();

int main()
{
	auto list = List<int>();

	list.Push(2);

	auto node  = new Node<float>();
	auto test  = node;
	auto test2 = test;

	test->value = 2.0f;
	auto test2v = test->value;

	MetodoTeste(1, 2, "");

	return 0;
}

int MetodoTeste(std::uint32_t first, int second, const std::string& desc)
{
	const auto value = first + second;

	if (value != 0)
	{
		return 100;
	}

	return 1;
}