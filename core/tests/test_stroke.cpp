#include <cmath>

#include "sculpt/stroke.h"
#include "test_framework.h"

using namespace sculpt;

TEST_CASE(stroke_even_spacing_on_a_line) {
    StrokeSpacer spacer;
    std::vector<StrokeSample> out;
    spacer.begin({0, 0, 1}, out);
    spacer.moveTo({100, 0, 1}, 10.0f, out);
    REQUIRE(out.size() == 11u);
    for (std::size_t i = 0; i < out.size(); ++i) {
        CHECK_NEAR(out[i].x, 10.0f * static_cast<float>(i), 1e-4f);
        CHECK_EQ(out[i].y, 0.0f);
    }
}

TEST_CASE(stroke_spacing_independent_of_sampling) {
    // The same path delivered in many small irregular steps yields the same dabs.
    StrokeSpacer coarse, fine;
    std::vector<StrokeSample> a, b;
    coarse.begin({0, 0, 1}, a);
    coarse.moveTo({0, 73, 1}, 7.0f, a);
    fine.begin({0, 0, 1}, b);
    float y = 0.0f;
    for (float step : {0.3f, 5.0f, 1.2f, 9.9f, 0.01f, 20.0f, 3.3f, 33.29f}) {
        y += step;
        fine.moveTo({0, y, 1}, 7.0f, b);
    }
    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i) CHECK_NEAR(a[i].y, b[i].y, 1e-3f);
}

TEST_CASE(stroke_pressure_is_interpolated) {
    StrokeSpacer spacer;
    std::vector<StrokeSample> out;
    spacer.begin({0, 0, 0.0f}, out);
    spacer.moveTo({10, 0, 1.0f}, 5.0f, out);
    REQUIRE(out.size() == 3u);
    CHECK_NEAR(out[1].pressure, 0.5f, 1e-6f);
    CHECK_NEAR(out[2].pressure, 1.0f, 1e-6f);
}

TEST_CASE(stroke_degenerate_input) {
    StrokeSpacer spacer;
    std::vector<StrokeSample> out;
    spacer.moveTo({5, 5, 1}, 10.0f, out);  // moveTo without begin starts the stroke.
    CHECK(spacer.active());
    CHECK_EQ(out.size(), 1u);
    spacer.moveTo({5, 5, 1}, 10.0f, out);  // No movement, no dab.
    CHECK_EQ(out.size(), 1u);
    spacer.moveTo({5, 5 + 1e6f, 1}, std::nanf(""), out);  // NaN spacing clamps; huge jump is capped.
    CHECK_EQ(out.size(), 1u + static_cast<std::size_t>(StrokeSpacer::kMaxDabsPerSegment));
    spacer.moveTo({std::nanf(""), 0, 1}, 10.0f, out);  // NaN sample is ignored.
    CHECK_EQ(out.size(), 1u + static_cast<std::size_t>(StrokeSpacer::kMaxDabsPerSegment));
    spacer.end();
    CHECK(!spacer.active());
}
