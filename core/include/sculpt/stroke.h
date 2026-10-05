// SculptCore — turns raw pointer samples into evenly spaced dab positions.
#pragma once

#include <vector>

namespace sculpt {

// A pointer sample in screen space (pixels).
struct StrokeSample {
    float x = 0.0f;
    float y = 0.0f;
    float pressure = 1.0f;
};

// Emits dabs every `spacing` pixels along the polyline of incoming samples.
// Working in screen space keeps spacing tied to the on-screen brush size and
// lets the host ray-cast each dab so every dab lands exactly on the surface.
class StrokeSpacer {
public:
    // Safety cap so a huge jump with a tiny spacing cannot stall the UI.
    static constexpr int kMaxDabsPerSegment = 4096;
    static constexpr float kMinSpacing = 0.5f;

    // Starts a stroke; the first sample always produces a dab.
    void begin(const StrokeSample& first, std::vector<StrokeSample>& out);

    // Adds a sample and appends the dabs crossed on the way to it.
    void moveTo(const StrokeSample& sample, float spacing, std::vector<StrokeSample>& out);

    bool active() const { return active_; }
    void end() { active_ = false; }

private:
    StrokeSample last_;
    float travelled_ = 0.0f;  // Distance since the last emitted dab.
    bool active_ = false;
};

}  // namespace sculpt
