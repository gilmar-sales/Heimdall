// Corpus sample: declarations, namespaces, classes and free functions.
#pragma once

#include <memory>
#include <string>
#include <vector>

namespace heimdall::samples
{

class Widget;
using WidgetPtr = std::shared_ptr<Widget>;

enum class Color : unsigned char
{
    Red,
    Green,
    Blue
};

struct Point
{
    double x = 0.0;
    double y = 0.0;

    Point operator+(const Point& other) const { return { x + other.x, y + other.y }; }
};

class Widget
{
  public:
    explicit Widget(std::string name);
    virtual ~Widget() = default;

    Widget(const Widget&) = delete;
    Widget& operator=(const Widget&) = delete;
    Widget(Widget&&) noexcept = default;
    Widget& operator=(Widget&&) noexcept = default;

    virtual void Draw(const Point& origin) const;
    const std::string& Name() const noexcept { return m_name; }

  protected:
    std::string m_name;
    std::vector<Point> m_points;
};

class Button final : public Widget
{
  public:
    using Widget::Widget;

    void Draw(const Point& origin) const override;
    void Click();

  private:
    bool m_pressed = false;
};

template <typename T, typename Alloc = std::allocator<T>> class Ring
{
  public:
    void Push(T value);
    T Pop();

  private:
    std::vector<T, Alloc> m_items;
    std::size_t m_head = 0;
};

} // namespace heimdall::samples
