#include "sculpt/deform.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace sculpt {

namespace {

float Smoothstep(float e0, float e1, float x) {
    if (e1 == e0) return x < e0 ? 0.0f : 1.0f;
    const float t = clamp01((x - e0) / (e1 - e0));
    return t * t * (3.0f - 2.0f * t);
}

Vec3 AnyPerpendicular(const Vec3& n) {
    const Vec3 axis = std::fabs(n.x) < 0.9f ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
    return normalizedOrZero(cross(axis, n));
}

}  // namespace

Mat3 axisAngle(const Vec3& axis, float angle) {
    const Vec3 a = normalizedOrZero(axis);
    if (lengthSq(a) == 0.0f || !std::isfinite(angle)) return Mat3::identity();
    const float c = std::cos(angle), s = std::sin(angle), t = 1.0f - c;
    return {{t * a.x * a.x + c, t * a.x * a.y - s * a.z, t * a.x * a.z + s * a.y},
            {t * a.x * a.y + s * a.z, t * a.y * a.y + c, t * a.y * a.z - s * a.x},
            {t * a.x * a.z - s * a.y, t * a.y * a.z + s * a.x, t * a.z * a.z + c}};
}

// --- Pose --------------------------------------------------------------------------------

void poseWeights(const Mesh& mesh, const Vec3& pivot, const Vec3& handle, float band, const PoseSettings& settings,
                 std::vector<float>& weights) {
    const std::uint32_t V = mesh.vertexCount();
    weights.assign(V, 0.0f);
    const Vec3 guide = handle - pivot;
    const float len = length(guide);
    if (!(len > 0.0f)) return;
    const Vec3 dir = guide / len;
    const Vec3 origin = pivot + guide * settings.originOffset;
    band = std::max(band, len * 1e-3f);
    for (std::uint32_t v = 0; v < V; ++v) weights[v] = Smoothstep(-band, band, dot(mesh.position(v) - origin, dir));
    if (settings.connectedOnly) {
        // Keep only what is connected to the handle on its side of the plane.
        std::uint32_t seed = 0;
        float best = kInfinity;
        for (std::uint32_t v = 0; v < V; ++v) {
            const float d = lengthSq(mesh.position(v) - handle);
            if (d < best) {
                best = d;
                seed = v;
            }
        }
        std::vector<std::uint8_t> reached(V, 0u);
        std::vector<std::uint32_t> stack = {seed};
        reached[seed] = 1u;
        while (!stack.empty()) {
            const std::uint32_t v = stack.back();
            stack.pop_back();
            for (std::uint32_t n : mesh.neighbors(v)) {
                if (reached[n] || weights[n] <= 0.0f) continue;
                reached[n] = 1u;
                stack.push_back(n);
            }
        }
        for (std::uint32_t v = 0; v < V; ++v)
            if (!reached[v]) weights[v] = 0.0f;
    }
    std::vector<float> next(V);
    for (int i = 0; i < settings.smoothIterations; ++i) {
        for (std::uint32_t v = 0; v < V; ++v) {
            const Span<std::uint32_t> n = mesh.neighbors(v);
            if (n.size() == 0) {
                next[v] = weights[v];
                continue;
            }
            float sum = 0.0f;
            for (std::uint32_t u : n) sum += weights[u];
            next[v] = 0.5f * weights[v] + 0.5f * sum / static_cast<float>(n.size());
        }
        weights.swap(next);
    }
}

