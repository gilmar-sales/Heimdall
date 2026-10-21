#pragma once

namespace first
{
    enum class Kind
    {
        Count
    };

    struct Item
    {
        int value;
    };

    using Alias = Kind;
}
