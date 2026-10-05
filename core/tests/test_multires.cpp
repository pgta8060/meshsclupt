#include "mesh_gen.h"
#include "sculpt/half.h"
#include "sculpt/multires.h"
#include "sculpt/subdivide.h"
#include "test_framework.h"

using namespace sculpt;

namespace {

PolyData ToPoly(const MeshInput& in) {
    PolyData p;
    p.positions = in.positions;
    p.faceSizes = in.faceSizes;
    p.faceVerts = in.faceVerts;
    return p;
}

float MaxDistance(const std::vector<Vec3>& a, const std::vector<Vec3>& b) {
    float worst = 0.0f;
    for (std::size_t i = 0; i < a.size() && i < b.size(); ++i) worst = std::max(worst, length(a[i] - b[i]));
    return a.size() == b.size() ? worst : 1e30f;
}

bool Closed(const PolyData& p) {
    EdgeTable edges;
    edges.build(p);
    for (std::uint32_t uses : edges.faceUses)
        if (uses != 2) return false;
    return true;
}

}  // namespace

TEST_CASE(half_float_round_trip) {
    const float values[] = {0.0f, 1.0f, -2.5f, 0.1f, 1e-5f, 65504.0f, -0.000123f};
    for (float v : values) CHECK_NEAR(halfToFloat(floatToHalf(v)), v, std::fabs(v) * 1e-3f + 1e-7f);
    CHECK_NEAR(halfToFloat(floatToHalf(1e9f)), 65504.0f, 1e-3f);  // Saturates, stays finite.
}

TEST_CASE(subdivide_cube_counts_and_rules) {
    const PolyData cube = ToPoly(sctest::makeCube());
    PolyData out;
    REQUIRE(subdivide(cube, SubdivOptions(), out));
    CHECK_EQ(out.positions.size(), 26u);  // 8 + 12 + 6
    CHECK_EQ(out.faceSizes.size(), 24u);
    CHECK(out.valid());
    CHECK(Closed(out));
    // Catmull-Clark vertex rule for a valence-3 corner of a +-1 cube: 5/9.
    CHECK_NEAR(out.positions[7].x, 5.0f / 9.0f, 1e-5f);
    CHECK_NEAR(out.positions[7].y, 5.0f / 9.0f, 1e-5f);
    CHECK_NEAR(out.positions[7].z, 5.0f / 9.0f, 1e-5f);
    // Child k of face f is (vertex k, edge k, face, edge k-1).
    CHECK_EQ(out.faceVerts[0], cube.faceVerts[0]);
    CHECK_EQ(out.faceVerts[2], 8u + 12u + 0u);
}

TEST_CASE(subdivide_grid_keeps_border_and_maps) {
    PolyData grid = ToPoly(sctest::makeGrid(4, 4.0f));
    CornerMap uv;
    uv.channel = 1;
    for (std::uint32_t v : grid.faceVerts) uv.values.push_back({grid.positions[v].x, grid.positions[v].y, 0.0f});
    grid.maps.push_back(uv);
    grid.material.assign(grid.faceSizes.size(), 3);
    PolyData out;
    REQUIRE(subdivide(grid, SubdivOptions(), out));
    CHECK(out.valid());
    for (const Vec3& p : out.positions) CHECK_NEAR(p.z, 0.0f, 1e-6f);  // Stays flat.
    const std::uint32_t corner = sctest::gridIndex(4, 0, 0);
    CHECK(out.positions[corner] == grid.positions[corner]);  // Border corner fixed.
    // Linear UVs: every corner's UV equals its position on a flat grid... except
    // smoothing moves interior vertices, so compare the vertex-point corners only.
    for (std::size_t c = 0; c < out.faceVerts.size(); c += 4)
        CHECK_NEAR(length(out.maps[0].values[c] - Vec3{grid.positions[out.faceVerts[c]].x, grid.positions[out.faceVerts[c]].y, 0}), 0.0f, 1e-5f);
    for (std::uint16_t m : out.material) CHECK_EQ(m, 3);
}

TEST_CASE(unsubdivide_recovers_the_coarse_mesh) {
    const PolyData cube = ToPoly(sctest::makeCube());
    PolyData fine, fine2, coarse;
    REQUIRE(subdivide(cube, SubdivOptions(), fine));
    REQUIRE(subdivide(fine, SubdivOptions(), fine2));
    std::vector<std::uint32_t> vmap, fmap;
    std::vector<std::uint8_t> rot;
    REQUIRE(unsubdivide(fine2, coarse, nullptr, &vmap, &fmap, &rot));
    CHECK_EQ(coarse.positions.size(), fine.positions.size());
    CHECK_EQ(coarse.faceSizes.size(), fine.faceSizes.size());
    // Re-subdividing the fitted coarse mesh lands close to the input.
    PolyData again;
    REQUIRE(subdivide(coarse, SubdivOptions(), again));
    REQUIRE(vmap.size() == again.positions.size());
    float worst = 0.0f;
    for (std::size_t v = 0; v < vmap.size(); ++v) worst = std::max(worst, length(again.positions[v] - fine2.positions[vmap[v]]));
    CHECK(worst < 0.05f);
    // Faces correspond with the reported rotation.
    for (std::size_t f = 0; f < fmap.size(); ++f)
        for (int k = 0; k < 4; ++k)
            CHECK_EQ(vmap[again.faceVerts[4 * f + k]], fine2.faceVerts[4 * fmap[f] + (k + rot[f]) % 4]);
    // The cube (not a subdivided mesh) is rejected.
    PolyData none;
    CHECK(!unsubdivide(cube, none));
}

