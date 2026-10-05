#include "sculpt/math.h"
#include "test_framework.h"

using namespace sculpt;

TEST_CASE(math_vector_basics) {
    const Vec3 a{1, 2, 3}, b{4, 5, 6};
    CHECK_EQ(dot(a, b), 32.0f);
    CHECK(cross(Vec3{1, 0, 0}, Vec3{0, 1, 0}) == (Vec3{0, 0, 1}));
    CHECK_NEAR(length(Vec3{3, 4, 0}), 5.0f, 1e-6f);
    CHECK(normalizedOrZero(Vec3{}) == Vec3{});
    CHECK(normalizedOrZero(Vec3{kInfinity, 0, 0}) == Vec3{});
    CHECK_NEAR(length(normalizedOrZero(Vec3{1e-10f, 2e-10f, 0})), 1.0f, 1e-5f);
    CHECK(!isFinite(Vec3{0, std::nanf(""), 0}));
}

TEST_CASE(math_aabb) {
    Aabb box;
    CHECK(box.empty());
    CHECK_EQ(box.surfaceArea(), 0.0f);
    box.expand(Vec3{-1, -1, -1});
    box.expand(Vec3{1, 2, 3});
    CHECK(!box.empty());
    CHECK_EQ(box.longestAxis(), 2);
    CHECK_EQ(box.distanceSq(Vec3{0, 0, 0}), 0.0f);
    CHECK_NEAR(box.distanceSq(Vec3{3, 0, 0}), 4.0f, 1e-6f);
    CHECK_NEAR(box.surfaceArea(), 2.0f * (2 * 3 + 3 * 4 + 4 * 2), 1e-5f);
}

TEST_CASE(math_ray_aabb) {
    Aabb box;
    box.expand(Vec3{-1, -1, -1});
    box.expand(Vec3{1, 1, 1});
    auto inv = [](const Vec3& d) { return Vec3{1.0f / d.x, 1.0f / d.y, 1.0f / d.z}; };
    float t = 0.0f;
    CHECK(intersectRayAabb(Vec3{-5, 0, 0}, inv(Vec3{1, 0, 0}), box, kInfinity, t));
    CHECK_NEAR(t, 4.0f, 1e-6f);
    CHECK(!intersectRayAabb(Vec3{-5, 3, 0}, inv(Vec3{1, 0, 0}), box, kInfinity, t));
    CHECK(!intersectRayAabb(Vec3{-5, 0, 0}, inv(Vec3{1, 0, 0}), box, 3.0f, t));  // Beyond tMax.
    CHECK(intersectRayAabb(Vec3{0, 0, 0}, inv(Vec3{0, 0, 1}), box, kInfinity, t));  // Origin inside.
    CHECK_EQ(t, 0.0f);
    // Axis-parallel ray lying exactly on a slab plane (0 * inf case).
    CHECK(intersectRayAabb(Vec3{-5, 1, 0}, inv(Vec3{1, 0, 0}), box, kInfinity, t));
}

TEST_CASE(math_ray_triangle) {
    const Vec3 a{0, 0, 0}, b{1, 0, 0}, c{0, 1, 0};  // Normal +Z.
    float t = 0, u = 0, v = 0;
    const Ray down{{0.25f, 0.25f, 5}, {0, 0, -1}};  // Hits the front face.
    CHECK(intersectRayTriangle(down, a, b, c, true, kInfinity, t, u, v));
    CHECK_NEAR(t, 5.0f, 1e-6f);
    CHECK_NEAR(u, 0.25f, 1e-6f);
    CHECK_NEAR(v, 0.25f, 1e-6f);

    const Ray up{{0.25f, 0.25f, -5}, {0, 0, 1}};  // Hits the back face.
    CHECK(intersectRayTriangle(up, a, b, c, false, kInfinity, t, u, v));
    CHECK(!intersectRayTriangle(up, a, b, c, true, kInfinity, t, u, v));

    const Ray miss{{2, 2, 5}, {0, 0, -1}};
    CHECK(!intersectRayTriangle(miss, a, b, c, false, kInfinity, t, u, v));
    const Ray behind{{0.25f, 0.25f, -5}, {0, 0, -1}};
    CHECK(!intersectRayTriangle(behind, a, b, c, false, kInfinity, t, u, v));
    CHECK(!intersectRayTriangle(down, a, b, c, false, 4.0f, t, u, v));  // Beyond tMax.
    // Degenerate (zero-area) triangle never reports a hit.
    CHECK(!intersectRayTriangle(down, a, b, b, false, kInfinity, t, u, v));
}
