struct Node
{
    Node* next;
    int value;
};

int main()
{
    Node novoNo = {
        .next = 20,
        .value = 20
    };
    novoNo.value = 20;
    auto root = new Node{
        .next = 0,
        .value = 20
    };

    auto next = new Node{
        .next = 0,
        .value = 20
    };

    root->next = next;

    return 0;
}