bool poseGroupWeights(const Mesh& mesh, const std::vector<std::int32_t>& faceGroups, std::uint32_t seedFace,
                      int smoothIterations, std::vector<float>& weights, Vec3& pivot) {
    const std::uint32_t V = mesh.vertexCount();
    weights.assign(V, 0.0f);
    if (seedFace >= mesh.faceCount() || faceGroups.size() != mesh.faceCount()) return false;
    const std::int32_t group = faceGroups[seedFace];
    // The connected polygons of the group.
    std::vector<std::uint8_t> inside(mesh.faceCount(), 0u);
    std::vector<std::uint32_t> stack = {seedFace};
    inside[seedFace] = 1u;
    while (!stack.empty()) {
        const std::uint32_t f = stack.back();
        stack.pop_back();
        for (std::uint32_t n : mesh.faceNeighbors(f)) {
            if (inside[n] || faceGroups[n] != group) continue;
            inside[n] = 1u;
            stack.push_back(n);
        }
    }
    Vec3 sum;
    std::size_t borderCount = 0;
    for (std::uint32_t v = 0; v < V; ++v) {
        int in = 0, out = 0;
        for (std::uint32_t f : mesh.vertexFaces(v)) (inside[f] ? in : out)++;
        if (in == 0) continue;
        weights[v] = out == 0 ? 1.0f : 0.5f;
        if (out > 0) {
            sum += mesh.position(v);
            ++borderCount;
        }
    }
    if (borderCount == 0) return false;
    pivot = sum / static_cast<float>(borderCount);
    std::vector<float> next(V);
    for (int i = 0; i < smoothIterations; ++i) {
        for (std::uint32_t v = 0; v < V; ++v) {
            const Span<std::uint32_t> n = mesh.neighbors(v);
            float total = 0.0f;
            for (std::uint32_t u : n) total += weights[u];
            next[v] = n.size() == 0 ? weights[v] : 0.5f * weights[v] + 0.5f * total / static_cast<float>(n.size());
        }
        weights.swap(next);
    }
    return true;
}

void posePositions(const std::vector<std::uint32_t>& vertices, const std::vector<Vec3>& original,
                   const std::vector<float>& weights, const Vec3& pivot, const Vec3& handle, const Vec3& target,
                   float amount, const PoseSettings& settings, std::vector<Vec3>& out) {
    out.resize(vertices.size());
    const Vec3 guide = handle - pivot;
    const float len = length(guide);
    const Vec3 origin = pivot + guide * settings.originOffset;
    const Vec3 dir = len > 0.0f ? guide / len : Vec3{0, 0, 1};
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        const Vec3& p = original[i];
        const float w = vertices[i] < weights.size() ? weights[vertices[i]] : 0.0f;
        if (w <= 0.0f || !(len > 0.0f)) {
            out[i] = p;
            continue;
        }
        const float along = clamp01(dot(p - origin, dir) / len);
        switch (settings.deformation) {
            case PoseDeformation::Twist: {
                const Mat3 r = axisAngle(dir, amount * along);
                out[i] = p + ((r * (p - origin) + origin) - p) * w;
                break;
            }
            case PoseDeformation::Scale:
                out[i] = origin + (p - origin) * (1.0f + amount * w);
                break;
            case PoseDeformation::Rotate:
            default: {
                const Vec3 from = normalizedOrZero(handle - origin), to = normalizedOrZero(target - origin);
                const Vec3 axis = cross(from, to);
                const float angle = std::atan2(length(axis), dot(from, to));
                const int segments = std::max(1, settings.ikSegments);
                Vec3 q = p;
                for (int s = segments - 1; s >= 0; --s) {
                    const Vec3 center = origin + (handle - origin) * (static_cast<float>(s) / static_cast<float>(segments));
                    const float ws = s == 0 ? w : w * clamp01(along * static_cast<float>(segments) - static_cast<float>(s));
                    if (ws <= 0.0f) continue;
                    const Mat3 r = axisAngle(axis, angle / static_cast<float>(segments));
                    q = q + ((r * (q - center) + center) - q) * ws;
                }
                out[i] = q;
                break;
            }
        }
    }
}

// --- Cloth -------------------------------------------------------------------------------

