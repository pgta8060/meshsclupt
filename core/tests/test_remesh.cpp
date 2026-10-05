#include "mesh_gen.h"
#include "sculpt/remesh.h"
#include "test_framework.h"

using namespace sculpt;

namespace {

PolyData Grid(std::uint32_t cells) {
    const MeshInput in = sctest::makeGrid(cells, 10.0f);
    PolyData p;
    p.positions = in.positions;
    p.faceSizes = in.faceSizes;
    p.faceVerts = in.faceVerts;
    CornerMap uv;
    for (std::uint32_t v : p.faceVerts) uv.values.push_back({p.positions[v].x / 10.0f + 0.5f, p.positions[v].y / 10.0f + 0.5f, 0});
    p.maps.push_back(uv);
    p.groups.assign(p.faceSizes.size(), 3);
    return p;
}

bool Manifold(const PolyData& p) {
    EdgeTable edges;
    edges.build(p);
    for (std::uint32_t uses : edges.faceUses)
        if (uses > 2) return false;
    return true;
}

}  // namespace

TEST_CASE(density_refines_only_the_painted_region) {
    PolyData grid = Grid(10);
    const std::size_t facesBefore = grid.faceSizes.size();
    std::vector<float> weight(grid.positions.size(), 0.0f);
    weight[sctest::gridIndex(10, 5, 5)] = 1.0f;
    DensityOptions options;
    options.targetLength = 0.2f;
    REQUIRE(remeshRegion(grid, weight, options));
    CHECK(grid.valid());
    CHECK(Manifold(grid));
    CHECK(grid.faceSizes.size() > facesBefore + 20);
    // Untouched quads stay quads; the surface stays flat; UVs still follow positions.
    std::size_t quads = 0;
    for (std::uint32_t n : grid.faceSizes) quads += n == 4 ? 1 : 0;
    CHECK_EQ(quads, facesBefore - 4);
    for (const Vec3& p : grid.positions) CHECK_NEAR(p.z, 0.0f, 1e-6f);
    for (std::size_t c = 0; c < grid.faceVerts.size(); ++c) {
        const Vec3& p = grid.positions[grid.faceVerts[c]];
        CHECK_NEAR(grid.maps[0].values[c].x, p.x / 10.0f + 0.5f, 1e-4f);
    }
    for (std::int32_t g : grid.groups) CHECK_EQ(g, 3);
}

TEST_CASE(density_reduce_collapses_small_edges) {
    PolyData grid = Grid(20);
    std::vector<float> weight(grid.positions.size(), 0.0f);
    for (std::uint32_t j = 5; j <= 15; ++j)
        for (std::uint32_t i = 5; i <= 15; ++i) weight[sctest::gridIndex(20, i, j)] = 1.0f;
    const std::size_t before = grid.positions.size();
    DensityOptions options;
    options.reduce = true;
    options.targetLength = 1.2f;  // Cells are 0.5 wide.
    REQUIRE(remeshRegion(grid, weight, options));
    CHECK(grid.valid());
    CHECK(Manifold(grid));
    CHECK(grid.positions.size() < before);
    for (const Vec3& p : grid.positions) CHECK_NEAR(p.z, 0.0f, 1e-6f);
}
