// SculptCore — small, dependency-free vector math used by the sculpt engine.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace sculpt {

constexpr float kInfinity = std::numeric_limits<float>::infinity();

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    constexpr Vec3() = default;
    constexpr Vec3(float px, float py, float pz) : x(px), y(py), z(pz) {}

    constexpr float operator[](int axis) const { return axis == 0 ? x : (axis == 1 ? y : z); }

    constexpr Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    constexpr Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    constexpr Vec3 operator-() const { return {-x, -y, -z}; }
    constexpr Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    constexpr Vec3 operator/(float s) const { return {x / s, y / s, z / s}; }

    Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
    Vec3& operator-=(const Vec3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
    Vec3& operator*=(float s) { x *= s; y *= s; z *= s; return *this; }

    constexpr bool operator==(const Vec3& o) const { return x == o.x && y == o.y && z == o.z; }
    constexpr bool operator!=(const Vec3& o) const { return !(*this == o); }
};

constexpr Vec3 operator*(float s, const Vec3& v) { return v * s; }

constexpr float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

constexpr Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

constexpr float lengthSq(const Vec3& v) { return dot(v, v); }
inline float length(const Vec3& v) { return std::sqrt(lengthSq(v)); }

inline bool isFinite(float f) { return std::isfinite(f); }
inline bool isFinite(const Vec3& v) { return isFinite(v.x) && isFinite(v.y) && isFinite(v.z); }

// Returns the unit vector, or the zero vector when the input is too small
// (or not finite) to normalise reliably. Callers decide on a fallback.
inline Vec3 normalizedOrZero(const Vec3& v) {
    const float len = length(v);
    if (!(len > 1e-20f) || !isFinite(len)) return {};
    return v / len;
}

inline Vec3 vmin(const Vec3& a, const Vec3& b) {
    return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)};
}
inline Vec3 vmax(const Vec3& a, const Vec3& b) {
    return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)};
}

constexpr Vec3 lerp(const Vec3& a, const Vec3& b, float t) { return a + (b - a) * t; }

inline float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

// Axis-aligned bounding box. A default-constructed box is empty.
struct Aabb {
    Vec3 lo{kInfinity, kInfinity, kInfinity};
    Vec3 hi{-kInfinity, -kInfinity, -kInfinity};

    bool empty() const { return lo.x > hi.x || lo.y > hi.y || lo.z > hi.z; }
    void expand(const Vec3& p) { lo = vmin(lo, p); hi = vmax(hi, p); }
    void expand(const Aabb& b) { lo = vmin(lo, b.lo); hi = vmax(hi, b.hi); }
    Vec3 center() const { return (lo + hi) * 0.5f; }
    Vec3 extent() const { return hi - lo; }

    int longestAxis() const {
        const Vec3 e = extent();
        if (e.x >= e.y && e.x >= e.z) return 0;
        return e.y >= e.z ? 1 : 2;
    }

    float surfaceArea() const {
        if (empty()) return 0.0f;
        const Vec3 e = extent();
        return 2.0f * (e.x * e.y + e.y * e.z + e.z * e.x);
    }

    // Squared distance from p to the box (0 when inside).
    float distanceSq(const Vec3& p) const {
        const float dx = std::max({lo.x - p.x, 0.0f, p.x - hi.x});
        const float dy = std::max({lo.y - p.y, 0.0f, p.y - hi.y});
        const float dz = std::max({lo.z - p.z, 0.0f, p.z - hi.z});
        return dx * dx + dy * dy + dz * dz;
    }

    bool operator==(const Aabb& o) const { return lo == o.lo && hi == o.hi; }
};

// 3x3 matrix stored as rows; used for symmetry (reflections, rotations).
struct Mat3 {
    Vec3 r0{1, 0, 0};
    Vec3 r1{0, 1, 0};
    Vec3 r2{0, 0, 1};