bool ClothSim::begin(const Mesh& mesh, const Vec3& center, float brushRadius, const ClothSettings& settings) {
    end();
    settings_ = settings;
    radius_ = std::max(brushRadius, 1e-6f);
    const float simRadius = radius_ * std::min(std::max(settings.simulationArea, 1.0f), 5.0f);
    std::unordered_map<std::uint32_t, std::uint32_t> index;
    for (std::uint32_t v = 0; v < mesh.vertexCount(); ++v) {
        const float d = length(mesh.position(v) - center);
        if (d > simRadius) continue;
        index.emplace(v, static_cast<std::uint32_t>(particles_.size()));
        particles_.push_back(v);
        x_.push_back(mesh.position(v));
        normal_.push_back(mesh.normal(v));
        invMass_.push_back(settings.pinBoundary && d > 0.85f * simRadius ? 0.0f : 1.0f);
    }
    if (particles_.empty()) return false;
    prev_ = x_;
    const float bendStiffness = clamp01(settings.bendStiffness) * (1.0f - clamp01(settings.bendiness));
    for (std::size_t i = 0; i < particles_.size(); ++i) {
        const std::uint32_t v = particles_[i];
        for (std::uint32_t n : mesh.neighbors(v)) {
            auto it = index.find(n);
            if (it == index.end()) continue;
            const std::uint32_t j = it->second;
            if (v < n) constraints_.push_back({static_cast<std::uint32_t>(i), j, length(x_[i] - x_[j]), 1.0f});
            if (bendStiffness <= 0.0f) continue;
            // Bending: to the neighbour of n farthest from v (roughly across n).
            std::uint32_t far = n;
            float farDistance = 0.0f;
            for (std::uint32_t m : mesh.neighbors(n)) {
                const float d = lengthSq(mesh.position(m) - mesh.position(v));
                if (m != v && d > farDistance) {
                    farDistance = d;
                    far = m;
                }
            }
            auto fit = index.find(far);
            if (far != n && fit != index.end() && v < far)
                constraints_.push_back({static_cast<std::uint32_t>(i), fit->second, std::sqrt(farDistance), bendStiffness});
        }
    }
    return true;
}

void ClothSim::step(const Vec3& brushCenter, float brushRadius, const Vec3& grab, bool shrink,
                    std::vector<std::uint32_t>& vertices, std::vector<Vec3>& targets) {
    vertices.clear();
    targets.clear();
    if (particles_.empty()) return;
    const ClothSettings& s = settings_;
    const float r = std::max(brushRadius, 1e-6f);
    // Drag the brush area.
    for (std::size_t i = 0; i < x_.size(); ++i) {
        if (invMass_[i] == 0.0f) continue;
        const float d = length(x_[i] - brushCenter) / r;
        if (d >= 1.0f) continue;
        x_[i] += grab * (clamp01(s.moveStrength) * (1.0f - d * d * (3.0f - 2.0f * d)));
    }
    // Shrink (Alt) and folds compress the rest shape near the brush.
    const float foldRadius = r * std::max(0.1f, s.foldSize) * 2.0f;
    for (Constraint& c : constraints_) {
        const Vec3 mid = (x_[c.a] + x_[c.b]) * 0.5f;
        const float d = length(mid - brushCenter);
        if (shrink && d < r) c.rest *= 0.97f;
        if (s.foldStrength > 0.0f && d < foldRadius) c.rest *= 1.0f - 0.02f * clamp01(s.foldStrength);
    }
    // Integrate.
    const Vec3 gravity = Vec3{0.0f, 0.0f, -1.0f} * (s.gravity * radius_ * 0.02f);
    for (std::size_t i = 0; i < x_.size(); ++i) {
        if (invMass_[i] == 0.0f) continue;
        const Vec3 velocity = (x_[i] - prev_[i]) * (1.0f - clamp01(s.damping));
        prev_[i] = x_[i];
        x_[i] += velocity + gravity + normal_[i] * (s.pressure * radius_ * 0.02f);
    }
    // Satisfy the constraints.
    const int iterations = std::min(std::max(s.iterations, 1), 50);
    for (int it = 0; it < iterations; ++it) {
        for (const Constraint& c : constraints_) {
            const float wa = invMass_[c.a], wb = invMass_[c.b];
            const float w = wa + wb;
            if (w == 0.0f) continue;
            const Vec3 d = x_[c.b] - x_[c.a];
            const float len = length(d);
            if (len < 1e-12f) continue;
            const Vec3 correction = d * ((len - c.rest) / (len * w) * c.stiffness);
            x_[c.a] += correction * wa;
            x_[c.b] -= correction * wb;
        }
    }
    // Plasticity: the rest shape slowly takes on the new form.
    if (s.plasticity > 0.0f)
        for (Constraint& c : constraints_) c.rest += (length(x_[c.b] - x_[c.a]) - c.rest) * clamp01(s.plasticity) * 0.2f;
    for (std::size_t i = 0; i < x_.size(); ++i) {
        if (invMass_[i] == 0.0f || !isFinite(x_[i])) continue;
        vertices.push_back(particles_[i]);
        targets.push_back(x_[i]);
    }
}