TEST_CASE(multires_edits_propagate_and_details_follow) {
    const PolyData cube = ToPoly(sctest::makeCube());
    Multires stack;
    REQUIRE(stack.reset(cube));
    REQUIRE(stack.addLevel(SubdivOptions()));
    REQUIRE(stack.addLevel(SubdivOptions()));
    CHECK_EQ(stack.topLevel(), 2);
    CHECK_EQ(stack.vertexCount(2), 98u);
    PolyData top;
    REQUIRE(stack.build(2, top));
    CHECK_EQ(top.positions.size(), 98u);

    // Fine bump + a uniform move at level 2.
    PolyData edited = top;
    for (Vec3& p : edited.positions) p.z += 1.0f;
    edited.positions[40] += Vec3{0.0f, 0.0f, 0.3f};
    REQUIRE(stack.commit(2, edited));
    PolyData again;
    REQUIRE(stack.build(2, again));
    CHECK(MaxDistance(again.positions, edited.positions) < 1e-4f);
    // The uniform move reached level 0; the bump mostly did not.
    PolyData base;
    REQUIRE(stack.build(0, base));
    for (std::size_t v = 0; v < 8; ++v) CHECK_NEAR(base.positions[v].z - cube.positions[v].z, 1.0f, 0.06f);

    // Rotate level 0 by 90 degrees: level 2 follows, bump included.
    PolyData rotated = base;
    const Mat3 r = Mat3::rotation(2, 1.5707963f);
    for (Vec3& p : rotated.positions) p = r * p;
    REQUIRE(stack.commit(0, rotated));
    PolyData turned;
    REQUIRE(stack.build(2, turned));
    std::vector<Vec3> expected = edited.positions;
    for (Vec3& p : expected) p = r * p;
    CHECK(MaxDistance(turned.positions, expected) < 1e-3f);
}

TEST_CASE(multires_attributes_and_serialization) {
    Multires stack;
    REQUIRE(stack.reset(ToPoly(sctest::makeCube())));
    REQUIRE(stack.addLevel(SubdivOptions()));
    REQUIRE(stack.addLevel(SubdivOptions()));
    PolyData level1;
    REQUIRE(stack.build(1, level1));
    level1.groups.assign(level1.faceSizes.size(), 0);
    level1.groups[5] = 7;
    level1.mask.assign(level1.positions.size(), 0.0f);
    level1.mask[3] = 1.0f;
    REQUIRE(stack.commit(1, level1));
    PolyData level2, level0;
    REQUIRE(stack.build(2, level2));
    REQUIRE(stack.build(0, level0));
    for (int k = 0; k < 4; ++k) CHECK_EQ(level2.groups[4 * 5 + k], 7);  // Children inherit.
    CHECK_NEAR(level2.mask[3], 1.0f, 1e-6f);
    CHECK_NEAR(level0.mask[3], 1.0f, 1e-6f);  // Down: same vertex index.

    const std::vector<std::uint8_t> bytes = stack.serialize();
    Multires loaded;
    REQUIRE(loaded.deserialize(bytes.data(), bytes.size()));
    PolyData reloaded;
    REQUIRE(loaded.build(2, reloaded));
    CHECK(MaxDistance(reloaded.positions, level2.positions) < 1e-3f);
    CHECK_EQ(reloaded.groups[20], 7);
    Multires broken;
    CHECK(!broken.deserialize(bytes.data(), bytes.size() / 2));
}

TEST_CASE(multires_delete_and_reverse) {
    const PolyData cube = ToPoly(sctest::makeCube());
    PolyData fine1, fine2;
    REQUIRE(subdivide(cube, SubdivOptions(), fine1));
    REQUIRE(subdivide(fine1, SubdivOptions(), fine2));
    for (Vec3& p : fine2.positions) p = p * 2.0f;

    Multires stack;
    REQUIRE(stack.reset(fine2));
    REQUIRE(stack.reverseSubdivision());
    REQUIRE(stack.reverseSubdivision());
    CHECK_EQ(stack.topLevel(), 2);
    CHECK_EQ(stack.base().positions.size(), 8u);
    PolyData top;
    REQUIRE(stack.build(2, top));
    REQUIRE(top.positions.size() == fine2.positions.size());
    // Same surface, possibly renumbered: compare as point sets via nearest match.
    float worst = 0.0f;
    for (const Vec3& p : top.positions) {
        float best = 1e30f;
        for (const Vec3& q : fine2.positions) best = std::min(best, length(p - q));
        worst = std::max(worst, best);
    }
    CHECK(worst < 1e-3f);

    REQUIRE(stack.deleteLower(1));
    CHECK_EQ(stack.topLevel(), 1);
    PolyData afterLower;
    REQUIRE(stack.build(1, afterLower));
    CHECK(MaxDistance(afterLower.positions, top.positions) < 1e-4f);
    REQUIRE(stack.deleteHigher(0));
    CHECK_EQ(stack.topLevel(), 0);
}
