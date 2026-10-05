#include <algorithm>

#include "mesh_gen.h"
#include "sculpt/session.h"
#include "test_framework.h"

using namespace sculpt;

namespace {

Dab dabAt(const Vec3& c, float radius, const Vec3& viewDir) {
    Dab d;
    d.center = c;
    d.radius = radius;
    d.viewDir = viewDir;
    return d;
}

// Simulates a stroke across the top of a sphere, ray-casting every dab like the host does.
void strokeAcrossSphere(SculptSession& session, BrushType type, int dabs) {
    BrushSettings s;
    s.type = type;
    s.strength = 0.8f;
    session.beginStroke();
    for (int i = 0; i < dabs; ++i) {
        const float x = -6.0f + 12.0f * static_cast<float>(i) / static_cast<float>(dabs);
        RayHit hit;
        if (!session.raycast(Ray{{x, 0.5f, 50.0f}, {0, 0, -1}}, hit)) continue;
        session.applyDab(s, dabAt(hit.position, 3.0f, {0, 0, -1}));
    }
}

}  // namespace

TEST_CASE(session_build_failure_leaves_invalid) {
    SculptSession session;
    std::string error;
    CHECK(!session.build(MeshInput{}, &error));
    CHECK(!error.empty());
    CHECK(!session.valid());
    RayHit hit;
    CHECK(!session.raycast(Ray{{0, 0, 5}, {0, 0, -1}}, hit));
    CHECK_EQ(session.applyDab(BrushSettings{}, Dab{}), 0u);
    CHECK(session.endStroke().empty());
}

TEST_CASE(session_undo_redo_is_exact) {
    SculptSession session;
    REQUIRE(session.build(sctest::makeUvSphere(48, 64, 10.0f)));
    const std::vector<Vec3> original = session.mesh().positions();
    const std::vector<Vec3> originalNormals = session.mesh().normals();

    strokeAcrossSphere(session, BrushType::Sculpt, 40);
    REQUIRE(session.strokeActive());
    StrokeDelta delta = session.endStroke();
    CHECK(!session.strokeActive());
    REQUIRE(!delta.empty());
    REQUIRE(delta.consistent());
    const std::vector<Vec3> sculpted = session.mesh().positions();
    CHECK(sculpted != original);
    CHECK(session.bvh().validate(session.mesh()));

    REQUIRE(session.applyDelta(delta, /*useBefore=*/true));
    CHECK(session.mesh().positions() == original);
    CHECK(session.mesh().normals() == originalNormals);
    CHECK(session.bvh().validate(session.mesh()));

    REQUIRE(session.applyDelta(delta, /*useBefore=*/false));
    CHECK(session.mesh().positions() == sculpted);
}

TEST_CASE(session_cancel_restores_exactly) {
    SculptSession session;
    REQUIRE(session.build(sctest::makeUvSphere(32, 40, 10.0f)));
    const std::vector<Vec3> original = session.mesh().positions();
    strokeAcrossSphere(session, BrushType::Inflate, 25);
    CHECK(session.mesh().positions() != original);
    session.cancelStroke();
    CHECK(!session.strokeActive());
    CHECK(session.mesh().positions() == original);
    CHECK(session.bvh().validate(session.mesh()));
    CHECK(session.endStroke().empty());
}

TEST_CASE(session_dirty_tracking) {
    SculptSession session;
    REQUIRE(session.build(sctest::makeUvSphere(32, 40, 10.0f)));
    CHECK(session.dirtyVertices().empty());
    strokeAcrossSphere(session, BrushType::Sculpt, 10);
    StrokeDelta delta = session.endStroke();
    std::vector<std::uint32_t> dirty = session.dirtyVertices();
    std::sort(dirty.begin(), dirty.end());
    std::vector<std::uint32_t> touched = delta.vertices;
    std::sort(touched.begin(), touched.end());
    CHECK(dirty == touched);
    session.clearDirty();
    CHECK(session.dirtyVertices().empty());
    REQUIRE(session.applyDelta(delta, true));
    CHECK_EQ(session.dirtyVertices().size(), delta.vertices.size());
}

TEST_CASE(session_rejects_bad_deltas) {
    SculptSession session;
    REQUIRE(session.build(sctest::makeCube()));
    StrokeDelta bad;
    bad.vertices = {99};
    bad.before = {{0, 0, 0}};
    bad.after = {{0, 0, 0}};
    CHECK(!session.applyDelta(bad, true));
    StrokeDelta inconsistent;
    inconsistent.vertices = {1, 2};
    inconsistent.before = {{0, 0, 0}};
    CHECK(!session.applyDelta(inconsistent, true));
    StrokeDelta nan;
    nan.vertices = {1};
    nan.before = {{0, std::nanf(""), 0}};
    nan.after = nan.before;
    CHECK(!session.applyDelta(nan, true));
    // Nothing was partially applied.
    CHECK(session.mesh().position(1) == (Vec3{1, -1, -1}));
    // Deltas cannot be applied in the middle of a stroke.
    StrokeDelta ok;
    ok.vertices = {1};
    ok.before = {{1, -1, -1}};
    ok.after = {{2, -1, -1}};
    session.beginStroke();
    CHECK(!session.applyDelta(ok, false));
    session.cancelStroke();
    CHECK(session.applyDelta(ok, false));
}

TEST_CASE(session_set_positions) {
    SculptSession session;
    REQUIRE(session.build(sctest::makeUvSphere(16, 20, 1.0f)));
    std::vector<Vec3> scaled = session.mesh().positions();
    for (Vec3& p : scaled) p *= 3.0f;
    REQUIRE(session.setPositions(scaled));
    CHECK(session.mesh().positions() == scaled);
    CHECK(session.bvh().validate(session.mesh()));
    RayHit hit;
    REQUIRE(session.raycast(Ray{{0, 0, 50}, {0, 0, -1}}, hit));
    CHECK_NEAR(hit.position.z, 3.0f, 1e-4f);
    scaled.pop_back();
    CHECK(!session.setPositions(scaled));
}

TEST_CASE(session_bvh_rebuilds_after_heavy_deformation) {
    SculptSession session;
    REQUIRE(session.build(sctest::makeUvSphere(32, 40, 10.0f)));
    BrushSettings s;
    s.type = BrushType::Inflate;
    s.strength = 1.0f;
    s.useFalloff = false;
    s.backfaceCull = false;
    session.beginStroke();
    for (int i = 0; i < 200; ++i) session.applyDab(s, dabAt({0, 0, 0}, 1000.0f, {0, 0, -1}));
    CHECK(session.bvh().degradation() > SculptSession::kRebuildThreshold);
    session.endStroke();
    CHECK_NEAR(session.bvh().degradation(), 1.0f, 1e-6f);  // Freshly rebuilt.
    CHECK(session.bvh().validate(session.mesh()));
    CHECK(session.memoryBytes() > 0u);
}