// --- Profile curve --------------------------------------------------------------------------

ProfileCurve::ProfileCurve() {
    ProfilePoint a, b;
    a.x = 0.0f;
    b.x = 1.0f;
    a.y = b.y = 1.0f;
    points_ = {a, b};
    normalize();
}

void ProfileCurve::normalize() {
    std::sort(points_.begin(), points_.end(), [](const ProfilePoint& a, const ProfilePoint& b) { return a.x < b.x; });
    if (points_.size() < 2) *this = ProfileCurve();
    points_.front().x = 0.0f;
    points_.back().x = 1.0f;
    const std::size_t n = points_.size();
    for (std::size_t i = 0; i < n; ++i) {
        ProfilePoint& p = points_[i];
        const float dxPrev = i > 0 ? p.x - points_[i - 1].x : 0.0f;
        const float dxNext = i + 1 < n ? points_[i + 1].x - p.x : 0.0f;
        if (p.type == ProfilePointType::Linear) {
            p.inX = p.inY = p.outX = p.outY = 0.0f;
            continue;
        }
        if (p.type == ProfilePointType::Smooth) {
            const ProfilePoint& a = points_[i > 0 ? i - 1 : i];
            const ProfilePoint& b = points_[i + 1 < n ? i + 1 : i];
            const float dx = b.x - a.x;
            const float slope = dx > 0.0f ? (b.y - a.y) / dx : 0.0f;
            p.inX = -dxPrev / 3.0f;
            p.inY = -slope * dxPrev / 3.0f;
            p.outX = dxNext / 3.0f;
            p.outY = slope * dxNext / 3.0f;
        }
        // Keep handles inside their segments so x stays monotonic.
        p.inX = std::min(0.0f, std::max(p.inX, -dxPrev));
        p.outX = std::max(0.0f, std::min(p.outX, dxNext));
    }
}

float ProfileCurve::evaluate(float x) const {
    if (points_.empty()) return 1.0f;
    x = clamp01(x);
    std::size_t i = 0;
    while (i + 2 < points_.size() && x > points_[i + 1].x) ++i;
    const ProfilePoint& a = points_[i];
    const ProfilePoint& b = points_[std::min(i + 1, points_.size() - 1)];
    if (b.x <= a.x) return a.y;
    if (a.type == ProfilePointType::Linear && b.type == ProfilePointType::Linear)
        return a.y + (b.y - a.y) * (x - a.x) / (b.x - a.x);
    const float x0 = a.x, x1 = a.x + a.outX, x2 = b.x + b.inX, x3 = b.x;
    const float y0 = a.y, y1 = a.y + a.outY, y2 = b.y + b.inY, y3 = b.y;
    auto bez = [](float p0, float p1, float p2, float p3, float t) {
        const float u = 1.0f - t;
        return u * u * u * p0 + 3.0f * u * u * t * p1 + 3.0f * u * t * t * p2 + t * t * t * p3;
    };
    float lo = 0.0f, hi = 1.0f;
    for (int it = 0; it < 30; ++it) {
        const float mid = 0.5f * (lo + hi);
        if (bez(x0, x1, x2, x3, mid) < x)
            lo = mid;
        else
            hi = mid;
    }
    return bez(y0, y1, y2, y3, 0.5f * (lo + hi));
}

