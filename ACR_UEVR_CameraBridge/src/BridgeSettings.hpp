#pragma once
#include <charconv>
#include <cmath>
#include <string>
#include <string_view>
namespace bridge {
struct Settings {
    bool enabled{true};
};
inline bool number(std::string_view s, double &v) {
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    return ec == std::errc{} && end == s.data() + s.size() && std::isfinite(v);
}
inline bool change(Settings &s, std::string_view key, std::string_view value) {
    double n{};
    if (!number(value, n))
        return false;
    if (key == "enabled" && (n == 0 || n == 1))
        s.enabled = n == 1;
    else
        return false;
    return true;
}
} // namespace bridge
