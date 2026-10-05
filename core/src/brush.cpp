#include "sculpt/brush.h"

#include <algorithm>
#include <cmath>

#include "sculpt/parallel.h"

namespace sculpt {
namespace {

// Displacement per dab, as a fraction of the brush radius at full strength
// and full weight. Tuned for the default spacing (15% of radius).
constexpr float kSculptRate = 0.1f;
constexpr float kInflateRate = 0.1f;
constexpr float kCarveRate = 0.08f;
constexpr float kKnifeRate = 0.12f;
constexpr float kClayOffset = 0.25f;   // Clay working plane height above the surface.
constexpr float kLayerHeight = 0.25f;  // Layer Mode: maximum height per stroke.
// Fraction of the distance covered per dab.
constexpr float kPinchRate = 0.25f;
constexpr float kSmoothRate = 1.0f;
constexpr float kContrastRate = 0.5f;
// Dabs below this vertex count run on one thread (threading costs more).
constexpr std::size_t kParallelGrain = 1024;
constexpr float kSqrt2 = 1.41421356f;

template <class Fn>
void forEach(std::size_t count, Fn&& fn) {
    parallelFor(count, kParallelGrain, [&](std::size_t b, std::size_t e) {
        for (std::size_t i = b; i < e; ++i) fn(i);
    });
}

// Orthonormal frame on the dab plane: x/y orient the alpha, n is the normal.
struct Frame {
    Vec3 n, x, y;
};

Frame makeFrame(const Vec3& normal, const Vec3& up) {
    Frame f;
    f.n = normal;
    f.y = normalizedOrZero(up - normal * dot(up, normal));
    if (lengthSq(f.y) == 0.0f) {
        const Vec3 a = std::fabs(normal.x) < 0.9f ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
        f.y = normalizedOrZero(cross(normal, a));
    }
    f.x = cross(f.y, f.n);
    return f;
}

bool hasAlpha(const BrushSettings& s) { return s.alpha && !s.alpha->empty(); }

// Radial weight; with an alpha and no falloff the square alpha corners count.
float radialWeight(const BrushSettings& s, float t) {
    if (s.useFalloff) return brushFalloff(t, true);
    const float limit = hasAlpha(s) ? kSqrt2 : 1.0f;
    return (t >= 0.0f && t <= limit) ? 1.0f : 0.0f;
}

// Signed alpha factor in [-1, 1] (values below alphaMid push the other way).
float alphaFactor(const BrushSettings& s, const Frame& f, const Vec3& rel, float invRadius, float t) {
    const float lx = dot(rel, f.x) * invRadius;
    const float ly = dot(rel, f.y) * invRadius;
    const float a = s.alpha->sample((lx + 1.0f) * 0.5f, (1.0f - ly) * 0.5f);
    const float mid = std::min(std::max(s.alphaMid, 0.0f), 0.99f);
    float factor = (a - mid) / (1.0f - mid);
    if (s.alphaFade > 0.0f) factor *= clamp01((1.0f - std::min(t, 1.0f)) / std::min(s.alphaFade, 1.0f));
    return factor;
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

Vec3 groupBorderSmoothTarget(const Mesh& mesh, const std::vector<std::uint8_t>& border, std::uint32_t v) {
    Vec3 sum;
    int count = 0;
    for (std::uint32_t n : mesh.neighbors(v)) {
        if (!border[n]) continue;
        sum += mesh.position(n);
        ++count;
    }
    if (count < 2) return smoothTarget(mesh, v);
    return sum / static_cast<float>(count);
}

Vec3 tangentPull(const Vec3& p, const Vec3& center, const Vec3& n) {
    Vec3 toCenter = center - p;
    toCenter -= n * dot(toCenter, n);
    return toCenter;
}

std::size_t writeTargets(DabTarget& t, std::size_t count) {
    DabScratch& s = t.scratch;
    std::size_t moved = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint32_t v = s.verts[i];
        const Vec3& target = s.targets[i];
        const Vec3& current = t.mesh.position(v);
        if (!isFinite(target) || target == current) continue;
        t.recorder.touch(v, current);
        t.mesh.setPosition(v, target);
        t.moved.push_back(v);
        ++moved;
    }
    return moved;
}

float maskFactor(const DabTarget& t, std::uint32_t v) {
    if (!t.mask || v >= t.mask->size()) return 1.0f;
    return 1.0f - clamp01((*t.mask)[v]);
}

std::size_t applyMove(DabTarget& t, const BrushSettings& settings, const Dab& dab, float strength) {
    StrokeState& state = t.state;
    DabScratch& s = t.scratch;
    if (!state.grabbed) {
        state.grabbed = true;
        s.verts.clear();
        t.bvh.gatherVertices(t.mesh, dab.center, dab.radius, s.verts, t.hiddenTriangles);
        const float invRadius = 1.0f / dab.radius;
        const float exponent = 1.0f + 3.0f * clamp01(settings.accuCurve);  // AccuCurve tightens the profile.
        for (std::uint32_t v : s.verts) {
            if (settings.backfaceCull && dot(t.mesh.normal(v), dab.viewDir) > 0.0f) continue;
            const float tt = length(t.mesh.position(v) - dab.center) * invRadius;
            const float w = std::pow(radialWeight(settings, tt), exponent) * maskFactor(t, v);
            if (!(w > 0.0f)) continue;
            state.grabVerts.push_back(v);
            state.grabWeights.push_back(w);
        }
    }
    if (lengthSq(dab.grabDelta) == 0.0f || state.grabVerts.empty() || !isFinite(dab.grabDelta)) return 0;
    s.verts = state.grabVerts;
    s.targets.resize(s.verts.size());
    forEach(s.verts.size(), [&](std::size_t i) {
        s.targets[i] = t.mesh.position(s.verts[i]) + dab.grabDelta * (state.grabWeights[i] * strength);
    });
    return writeTargets(t, s.verts.size());
}

}  // namespace

const BrushInfo& brushInfo(BrushType type) {
    // name, script name, available, signed, layer mode, strength, backface cull, falloff
    static const BrushInfo kInfo[] = {
        {"Sculpt", "sculpt", true, true, true, 0.5f, true, true},
        {"Smooth", "smooth", true, false, false, 0.5f, true, true},
        {"Inflate", "inflate", true, true, false, 0.3f, true, true},
        {"Pinch", "pinch", true, true, false, 0.3f, true, true},
        {"Clay", "clay", true, true, false, 0.4f, true, true},
        {"Clay Buildup", "clayBuildup", true, true, false, 0.5f, true, true},
        {"Carve", "carve", true, true, false, 0.4f, true, true},
        {"Knife", "knife", true, true, false, 0.5f, true, true},
        {"Contrast", "contrast", true, true, false, 0.3f, true, true},
        {"Scrape", "scrape", true, true, false, 0.5f, true, true},
        {"Polish", "polish", true, true, false, 0.5f, true, true},
        {"Move", "move", true, false, false, 1.0f, false, true},
        {"Snake Hook", "snakeHook", true, false, false, 1.0f, false, true},
        {"Face groups", "faceGroups", true, false, false, 1.0f, true, false},
        {"Smooth SG Border", "smoothGroupBorder", true, false, false, 0.5f, true, true},
        {"Density", "density", true, true, false, 0.5f, true, true},
        {"Revert", "revert", true, false, false, 0.5f, true, true},
        {"Clip", "clip", true, false, false, 1.0f, false, false},
        {"Cutter", "cutter", true, false, false, 1.0f, false, false},
        {"Slice", "slice", true, false, false, 1.0f, false, false},
        {"Cloth", "cloth", true, true, false, 0.5f, true, true},
        {"Pose", "pose", true, false, false, 1.0f, false, true},
        {"Curve Tube", "curveTube", true, false, false, 1.0f, false, true},
        {"Displace", "displace", true, true, true, 0.5f, true, true},
        {"Paint Mask", "maskPaint", true, true, false, 0.7f, true, true},
    };
    static_assert(sizeof(kInfo) / sizeof(kInfo[0]) == static_cast<std::size_t>(BrushType::Count),
                  "Every brush needs a BrushInfo entry.");
    const int i = static_cast<int>(type);
    return kInfo[(i >= 0 && i < static_cast<int>(BrushType::Count)) ? i : 0];
}

const char* brushName(BrushType type) {
    if (static_cast<int>(type) < 0 || type >= BrushType::Count) return "Unknown";
    return brushInfo(type).name;
}

bool isSignedBrush(BrushType type) { return type < BrushType::Count && brushInfo(type).isSigned; }

bool isGeometryBrush(BrushType type) {
    // Brushes moved by applyDab kernels (the others are handled by the session or the host).
    switch (type) {
        case BrushType::FaceGroups:
        case BrushType::MaskPaint:
        case BrushType::Density:
        case BrushType::Clip:
        case BrushType::Cutter:
        case BrushType::Slice:
        case BrushType::Cloth:
        case BrushType::Pose:
        case BrushType::CurveTube:
        case BrushType::Displace:
            return false;
        default:
            return type < BrushType::Count && static_cast<int>(type) >= 0 && brushInfo(type).available;
    }
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

float dabWeight(const BrushSettings& settings, const Dab& dab, const Vec3& frameNormal, const Vec3& position) {
    if (!(dab.radius > 0.0f)) return 0.0f;
    const Vec3 rel = position - dab.center;
    const float invRadius = 1.0f / dab.radius;
    const float t = length(rel) * invRadius;
    float w = radialWeight(settings, t);
    if (w != 0.0f && hasAlpha(settings)) w *= alphaFactor(settings, makeFrame(frameNormal, dab.up), rel, invRadius, t);
    return w;
}

std::size_t applyDab(DabTarget& t, const BrushSettings& settings, const Dab& dab) {
    if (!(dab.radius > 0.0f) || !isFinite(dab.radius) || !isFinite(dab.center)) return 0;
    if (!isGeometryBrush(settings.type)) return 0;
    const float strength = clamp01(settings.strength) * clamp01(dab.pressure) * std::max(0.0f, dab.amount);
    if (!(strength > 0.0f)) return 0;
    if (settings.type == BrushType::Move) return applyMove(t, settings, dab, strength);
    if (settings.type == BrushType::SmoothGroupBorder && !t.groupBorder) return 0;

    Mesh& mesh = t.mesh;
    DabScratch& s = t.scratch;
    const bool alpha = hasAlpha(settings);
    const float invRadius = 1.0f / dab.radius;
    const Vec3 center = dab.center;

    s.verts.clear();
    t.bvh.gatherVertices(mesh, center, dab.radius * (alpha && !settings.useFalloff ? kSqrt2 : 1.0f), s.verts,
                         t.hiddenTriangles);
    if (s.verts.empty()) return 0;

    // Pass 1: plain radial weight (no alpha/strength/mask) for the area plane.
    s.falloff.resize(s.verts.size());
    forEach(s.verts.size(), [&](std::size_t i) {
        const std::uint32_t v = s.verts[i];
        float w = 0.0f;
        if (!(settings.backfaceCull && dot(mesh.normal(v), dab.viewDir) > 0.0f))
            w = radialWeight(settings, length(mesh.position(v) - center) * invRadius);
        s.falloff[i] = w;
    });
    Vec3 normalSum, centerSum;
    float weightSum = 0.0f;
    for (std::size_t i = 0; i < s.verts.size(); ++i) {
        const float w = s.falloff[i];
        if (w <= 0.0f) continue;
        normalSum += mesh.normal(s.verts[i]) * w;
        centerSum += mesh.position(s.verts[i]) * w;
        weightSum += w;
    }
    if (!(weightSum > 0.0f)) return 0;
    Vec3 n = normalizedOrZero(normalSum);
    if (lengthSq(n) == 0.0f) n = normalizedOrZero(-dab.viewDir);
    const Vec3 c = centerSum / weightSum;
    const Frame frame = makeFrame(n, dab.up);

    // Pass 2: final signed weight = radial * alpha * strength * mask.
    const bool square = settings.type == BrushType::ClayBuildup && !alpha;
    s.weights.resize(s.verts.size());
    forEach(s.verts.size(), [&](std::size_t i) {
        float w = s.falloff[i];
        if (w != 0.0f) {
            const std::uint32_t v = s.verts[i];
            const Vec3 rel = mesh.position(v) - center;
            if (square) {
                // Square footprint aligned to the stroke frame.
                const float tq = std::max(std::fabs(dot(rel, frame.x)), std::fabs(dot(rel, frame.y))) * invRadius;
                w = radialWeight(settings, tq);
            }
            if (alpha) w *= alphaFactor(settings, frame, rel, invRadius, length(rel) * invRadius);
            w *= strength * maskFactor(t, v);
        }
        s.weights[i] = w;
    });
    std::size_t kept = 0;
    for (std::size_t i = 0; i < s.verts.size(); ++i) {
        if (s.weights[i] == 0.0f || !isFinite(s.weights[i])) continue;
        s.verts[kept] = s.verts[i];
        s.weights[kept] = s.weights[i];
        ++kept;
    }
    s.verts.resize(kept);
    s.weights.resize(kept);
    if (kept == 0) return 0;

    const float sign = (isSignedBrush(settings.type) && (settings.subtract != dab.invert)) ? -1.0f : 1.0f;
    const float r = dab.radius;
    s.targets.resize(kept);

    switch (settings.type) {
        case BrushType::Sculpt: {
            if (settings.layerMode) {
                // Settle every vertex at one height per stroke: repeated passes
                // only change it if the new dab asks for more.
                for (std::size_t i = 0; i < kept; ++i) {
                    const std::uint32_t v = s.verts[i];
                    auto it = t.state.layer.find(v);
                    if (it == t.state.layer.end())
                        it = t.state.layer.emplace(v, StrokeState::LayerEntry{mesh.position(v), mesh.normal(v), 0.0f}).first;
                    StrokeState::LayerEntry& e = it->second;
                    const float desired = sign * r * kLayerHeight * s.weights[i];
                    if (std::fabs(desired) > std::fabs(e.height) &&
                        (e.height == 0.0f || (desired > 0.0f) == (e.height > 0.0f)))
                        e.height = desired;
                    s.targets[i] = e.origin + e.normal * e.height;
                }
            } else {
                const Vec3 offset = n * (sign * r * kSculptRate);
                forEach(kept, [&](std::size_t i) { s.targets[i] = mesh.position(s.verts[i]) + offset * s.weights[i]; });
            }
            break;
        }
        case BrushType::Smooth:
            forEach(kept, [&](std::size_t i) {
                const std::uint32_t v = s.verts[i];
                const Vec3& p = mesh.position(v);
                s.targets[i] = p + (smoothTarget(mesh, v) - p) * clamp01(std::fabs(s.weights[i]) * kSmoothRate);
            });
            break;
        case BrushType::Revert:
            // Back toward the captured Surface Snapshot (no snapshot: no effect).
            if (!t.reference || t.reference->size() != mesh.vertexCount()) return 0;
            forEach(kept, [&](std::size_t i) {
                const std::uint32_t v = s.verts[i];
                const Vec3& p = mesh.position(v);
                s.targets[i] = p + ((*t.reference)[v] - p) * clamp01(std::fabs(s.weights[i]) * kSmoothRate);
            });
            break;
        case BrushType::SmoothGroupBorder:
            forEach(kept, [&](std::size_t i) {
                const std::uint32_t v = s.verts[i];
                const Vec3& p = mesh.position(v);
                if (!(*t.groupBorder)[v]) {
                    s.targets[i] = p;
                    return;
                }
                const Vec3 target = groupBorderSmoothTarget(mesh, *t.groupBorder, v);
                s.targets[i] = p + (target - p) * clamp01(std::fabs(s.weights[i]) * kSmoothRate);
            });
            break;
        case BrushType::Inflate: {
            const float amount = sign * r * kInflateRate;
            forEach(kept, [&](std::size_t i) {
                const std::uint32_t v = s.verts[i];
                s.targets[i] = mesh.position(v) + mesh.normal(v) * (amount * s.weights[i]);
            });
            break;
        }
        case BrushType::Pinch:
            forEach(kept, [&](std::size_t i) {
                const Vec3& p = mesh.position(s.verts[i]);
                s.targets[i] = p + tangentPull(p, center, n) * (sign * kPinchRate * s.weights[i]);
            });
            break;
        case BrushType::Clay:
        case BrushType::ClayBuildup: {
            const bool buildup = settings.type == BrushType::ClayBuildup;
            const float lift = sign * r * kClayOffset * (buildup ? 1.5f : 1.0f) * clamp01(settings.strength);
            const Vec3 planePoint = c + n * lift;
            const float exponent = 1.0f + 4.0f * clamp01(settings.clayBorder);
            forEach(kept, [&](std::size_t i) {
                const Vec3& p = mesh.position(s.verts[i]);
                const float d = dot(planePoint - p, n);  // > 0: below the plane.
                if (d * sign <= 0.0f) {
                    s.targets[i] = p;
                    return;
                }
                const float w = clamp01(std::fabs(s.weights[i]) * (buildup ? 1.5f : 1.0f));
                const float shaped = 1.0f - std::pow(1.0f - w, exponent);  // Border: flatter plateau.
                s.targets[i] = p + n * (d * shaped * 0.5f);
            });
            break;
        }
        case BrushType::Carve:
        case BrushType::Knife: {
            const bool knife = settings.type == BrushType::Knife;
            const float depth = (knife ? kKnifeRate : kCarveRate) * r;
            const float pinch = knife ? 0.6f : 0.3f;
            forEach(kept, [&](std::size_t i) {
                const Vec3& p = mesh.position(s.verts[i]);
                float w = s.weights[i];
                if (knife) w = w * w * w;  // Much tighter profile for a hard cut.
                // Add (default) digs in; Alt/Sub raises.
                s.targets[i] = p + tangentPull(p, center, n) * (pinch * std::fabs(w)) - n * (sign * depth * w);
            });
            break;
        }
        case BrushType::Contrast:
            forEach(kept, [&](std::size_t i) {
                const Vec3& p = mesh.position(s.verts[i]);
                const float h = dot(p - c, n);
                s.targets[i] = p + n * (h * kContrastRate * sign * s.weights[i]);
            });
            break;
        case BrushType::Scrape: {
            StrokeState& state = t.state;
            if ((settings.scrapeOriginalPlane || settings.scrapeOriginalNormal) && !state.havePlane) {
                state.havePlane = true;
                state.planePoint = c;
                state.planeNormal = n;
            }
            const Vec3 pp = settings.scrapeOriginalPlane ? state.planePoint : c;
            const Vec3 pn = settings.scrapeOriginalNormal ? state.planeNormal : n;
            forEach(kept, [&](std::size_t i) {
                const Vec3& p = mesh.position(s.verts[i]);
                const float h = dot(p - pp, pn);
                // Add: shave high points. Alt/Sub: fill low areas.
                const bool affect = sign > 0.0f ? h > 0.0f : h < 0.0f;
                s.targets[i] = affect ? p - pn * (h * clamp01(std::fabs(s.weights[i]))) : p;
            });
            break;
        }
        case BrushType::Polish: {
            const float hardness = clamp01(settings.polishHardness);
            forEach(kept, [&](std::size_t i) {
                const std::uint32_t v = s.verts[i];
                const Vec3& p = mesh.position(v);
                const float h = dot(p - c, n);
                if (sign < 0.0f && h >= 0.0f) {  // Alt: fill low areas only.
                    s.targets[i] = p;
                    return;
                }
                const Vec3 flat = p - n * h;
                const Vec3 target = flat * hardness + smoothTarget(mesh, v) * (1.0f - hardness);
                s.targets[i] = p + (target - p) * clamp01(std::fabs(s.weights[i]));
            });
            break;
        }
        case BrushType::SnakeHook:
            if (!isFinite(dab.grabDelta)) return 0;
            forEach(kept, [&](std::size_t i) {
                s.targets[i] = mesh.position(s.verts[i]) + dab.grabDelta * std::fabs(s.weights[i]);
            });
            break;
        default:
            return 0;
    }
    return writeTargets(t, kept);
}

}  // namespace sculpt