int ProfileCurve::addPoint(float x, float y) {
    ProfilePoint p;
    p.x = std::min(std::max(x, 0.001f), 0.999f);
    p.y = y;
    p.type = ProfilePointType::Smooth;
    points_.push_back(p);
    normalize();
    for (std::size_t i = 0; i < points_.size(); ++i)
        if (points_[i].x == p.x && points_[i].y == p.y) return static_cast<int>(i);
    return -1;
}

void ProfileCurve::removePoint(int index) {
    if (index <= 0 || index + 1 >= static_cast<int>(points_.size())) return;
    points_.erase(points_.begin() + index);
    normalize();
}

std::string ProfileCurve::toText() const {
    std::ostringstream os;
    for (const ProfilePoint& p : points_)
        os << p.x << ',' << p.y << ',' << p.inX << ',' << p.inY << ',' << p.outX << ',' << p.outY << ','
           << static_cast<int>(p.type) << ';';
    return os.str();
}

bool ProfileCurve::fromText(const std::string& text) {
    std::vector<ProfilePoint> points;
    std::istringstream is(text);
    std::string item;
    while (std::getline(is, item, ';')) {
        ProfilePoint p;
        int type = 2;
        if (std::sscanf(item.c_str(), "%f,%f,%f,%f,%f,%f,%d", &p.x, &p.y, &p.inX, &p.inY, &p.outX, &p.outY, &type) != 7)
            continue;
        if (!std::isfinite(p.x) || !std::isfinite(p.y)) continue;
        p.type = static_cast<ProfilePointType>(std::min(std::max(type, 0), 3));
        points.push_back(p);
    }
    if (points.size() < 2) return false;
    points_ = std::move(points);
    normalize();
    return true;
}

// --- Curve Tube --------------------------------------------------------------------------------

std::vector<Vec3> sampleCurve(const std::vector<Vec3>& controls, float spacing) {
    std::vector<Vec3> dense;
    const std::size_t n = controls.size();
    if (n < 2) return controls;
    for (std::size_t i = 0; i + 1 < n; ++i) {
        const Vec3& p0 = controls[i > 0 ? i - 1 : i];
        const Vec3& p1 = controls[i];
        const Vec3& p2 = controls[i + 1];
        const Vec3& p3 = controls[i + 2 < n ? i + 2 : i + 1];
        for (int k = 0; k < 10; ++k) {
            const float t = static_cast<float>(k) / 10.0f, t2 = t * t, t3 = t2 * t;
            dense.push_back((p1 * 2.0f + (p2 - p0) * t + (p0 * 2.0f - p1 * 5.0f + p2 * 4.0f - p3) * t2 +
                             (p1 * 3.0f - p0 - p2 * 3.0f + p3) * t3) *
                            0.5f);
        }
    }
    dense.push_back(controls.back());
    float total = 0.0f;
    for (std::size_t i = 1; i < dense.size(); ++i) total += length(dense[i] - dense[i - 1]);
    if (!(spacing > 0.0f) || !(total > 0.0f)) return {dense.front(), dense.back()};
    const int count = std::max(1, static_cast<int>(std::lround(total / spacing)));
    std::vector<Vec3> out = {dense.front()};
    float walked = 0.0f;
    std::size_t seg = 1;
    for (int s = 1; s < count; ++s) {
        const float target = total * static_cast<float>(s) / static_cast<float>(count);
        while (seg < dense.size()) {
            const float len = length(dense[seg] - dense[seg - 1]);
            if (walked + len >= target) {
                const float t = len > 0.0f ? (target - walked) / len : 0.0f;
                out.push_back(lerp(dense[seg - 1], dense[seg], t));
                break;
            }
            walked += len;
            ++seg;
        }
    }
    out.push_back(dense.back());
    return out;
}

