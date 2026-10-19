namespace demo {
template<class T>
class Box {
public:
    T value;
};
enum class Color : int { Red = 1, Blue = 2 };
using Count = unsigned;
constexpr int twice(int x) noexcept { return x * 2; }
int run() {
    Box<int> box;
    int values[3] {1, 2, 3};
    auto transform = [](int x) -> int { return x + 1; };
    for (auto x : values) { transform(x); }
    for (int i = 0; i < 3; ++i) { box.value = twice(i); }
    return nullptr == nullptr ? 0 : 1;
}
}
