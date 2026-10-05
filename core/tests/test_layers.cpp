#include "mesh_gen.h"
#include "sculpt/layers.h"
#include "test_framework.h"

using namespace sculpt;

TEST_CASE(layer_stack_offsets_and_strength_slider) {
    LayerStack stack;
    SculptLayer a, b;
    a.delta = {{1, 0, 0}, {0, 0, 0}};
    b.delta = {{0, 2, 0}, {0, 0, 1}};
    b.strength = 0.5f;
    stack.layers = {a, b};
    CHECK(stack.offset(0) == (Vec3{1, 1, 0}));
    stack.layers[0].enabled = false;
    CHECK(stack.offset(0) == (Vec3{0, 1, 0}));
    CHECK(stack.sizesMatch(2));
    CHECK(!stack.sizesMatch(3));
    CHECK_NEAR(layerStrengthFromSlider(0.5f), 1.0f, 1e-6f);
    CHECK_NEAR(layerStrengthFromSlider(1.0f), 5.0f, 1e-6f);
    CHECK_NEAR(layerStrengthFromSlider(0.25f), 0.5f, 1e-6f);
    for (float s : {0.0f, 0.3f, 1.0f, 2.5f, 5.0f}) CHECK_NEAR(layerStrengthFromSlider(layerSliderFromStrength(s)), s, 1e-4f);

    stack.active = 1;
    stack.layers[1].name = "Detail";
    const std::vector<std::uint8_t> bytes = serializeLayers(stack);
    LayerStack loaded;
    REQUIRE(deserializeLayers(bytes.data(), bytes.size(), loaded));
    CHECK_EQ(loaded.active, 1);
    CHECK_EQ(loaded.layers[1].name, std::string("Detail"));
    CHECK(!loaded.layers[0].enabled);
    CHECK_NEAR(loaded.layers[1].delta[0].y, 2.0f, 1e-3f);
    CHECK(!deserializeLayers(bytes.data(), bytes.size() - 3, loaded));
}

TEST_CASE(displacement_uv_and_triplanar) {
    // 2x1 image: left black, right white.
    const Alpha image(2, 1, {0.0f, 1.0f});
    CHECK_NEAR(sampleWrapped(image, 0.25f, 0.5f), 0.0f, 1e-5f);
    CHECK_NEAR(sampleWrapped(image, 0.75f, 0.5f), 1.0f, 1e-5f);
    CHECK_NEAR(sampleWrapped(image, 1.75f, 0.5f), 1.0f, 1e-5f);  // Wraps.

    const std::vector<Vec3> positions = {{0, 0, 0}, {1, 0, 0}};
    const std::vector<Vec3> normals = {{0, 0, 1}, {0, 0, 1}};
    const std::vector<Vec3> uvs = {{0.25f, 0.5f, 0}, {0.75f, 0.5f, 0}};
    DisplaceSettings s;
    s.strength = 2.0f;
    Aabb box;
    box.expand({0, 0, 0});
    box.expand({1, 1, 1});
    std::vector<Vec3> out;
    computeDisplacement(positions, normals, &uvs, box, image, s, out);
    CHECK_NEAR(out[0].z, -1.0f, 1e-5f);  // (0 - 0.5) * 2
    CHECK_NEAR(out[1].z, 1.0f, 1e-5f);
    s.waterLevel = 0.5f;  // Neutral value 1: white stays on the surface.
    computeDisplacement(positions, normals, &uvs, box, image, s, out);
    CHECK_NEAR(out[1].z, 0.0f, 1e-5f);
    s.triplanar = true;  // Z plane: u = x (relative to the box).
    s.waterLevel = 0.0f;
    const std::vector<Vec3> spread = {{0.25f, 0, 0}, {0.75f, 0, 0}};
    computeDisplacement(spread, normals, nullptr, box, image, s, out);
    CHECK_NEAR(out[0].z, -1.0f, 1e-5f);
    CHECK_NEAR(out[1].z, 1.0f, 1e-5f);

    const Alpha blurred = blurImage(Alpha(4, 1, {0, 0, 1, 1}), 2.0f);
    CHECK(blurred.values()[1] > 0.0f && blurred.values()[1] < 1.0f);
}
