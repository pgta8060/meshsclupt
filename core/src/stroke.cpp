#include "sculpt/stroke.h"

#include <algorithm>
#include <cmath>

namespace sculpt {

void StrokeSpacer::begin(const StrokeSample& first, std::vector<StrokeSample>& out) {
    last_ = first;
    travelled_ = 0.0f;
    active_ = true;
    out.push_back(first);
}

void StrokeSpacer::moveTo(const StrokeSample& sample, float spacing, std::vector<StrokeSample>& out) {
    if (!active_) {
        begin(sample, out);
        return;
    }
    if (!(spacing >= kMinSpacing)) spacing = kMinSpacing;  // Also catches NaN.

    const float dx = sample.x - last_.x;
    const float dy = sample.y - last_.y;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (!(len > 0.0f) || !std::isfinite(len)) {
        last_.pressure = sample.pressure;
        return;
    }

    float next = spacing - travelled_;  // Distance along this segment to the next dab.
    int emitted = 0;
    while (next <= len && emitted < kMaxDabsPerSegment) {
        const float t = next / len;
        out.push_back({last_.x + dx * t, last_.y + dy * t, last_.pressure + (sample.pressure - last_.pressure) * t});
        ++emitted;
        next += spacing;
    }
    travelled_ = (emitted == kMaxDabsPerSegment) ? 0.0f : len - (next - spacing);
    last_ = sample;
}

bool LazyMouse::update(const StrokeSample& pointer, float distance, StrokeSample& out) {
    if (!std::isfinite(pointer.x) || !std::isfinite(pointer.y)) return false;
    if (!active_) {
        reset(pointer);
        out = brush_;
        return true;
    }
    const float dx = pointer.x - brush_.x;
    const float dy = pointer.y - brush_.y;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (!(distance > 0.0f)) {
        brush_ = pointer;
        out = brush_;
        return len > 0.0f;
    }
    if (len <= distance) return false;  // Still inside the string's slack.
    const float t = (len - distance) / len;
    brush_.x += dx * t;
    brush_.y += dy * t;
    brush_.pressure = pointer.pressure;
    out = brush_;
    return true;
}

float Scatter::next01() {
    state_ ^= state_ >> 12;
    state_ ^= state_ << 25;
    state_ ^= state_ >> 27;
    return static_cast<float>((state_ * 0x2545F4914F6CDD1Dull) >> 40) / static_cast<float>(1ull << 24);
}

void Scatter::generate(const StrokeSample& center, float brushRadiusPx, const ScatterSettings& settings,
                       std::vector<ScatterDab>& out) {
    const int count = std::min(std::max(settings.density, 1), 32);
    const float spread = std::max(0.0f, settings.radius) * brushRadiusPx;
    for (int i = 0; i < count; ++i) {
        const float angle = 6.28318530718f * next01();
        const float dist = spread * std::sqrt(next01());
        ScatterDab d;
        d.sample = center;
        d.sample.x += std::cos(angle) * dist;
        d.sample.y += std::sin(angle) * dist;
        d.sizeScale = 1.0f - std::min(std::max(settings.sizeJitter, 0.0f), 1.0f) * next01();
        d.amount = 1.0f - std::min(std::max(settings.amountJitter, 0.0f), 1.0f) * next01();
        out.push_back(d);
    }
}

}  // namespace sculpt
