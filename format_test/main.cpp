struct Node
{
    Node* next;
    int value;
};

auto test = sizeof(Node);

int main()
{
    auto next = new Node
    {
        .value = 20
    };
    auto root = new Node
    {
        .next = next,
        .value = 20
    };

    return 0;
}
