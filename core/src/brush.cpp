#include "sculpt/brush.h"

#include "sculpt/parallel.h"

namespace sculpt {
namespace {

// Displacement per dab, as a fraction of the brush radius at full strength
// and full falloff weight. Tuned for the default spacing (10% of radius).
constexpr float kSculptRate = 0.1f;
constexpr float kInflateRate = 0.1f;
// Fraction of the distance to the brush centre covered per dab.
constexpr float kPinchRate = 0.25f;
// Fraction of the distance to the neighbour average covered per dab.
constexpr float kSmoothRate = 1.0f;
// Dabs below this vertex count run on one thread (threading costs more).
constexpr std::size_t kParallelGrain = 1024;

template <class Fn>
void forEachKept(std::size_t kept, Fn&& fn) {
    parallelFor(kept, kParallelGrain, [&](std::size_t b, std::size_t e) {
        for (std::size_t i = b; i < e; ++i) fn(i);
    });
}

// Falloff-weighted average of the gathered vertex normals. Falls back to
// facing the viewer when the region has no usable normal (e.g. a flat sliver
// seen edge-on where everything cancels out).
Vec3 areaNormal(const Mesh& mesh, const DabScratch& s, const Dab& dab) {
    Vec3 sum;
    for (std::size_t i = 0; i < s.verts.size(); ++i) sum += mesh.normal(s.verts[i]) * s.weights[i];
    Vec3 n = normalizedOrZero(sum);
    if (lengthSq(n) == 0.0f) n = normalizedOrZero(-dab.viewDir);
    return n;
}

Vec3 smoothTarget(const Mesh& mesh, std::uint32_t v) {
    // Border vertices only average along the border so open edges keep their
    // outline instead of shrinking inwards.
    const bool border = mesh.isBorder(v);
    const Span<std::uint32_t> ring = border ? mesh.borderNeighbors(v) : mesh.neighbors(v);
    if (ring.empty() || (border && ring.size() < 2)) return mesh.position(v);
    Vec3 sum;
    for (std::uint32_t n : ring) sum += mesh.position(n);
    return sum / static_cast<float>(ring.size());
}

}  // namespace

const char* brushName(BrushType type) {
    switch (type) {
        case BrushType::Sculpt: return "Sculpt";
        case BrushType::Smooth: return "Smooth";
        case BrushType::Inflate: return "Inflate";
        case BrushType::Pinch: return "Pinch";
        case BrushType::Count: break;
    }
    return "Unknown";
}

bool isSignedBrush(BrushType type) {
    return type == BrushType::Sculpt || type == BrushType::Inflate || type == BrushType::Pinch;
}

float brushFalloff(float t, bool useFalloff) {
    if (!(t >= 0.0f) || t > 1.0f) return 0.0f;  // Also rejects NaN.
    if (!useFalloff) return 1.0f;
    return 1.0f - t * t * (3.0f - 2.0f * t);
}

void StrokeRecorder::begin(std::uint32_t vertexCount) {
    touched_.begin(vertexCount);
    vertices_.clear();
    originals_.clear();
    active_ = true;
}

void StrokeRecorder::touch(std::uint32_t v, const Vec3& originalPosition) {
    if (!active_ || !touched_.visit(v)) return;
    vertices_.push_back(v);
    originals_.push_back(originalPosition);
}

void StrokeRecorder::take(std::vector<std::uint32_t>& vertices, std::vector<Vec3>& originals) {
    vertices.swap(vertices_);
    originals.swap(originals_);
    vertices_.clear();
    originals_.clear();
}

std::size_t applyDab(Mesh& mesh, const Bvh& bvh, const BrushSettings& settings, const Dab& dab,
                     StrokeRecorder& recorder, DabScratch& s, std::vector<std::uint32_t>& moved) {
    if (!(dab.radius > 0.0f) || !isFinite(dab.radius) || !isFinite(dab.center)) return 0;
    const float strength = clamp01(settings.strength) * clamp01(dab.pressure);
    if (!(strength > 0.0f)) return 0;

    s.verts.clear();
    bvh.gatherVertices(mesh, dab.center, dab.radius, s.verts);

    // Weights (in parallel), then drop vertices with no influence.
    const float invRadius = 1.0f / dab.radius;
    s.weights.resize(s.verts.size());
    parallelFor(s.verts.size(), kParallelGrain, [&](std::size_t b, std::size_t e) {
        for (std::size_t i = b; i < e; ++i) {
            const std::uint32_t v = s.verts[i];
            float w = 0.0f;
            if (!(settings.backfaceCull && dot(mesh.normal(v), dab.viewDir) > 0.0f)) {
                const float t = length(mesh.position(v) - dab.center) * invRadius;
                w = brushFalloff(t, settings.useFalloff) * strength;
            }
            s.weights[i] = w;
        }
    });
    std::size_t kept = 0;
    for (std::size_t i = 0; i < s.verts.size(); ++i) {
        if (!(s.weights[i] > 0.0f)) continue;
        s.verts[kept] = s.verts[i];
        s.weights[kept] = s.weights[i];
        ++kept;
    }
    s.verts.resize(kept);
    s.weights.resize(kept);
    if (kept == 0) return 0;

    const float sign = (isSignedBrush(settings.type) && (settings.subtract != dab.invert)) ? -1.0f : 1.0f;

    // Compute every target from the unmodified mesh first (Jacobi style), so
    // the result does not depend on vertex order.
    s.targets.resize(kept);
    switch (settings.type) {
        case BrushType::Sculpt: {
            const Vec3 offset = areaNormal(mesh, s, dab) * (sign * dab.radius * kSculptRate);
            forEachKept(kept, [&](std::size_t i) { s.targets[i] = mesh.position(s.verts[i]) + offset * s.weights[i]; });
            break;
        }
        case BrushType::Inflate: {
            const float amount = sign * dab.radius * kInflateRate;
            forEachKept(kept, [&](std::size_t i) {
                const std::uint32_t v = s.verts[i];
                s.targets[i] = mesh.position(v) + mesh.normal(v) * (amount * s.weights[i]);
            });
            break;
        }
        case BrushType::Pinch: {
            const Vec3 n = areaNormal(mesh, s, dab);
            forEachKept(kept, [&](std::size_t i) {
                const Vec3& p = mesh.position(s.verts[i]);
                Vec3 toCenter = dab.center - p;
                toCenter -= n * dot(toCenter, n);  // Stay in the tangent plane.
                s.targets[i] = p + toCenter * (sign * kPinchRate * s.weights[i]);
            });
            break;
        }
        case BrushType::Smooth: {
            forEachKept(kept, [&](std::size_t i) {
                const std::uint32_t v = s.verts[i];
                const Vec3& p = mesh.position(v);
                s.targets[i] = p + (smoothTarget(mesh, v) - p) * clamp01(s.weights[i] * kSmoothRate);
            });
            break;
        }
        case BrushType::Count:
            return 0;
    }

    std::size_t count = 0;
    for (std::size_t i = 0; i < kept; ++i) {
        const std::uint32_t v = s.verts[i];
        const Vec3& target = s.targets[i];
        const Vec3& current = mesh.position(v);
        if (!isFinite(target) || target == current) continue;
        recorder.touch(v, current);
        mesh.setPosition(v, target);
        moved.push_back(v);
        ++count;
    }
    return count;
}

}  // namespace sculpt
