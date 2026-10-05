#include <cmath>

#include "mesh_gen.h"
#include "sculpt/deform.h"
#include "test_framework.h"

using namespace sculpt;

TEST_CASE(profile_curve_evaluates_and_round_trips) {
    ProfileCurve curve;
    CHECK_NEAR(curve.evaluate(0.5f), 1.0f, 1e-4f);
    const int mid = curve.addPoint(0.5f, 0.2f);
    CHECK_EQ(mid, 1);
    CHECK_NEAR(curve.evaluate(0.5f), 0.2f, 0.02f);
    CHECK(curve.evaluate(0.0f) > 0.9f);
    ProfileCurve copy;
    REQUIRE(copy.fromText(curve.toText()));
    CHECK_EQ(copy.points().size(), 3u);
    CHECK_NEAR(copy.evaluate(0.3f), curve.evaluate(0.3f), 1e-3f);
    copy.removePoint(0);  // The ends stay.
    CHECK_EQ(copy.points().size(), 3u);
}

TEST_CASE(curve_tube_is_closed) {
    const std::vector<Vec3> controls = {{0, 0, 0}, {5, 0, 1}, {10, 3, 0}};
    const std::vector<Vec3> curve = sampleCurve(controls, 1.0f);
    CHECK(curve.size() > 8);
    CHECK_NEAR(length(curve.back() - controls.back()), 0.0f, 1e-4f);
    PolyData poly;
    poly.maps.push_back({1, {}});
    TubeSettings settings;
    settings.sides = 8;
    settings.radius = 0.5f;
    REQUIRE(appendTube(poly, curve, settings, 3));
    CHECK(poly.valid());
    EdgeTable edges;
    edges.build(poly);
    for (std::uint32_t uses : edges.faceUses) CHECK_EQ(uses, 2u);
    CHECK_EQ(poly.maps[0].values.size(), poly.faceVerts.size());
}

TEST_CASE(pose_rotates_the_handle_side) {
    Mesh mesh;
    REQUIRE(mesh.build(sctest::makeGrid(10, 10.0f)));
    PoseSettings settings;
    std::vector<float> weights;
    const Vec3 pivot{0, 0, 0}, handle{4, 0, 0};
    poseWeights(mesh, pivot, handle, 0.5f, settings, weights);
    std::vector<std::uint32_t> vertices;
    std::vector<Vec3> original;
    for (std::uint32_t v = 0; v < mesh.vertexCount(); ++v) {
        vertices.push_back(v);
        original.push_back(mesh.position(v));
    }
    std::vector<Vec3> out;
    posePositions(vertices, original, weights, pivot, handle, Vec3{0, 0, 4}, 0.0f, settings, out);
    for (std::uint32_t v = 0; v < mesh.vertexCount(); ++v) {
        if (original[v].x < -2.5f) CHECK_NEAR(length(out[v] - original[v]), 0.0f, 1e-4f);
        if (original[v].x > 2.0f && weights[v] > 0.99f) CHECK(out[v].z > 1.0f);
    }
}

TEST_CASE(cloth_moves_under_the_brush) {
    Mesh mesh;
    REQUIRE(mesh.build(sctest::makeGrid(20, 10.0f)));
    ClothSim cloth;
    ClothSettings settings;
    REQUIRE(cloth.begin(mesh, Vec3{0, 0, 0}, 2.0f, settings));
    std::vector<std::uint32_t> vertices;
    std::vector<Vec3> targets;
    cloth.step(Vec3{0, 0, 0}, 2.0f, Vec3{0, 0, 1}, false, vertices, targets);
    REQUIRE(!vertices.empty());
    float lift = 0.0f;
    for (const Vec3& t : targets) lift = std::max(lift, t.z);
    CHECK(lift > 0.2f);
}
