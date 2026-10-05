// SculptCore — brush definitions and the per-dab deformation kernels.
#pragma once

#include <cstdint>
#include <vector>

#include "sculpt/bvh.h"
#include "sculpt/math.h"
#include "sculpt/mesh.h"

namespace sculpt {

// Order is part of the public MAXScript/UI contract: append only.
enum class BrushType : int {
    Sculpt = 0,   // Displace along the area normal (Alt/Sub: dig in).
    Smooth = 1,   // Laplacian relax; open borders slide along the border.
    Inflate = 2,  // Push each vertex along its own normal (Alt/Sub: deflate).
    Pinch = 3,    // Pull toward the brush centre in the tangent plane (Alt/Sub: spread).
    Count
};

const char* brushName(BrushType type);
bool isSignedBrush(BrushType type);  // Whether Add/Sub and Alt change the effect.

// Falloff from the brush centre (t = 0) to its rim (t = 1).
// Smooth: 1 - 3t^2 + 2t^3 (zero slope at both ends). Hard: 1 everywhere inside.
float brushFalloff(float t, bool useFalloff);

struct BrushSettings {
    BrushType type = BrushType::Sculpt;
    float strength = 0.5f;     // 0..1
    bool subtract = false;     // Sub mode for signed brushes.
    bool useFalloff = true;
    bool backfaceCull = true;  // Skip vertices facing away from the viewer.
};

// One brush application at a single point.
struct Dab {
    Vec3 center;               // Object space.
    float radius = 1.0f;       // Object space.
    float pressure = 1.0f;     // 0..1 (pen pressure; 1 for a mouse).
    Vec3 viewDir{0, 0, -1};    // Object-space direction from the eye into the scene.
    bool invert = false;       // Temporary flip (Alt) for signed brushes.
};

// Remembers the original position of every vertex the first time a stroke
// touches it, so a stroke can be undone/cancelled exactly.
class StrokeRecorder {
public:
    void begin(std::uint32_t vertexCount);
    void touch(std::uint32_t v, const Vec3& originalPosition);
    bool active() const { return active_; }
    void end() { active_ = false; }

    const std::vector<std::uint32_t>& vertices() const { return vertices_; }
    const std::vector<Vec3>& originals() const { return originals_; }

    // Moves the recorded data out (the recorder becomes empty).
    void take(std::vector<std::uint32_t>& vertices, std::vector<Vec3>& originals);

private:
    VisitSet touched_;
    std::vector<std::uint32_t> vertices_;
    std::vector<Vec3> originals_;
    bool active_ = false;
};

// Reusable buffers so dabs do not allocate in steady state.
struct DabScratch {
    std::vector<std::uint32_t> verts;
    std::vector<float> weights;
    std::vector<Vec3> targets;
};

// Applies one dab. Moved vertices are appended to `moved` (no duplicates
// within this dab). Normals and the BVH are NOT updated: the caller batches that.
// Returns the number of vertices moved.
std::size_t applyDab(Mesh& mesh, const Bvh& bvh, const BrushSettings& settings, const Dab& dab,
                     StrokeRecorder& recorder, DabScratch& scratch, std::vector<std::uint32_t>& moved);

}  // namespace sculpt
