#include <algorithm>

#include "mesh_gen.h"
#include "sculpt/bvh.h"
#include "test_framework.h"

using namespace sculpt;

namespace {

bool bruteRaycast(const Mesh& mesh, const Ray& ray, bool cull, RayHit& hit) {
    bool found = false;
    float best = kInfinity;
    for (std::uint32_t t = 0; t < mesh.triangleCount(); ++t) {
        const auto tri = mesh.triangle(t);
        float th, u, v;
        if (intersectRayTriangle(ray, mesh.position(tri[0]), mesh.position(tri[1]), mesh.position(tri[2]), cull, best,
                                 th, u, v)) {
            best = th;
            hit.t = th;
            hit.triangle = t;
            found = true;
        }
    }
    return found;
}

std::vector<std::uint32_t> bruteGather(const Mesh& mesh, const Vec3& c, float r) {
    std::vector<bool> used(mesh.vertexCount(), false);
    for (std::uint32_t t = 0; t < mesh.triangleCount(); ++t)
        for (std::uint32_t v : mesh.triangle(t)) used[v] = true;
    std::vector<std::uint32_t> out;
    for (std::uint32_t v = 0; v < mesh.vertexCount(); ++v)
        if (used[v] && lengthSq(mesh.position(v) - c) <= r * r) out.push_back(v);
    return out;
}

Mesh noisySphere(std::uint64_t seed) {
    MeshInput in = sctest::makeUvSphere(40, 48, 10.0f);
    sctest::Rng rng(seed);
    for (Vec3& p : in.positions) p += Vec3{rng.uniform(-0.2f, 0.2f), rng.uniform(-0.2f, 0.2f), rng.uniform(-0.2f, 0.2f)};
    Mesh mesh;
    mesh.build(in);
    return mesh;
}

Ray randomRayTowardOrigin(sctest::Rng& rng) {
    const Vec3 origin{rng.uniform(-30, 30), rng.uniform(-30, 30), rng.uniform(-30, 30)};
    const Vec3 target{rng.uniform(-12, 12), rng.uniform(-12, 12), rng.uniform(-12, 12)};
    return {origin, target - origin};
}

void checkAgainstBruteForce(const Mesh& mesh, const Bvh& bvh, std::uint64_t seed) {
    sctest::Rng rng(seed);
    for (int i = 0; i < 400; ++i) {
        const Ray ray = randomRayTowardOrigin(rng);
        for (bool cull : {false, true}) {
            RayHit a, b;
            const bool hitA = bvh.raycast(mesh, ray, a, cull);
            const bool hitB = bruteRaycast(mesh, ray, cull, b);
            CHECK_EQ(hitA, hitB);
            if (hitA && hitB) CHECK_NEAR(a.t, b.t, 1e-5f * std::max(1.0f, b.t));
        }
    }
    for (int i = 0; i < 100; ++i) {
        const Vec3 c{rng.uniform(-12, 12), rng.uniform(-12, 12), rng.uniform(-12, 12)};
        const float r = rng.uniform(0.0f, 6.0f);
        std::vector<std::uint32_t> got;
        bvh.gatherVertices(mesh, c, r, got);
        std::vector<std::uint32_t> sorted = got;
        std::sort(sorted.begin(), sorted.end());
        CHECK(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());  // No duplicates.
        CHECK(sorted == bruteGather(mesh, c, r));
    }
}

}  // namespace

TEST_CASE(bvh_matches_brute_force) {
    const Mesh mesh = noisySphere(11);
    Bvh bvh;
    bvh.build(mesh);
    REQUIRE(bvh.validate(mesh));
    checkAgainstBruteForce(mesh, bvh, 99);
}

