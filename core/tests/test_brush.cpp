#include <cmath>

#include "mesh_gen.h"
#include "sculpt/brush.h"
#include "test_framework.h"

using namespace sculpt;

namespace {

struct Fixture {
    Mesh mesh;
    Bvh bvh;
    StrokeRecorder recorder;
    StrokeState state;
    DabScratch scratch;
    std::vector<std::uint32_t> moved;

    explicit Fixture(MeshInput in) {
        mesh.build(std::move(in));
        bvh.build(mesh);
        recorder.begin(mesh.vertexCount());
    }

    std::size_t dab(const BrushSettings& s, const Dab& d) {
        moved.clear();
        DabTarget target{mesh, bvh, recorder, state, scratch, moved};
        const std::size_t n = applyDab(target, s, d);
        mesh.updateNormals(moved);
        std::vector<std::uint32_t> tris;
        mesh.collectTriangles(moved, tris);
        bvh.refit(mesh, tris);
        return n;
    }
};

constexpr std::uint32_t kCells = 20;  // 20x20 grid of size 20 -> 1 unit spacing.

Dab centerDab(float radius) {
    Dab d;
    d.center = {0, 0, 0};
    d.radius = radius;
    d.viewDir = {0, 0, -1};  // Looking down at the +Z-facing grid.
    return d;
}

std::uint32_t centerVertex() { return sctest::gridIndex(kCells, kCells / 2, kCells / 2); }

}  // namespace

TEST_CASE(brush_falloff_curve) {
    CHECK_EQ(brushFalloff(0.0f, true), 1.0f);
    CHECK_EQ(brushFalloff(1.0f, true), 0.0f);
    CHECK_NEAR(brushFalloff(0.5f, true), 0.5f, 1e-6f);
    float prev = 2.0f;
    for (int i = 0; i <= 100; ++i) {
        const float f = brushFalloff(static_cast<float>(i) / 100.0f, true);
        CHECK(f <= prev);
        CHECK(f >= 0.0f && f <= 1.0f);
        prev = f;
    }
    CHECK_EQ(brushFalloff(0.7f, false), 1.0f);
    CHECK_EQ(brushFalloff(1.01f, false), 0.0f);
    CHECK_EQ(brushFalloff(-0.1f, true), 0.0f);
    CHECK_EQ(brushFalloff(std::nanf(""), true), 0.0f);
}

TEST_CASE(brush_names_and_signs) {
    CHECK(std::string(brushName(BrushType::Sculpt)) == "Sculpt");
    CHECK(std::string(brushName(BrushType::Pinch)) == "Pinch");
    CHECK(isSignedBrush(BrushType::Sculpt));
    CHECK(!isSignedBrush(BrushType::Smooth));
}

TEST_CASE(brush_sculpt_raises_and_digs) {
    for (int mode = 0; mode < 4; ++mode) {
        Fixture f(sctest::makeGrid(kCells, 20.0f));
        BrushSettings s;
        s.type = BrushType::Sculpt;
        s.subtract = (mode & 1) != 0;
        Dab d = centerDab(4.0f);
        d.invert = (mode & 2) != 0;
        const float expectedSign = (s.subtract != d.invert) ? -1.0f : 1.0f;

        CHECK(f.dab(s, d) > 0u);
        const Vec3 c = f.mesh.position(centerVertex());
        CHECK(c.z * expectedSign > 0.0f);
        CHECK_NEAR(c.x, 0.0f, 1e-6f);  // Moves along the area normal only.
        CHECK_NEAR(c.y, 0.0f, 1e-6f);
        // Nothing outside the radius moves.
        for (std::uint32_t v = 0; v < f.mesh.vertexCount(); ++v) {
            const Vec3 p = f.mesh.position(v);
            if (std::hypot(p.x, p.y) > 4.0f + 1e-4f) CHECK_EQ(p.z, 0.0f);
        }
        // Peak at the centre, decaying outwards.
        const float z0 = std::fabs(f.mesh.position(centerVertex()).z);
        const float z2 = std::fabs(f.mesh.position(sctest::gridIndex(kCells, kCells / 2 + 2, kCells / 2)).z);
        CHECK(z0 > z2);
    }
}

TEST_CASE(brush_strength_and_pressure_scale_effect) {
    auto heightFor = [](float strength, float pressure) {
        Fixture f(sctest::makeGrid(kCells, 20.0f));
        BrushSettings s;
        s.strength = strength;
        Dab d = centerDab(4.0f);
        d.pressure = pressure;
        f.dab(s, d);
        return f.mesh.position(centerVertex()).z;
    };
    CHECK_EQ(heightFor(0.0f, 1.0f), 0.0f);
    CHECK_EQ(heightFor(1.0f, 0.0f), 0.0f);
    CHECK_NEAR(heightFor(1.0f, 1.0f), 2.0f * heightFor(0.5f, 1.0f), 1e-5f);
    CHECK_NEAR(heightFor(1.0f, 0.5f), heightFor(0.5f, 1.0f), 1e-6f);
    CHECK_NEAR(heightFor(5.0f, 1.0f), heightFor(1.0f, 1.0f), 1e-6f);  // Clamped to 1.
}

TEST_CASE(brush_backface_cull) {
    Fixture f(sctest::makeGrid(kCells, 20.0f));
    BrushSettings s;
    Dab d = centerDab(4.0f);
    d.viewDir = {0, 0, 1};  // Looking at the grid from below: it faces away.
    s.backfaceCull = true;
    CHECK_EQ(f.dab(s, d), 0u);
    s.backfaceCull = false;
    CHECK(f.dab(s, d) > 0u);
}

