#include <algorithm>
#include <string>

#include "mesh_gen.h"
#include "sculpt/mesh.h"
#include "test_framework.h"

using namespace sculpt;

TEST_CASE(mesh_cube_topology) {
    Mesh mesh;
    std::string error;
    REQUIRE(mesh.build(sctest::makeCube(), &error));
    CHECK(error.empty());
    CHECK_EQ(mesh.vertexCount(), 8u);
    CHECK_EQ(mesh.faceCount(), 6u);
    CHECK_EQ(mesh.triangleCount(), 12u);
    for (std::uint32_t v = 0; v < 8; ++v) {
        CHECK_EQ(mesh.neighbors(v).size(), 3u);  // Cube corners have 3 edges.
        CHECK(!mesh.isBorder(v));
        // Each corner belongs to 3 quads, i.e. 3 to 6 of their triangles.
        CHECK(mesh.vertexTriangles(v).size() >= 3u && mesh.vertexTriangles(v).size() <= 6u);
        // Normals point away from the centre.
        CHECK(dot(mesh.normal(v), mesh.position(v)) > 0.0f);
        CHECK_NEAR(length(mesh.normal(v)), 1.0f, 1e-5f);
        // Adjacency excludes the vertex itself and contains no duplicates.
        std::vector<std::uint32_t> ring(mesh.neighbors(v).begin(), mesh.neighbors(v).end());
        CHECK(std::find(ring.begin(), ring.end(), v) == ring.end());
        std::sort(ring.begin(), ring.end());
        CHECK(std::adjacent_find(ring.begin(), ring.end()) == ring.end());
    }
}

TEST_CASE(mesh_grid_borders) {
    const std::uint32_t cells = 6;
    Mesh mesh;
    REQUIRE(mesh.build(sctest::makeGrid(cells, 6.0f)));
    for (std::uint32_t j = 0; j <= cells; ++j) {
        for (std::uint32_t i = 0; i <= cells; ++i) {
            const std::uint32_t v = sctest::gridIndex(cells, i, j);
            const bool onEdge = i == 0 || j == 0 || i == cells || j == cells;
            CHECK_EQ(mesh.isBorder(v), onEdge);
            if (onEdge) CHECK_EQ(mesh.borderNeighbors(v).size(), 2u);
            CHECK(mesh.normal(v) == (Vec3{0, 0, 1}));
        }
    }
    CHECK_EQ(mesh.neighbors(sctest::gridIndex(cells, 3, 3)).size(), 4u);  // Quad edges only, no diagonals.
    CHECK_EQ(mesh.neighbors(sctest::gridIndex(cells, 0, 0)).size(), 2u);
}

TEST_CASE(mesh_rejects_invalid_input) {
    auto expectFail = [](MeshInput in, const char* what) {
        Mesh mesh;
        std::string error;
        const bool ok = mesh.build(std::move(in), &error);
        if (ok || error.empty() || !mesh.empty()) sctest::fail(__FILE__, __LINE__, std::string("accepted: ") + what);
    };
    expectFail(MeshInput{}, "empty");
    {
        MeshInput in = sctest::makeCube();
        in.faceSizes.clear();
        in.faceVerts.clear();
        expectFail(in, "no faces");
    }
    {
        MeshInput in = sctest::makeCube();
        in.positions[3].y = std::nanf("");
        expectFail(in, "NaN position");
    }
    {
        MeshInput in = sctest::makeCube();
        in.faceVerts[5] = 99;
        expectFail(in, "index out of range");
    }
    {
        MeshInput in = sctest::makeCube();
        in.faceSizes[0] = 2;
        in.faceSizes[1] = 6;
        expectFail(in, "polygon with 2 corners");
    }
    {
        MeshInput in = sctest::makeCube();
        in.faceSizes.back() = 5;
        expectFail(in, "corner count mismatch");
    }
    {
        MeshInput in = sctest::makeCube();
        in.triVerts = {0, 1, 2, 3};
        in.triFaces = {0};
        expectFail(in, "triangle count not multiple of 3");
    }
    {
        MeshInput in = sctest::makeCube();
        in.triVerts = {0, 1, 2};
        in.triFaces = {7};
        expectFail(in, "triangle face out of range");
    }
}

TEST_CASE(mesh_explicit_triangulation) {
    MeshInput in;
    in.positions = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
    in.faceSizes = {4};
    in.faceVerts = {0, 1, 2, 3};
    in.triVerts = {0, 1, 3, 1, 2, 3};  // The other diagonal than a fan would use.
    in.triFaces = {0, 0};
    Mesh mesh;
    REQUIRE(mesh.build(in));
    CHECK_EQ(mesh.triangleCount(), 2u);
    CHECK(mesh.triangle(1) == (std::array<std::uint32_t, 3>{1, 2, 3}));
    CHECK_EQ(mesh.vertexTriangles(1).size(), 2u);
    CHECK_EQ(mesh.vertexTriangles(0).size(), 1u);
}

TEST_CASE(mesh_degenerate_polygons_are_tolerated) {
    MeshInput in;
    in.positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    in.faceSizes = {4};
    in.faceVerts = {0, 1, 1, 2};  // Repeated corner.
    Mesh mesh;
    REQUIRE(mesh.build(in));
    CHECK_EQ(mesh.neighbors(1).size(), 2u);
    for (std::uint32_t v = 0; v < 3; ++v) CHECK(isFinite(mesh.normal(v)));
}

TEST_CASE(mesh_incremental_normals_match_full_recompute) {
    Mesh incremental;
    REQUIRE(incremental.build(sctest::makeUvSphere(24, 32, 5.0f)));
    Mesh full = incremental;
    sctest::Rng rng(7);
    for (int round = 0; round < 20; ++round) {
        std::vector<std::uint32_t> moved;
        for (int k = 0; k < 15; ++k) {
            const auto v = static_cast<std::uint32_t>(rng.next() % incremental.vertexCount());
            const Vec3 p = incremental.position(v) + Vec3{rng.uniform(-0.3f, 0.3f), rng.uniform(-0.3f, 0.3f),
                                                          rng.uniform(-0.3f, 0.3f)};
            incremental.setPosition(v, p);
            full.setPosition(v, p);
            moved.push_back(v);  // Duplicates allowed.
        }
        incremental.updateNormals(moved);
        full.recomputeAllNormals();
        for (std::uint32_t v = 0; v < full.vertexCount(); ++v) {
            if (incremental.normal(v) != full.normal(v)) {
                sctest::fail(__FILE__, __LINE__, "incremental normal differs from full recompute");
                return;
            }
        }
    }
}

TEST_CASE(mesh_sphere_is_closed) {
    Mesh mesh;
    REQUIRE(mesh.build(sctest::makeUvSphere(16, 20, 1.0f)));
    for (std::uint32_t v = 0; v < mesh.vertexCount(); ++v) {
        CHECK(!mesh.isBorder(v));
        CHECK(dot(mesh.normal(v), mesh.position(v)) > 0.9f);
    }
    const Aabb box = mesh.bounds();
    CHECK_NEAR(box.hi.z, 1.0f, 1e-6f);
    CHECK_NEAR(box.lo.z, -1.0f, 1e-6f);
    CHECK(mesh.memoryBytes() > 0u);
}
