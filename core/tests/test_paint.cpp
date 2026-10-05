#include <cmath>

#include "mesh_gen.h"
#include "sculpt/paint.h"
#include "sculpt/session.h"
#include "test_framework.h"

using namespace sculpt;

namespace {

// A 10 x 10 grid with UVs spanning 0..1, and its texel map.
struct Fixture {
    Mesh mesh;
    TexelMap map;
    PaintCanvas canvas;
    Fixture() {
        mesh.build(sctest::makeGrid(10, 10.0f));
        std::vector<Vec3> uvs;
        for (std::uint32_t t = 0; t < mesh.triangleCount(); ++t)
            for (std::uint32_t v : mesh.triangle(t)) {
                const Vec3& p = mesh.position(v);
                uvs.push_back({(p.x + 5.0f) / 10.0f, (p.y + 5.0f) / 10.0f, 0.0f});
            }
        map.build(mesh, uvs, 64, 64);
        Image base;
        base.resize(64, 64, 128, 128, 128, 255);
        canvas.reset(base);
    }
};

}  // namespace

TEST_CASE(texel_map_covers_the_grid) {
    Fixture f;
    REQUIRE(f.map.valid());
    for (std::size_t t = 0; t < 64u * 64u; ++t) CHECK(f.map.covered(t));
    // Texel (32, 32) shows UV (0.5078, 0.4922): near the grid centre.
    const Vec3 p = f.map.position(f.mesh, 32u * 64u + 32u);
    CHECK_NEAR(p.x, 0.078f, 0.02f);
    CHECK_NEAR(p.y, -0.078f, 0.02f);
}

TEST_CASE(paint_dab_respects_opacity_and_undo) {
    Fixture f;
    PaintStroke stroke;
    stroke.begin(f.canvas, f.map, f.mesh);
    PaintSettings settings;
    settings.opacity = 0.5f;
    settings.hardness = 1.0f;
    PaintDab dab;
    dab.center = {0, 0, 0};
    dab.radius = 2.0f;
    dab.viewDir = {0, 0, -1};
    dab.color = {1, 0, 0};
    for (int i = 0; i < 5; ++i) stroke.dab(settings, dab, {}, {Mat3::identity()});
    const PaintDelta delta = stroke.end();
    REQUIRE(!delta.empty());
    const std::uint8_t* center = f.canvas.base().pixel(32, 32);
    CHECK_NEAR(center[0], 191.0f, 2.0f);  // Half way from grey to red, however many dabs.
    CHECK_NEAR(center[1], 64.0f, 2.0f);
    CHECK_EQ(f.canvas.base().pixel(2, 2)[0], 128);
    REQUIRE(applyPaintDelta(f.canvas, delta, true));
    CHECK_EQ(f.canvas.base().pixel(32, 32)[0], 128);
    REQUIRE(applyPaintDelta(f.canvas, delta, false));
    CHECK_NEAR(f.canvas.base().pixel(32, 32)[0], 191.0f, 2.0f);
}

TEST_CASE(paint_layers_blend_and_mask) {
    Fixture f;
    PaintLayer layer;
    layer.name = "Layer 1";
    layer.image = std::make_shared<Image>();
    layer.image->resize(64, 64, 0, 0, 0, 0);
    layer.id = f.canvas.newLayerId();
    layer.blend = PaintBlend::Multiply;
    f.canvas.layers().push_back(layer);
    f.canvas.setActive(0);
    std::vector<float> mask(f.mesh.vertexCount(), 0.0f);
    for (std::uint32_t v = 0; v < f.mesh.vertexCount(); ++v)
        if (f.mesh.position(v).x < 0.0f) mask[v] = 1.0f;  // Left half protected.
    PaintStroke stroke;
    stroke.begin(f.canvas, f.map, f.mesh);
    PaintSettings settings;
    settings.colorA = {0.5f, 0.5f, 0.5f};
    REQUIRE(stroke.fill(settings, mask) > 0);
    stroke.end();
    f.canvas.markAll();
    f.canvas.updateComposite();
    CHECK_NEAR(f.canvas.composite().pixel(60, 32)[0], 64.0f, 2.0f);  // Grey multiplied by 0.5.
    CHECK_EQ(f.canvas.composite().pixel(3, 32)[0], 128);             // Masked: untouched.
}

TEST_CASE(paint_adjustments) {
    Image a;
    a.resize(2, 2, 200, 100, 50, 255);
    Image out;
    adjustHsl(a, out, 0.0f, 0.0f, 0.0f);
    CHECK_NEAR(out.pixel(0, 0)[0], 200.0f, 1.0f);
    CHECK_NEAR(out.pixel(0, 0)[2], 50.0f, 1.0f);
    adjustHsl(a, out, 0.0f, -100.0f, 0.0f);  // Fully desaturated.
    CHECK_EQ(out.pixel(0, 0)[0], out.pixel(0, 0)[2]);
    adjustLevels(a, out, 0.0f, 1.0f, 255.0f, 0.0f, 255.0f);
    CHECK_EQ(out.pixel(1, 1)[1], 100);
    adjustBrightnessContrast(a, out, 100.0f, 0.0f);
    CHECK_EQ(out.pixel(1, 1)[0], 255);
    const Image half = resampleImage(a, 1, 1);
    CHECK_EQ(half.pixel(0, 0)[1], 100);
}

TEST_CASE(displace_dab_raises_the_stencil) {
    SculptSession session;
    REQUIRE(session.build(sctest::makeGrid(20, 10.0f)));
    BrushSettings settings;
    settings.type = BrushType::Displace;
    settings.strength = 1.0f;
    settings.layerMode = true;
    Dab dab;
    dab.center = {0, 0, 0};
    dab.radius = 3.0f;
    dab.viewDir = {0, 0, -1};
    const std::function<bool(const Vec3&, float&)> height = [](const Vec3& p, float& h) {
        h = p.x > 0.0f ? 1.0f : 0.0f;
        return true;
    };
    session.beginStroke();
    for (int i = 0; i < 4; ++i) session.applyDisplaceDab(settings, dab, height, 0.5f);
    const StrokeDelta delta = session.endStroke();
    CHECK(!delta.empty());
    float right = 0.0f, left = 0.0f;
    for (std::uint32_t v = 0; v < session.mesh().vertexCount(); ++v) {
        const Vec3& p = session.mesh().position(v);
        if (p.x > 0.2f) right = std::max(right, p.z);
        if (p.x < -0.2f) left = std::max(left, std::fabs(p.z));
    }
    CHECK_NEAR(right, 1.5f, 0.05f);  // Layer Mode: one height, however many dabs.
    CHECK_NEAR(left, 0.0f, 1e-5f);
}