TEST_CASE(brush_invalid_dabs_do_nothing) {
    Fixture f(sctest::makeGrid(kCells, 20.0f));
    BrushSettings s;
    Dab d = centerDab(0.0f);
    CHECK_EQ(f.dab(s, d), 0u);
    d.radius = std::nanf("");
    CHECK_EQ(f.dab(s, d), 0u);
    d = centerDab(4.0f);
    d.center.x = kInfinity;
    CHECK_EQ(f.dab(s, d), 0u);
    d = centerDab(4.0f);
    d.center = {100, 100, 0};  // Nothing under the brush.
    CHECK_EQ(f.dab(s, d), 0u);
    s.type = BrushType::Count;
    CHECK_EQ(f.dab(s, centerDab(4.0f)), 0u);
    for (std::uint32_t v = 0; v < f.mesh.vertexCount(); ++v) CHECK_EQ(f.mesh.position(v).z, 0.0f);
}

TEST_CASE(brush_smooth_relaxes_noise) {
    MeshInput in = sctest::makeGrid(kCells, 20.0f);
    sctest::Rng rng(3);
    for (Vec3& p : in.positions) p.z = rng.uniform(-0.5f, 0.5f);
    Fixture f(std::move(in));

    auto roughness = [&]() {
        double sum = 0.0;
        for (std::uint32_t v = 0; v < f.mesh.vertexCount(); ++v) {
            const Vec3 p = f.mesh.position(v);
            if (std::hypot(p.x, p.y) < 3.0f) sum += static_cast<double>(p.z) * static_cast<double>(p.z);
        }
        return sum;
    };
    const double before = roughness();
    BrushSettings s;
    s.type = BrushType::Smooth;
    s.strength = 1.0f;
    s.backfaceCull = false;
    for (int i = 0; i < 10; ++i) f.dab(s, centerDab(6.0f));
    CHECK(roughness() < before * 0.25);
}

TEST_CASE(brush_smooth_keeps_flat_plane_and_border_outline) {
    Fixture f(sctest::makeGrid(kCells, 20.0f));
    BrushSettings s;
    s.type = BrushType::Smooth;
    s.strength = 1.0f;
    Dab d = centerDab(6.0f);
    d.center = {-10, 0, 0};  // On the left border.
    for (int i = 0; i < 5; ++i) f.dab(s, d);
    for (std::uint32_t v = 0; v < f.mesh.vertexCount(); ++v) {
        const Vec3 p = f.mesh.position(v);
        CHECK_EQ(p.z, 0.0f);
        // Straight border vertices stay on the border line x = -10.
        if (f.mesh.isBorder(v) && sctest::gridIndex(kCells, 0, v / (kCells + 1)) == v) CHECK_NEAR(p.x, -10.0f, 1e-5f);
    }
}

TEST_CASE(brush_inflate_grows_sphere) {
    Fixture f(sctest::makeUvSphere(30, 40, 10.0f));
    BrushSettings s;
    s.type = BrushType::Inflate;
    Dab d;
    d.center = {10, 0, 0};
    d.radius = 4.0f;
    d.viewDir = {-1, 0, 0};
    REQUIRE(f.dab(s, d) > 0u);
    for (std::uint32_t v : f.moved) CHECK(length(f.mesh.position(v)) > 10.0f - 1e-4f);
    d.invert = true;
    std::vector<float> before;
    for (std::uint32_t v : f.moved) before.push_back(length(f.mesh.position(v)));
    std::vector<std::uint32_t> prevMoved = f.moved;
    f.dab(s, d);
    for (std::size_t i = 0; i < prevMoved.size(); ++i) CHECK(length(f.mesh.position(prevMoved[i])) < before[i] + 1e-5f);
}

TEST_CASE(brush_pinch_pulls_toward_center) {
    Fixture f(sctest::makeGrid(kCells, 20.0f));
    BrushSettings s;
    s.type = BrushType::Pinch;
    const std::uint32_t v = sctest::gridIndex(kCells, kCells / 2 + 2, kCells / 2);  // At (2, 0, 0).
    f.dab(s, centerDab(4.0f));
    CHECK(f.mesh.position(v).x < 2.0f);
    CHECK(f.mesh.position(v).x > 0.0f);
    CHECK_EQ(f.mesh.position(v).z, 0.0f);  // Stays in the tangent plane.
    Fixture g(sctest::makeGrid(kCells, 20.0f));
    s.subtract = true;
    g.dab(s, centerDab(4.0f));
    CHECK(g.mesh.position(v).x > 2.0f);  // Spread.
}

TEST_CASE(brush_recorder_keeps_first_original) {
    Fixture f(sctest::makeGrid(kCells, 20.0f));
    BrushSettings s;
    f.dab(s, centerDab(4.0f));
    f.dab(s, centerDab(4.0f));
    const auto& verts = f.recorder.vertices();
    const auto& originals = f.recorder.originals();
    REQUIRE(verts.size() == originals.size());
    REQUIRE(!verts.empty());
    for (const Vec3& o : originals) CHECK_EQ(o.z, 0.0f);  // Pre-stroke positions, not intermediate ones.
    std::vector<std::uint32_t> sorted = verts;
    std::sort(sorted.begin(), sorted.end());
    CHECK(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());
}
