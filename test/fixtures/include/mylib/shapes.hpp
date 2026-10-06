#ifndef MYLIB_SHAPES_HPP
#define MYLIB_SHAPES_HPP

// Guarded body, a decoration macro on every shape of declaration, inheritance
// and an alias: what member access completion needs from a real library header.
#if __cplusplus >= 201103L
namespace mylib
{
    struct Base
    {
        int base_value;
        void base_run();
    };

    class MYLIB_API Shape : public Base
    {
    public:
        MYLIB_NODISCARD double area() const MYLIB_NOEXCEPT;

    private:
        double cached;
    };

    using ShapeAlias = Shape;
}
#endif

#endif
