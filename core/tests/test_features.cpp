// Light coverage of phase 4-6 engine features.
#include <algorithm>

#include "mesh_gen.h"
#include "sculpt/session.h"
#include "sculpt/symmetry.h"
#include "test_framework.h"

using namespace sculpt;

namespace {

Dab topDab(float x, float radius) {
    Dab d;
    d.center = {x, 0.0f, 10.0f};
    d.radius = radius;
    d.viewDir = {0, 0, -1};
    d.grabDelta = {0.0f, 0.0f, 0.5f};
    return d;
}

}  // namespace

TEST_CASE(features_every_available_brush_moves_and_undoes) {
    for (int b = 0; b < static_cast<int>(BrushType::Count); ++b) {
        const BrushType type = static_cast<BrushType>(b);
        if (!isGeometryBrush(type)) continue;
        SculptSession session;
        REQUIRE(session.build(sctest::makeUvSphere(60, 80, 10.0f)));
        if (type == BrushType::SmoothGroupBorder) {
            std::vector<std::uint64_t> keys(session.mesh().faceCount());
            for (std::uint32_t f = 0; f < keys.size(); ++f) keys[f] = session.mesh().faceCenter(f).x > 0.0f ? 1u : 2u;
            session.autoGroups(AutoGroupMode::MaterialIDs, &keys);
        }
        if (type == BrushType::Revert) {  // Revert needs a snapshot to pull toward.
            std::vector<Vec3> reference = session.mesh().positions();
            for (Vec3& p : reference) p.z += 1.0f;
            session.setReferencePositions(reference);
        }
        const std::vector<Vec3> original = session.mesh().positions();
        BrushSettings s;
        s.type = type;
        s.strength = 0.8f;
        s.layerMode = type == BrushType::Sculpt;
        session.beginStroke();
        std::size_t moved = 0;
        for (int i = 0; i < 6; ++i) moved += session.applyDab(s, topDab(-0.5f + 0.2f * static_cast<float>(i), 4.0f));
        const StrokeDelta delta = session.endStroke();
        if (moved == 0) sctest::fail(__FILE__, __LINE__, std::string("brush did nothing: ") + brushName(type));
        CHECK(session.bvh().validate(session.mesh()));
        REQUIRE(session.applyDelta(delta, true));
        if (session.mesh().positions() != original)
            sctest::fail(__FILE__, __LINE__, std::string("undo not exact: ") + brushName(type));
    }
}

TEST_CASE(features_mask_protects_and_ops_undo) {
    SculptSession session;
    REQUIRE(session.build(sctest::makeGrid(20, 20.0f)));
    StrokeDelta fill = session.maskFill(true);
    CHECK(session.hasMask());
    CHECK_EQ(fill.maskVertices.size(), static_cast<std::size_t>(session.mesh().vertexCount()));
    BrushSettings s;
    session.beginStroke();
    Dab d = topDab(0.0f, 4.0f);
    d.center.z = 0.0f;
    CHECK_EQ(session.applyDab(s, d), 0u);  // Fully masked: nothing moves.
    session.endStroke();

    const std::vector<float> before = session.mask();
    StrokeDelta inv = session.maskInvert();
    CHECK(!session.hasMask());
    StrokeDelta grow = session.maskGrow();
    CHECK(grow.empty());
    REQUIRE(session.applyDelta(inv, true));
    CHECK(session.mask() == before);

    // Mask painting with Alt erases.
    session.maskClear();
    BrushSettings maskBrush;
    maskBrush.type = BrushType::MaskPaint;
    session.beginStroke();
    CHECK(session.applyDab(maskBrush, d) > 0u);
    StrokeDelta painted = session.endStroke();
    CHECK(!painted.maskVertices.empty());
    CHECK(session.hasMask());
    StrokeDelta blur = session.maskBlur(2);
    CHECK(!blur.empty());
    StrokeDelta cavity = session.maskByCavity(0.5f);
    StrokeDelta ao = session.maskByAO(0.5f, 8);
    (void)cavity;
    (void)ao;
}

TEST_CASE(features_groups_and_visibility) {
    SculptSession session;
    REQUIRE(session.build(sctest::makeUvSphere(20, 24, 5.0f)));
    StrokeDelta elements = session.autoGroups(AutoGroupMode::Elements);
    CHECK(!elements.empty());  // One element: every face moves from group 0 to group 1.
    CHECK(std::all_of(session.faceGroups().begin(), session.faceGroups().end(), [](std::int32_t g) { return g == 1; }));
    session.maskSelect([](const Vec3& p) { return p.z > 2.0f; }, false);
    StrokeDelta fromMask = session.groupFromMask();
    REQUIRE(!fromMask.empty());
    const std::int32_t cap = session.faceGroups()[0];  // North pole fan.
    StrokeDelta isolate = session.isolateOrHideGroup(cap);
    REQUIRE(isolate.hasHidden);
    CHECK(session.anyHidden());
    RayHit hit;
    CHECK(!session.raycast(Ray{{0, 0, -50}, {0, 0, 1}}, hit, true));  // Bottom is hidden.
    StrokeDelta hide = session.isolateOrHideGroup(cap);  // Second click hides it.
    CHECK(hide.hasHidden);
    StrokeDelta show = session.showAll();
    CHECK(!session.anyHidden());
    REQUIRE(session.applyDelta(show, true));
    CHECK(session.anyHidden());
    CHECK(!session.groupBorderVertices().empty());

    // Angle groups split at hard edges: a cube gives one group per side.
    SculptSession cube;
    REQUIRE(cube.build(sctest::makeCube()));
    cube.autoGroups(AutoGroupMode::Angle, nullptr, 30.0f);
    std::vector<std::int32_t> ids = cube.faceGroups();
    std::sort(ids.begin(), ids.end());
    CHECK_EQ(static_cast<std::size_t>(std::unique(ids.begin(), ids.end()) - ids.begin()), 6u);
}

TEST_CASE(features_symmetry) {
    SymmetrySettings settings;
    settings.mirrorX = true;
    CHECK_EQ(symmetryTransforms(settings).size(), 2u);
    settings.radial = true;
    settings.radialCount = 4;
    settings.mirrorX = false;
    CHECK_EQ(symmetryTransforms(settings).size(), 4u);

    SculptSession session;
    REQUIRE(session.build(sctest::makeGrid(20, 20.0f)));
    SymmetrySettings mirror;
    mirror.mirrorX = true;
    session.setSymmetry(symmetryTransforms(mirror));
    BrushSettings s;
    Dab d;
    d.center = {5.0f, 0.0f, 0.0f};
    d.radius = 2.0f;
    session.beginStroke();
    session.applyDab(s, d);
    session.endStroke();
    const std::uint32_t right = sctest::gridIndex(20, 15, 10);  // (5, 0)
    const std::uint32_t left = sctest::gridIndex(20, 5, 10);    // (-5, 0)
    CHECK(session.mesh().position(right).z > 0.0f);
    CHECK_NEAR(session.mesh().position(left).z, session.mesh().position(right).z, 1e-5f);
}
