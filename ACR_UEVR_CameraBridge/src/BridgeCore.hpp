#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

namespace bridge {
constexpr double pi = 3.14159265358979323846;
struct Rotation {
    double pitch{}, yaw{}, roll{};
};
struct Quaternion {
    double x{}, y{}, z{}, w{1};
};
inline bool finite(Rotation r) { return std::isfinite(r.pitch) && std::isfinite(r.yaw) && std::isfinite(r.roll); }
inline double wrap(double d) { return std::remainder(d, 360.0); }
inline bool same(Rotation a, Rotation b, double tolerance = 0.001) {
    return finite(a) && finite(b) && std::abs(wrap(a.pitch - b.pitch)) < tolerance &&
           std::abs(wrap(a.yaw - b.yaw)) < tolerance && std::abs(wrap(a.roll - b.roll)) < tolerance;
}
inline Quaternion mul(Quaternion a, Quaternion b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
inline std::optional<Quaternion> normalized(Quaternion q) {
    const double n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (!std::isfinite(n) || n < 0.1 || n > 10)
        return {};
    return Quaternion{q.x / n, q.y / n, q.z / n, q.w / n};
}
inline Quaternion axis(int i, double degrees) {
    double a = degrees * pi / 360, s = std::sin(a);
    return {i == 0 ? s : 0, i == 1 ? s : 0, i == 2 ? s : 0, std::cos(a)};
}
// UEVR's game-base convention: Y(-yaw) * X(pitch) * Z(-roll).
inline Quaternion base_rotation(Rotation r) { return mul(mul(axis(1, -r.yaw), axis(0, r.pitch)), axis(2, -r.roll)); }
inline Rotation to_rotation(Quaternion q) {
    const double r02 = 2 * (q.x * q.z + q.w * q.y), r22 = 1 - 2 * (q.x * q.x + q.y * q.y);
    const double r12 = 2 * (q.y * q.z - q.w * q.x), r10 = 2 * (q.x * q.y + q.w * q.z),
                 r11 = 1 - 2 * (q.x * q.x + q.z * q.z);
    return {std::asin(std::clamp(-r12, -1.0, 1.0)) * 180 / pi, -std::atan2(r02, r22) * 180 / pi,
            -std::atan2(r10, r11) * 180 / pi};
}
inline std::optional<Quaternion> relative_head(Quaternion recenter, Quaternion hmd) {
    auto a = normalized(recenter), b = normalized(hmd);
    if (!a || !b)
        return {};
    return normalized(mul(*a, *b));
}
inline std::optional<Rotation> head_center(Rotation clean, Quaternion recenter, Quaternion hmd, bool decoupled) {
    if (!finite(clean))
        return {};
    auto a = normalized(recenter), b = normalized(hmd);
    if (!a || !b)
        return {};
    auto base = base_rotation(clean);
    if (decoupled) {
        // Project the camera forward vector to the horizontal plane.
        const double x = 2 * (base.x * base.z + base.w * base.y), z = 1 - 2 * (base.x * base.x + base.y * base.y);
        if (x * x + z * z < 1e-10)
            return {};
        base = axis(1, std::atan2(x, z) * 180 / pi);
    }
    auto out = normalized(mul(mul(base, *a), *b));
    if (!out)
        return {};
    return to_rotation(*out);
}

struct Candidate {
    std::size_t offset;
    int slot;
};
// Forwarder scan adapted from itsloopyo's MIT camera resolver; license bundled.
inline std::vector<Candidate> forwarders(std::span<const std::uint8_t> code, std::uint32_t field) {
    std::vector<Candidate> out;
    for (std::size_t i = 0; i + 22 <= code.size(); ++i) {
        const auto *p = code.data() + i;
        if (p[0] != 0x48 || p[1] != 0x8b || p[2] != 0x89)
            continue;
        std::uint32_t f{}, disp{};
        std::memcpy(&f, p + 3, 4);
        std::memcpy(&disp, p + 18, 4);
        if (f != field || p[7] != 0x48 || p[8] != 0x85 || p[9] != 0xc9 || p[10] != 0x74 || p[11] != 0x0a ||
            p[12] != 0x48 || p[13] != 0x8b || p[14] != 0x01 || p[15] != 0x48 || p[16] != 0xff || p[17] != 0xa0 ||
            disp % 8 || disp >= 0x4000)
            continue;
        out.push_back({i, static_cast<int>(disp / 8)});
    }
    return out;
}
inline bool update_markers(std::span<const std::uint8_t> code) {
    bool saves = false, calls = false;
    for (std::size_t i = 0; i + 6 < code.size(); ++i) {
        saves |= code[i] == 0x0f && code[i + 1] == 0x28 && code[i + 2] == 0xf1;
        calls |= code[i] == 0xff && code[i + 1] == 0x90;
    }
    return saves && calls;
}
inline std::optional<int> unique_slot(const std::vector<int> &slots) {
    if (slots.empty())
        return {};
    for (int s : slots)
        if (s != slots.front())
            return {};
    return slots.front();
}
} // namespace bridge