    static Mat3 identity() { return {}; }
    static Mat3 diagonal(const Vec3& d) { return {{d.x, 0, 0}, {0, d.y, 0}, {0, 0, d.z}}; }
    // Right-handed rotation by `angle` radians about the X (0), Y (1) or Z (2) axis.
    static Mat3 rotation(int axis, float angle) {
        const float c = std::cos(angle), s = std::sin(angle);
        if (axis == 0) return {{1, 0, 0}, {0, c, -s}, {0, s, c}};
        if (axis == 1) return {{c, 0, s}, {0, 1, 0}, {-s, 0, c}};
        return {{c, -s, 0}, {s, c, 0}, {0, 0, 1}};
    }

    Vec3 operator*(const Vec3& v) const { return {dot(r0, v), dot(r1, v), dot(r2, v)}; }
    Mat3 operator*(const Mat3& o) const {
        const Vec3 c0{o.r0.x, o.r1.x, o.r2.x}, c1{o.r0.y, o.r1.y, o.r2.y}, c2{o.r0.z, o.r1.z, o.r2.z};
        return {{dot(r0, c0), dot(r0, c1), dot(r0, c2)},
                {dot(r1, c0), dot(r1, c1), dot(r1, c2)},
                {dot(r2, c0), dot(r2, c1), dot(r2, c2)}};
    }
    float determinant() const { return dot(r0, cross(r1, r2)); }
    bool nearlyEquals(const Mat3& o, float eps = 1e-4f) const {
        const Vec3 d0 = r0 - o.r0, d1 = r1 - o.r1, d2 = r2 - o.r2;
        return lengthSq(d0) + lengthSq(d1) + lengthSq(d2) <= eps * eps;
    }
};

struct Ray {
    Vec3 origin;
    Vec3 dir;  // Does not need to be unit length; hit distances are in units of |dir|.
};

// Slab test. invDir is 1/dir per component (may contain infinities).
// Returns true when the ray enters the box before tMax; tEnter receives the entry distance.
inline bool intersectRayAabb(const Vec3& origin, const Vec3& invDir, const Aabb& box, float tMax,
                             float& tEnter) {
    float t0 = 0.0f;
    float t1 = tMax;
    for (int axis = 0; axis < 3; ++axis) {
        const float o = origin[axis];
        const float inv = invDir[axis];
        float tNear = (box.lo[axis] - o) * inv;
        float tFar = (box.hi[axis] - o) * inv;
        // 0 * inf produces NaN when the origin lies exactly on a slab plane of a
        // parallel ray; treat that case as "inside the slab".
        if (std::isnan(tNear)) tNear = -kInfinity;
        if (std::isnan(tFar)) tFar = kInfinity;
        if (tNear > tFar) std::swap(tNear, tFar);
        t0 = tNear > t0 ? tNear : t0;
        t1 = tFar < t1 ? tFar : t1;
        if (t0 > t1) return false;
    }
    tEnter = t0;
    return true;
}

// Möller–Trumbore ray/triangle test. Returns true for hits with t in (0, tMax).
// When cullBackfaces is set, triangles whose winding faces away from the ray are ignored.
inline bool intersectRayTriangle(const Ray& ray, const Vec3& a, const Vec3& b, const Vec3& c,
                                 bool cullBackfaces, float tMax, float& t, float& u, float& v) {
    const Vec3 e1 = b - a;
    const Vec3 e2 = c - a;
    const Vec3 p = cross(ray.dir, e2);
    const float det = dot(e1, p);
    const float eps = 1e-12f;
    if (cullBackfaces) {
        if (det <= eps) return false;
    } else if (std::fabs(det) <= eps) {
        return false;
    }
    const float invDet = 1.0f / det;
    const Vec3 s = ray.origin - a;
    u = dot(s, p) * invDet;
    if (u < 0.0f || u > 1.0f) return false;
    const Vec3 q = cross(s, e1);
    v = dot(ray.dir, q) * invDet;
    if (v < 0.0f || u + v > 1.0f) return false;
    t = dot(e2, q) * invDet;
    return t > 0.0f && t < tMax;
}

}  // namespace sculpt
