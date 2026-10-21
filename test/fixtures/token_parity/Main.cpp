#include "HeaderA.hpp"
#include "HeaderB.hpp"

int Compute(first::Item object, int value)
{
    int sum = value;
    for (int index = 0; index < 3; ++index)
    {
        sum += index;
    }

    auto kind = first::Kind::Count;
    auto alias = first::Alias::Count;
    second::Kind other;
    const int* pointee = &value;
    int * const pointer = &value;
    return object.value + sum + *pointee + *pointer;
}
