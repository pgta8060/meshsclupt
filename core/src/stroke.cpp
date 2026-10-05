#include "sculpt/stroke.h"

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

}  // namespace sculpt