bool appendTube(PolyData& poly, const std::vector<Vec3>& curve, const TubeSettings& settings, std::int32_t group,
                std::string* error) {
    if (curve.size() < 2 || !(settings.radius > 0.0f)) {
        if (error) *error = "the tube needs at least two points";
        return false;
    }
    std::vector<std::array<float, 2>> ring;
    if (settings.section && settings.section->size() >= 3) {
        ring.assign(settings.section->begin(), settings.section->end());
        float area = 0.0f;
        for (std::size_t i = 0; i < ring.size(); ++i) {
            const auto& a = ring[i];
            const auto& b = ring[(i + 1) % ring.size()];
            area += a[0] * b[1] - b[0] * a[1];
        }
        if (area < 0.0f) std::reverse(ring.begin(), ring.end());  // Counter-clockwise keeps faces outward.
    } else {
        const int sides = std::min(std::max(settings.sides, 3), 64);
        for (int k = 0; k < sides; ++k) {
            const float a = 6.28318530718f * static_cast<float>(k) / static_cast<float>(sides);
            ring.push_back({std::cos(a), std::sin(a)});
        }
    }
    const std::size_t n = curve.size(), sides = ring.size();
    float total = 0.0f;
    for (std::size_t i = 1; i < n; ++i) total += length(curve[i] - curve[i - 1]);
    if (!(total > 0.0f)) total = 1.0f;

    // Rings along the curve with parallel-transported frames (computed as we
    // go: indexing per-point frame arrays trips GCC's -O3 null-dereference check).
    const std::uint32_t base = static_cast<std::uint32_t>(poly.positions.size());
    const bool param = settings.profileParam && settings.profileParam->size() == n;
    std::vector<float> along;
    along.reserve(n);
    Vec3 normal;
    float walked = 0.0f;
    for (std::size_t i = 0; i < n; ++i) {
        const Vec3 tangent = normalizedOrZero(curve[std::min(i + 1, n - 1)] - curve[i > 0 ? i - 1 : 0]);
        if (i == 0) {
            normal = AnyPerpendicular(tangent);
        } else {
            walked += length(curve[i] - curve[i - 1]);
            const Vec3 nn = normal - tangent * dot(normal, tangent);
            normal = lengthSq(nn) > 1e-12f ? normalizedOrZero(nn) : AnyPerpendicular(tangent);
        }
        const Vec3 binormal = cross(tangent, normal);
        const float u = walked / total;  // Tapers follow the length.
        along.push_back(u);
        const float t = param ? clamp01((*settings.profileParam)[i]) : u;
        float r = settings.radius * (settings.profile ? std::max(0.0f, settings.profile->evaluate(t)) : 1.0f);
        r *= settings.taperStart + (1.0f - settings.taperStart) * Smoothstep(0.0f, 0.3f, u);
        r *= 1.0f + (settings.taperEnd - 1.0f) * Smoothstep(0.7f, 1.0f, u);
        for (const auto& s : ring) poly.positions.push_back(curve[i] + normal * (s[0] * r) + binormal * (s[1] * r));
    }
    const std::uint32_t startCenter = static_cast<std::uint32_t>(poly.positions.size());
    poly.positions.push_back(curve.front());
    const std::uint32_t endCenter = static_cast<std::uint32_t>(poly.positions.size());
    poly.positions.push_back(curve.back());
    if (!poly.mask.empty()) poly.mask.resize(poly.positions.size(), 0.0f);

    const bool hasMaterial = !poly.material.empty(), hasSmoothing = !poly.smoothing.empty();
    const bool hasGroups = !poly.groups.empty() || poly.faceSizes.empty(), hasHidden = !poly.hidden.empty();
    if (hasGroups && poly.groups.size() != poly.faceSizes.size()) poly.groups.resize(poly.faceSizes.size(), 0);
    const std::uint16_t material = hasMaterial && !poly.material.empty() ? poly.material.front() : 0;
    auto vertex = [&](std::size_t i, std::size_t k) { return base + static_cast<std::uint32_t>(i * sides + k % sides); };
    auto addFace = [&](std::initializer_list<std::uint32_t> corners, const std::vector<Vec3>& uv, std::uint32_t smoothing) {
        poly.faceSizes.push_back(static_cast<std::uint32_t>(corners.size()));
        for (std::uint32_t c : corners) poly.faceVerts.push_back(c);  // (Range insert trips GCC's -O3 null-dereference check.)
        if (hasMaterial) poly.material.push_back(material);
        if (hasSmoothing) poly.smoothing.push_back(smoothing);
        if (hasGroups) poly.groups.push_back(group);
        if (hasHidden) poly.hidden.push_back(0u);
        for (CornerMap& map : poly.maps) {
            for (std::size_t k = 0; k < corners.size(); ++k)
                map.values.push_back(map.channel == 1 ? uv[k] : (map.channel == 0 ? Vec3{1, 1, 1} : Vec3()));
        }
    };
    for (std::size_t i = 0; i + 1 < n; ++i) {
        const float v0 = along[i], v1 = along[i + 1];
        for (std::size_t k = 0; k < sides; ++k) {
            const float u0 = static_cast<float>(k) / static_cast<float>(sides);
            const float u1 = static_cast<float>(k + 1) / static_cast<float>(sides);
            addFace({vertex(i, k), vertex(i, k + 1), vertex(i + 1, k + 1), vertex(i + 1, k)},
                    {{u0, v0, 0}, {u1, v0, 0}, {u1, v1, 0}, {u0, v1, 0}}, 1u);
        }
    }
    for (std::size_t k = 0; k < sides; ++k) {
        const auto& a = ring[k];
        const auto& b = ring[(k + 1) % sides];
        const Vec3 ua{0.5f + 0.5f * a[0], 0.5f + 0.5f * a[1], 0}, ub{0.5f + 0.5f * b[0], 0.5f + 0.5f * b[1], 0};
        addFace({startCenter, vertex(0, k + 1), vertex(0, k)}, {{0.5f, 0.5f, 0}, ub, ua}, 2u);
        addFace({endCenter, vertex(n - 1, k), vertex(n - 1, k + 1)}, {{0.5f, 0.5f, 0}, ua, ub}, 2u);
    }
    return poly.valid(error);
}

