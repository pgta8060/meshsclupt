// SculptCore — turns raw pointer samples into evenly spaced dab positions.
#pragma once

#include <cstdint>
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

// Lazy Mouse: the brush trails the pointer on a "string" of the given length
// (pixels), which steadies hand-drawn strokes.
class LazyMouse {
public:
    void reset(const StrokeSample& start) {
        brush_ = start;
        active_ = true;
    }
    // Feeds the pointer position; returns true (and the new brush position in
    // `out`) when the brush moved.
    bool update(const StrokeSample& pointer, float distance, StrokeSample& out);
    const StrokeSample& brush() const { return brush_; }

private:
    StrokeSample brush_;
    bool active_ = false;
};

struct ScatterSettings {
    int density = 3;           // Dabs per stroke sample (1..32).
    float radius = 1.0f;       // Spread around the stroke, in brush radii.
    float sizeJitter = 0.3f;   // 0..1 random size reduction.
    float amountJitter = 0.3f; // 0..1 random strength reduction.
};

struct ScatterDab {
    StrokeSample sample;
    float sizeScale = 1.0f;
    float amount = 1.0f;
};

// Deterministic scattered dab placement (same seed, same stroke, same result).
class Scatter {
public:
    explicit Scatter(std::uint64_t seed = 0x5C0FFEEull) : state_(seed ? seed : 1ull) {}
    void generate(const StrokeSample& center, float brushRadiusPx, const ScatterSettings& settings,
                  std::vector<ScatterDab>& out);

private:
    float next01();
    std::uint64_t state_;
};

}  // namespace sculpt
