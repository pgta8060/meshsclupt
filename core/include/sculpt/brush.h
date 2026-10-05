// SculptCore — brush definitions and the per-dab kernels.
#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "sculpt/alpha.h"
#include "sculpt/bvh.h"
#include "sculpt/math.h"
#include "sculpt/mesh.h"

namespace sculpt {

// Order is part of the public MAXScript/UI/file contract: append only.
enum class BrushType : int {
    Sculpt = 0,             // Displace along the area normal.
    Smooth = 1,             // Laplacian relax; open borders slide along the border.
    Inflate = 2,            // Push each vertex along its own normal.
    Pinch = 3,              // Pull toward the brush centre in the tangent plane.
    Clay = 4,               // Build toward a raised working plane.
    ClayBuildup = 5,        // Stronger, square-footprint clay.
    Carve = 6,              // Groove (pinch + push in).
    Knife = 7,              // Sharp cut.
    Contrast = 8,           // Exaggerate height differences around the area plane.
    Scrape = 9,             // Shave points above the area plane (Alt: fill below).
    Polish = 10,            // Flatten toward a plane while relaxing.
    Move = 11,              // Grab the region under the brush at stroke start.
    SnakeHook = 12,         // Pull geometry along the stroke.
    FaceGroups = 13,        // Paint SculptGroup ids.
    SmoothGroupBorder = 14, // Smooth only SculptGroup borders.
    Density = 15,           // Paints where to remesh; the host remeshes on release (Alt: reduce).
    Revert = 16,            // Toward the Surface Snapshot.
    Clip = 17,              // Screen-shape flatten (host gesture).
    Cutter = 18,            // Screen-shape cut with caps (host gesture).
    Slice = 19,             // Screen-line split into two parts (host gesture).
    Cloth = 20,             // Cloth simulation under the brush (host gesture).
    Pose = 21,              // Rotate/twist/scale around a guide (host gesture).
    CurveTube = 22,         // Live tube along a curve (host gesture).
    Displace = 23,          // Stencil as height (host supplies the stencil).
    MaskPaint = 24,         // Mask tool (not shown in the brush palette).
    Count
};

struct BrushInfo {
    const char* name;           // UI name.
    const char* scriptName;     // MAXScript enum name.
    bool available;             // Implemented in this build.
    bool isSigned;              // Add/Sub and Alt flip the effect.
    bool supportsLayerMode;
    float defaultStrength;
    bool defaultBackfaceCull;
    bool defaultUseFalloff;
};
const BrushInfo& brushInfo(BrushType type);
const char* brushName(BrushType type);
bool isSignedBrush(BrushType type);
bool isGeometryBrush(BrushType type);  // Moves vertices (vs. mask/group painting).

// Falloff from the brush centre (t = 0) to its rim (t = 1).
// Smooth: 1 - 3t^2 + 2t^3 (zero slope at both ends). Hard: 1 everywhere inside.
float brushFalloff(float t, bool useFalloff);

struct BrushSettings {
    BrushType type = BrushType::Sculpt;
    float strength = 0.5f;     // 0..1
    bool subtract = false;     // Sub mode for signed brushes.
    bool useFalloff = true;
    bool backfaceCull = true;  // Skip vertices facing away from the viewer.
    bool layerMode = false;    // Sculpt: settle at one height per stroke.

    float clayBorder = 0.5f;     // Clay: 0 soft round .. 1 flat with a hard border.
    float polishHardness = 0.5f; // Polish: 0 mostly relax .. 1 mostly flatten.
    float accuCurve = 0.0f;      // Move: 0 smooth .. 1 tighter falloff.
    bool scrapeOriginalPlane = false;
    bool scrapeOriginalNormal = false;

    const Alpha* alpha = nullptr;  // Optional height stamp.
    float alphaMid = 0.0f;         // Alpha value that maps to "no effect".
    float alphaFade = 0.0f;        // 0..1: fades the alpha toward the rim.

    int faceGroupId = 0;  // FaceGroups: id to paint.
};

// One brush application at a single point.
struct Dab {
    Vec3 center;               // Object space.
    float radius = 1.0f;       // Object space.
    float pressure = 1.0f;     // 0..1
    float amount = 1.0f;       // Extra strength multiplier (scatter jitter).
    Vec3 viewDir{0, 0, -1};    // Object-space direction from the eye into the scene.
    Vec3 up{0, 1, 0};          // Alpha "up" (screen up, or the stroke direction).
    Vec3 grabDelta;            // Move / Snake Hook: displacement since the previous dab.
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
    bool touched(std::uint32_t v) const { return active_ && touched_.visited(v); }

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

// Per-stroke state of one symmetry instance.
struct StrokeState {
    bool havePlane = false;  // Scrape "original plane/normal".
    Vec3 planePoint;
    Vec3 planeNormal;

    bool grabbed = false;  // Move: region captured at the first dab.
    std::vector<std::uint32_t> grabVerts;
    std::vector<float> grabWeights;

    struct LayerEntry {
        Vec3 origin;
        Vec3 normal;
        float height = 0.0f;
    };
    std::unordered_map<std::uint32_t, LayerEntry> layer;  // Layer Mode heights.

    void reset() { *this = StrokeState(); }
};

// Reusable buffers so dabs do not allocate in steady state.
struct DabScratch {
    std::vector<std::uint32_t> verts;
    std::vector<float> falloff;  // Radial (and alpha) weight without strength/mask.
    std::vector<float> weights;  // Final signed weight.
    std::vector<Vec3> targets;
};

// Everything a geometry dab reads or writes.
struct DabTarget {
    Mesh& mesh;
    const Bvh& bvh;
    StrokeRecorder& recorder;
    StrokeState& state;
    DabScratch& scratch;
    std::vector<std::uint32_t>& moved;          // Moved vertices are appended (no duplicates per dab).
    const std::vector<float>* mask = nullptr;   // Per-vertex 0..1 protection (may be null).
    const std::uint8_t* hiddenTriangles = nullptr;
    const std::vector<std::uint8_t>* groupBorder = nullptr;  // SmoothGroupBorder: per-vertex flag.
    const std::vector<Vec3>* reference = nullptr;            // Revert: Surface Snapshot positions.
};

// Applies one dab of a geometry brush. Normals and the BVH are NOT updated:
// the caller batches that. Returns the number of vertices moved.
std::size_t applyDab(DabTarget& target, const BrushSettings& settings, const Dab& dab);

// Radial + alpha weight of a vertex position for a dab (0 outside).
// `frameNormal` orients the alpha plane. Exposed for mask/group painting.
float dabWeight(const BrushSettings& settings, const Dab& dab, const Vec3& frameNormal, const Vec3& position);

}  // namespace sculpt