TEST_CASE(bvh_hit_details) {
    Mesh mesh;
    REQUIRE(mesh.build(sctest::makeGrid(10, 10.0f)));
    Bvh bvh;
    bvh.build(mesh);
    RayHit hit;
    REQUIRE(bvh.raycast(mesh, Ray{{1.3f, -2.1f, 10.0f}, {0, 0, -2}}, hit));
    CHECK_NEAR(hit.t, 5.0f, 1e-5f);  // |dir| = 2.
    CHECK_NEAR(hit.position.x, 1.3f, 1e-5f);
    CHECK_NEAR(hit.position.y, -2.1f, 1e-5f);
    CHECK_NEAR(hit.position.z, 0.0f, 1e-6f);
    CHECK(hit.geometricNormal == (Vec3{0, 0, 1}));
    // From below with culling the grid is invisible.
    CHECK(!bvh.raycast(mesh, Ray{{1.3f, -2.1f, -10.0f}, {0, 0, 1}}, hit, true));
    CHECK(bvh.raycast(mesh, Ray{{1.3f, -2.1f, -10.0f}, {0, 0, 1}}, hit, false));
    // Off the grid.
    CHECK(!bvh.raycast(mesh, Ray{{50, 0, 10}, {0, 0, -1}}, hit));
}

TEST_CASE(bvh_refit_after_deformation) {
    MeshInput in = sctest::makeUvSphere(40, 48, 10.0f);
    Mesh mesh;
    REQUIRE(mesh.build(in));
    Bvh bvh;
    bvh.build(mesh);
    CHECK_NEAR(bvh.degradation(), 1.0f, 1e-6f);

    // Push a cap of vertices outward, then refit only the touched triangles.
    std::vector<std::uint32_t> moved;
    for (std::uint32_t v = 0; v < mesh.vertexCount(); ++v) {
        if (mesh.position(v).z > 6.0f) {
            mesh.setPosition(v, mesh.position(v) * 1.8f);
            moved.push_back(v);
        }
    }
    REQUIRE(!moved.empty());
    mesh.updateNormals(moved);
    std::vector<std::uint32_t> tris;
    mesh.collectTriangles(moved, tris);
    bvh.refit(mesh, tris);
    REQUIRE(bvh.validate(mesh));
    CHECK(bvh.degradation() > 1.0f);
    checkAgainstBruteForce(mesh, bvh, 5);

    // Refitting everything gives the same tight boxes.
    Bvh full = bvh;
    full.refitAll(mesh);
    CHECK_NEAR(full.degradation(), bvh.degradation(), 1e-5f);
}

TEST_CASE(bvh_degenerate_inputs) {
    // All triangles share one centroid: build must still terminate and be valid.
    MeshInput in;
    for (int i = 0; i < 60; ++i) in.positions.push_back({0, 0, 0});
    for (std::uint32_t i = 0; i + 2 < 60; i += 3) {
        in.faceSizes.push_back(3);
        in.faceVerts.insert(in.faceVerts.end(), {i, i + 1, i + 2});
    }
    Mesh mesh;
    REQUIRE(mesh.build(in));
    Bvh bvh;
    bvh.build(mesh);
    CHECK(bvh.validate(mesh));
    RayHit hit;
    CHECK(!bvh.raycast(mesh, Ray{{0, 0, 5}, {0, 0, -1}}, hit));
    std::vector<std::uint32_t> got;
    bvh.gatherVertices(mesh, Vec3{0, 0, 0}, 0.5f, got);
    CHECK_EQ(got.size(), 60u);
    got.clear();
    bvh.gatherVertices(mesh, Vec3{0, 0, 0}, std::nanf(""), got);  // NaN radius: no crash, nothing.
    CHECK(got.empty());

    Bvh empty;
    CHECK(!empty.raycast(mesh, Ray{{0, 0, 5}, {0, 0, -1}}, hit));
    empty.gatherVertices(mesh, Vec3{}, 1.0f, got);
    CHECK(got.empty());
}

TEST_CASE(bvh_single_triangle) {
    MeshInput in;
    in.positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    in.faceSizes = {3};
    in.faceVerts = {0, 1, 2};
    Mesh mesh;
    REQUIRE(mesh.build(in));
    Bvh bvh;
    bvh.build(mesh);
    CHECK(bvh.validate(mesh));
    CHECK_EQ(bvh.nodeCount(), 1u);
    RayHit hit;
    CHECK(bvh.raycast(mesh, Ray{{0.2f, 0.2f, 1}, {0, 0, -1}}, hit));
}
