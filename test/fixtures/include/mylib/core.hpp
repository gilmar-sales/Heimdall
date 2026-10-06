#pragma once

#define _MYLIB_CORE_GUARD 1

namespace mylib
{

    struct Widget
    {
        int value;
    };

    void run();

    int helper_value;

    namespace _impl
    {
        int hidden_helper;
    }

} // namespace mylib
