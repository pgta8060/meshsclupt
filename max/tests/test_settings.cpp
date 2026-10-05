// Tests for the plugin's Max-independent logic (SculptSettings).
#include <cmath>

#include "../../core/tests/test_framework.h"
#include "SculptSettings.h"

using sculpt::BrushType;

namespace {

struct CountingListener : SculptSettings::Listener {
    int calls = 0;
    void OnSculptSettingsChanged() override { ++calls; }
};

}  // namespace

TEST_CASE(settings_clamp_round_and_ignore_nan) {
    SculptSettings& s = SculptSettings::Get();
    s.ResetToDefaults();
    s.Set(Prop::BrushSize, -5.0f);
    CHECK_EQ(s.Size(), 1.0f);
    s.Set(Prop::BrushSize, 1e9f);
    CHECK_EQ(s.Size(), 2000.0f);
    s.Set(Prop::BrushSize, std::nanf(""));
    CHECK_EQ(s.Size(), 2000.0f);
    s.Set(Prop::RadialCount, 7.6f);
    CHECK_EQ(s.Int(Prop::RadialCount), 8);
    s.Set(Prop::MirrorX, 0.3f);
    CHECK(s.Bool(Prop::MirrorX));
    s.SetStrength(3.0f);
    CHECK_EQ(s.Strength(), 1.0f);
    s.ResetToDefaults();
}

TEST_CASE(settings_brush_values_are_per_brush) {
    SculptSettings& s = SculptSettings::Get();
    s.ResetToDefaults();
    s.SetBrushValue(BrushType::Sculpt, BrushProp::Strength, 0.7f);
    s.SetBrushValue(BrushType::Smooth, BrushProp::Strength, 0.2f);
    s.SetBrush(BrushType::Smooth);
    CHECK_NEAR(s.Strength(), 0.2f, 1e-6f);
    CHECK(!s.MakeBrush(BrushType::Move).backfaceCull);  // Move defaults to no culling.
    s.SetBrush(BrushType::Count);                       // Not a palette brush: ignored.
    CHECK(s.Brush() == BrushType::Smooth);
    s.SetBrush(BrushType::MaskPaint);                   // The mask tool is not a palette brush.
    CHECK(s.Brush() == BrushType::Smooth);
    s.SetBrush(BrushType::Cloth);
    CHECK(s.Brush() == BrushType::Cloth);
    s.ResetToDefaults();
}

TEST_CASE(settings_palette_order_and_moves) {
    SculptSettings& s = SculptSettings::Get();
    s.ResetToDefaults();
    const std::vector<BrushType> defaults = s.PaletteOrder();
    REQUIRE(defaults.size() >= 5u);
    CHECK(defaults[0] == BrushType::Sculpt);
    s.MovePaletteItem(0, 2);
    CHECK(s.PaletteOrder()[2] == BrushType::Sculpt);
    s.SetPaletteOrder({BrushType::Smooth, BrushType::Smooth, BrushType::Cloth});
    CHECK(s.PaletteOrder()[0] == BrushType::Smooth);
    CHECK_EQ(s.PaletteOrder().size(), defaults.size());  // Duplicates/unavailable dropped, rest appended.
    s.ResetToDefaults();
}

TEST_CASE(settings_text_round_trip) {
    SculptSettings& s = SculptSettings::Get();
    s.ResetToDefaults();
    s.Set(Prop::BrushSize, 77.0f);
    s.SetBool(Prop::MirrorZ, true);
    s.SetBrushValue(BrushType::Clay, BrushProp::Strength, 0.9f);
    s.MovePaletteItem(3, 0);
    s.SetAlphaId("builtin:2");
    s.ToggleAlphaFavorite("C:/alphas/a.png");
    const std::string text = s.ToText();
    const std::vector<BrushType> palette = s.PaletteOrder();

    s.ResetToDefaults();
    CountingListener l;
    s.AddListener(&l);
    s.FromText(text + "unknownKey=5\nbrush.nope.strength=1\n");
    s.RemoveListener(&l);
    CHECK_EQ(l.calls, 1);  // One notification for a whole load.
    CHECK_EQ(s.Size(), 77.0f);
    CHECK(s.Bool(Prop::MirrorZ));
    CHECK_NEAR(s.BrushValue(BrushType::Clay, BrushProp::Strength), 0.9f, 1e-6f);
    CHECK(s.PaletteOrder() == palette);
    CHECK(s.AlphaId() == "builtin:2");
    REQUIRE(s.AlphaFavorites().size() == 1u);
    s.ResetToDefaults();
}

TEST_CASE(settings_listener_notified_once_per_change) {
    SculptSettings& s = SculptSettings::Get();
    s.ResetToDefaults();
    CountingListener a;
    s.AddListener(&a);
    s.AddListener(&a);
    s.Set(Prop::BrushSize, 21.0f);
    s.Set(Prop::BrushSize, 21.0f);  // Unchanged: no call.
    CHECK_EQ(a.calls, 1);
    s.RemoveListener(&a);
    s.Set(Prop::BrushSize, 22.0f);
    CHECK_EQ(a.calls, 1);
    s.ResetToDefaults();
}
