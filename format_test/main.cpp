struct Node
{
    Node* next;
    int value;
};

int main()
{
    auto root = new Node{
        .next = 0,
        .value = 10
    };

    auto next = new Node{
        .next = 0,
        .value = 20
    };

    root->next = next;

    return 0;
}
