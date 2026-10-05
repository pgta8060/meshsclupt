#include <cmath>

#include "mesh_gen.h"
#include "sculpt/cut.h"
#include "test_framework.h"

using namespace sculpt;

namespace {

PolyData Sphere() {
    const MeshInput in = sctest::makeUvSphere(24, 32, 10.0f);
    PolyData p;
    p.positions = in.positions;
    p.faceSizes = in.faceSizes;
    p.faceVerts = in.faceVerts;
    p.groups.assign(p.faceSizes.size(), 1);
    return p;
}

// Every edge used by exactly two polygons, in opposite directions.
bool ClosedAndOriented(const PolyData& p) {
    EdgeTable edges;
    edges.build(p);
    for (std::uint32_t uses : edges.faceUses)
        if (uses != 2) return false;
    std::vector<std::uint32_t> starts = p.faceStarts();
    std::vector<int> balance(edges.size(), 0);
    for (std::size_t f = 0; f < p.faceSizes.size(); ++f)
        for (std::uint32_t k = 0; k < p.faceSizes[f]; ++k) {
            const std::uint32_t c = starts[f] + k;
            const std::uint32_t a = p.faceVerts[c], b = p.faceVerts[starts[f] + (k + 1) % p.faceSizes[f]];
            balance[edges.cornerEdge[c]] += a < b ? 1 : -1;
        }
    for (int b : balance)
        if (b != 0) return false;
    return true;
}

std::size_t Components(const PolyData& p) {
    VertexAdjacency adjacency;
    adjacency.build(p);
    std::vector<int> seen(p.positions.size(), 0);
    std::size_t count = 0;
    for (std::size_t s = 0; s < p.positions.size(); ++s) {
        if (seen[s]) continue;
        ++count;
        std::vector<std::uint32_t> stack = {static_cast<std::uint32_t>(s)};
        seen[s] = 1;
        while (!stack.empty()) {
            const std::uint32_t v = stack.back();
            stack.pop_back();
            for (std::uint32_t i = adjacency.offsets[v]; i < adjacency.offsets[v + 1]; ++i)
                if (!seen[adjacency.neighbors[i]]) {
                    seen[adjacency.neighbors[i]] = 1;
                    stack.push_back(adjacency.neighbors[i]);
                }
        }
    }
    return count;
}

}  // namespace

TEST_CASE(cutter_plane_removes_half_and_caps) {
    PolyData sphere = Sphere();
    CutOptions o;
    o.field = [](const Vec3& p) { return p.z - 1.3f; };
    o.capGroup = 9;
    CutStats stats;
    REQUIRE(cutMesh(sphere, o, &stats));
    CHECK(sphere.valid());
    CHECK(ClosedAndOriented(sphere));
    CHECK(stats.capFaces > 0);
    CHECK_EQ(stats.openLoops, 0u);
    for (const Vec3& p : sphere.positions) CHECK(p.z >= 1.3f - 1e-3f);
    bool capGroup = false;
    for (std::int32_t g : sphere.groups) capGroup = capGroup || g == 9;
    CHECK(capGroup);
}

TEST_CASE(cutter_circle_makes_a_tunnel) {
    PolyData sphere = Sphere();
    CutOptions o;
    o.field = [](const Vec3& p) { return std::sqrt(p.x * p.x + p.y * p.y) - 3.0f; };
    o.project = [](const Vec3& p, float& x, float& y) {
        x = p.x;
        y = p.y;
        return true;
    };
    CutStats stats;
    REQUIRE(cutMesh(sphere, o, &stats));
    CHECK(sphere.valid());
    CHECK(ClosedAndOriented(sphere));
    CHECK_EQ(Components(sphere), 1u);  // One body with a hole, not two caps.
}

TEST_CASE(slice_separates_two_closed_parts) {
    PolyData sphere = Sphere();
    CutOptions o;
    o.slice = true;
    o.field = [](const Vec3& p) { return p.x - 0.7f; };
    o.secondGroup = 5;
    REQUIRE(cutMesh(sphere, o));
    CHECK(sphere.valid());
    CHECK(ClosedAndOriented(sphere));
    CHECK_EQ(Components(sphere), 2u);
    PolyData untouched = Sphere();
    CutOptions miss;
    miss.field = [](const Vec3& p) { return p.z + 100.0f; };
    CHECK(!cutMesh(untouched, miss));
}
