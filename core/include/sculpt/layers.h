// SculptCore — sculpt layers and image displacement.
//
// The displayed position of a vertex is base + sum(enabled strength * delta)
// over the layers, so layers can be turned off, weakened (down to 0) or
// boosted (up to 5x) without touching the base sculpt.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "sculpt/alpha.h"
#include "sculpt/math.h"

namespace sculpt {

struct SculptLayer {
    std::string name;
    bool enabled = true;
    float strength = 1.0f;  // 0..kMaxLayerStrength
    bool displace = false;  // Owned by the Displace rollout.
    std::vector<Vec3> delta;  // Per vertex, object space.
};

constexpr float kMaxLayerStrength = 5.0f;

struct LayerStack {
    std::vector<SculptLayer> layers;
    int active = -1;  // Layer that records strokes (-1: the base).

    bool empty() const { return layers.empty(); }
    float factor(std::size_t i) const { return layers[i].enabled ? layers[i].strength : 0.0f; }
    // Sum of every layer's contribution at vertex v.
    Vec3 offset(std::size_t v) const;
    int displaceLayer() const;  // Index of the Displace layer or -1.
    bool sizesMatch(std::size_t vertexCount) const;
    std::size_t memoryBytes() const;
};

// Compact binary form (deltas as half floats).
std::vector<std::uint8_t> serializeLayers(const LayerStack& stack);
bool deserializeLayers(const std::uint8_t* data, std::size_t size, LayerStack& out);

// Slider position (0..1) <-> layer strength: the middle is 1x, the left half
// 0..1x, the right half boosts progressively up to 5x.
float layerStrengthFromSlider(float t);
float layerSliderFromStrength(float strength);

// --- Displacement from an image ----------------------------------------------------

struct DisplaceSettings {
    bool triplanar = false;  // false: per-vertex UVs.
    float strength = 1.0f;   // Object units; negative pushes inward.
    float waterLevel = 0.0f; // Neutral map value = 0.5 + waterLevel.
    float contrast = 1.0f;   // Around mid gray.
    float tileU = 1.0f, tileV = 1.0f;
    float offsetU = 0.0f, offsetV = 0.0f;
};

// Bilinear sample with wrap-around (u right, v down, repeating).
float sampleWrapped(const Alpha& image, float u, float v);
// Approximate Gaussian blur (three box passes) with the given radius in pixels.
Alpha blurImage(const Alpha& image, float radiusPx);

// Offsets along `normals`. `uvs` (per vertex, map v pointing up) is used
// unless triplanar; `bounds` sizes the triplanar projection.
void computeDisplacement(const std::vector<Vec3>& positions, const std::vector<Vec3>& normals,
                         const std::vector<Vec3>* uvs, const Aabb& bounds, const Alpha& image,
                         const DisplaceSettings& settings, std::vector<Vec3>& offsets);

}  // namespace sculpt