void groupProfileTargets(const Mesh& mesh, const std::vector<std::int32_t>& faceGroups, std::int32_t group, int axis,
                         const ProfileCurve& profile, std::vector<std::uint32_t>& vertices, std::vector<Vec3>& targets) {
    vertices.clear();
    targets.clear();
    if (faceGroups.size() != mesh.faceCount() || axis < 0 || axis > 2) return;
    for (std::uint32_t v = 0; v < mesh.vertexCount(); ++v) {
        bool all = mesh.vertexFaces(v).size() > 0;
        for (std::uint32_t f : mesh.vertexFaces(v)) all = all && faceGroups[f] == group;
        if (all) vertices.push_back(v);
    }
    if (vertices.empty()) return;
    auto coord = [axis](const Vec3& p) { return axis == 0 ? p.x : (axis == 1 ? p.y : p.z); };
    float lo = kInfinity, hi = -kInfinity;
    Vec3 center;
    for (std::uint32_t v : vertices) {
        lo = std::min(lo, coord(mesh.position(v)));
        hi = std::max(hi, coord(mesh.position(v)));
        center += mesh.position(v);
    }
    center = center / static_cast<float>(vertices.size());
    const float span = hi - lo;
    for (std::uint32_t v : vertices) {
        const Vec3& p = mesh.position(v);
        const float t = span > 0.0f ? (coord(p) - lo) / span : 0.5f;
        Vec3 radial = p - center;
        if (axis == 0) radial.x = 0.0f;
        if (axis == 1) radial.y = 0.0f;
        if (axis == 2) radial.z = 0.0f;
        targets.push_back(p + radial * (std::max(0.0f, profile.evaluate(t)) - 1.0f));
    }
}

}  // namespace sculpt
