// SculptCore — IEEE 754 half-precision conversion for compact storage.
#pragma once

#include <cstdint>
#include <cstring>

namespace sculpt {

// Round-to-nearest-even; overflow saturates to the largest finite half,
// NaN becomes 0 (stored data must stay finite).
inline std::uint16_t floatToHalf(float value) {
    std::uint32_t bits;
    std::memcpy(&bits, &value, 4);
    const std::uint32_t sign = (bits >> 16) & 0x8000u;
    const std::uint32_t exponent = (bits >> 23) & 0xffu;
    std::uint32_t mantissa = bits & 0x7fffffu;
    if (exponent == 0xffu) return mantissa ? 0u : static_cast<std::uint16_t>(sign | 0x7bffu);  // NaN -> 0, inf -> max
    const int e = static_cast<int>(exponent) - 127 + 15;
    if (e >= 31) return static_cast<std::uint16_t>(sign | 0x7bffu);
    if (e <= 0) {
        if (e < -10) return static_cast<std::uint16_t>(sign);  // Underflow to zero.
        mantissa |= 0x800000u;
        const int shift = 14 - e;
        std::uint32_t half = mantissa >> shift;
        const std::uint32_t rest = mantissa & ((1u << shift) - 1u);
        const std::uint32_t halfway = 1u << (shift - 1);
        if (rest > halfway || (rest == halfway && (half & 1u))) ++half;
        return static_cast<std::uint16_t>(sign | half);
    }
    std::uint32_t half = (static_cast<std::uint32_t>(e) << 10) | (mantissa >> 13);
    const std::uint32_t rest = mantissa & 0x1fffu;
    if (rest > 0x1000u || (rest == 0x1000u && (half & 1u))) ++half;  // May carry into the exponent: still correct.
    if (half >= 0x7c00u) half = 0x7bffu;
    return static_cast<std::uint16_t>(sign | half);
}

inline float halfToFloat(std::uint16_t half) {
    const std::uint32_t sign = (static_cast<std::uint32_t>(half) & 0x8000u) << 16;
    const std::uint32_t exponent = (half >> 10) & 0x1fu;
    std::uint32_t mantissa = half & 0x3ffu;
    std::uint32_t bits;
    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else {  // Subnormal: normalise.
            int e = -1;
            do {
                ++e;
                mantissa <<= 1;
            } while ((mantissa & 0x400u) == 0);
            bits = sign | (static_cast<std::uint32_t>(127 - 15 - e) << 23) | ((mantissa & 0x3ffu) << 13);
        }
    } else if (exponent == 31) {
        bits = sign | 0x7f800000u | (mantissa << 13);
    } else {
        bits = sign | ((exponent - 15 + 127) << 23) | (mantissa << 13);
    }
    float out;
    std::memcpy(&out, &bits, 4);
    return out;
}

}  // namespace sculpt
