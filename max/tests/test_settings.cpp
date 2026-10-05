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

// Removes itself while being notified; other listeners must still be called.
struct SelfRemovingListener : SculptSettings::Listener {
    int calls = 0;
    void OnSculptSettingsChanged() override {
        ++calls;
        SculptSettings::Get().RemoveListener(this);
    }
};

// Writes a setting back from inside the notification (must not recurse forever).
struct WritingListener : SculptSettings::Listener {
    int calls = 0;
    void OnSculptSettingsChanged() override {
        ++calls;
        SculptSettings::Get().SetSize(SculptSettings::Get().Size() + 1.0f);
    }
};

}  // namespace

TEST_CASE(settings_clamp_and_ignore_nan) {
    SculptSettings& s = SculptSettings::Get();
    s.SetSize(40.0f);
    s.SetSize(-5.0f);
    CHECK_EQ(s.Size(), SculptSettings::kMinSize);
    s.SetSize(1e9f);
    CHECK_EQ(s.Size(), SculptSettings::kMaxSize);
    s.SetSize(33.0f);
    s.SetSize(std::nanf(""));
    CHECK_EQ(s.Size(), 33.0f);

    s.SetSpacing(0.0f);
    CHECK_EQ(s.Spacing(), SculptSettings::kMinSpacing);
    s.SetSpacing(100.0f);
    CHECK_EQ(s.Spacing(), SculptSettings::kMaxSpacing);

    s.SetBrush(BrushType::Sculpt);
    s.SetStrength(2.0f);
    CHECK_EQ(s.Strength(), 1.0f);
    s.SetStrength(-1.0f);
    CHECK_EQ(s.Strength(), 0.0f);
    s.SetStrength(std::nanf(""));
    CHECK_EQ(s.Strength(), 0.0f);
}

TEST_CASE(settings_strength_is_per_brush) {
    SculptSettings& s = SculptSettings::Get();
    s.SetStrengthOf(BrushType::Sculpt, 0.7f);
    s.SetStrengthOf(BrushType::Smooth, 0.2f);
    s.SetBrush(BrushType::Sculpt);
    CHECK_NEAR(s.Strength(), 0.7f, 1e-6f);
    s.SetBrush(BrushType::Smooth);
    CHECK_NEAR(s.Strength(), 0.2f, 1e-6f);

    s.SetSubtract(true);
    s.SetUseFalloff(false);
    s.SetBackfaceCull(false);
    const sculpt::BrushSettings smooth = s.MakeBrush(BrushType::Smooth);
    CHECK(smooth.type == BrushType::Smooth);
    CHECK_NEAR(smooth.strength, 0.2f, 1e-6f);
    CHECK(smooth.subtract);
    CHECK(!smooth.useFalloff);
    CHECK(!smooth.backfaceCull);
    // Shift-smooth while Sculpt is active uses Smooth's own strength.
    s.SetBrush(BrushType::Sculpt);
    CHECK_NEAR(s.MakeBrush(BrushType::Smooth).strength, 0.2f, 1e-6f);

    s.SetSubtract(false);
    s.SetUseFalloff(true);
    s.SetBackfaceCull(true);
}

TEST_CASE(settings_reject_invalid_brush) {
    SculptSettings& s = SculptSettings::Get();
    s.SetBrush(BrushType::Inflate);
    s.SetBrush(BrushType::Count);
    CHECK(s.Brush() == BrushType::Inflate);
    s.SetBrush(static_cast<BrushType>(-3));
    CHECK(s.Brush() == BrushType::Inflate);
    s.SetBrush(BrushType::Sculpt);
}

TEST_CASE(settings_notify_listeners_once_per_change) {
    SculptSettings& s = SculptSettings::Get();
    s.SetSize(20.0f);
    CountingListener a;
    s.AddListener(&a);
    s.AddListener(&a);  // Duplicate registration is ignored.
    s.SetSize(21.0f);
    CHECK_EQ(a.calls, 1);
    s.SetSize(21.0f);  // No change, no notification.
    CHECK_EQ(a.calls, 1);
    s.SetSubtract(!s.Subtract());
    s.SetSubtract(!s.Subtract());
    CHECK_EQ(a.calls, 3);
    s.RemoveListener(&a);
    s.SetSize(22.0f);
    CHECK_EQ(a.calls, 3);
}

TEST_CASE(settings_listener_edge_cases) {
    SculptSettings& s = SculptSettings::Get();
    s.SetSize(50.0f);
    SelfRemovingListener first;
    CountingListener second;
    s.AddListener(&first);
    s.AddListener(&second);
    s.SetSize(51.0f);
    CHECK_EQ(first.calls, 1);
    CHECK_EQ(second.calls, 1);
    s.SetSize(52.0f);
    CHECK_EQ(first.calls, 1);  // It removed itself.
    CHECK_EQ(second.calls, 2);
    s.RemoveListener(&second);

    WritingListener writer;
    s.AddListener(&writer);
    s.SetSize(60.0f);  // Writer changes the size again inside the callback.
    CHECK_EQ(writer.calls, 1);
    CHECK_EQ(s.Size(), 61.0f);
    s.RemoveListener(&writer);
}
