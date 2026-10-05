// Whole-mesh mask, SculptGroup and visibility operations of SculptSession.
#include <algorithm>
#include <cmath>
#include <map>

#include "sculpt/parallel.h"
#include "sculpt/session.h"

namespace sculpt {
namespace {

float smoothstep(float e0, float e1, float x) {
    if (!(e1 > e0)) return x >= e1 ? 1.0f : 0.0f;
    const float t = clamp01((x - e0) / (e1 - e0));
    return t * t * (3.0f - 2.0f * t);
}

// Signed mean-curvature-like value per vertex: > 0 in cavities, < 0 on ridges,
// normalised by the local edge length so it is scale independent.
std::vector<float> vertexCavity(const Mesh& mesh) {
    const std::uint32_t V = mesh.vertexCount();
    std::vector<float> cavity(V, 0.0f);
    parallelFor(V, 4096, [&](std::size_t b, std::size_t e) {
        for (std::size_t vi = b; vi < e; ++vi) {
            const auto v = static_cast<std::uint32_t>(vi);
            const Span<std::uint32_t> ring = mesh.neighbors(v);
            if (ring.empty()) continue;
            Vec3 avg;
            float edge = 0.0f;
            for (std::uint32_t n : ring) {
                avg += mesh.position(n);
                edge += length(mesh.position(n) - mesh.position(v));
            }
            avg = avg / static_cast<float>(ring.size());
            edge /= static_cast<float>(ring.size());
            if (edge > 0.0f) cavity[v] = dot(avg - mesh.position(v), mesh.normal(v)) / edge;
        }
    });
    return cavity;
}

// Groups polygons into connected components where `joins(a, b)` holds for
// neighbouring polygons. Hidden polygons keep their group. Ids start at 1.
template <class Joins>
std::vector<std::int32_t> floodGroups(const Mesh& mesh, const std::vector<std::uint8_t>& hidden,
                                      const std::vector<std::int32_t>& current, Joins&& joins) {
    const std::uint32_t F = mesh.faceCount();
    std::vector<std::int32_t> out = current;
    std::vector<std::uint8_t> done(F, 0u);
    std::vector<std::uint32_t> stack;
    std::int32_t next = 1;
    for (std::uint32_t seed = 0; seed < F; ++seed) {
        if (done[seed] || hidden[seed]) continue;
        const std::int32_t id = next++;
        stack.push_back(seed);
        done[seed] = 1u;
        while (!stack.empty()) {
            const std::uint32_t f = stack.back();
            stack.pop_back();
            out[f] = id;
            for (std::uint32_t n : mesh.faceNeighbors(f)) {
                if (done[n] || hidden[n] || !joins(f, n)) continue;
                done[n] = 1u;
                stack.push_back(n);
            }
        }
    }
    return out;
}

}  // namespace

StrokeDelta SculptSession::finishMaskOp(const std::vector<float>& newMask) {
    StrokeDelta delta;
    if (!valid() || recorder_.active() || newMask.size() != mask_.size()) return delta;
    for (std::uint32_t v = 0; v < mask_.size(); ++v) {
        const float value = isFinite(newMask[v]) ? clamp01(newMask[v]) : mask_[v];
        if (value == mask_[v]) continue;
        delta.maskVertices.push_back(v);
        delta.maskBefore.push_back(mask_[v]);
        delta.maskAfter.push_back(value);
        setMaskValue(v, value, false);
    }
    refreshHasMask();
    return delta;
}

StrokeDelta SculptSession::finishGroupOp(const std::vector<std::int32_t>& newGroups) {
    StrokeDelta delta;
    if (!valid() || recorder_.active() || newGroups.size() != faceGroups_.size()) return delta;
    for (std::uint32_t f = 0; f < faceGroups_.size(); ++f) {
        const std::int32_t g = std::max<std::int32_t>(0, newGroups[f]);
        if (g == faceGroups_[f]) continue;
        delta.groupFaces.push_back(f);
        delta.groupBefore.push_back(faceGroups_[f]);
        delta.groupAfter.push_back(g);
        setGroupValue(f, g, false);
    }
    return delta;
}

StrokeDelta SculptSession::finishHiddenOp(const std::vector<std::uint8_t>& newHidden) {
    StrokeDelta delta;
    if (!valid() || recorder_.active() || newHidden.size() != hiddenFaces_.size() || newHidden == hiddenFaces_)
        return delta;
    delta.hasHidden = true;
    delta.hiddenBefore = hiddenFaces_;
    delta.hiddenAfter = newHidden;
    hiddenFaces_ = newHidden;
    refreshHiddenTriangles();
    visibilityDirty_ = true;
    return delta;
}

// --- Mask -----------------------------------------------------------------------

StrokeDelta SculptSession::maskClear(bool visibleOnly) {
    std::vector<float> next = mask_;
    for (std::uint32_t v = 0; v < next.size(); ++v)
        if (!visibleOnly || vertexVisible(v)) next[v] = 0.0f;
    return finishMaskOp(next);
}

StrokeDelta SculptSession::maskFill(bool visibleOnly) {
    std::vector<float> next = mask_;
    for (std::uint32_t v = 0; v < next.size(); ++v)
        if (!visibleOnly || vertexVisible(v)) next[v] = 1.0f;
    return finishMaskOp(next);
}

StrokeDelta SculptSession::maskToggleVisible() {
    bool anyVisibleMasked = false;
    for (std::uint32_t v = 0; v < mask_.size() && !anyVisibleMasked; ++v)
        anyVisibleMasked = mask_[v] > 0.0f && vertexVisible(v);
    return anyVisibleMasked ? maskClear(true) : maskFill(true);
}

StrokeDelta SculptSession::maskInvert(bool visibleOnly) {
    std::vector<float> next = mask_;
    for (std::uint32_t v = 0; v < next.size(); ++v)
        if (!visibleOnly || vertexVisible(v)) next[v] = 1.0f - next[v];
    return finishMaskOp(next);
}

StrokeDelta SculptSession::maskBlur(int iterations) {
    std::vector<float> current = mask_;
    std::vector<float> next(current.size());
    for (int it = 0; it < std::max(1, iterations); ++it) {
        parallelFor(current.size(), 4096, [&](std::size_t b, std::size_t e) {
            for (std::size_t vi = b; vi < e; ++vi) {
                const auto v = static_cast<std::uint32_t>(vi);
                float sum = current[v];
                float count = 1.0f;
                for (std::uint32_t n : mesh_.neighbors(v)) {
                    sum += current[n];
                    count += 1.0f;
                }
                next[v] = sum / count;
            }
        });
        current.swap(next);
    }
    return finishMaskOp(current);
}

StrokeDelta SculptSession::maskSharpen() {
    std::vector<float> next = mask_;
    for (float& m : next) m = clamp01((m - 0.5f) * 2.0f + 0.5f);
    return finishMaskOp(next);
}

StrokeDelta SculptSession::maskGrow() {
    std::vector<float> next = mask_;
    parallelFor(next.size(), 4096, [&](std::size_t b, std::size_t e) {
        for (std::size_t vi = b; vi < e; ++vi) {
            const auto v = static_cast<std::uint32_t>(vi);
            for (std::uint32_t n : mesh_.neighbors(v)) next[v] = std::max(next[v], mask_[n]);
        }
    });
    return finishMaskOp(next);
}

StrokeDelta SculptSession::maskShrink() {
    std::vector<float> next = mask_;
    parallelFor(next.size(), 4096, [&](std::size_t b, std::size_t e) {
        for (std::size_t vi = b; vi < e; ++vi) {
            const auto v = static_cast<std::uint32_t>(vi);
            for (std::uint32_t n : mesh_.neighbors(v)) next[v] = std::min(next[v], mask_[n]);
        }
    });
    return finishMaskOp(next);
}

StrokeDelta SculptSession::maskByCavity(float coverage) {
    const std::vector<float> cavity = vertexCavity(mesh_);
    // Normalise by a robust maximum (95th percentile of positive values).
    std::vector<float> positive;
    for (float c : cavity)
        if (c > 0.0f) positive.push_back(c);
    if (positive.empty()) return finishMaskOp(std::vector<float>(mask_.size(), 0.0f));
    const std::size_t k = std::min(positive.size() - 1, positive.size() * 95 / 100);
    std::nth_element(positive.begin(), positive.begin() + static_cast<std::ptrdiff_t>(k), positive.end());
    const float scale = positive[k] > 0.0f ? positive[k] : 1.0f;
    const float threshold = 1.0f - clamp01(coverage);
    std::vector<float> next(mask_.size());
    for (std::uint32_t v = 0; v < next.size(); ++v)
        next[v] = vertexVisible(v) ? smoothstep(threshold * 0.8f, threshold * 0.8f + 0.2f + 1e-3f, cavity[v] / scale) : mask_[v];
    return finishMaskOp(next);
}

StrokeDelta SculptSession::maskByAO(float coverage, int rays) {
    const std::uint32_t V = mesh_.vertexCount();
    rays = std::min(std::max(rays, 4), 256);
    const Aabb box = bvh_.rootBounds();
    const float maxDistance = 0.25f * length(box.extent());
    const float epsilon = 1e-4f * length(box.extent());
    std::vector<float> occlusion(V, 0.0f);
    // Deterministic cosine-weighted hemisphere directions (Fibonacci spiral).
    std::vector<Vec3> local(static_cast<std::size_t>(rays));
    for (int i = 0; i < rays; ++i) {
        const float u = (static_cast<float>(i) + 0.5f) / static_cast<float>(rays);
        const float r = std::sqrt(u);
        const float phi = 2.39996323f * static_cast<float>(i);
        local[static_cast<std::size_t>(i)] = {r * std::cos(phi), r * std::sin(phi), std::sqrt(std::max(0.0f, 1.0f - u))};
    }
    const std::uint8_t* hidden = hiddenTriangles();
    parallelFor(V, 256, [&](std::size_t b, std::size_t e) {
        for (std::size_t vi = b; vi < e; ++vi) {
            const auto v = static_cast<std::uint32_t>(vi);
            const Vec3 n = mesh_.normal(v);
            if (lengthSq(n) == 0.0f) continue;
            const Vec3 a = std::fabs(n.x) < 0.9f ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
            const Vec3 tx = normalizedOrZero(cross(n, a));
            const Vec3 ty = cross(n, tx);
            const Vec3 origin = mesh_.position(v) + n * epsilon;
            int blocked = 0;
            for (const Vec3& d : local) {
                RayHit hit;
                const Ray ray{origin, tx * d.x + ty * d.y + n * d.z};
                if (bvh_.raycast(mesh_, ray, hit, false, maxDistance, hidden)) ++blocked;
            }
            occlusion[v] = static_cast<float>(blocked) / static_cast<float>(local.size());
        }
    });
    const float threshold = 1.0f - clamp01(coverage);
    std::vector<float> next(V);
    for (std::uint32_t v = 0; v < V; ++v)
        next[v] = vertexVisible(v) ? smoothstep(threshold * 0.8f, threshold * 0.8f + 0.2f + 1e-3f, occlusion[v]) : mask_[v];
    return finishMaskOp(next);
}

StrokeDelta SculptSession::maskSelect(const std::function<bool(const Vec3&)>& inside, bool erase) {
    std::vector<float> next = mask_;
    for (std::uint32_t v = 0; v < next.size(); ++v)
        if (vertexVisible(v) && inside(mesh_.position(v))) next[v] = erase ? 0.0f : 1.0f;
    return finishMaskOp(next);
}

// --- SculptGroups -----------------------------------------------------------------

StrokeDelta SculptSession::groupFromMask(float threshold) {
    std::vector<std::int32_t> next = faceGroups_;
    const std::int32_t id = nextGroupId();
    bool any = false;
    for (std::uint32_t f = 0; f < next.size(); ++f) {
        if (hiddenFaces_[f]) continue;
        const Span<std::uint32_t> poly = mesh_.faceVertices(f);
        float sum = 0.0f;
        for (std::uint32_t v : poly) sum += mask_[v];
        if (sum / static_cast<float>(poly.size()) >= threshold) {
            next[f] = id;
            any = true;
        }
    }
    if (!any) return StrokeDelta();
    return finishGroupOp(next);
}

StrokeDelta SculptSession::autoGroups(AutoGroupMode mode, const std::vector<std::uint64_t>* keys, float angleDegrees) {
    const std::uint32_t F = mesh_.faceCount();
    std::vector<std::int32_t> next;
    switch (mode) {
        case AutoGroupMode::Elements:
            next = floodGroups(mesh_, hiddenFaces_, faceGroups_, [](std::uint32_t, std::uint32_t) { return true; });
            break;
        case AutoGroupMode::Angle: {
            const float cosLimit = std::cos(std::min(std::max(angleDegrees, 0.0f), 180.0f) * 0.0174532925f);
            std::vector<Vec3> normals(F);
            for (std::uint32_t f = 0; f < F; ++f) normals[f] = mesh_.faceNormal(f);
            next = floodGroups(mesh_, hiddenFaces_, faceGroups_,
                               [&](std::uint32_t a, std::uint32_t b) { return dot(normals[a], normals[b]) >= cosLimit; });
            break;
        }
        case AutoGroupMode::Curvature: {
            const std::vector<float> cavity = vertexCavity(mesh_);
            double sq = 0.0;
            for (float c : cavity) sq += static_cast<double>(c) * static_cast<double>(c);
            const float spread = cavity.empty() ? 0.0f : static_cast<float>(std::sqrt(sq / static_cast<double>(cavity.size())));
            std::vector<std::uint8_t> cls(F, 0u);  // 0 flat, 1 convex, 2 concave
            for (std::uint32_t f = 0; f < F; ++f) {
                float sum = 0.0f;
                const Span<std::uint32_t> poly = mesh_.faceVertices(f);
                for (std::uint32_t v : poly) sum += cavity[v];
                const float c = sum / static_cast<float>(poly.size());
                cls[f] = c > 0.5f * spread ? 2u : (c < -0.5f * spread ? 1u : 0u);
            }
            next = floodGroups(mesh_, hiddenFaces_, faceGroups_,
                               [&](std::uint32_t a, std::uint32_t b) { return cls[a] == cls[b]; });
            break;
        }
        case AutoGroupMode::SmoothGroups:
        case AutoGroupMode::UVIslands: {
            if (!keys || keys->size() != F) return StrokeDelta();
            const bool smoothGroups = mode == AutoGroupMode::SmoothGroups;
            next = floodGroups(mesh_, hiddenFaces_, faceGroups_, [&](std::uint32_t a, std::uint32_t b) {
                // Smoothing groups join when they share a bit; islands when ids match.
                return smoothGroups ? (((*keys)[a] & (*keys)[b]) != 0u || (*keys)[a] == (*keys)[b])
                                    : (*keys)[a] == (*keys)[b];
            });
            break;
        }
        case AutoGroupMode::MaterialIDs: {
            if (!keys || keys->size() != F) return StrokeDelta();
            std::map<std::uint64_t, std::int32_t> ids;
            next = faceGroups_;
            for (std::uint32_t f = 0; f < F; ++f) {
                if (hiddenFaces_[f]) continue;
                auto it = ids.find((*keys)[f]);
                if (it == ids.end()) it = ids.emplace((*keys)[f], static_cast<std::int32_t>(ids.size() + 1)).first;
                next[f] = it->second;
            }
            break;
        }
    }
    return finishGroupOp(next);
}

// --- Visibility ---------------------------------------------------------------------

StrokeDelta SculptSession::showAll() {
    return finishHiddenOp(std::vector<std::uint8_t>(hiddenFaces_.size(), 0u));
}

StrokeDelta SculptSession::isolateOrHideGroup(std::int32_t group) {
    const std::uint32_t F = mesh_.faceCount();
    bool onlyThisVisible = true;
    for (std::uint32_t f = 0; f < F && onlyThisVisible; ++f)
        if (!hiddenFaces_[f] && faceGroups_[f] != group) onlyThisVisible = false;
    std::vector<std::uint8_t> next(F);
    for (std::uint32_t f = 0; f < F; ++f) {
        const bool inGroup = faceGroups_[f] == group;
        next[f] = onlyThisVisible ? static_cast<std::uint8_t>(inGroup ? 1u : 0u)   // Hide it, show the rest.
                                  : static_cast<std::uint8_t>(inGroup ? 0u : 1u);  // Isolate it.
    }
    // Never hide everything.
    if (std::all_of(next.begin(), next.end(), [](std::uint8_t h) { return h != 0u; })) return StrokeDelta();
    return finishHiddenOp(next);
}

StrokeDelta SculptSession::invertVisibility() {
    std::vector<std::uint8_t> next(hiddenFaces_.size());
    for (std::size_t f = 0; f < next.size(); ++f) next[f] = hiddenFaces_[f] ? 0u : 1u;
    if (std::all_of(next.begin(), next.end(), [](std::uint8_t h) { return h != 0u; })) return StrokeDelta();
    return finishHiddenOp(next);
}

}  // namespace sculpt
