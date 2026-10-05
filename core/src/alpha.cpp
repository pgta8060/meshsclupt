#include "sculpt/alpha.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "sculpt/math.h"

namespace sculpt {
namespace {

float smoothstep(float e0, float e1, float x) {
    const float t = clamp01((x - e0) / (e1 - e0));
    return t * t * (3.0f - 2.0f * t);
}

}  // namespace

Alpha::Alpha(int width, int height, std::vector<float> values) {
    if (width <= 0 || height <= 0 || values.size() != static_cast<std::size_t>(width) * static_cast<std::size_t>(height))
        return;
    for (float& v : values) v = isFinite(v) ? clamp01(v) : 0.0f;
    width_ = width;
    height_ = height;
    values_ = std::move(values);
}

Alpha Alpha::builtin(int index, int size) {
    size = std::max(8, size);
    std::vector<float> values(static_cast<std::size_t>(size) * static_cast<std::size_t>(size));
    const float edge = std::exp(-4.0f);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const float fx = (static_cast<float>(x) + 0.5f) / static_cast<float>(size) * 2.0f - 1.0f;
            const float fy = (static_cast<float>(y) + 0.5f) / static_cast<float>(size) * 2.0f - 1.0f;
            const float r = std::sqrt(fx * fx + fy * fy);
            float v = 0.0f;
            switch (index) {
                case 0:  // Soft round (gaussian reaching 0 at the rim).
                    v = r >= 1.0f ? 0.0f : (std::exp(-4.0f * r * r) - edge) / (1.0f - edge);
                    break;
                case 1:  // Small sharp dot.
                    v = 1.0f - smoothstep(0.25f, 0.35f, r);
                    break;
                case 2:  // Square with a slightly soft edge.
                    v = (1.0f - smoothstep(0.75f, 0.85f, std::fabs(fx))) * (1.0f - smoothstep(0.75f, 0.85f, std::fabs(fy)));
                    break;
                default:  // Round disc with a soft rim.
                    v = 1.0f - smoothstep(0.55f, 0.75f, r);
                    break;
            }
            values[static_cast<std::size_t>(y) * static_cast<std::size_t>(size) + static_cast<std::size_t>(x)] = v;
        }
    }
    return Alpha(size, size, std::move(values));
}

float Alpha::sample(float u, float v) const {
    if (values_.empty() || !(u >= 0.0f) || !(v >= 0.0f) || u > 1.0f || v > 1.0f) return 0.0f;
    const float x = u * static_cast<float>(width_) - 0.5f;
    const float y = v * static_cast<float>(height_) - 0.5f;
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const float tx = x - static_cast<float>(x0);
    const float ty = y - static_cast<float>(y0);
    auto at = [this](int px, int py) {
        px = std::min(std::max(px, 0), width_ - 1);
        py = std::min(std::max(py, 0), height_ - 1);
        return values_[static_cast<std::size_t>(py) * static_cast<std::size_t>(width_) + static_cast<std::size_t>(px)];
    };
    const float top = at(x0, y0) + (at(x0 + 1, y0) - at(x0, y0)) * tx;
    const float bottom = at(x0, y0 + 1) + (at(x0 + 1, y0 + 1) - at(x0, y0 + 1)) * tx;
    return top + (bottom - top) * ty;
}

}  // namespace sculpt
