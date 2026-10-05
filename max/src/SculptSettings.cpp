#include "SculptSettings.h"

#include <algorithm>
#include <cmath>

namespace {

float ClampOr(float value, float lo, float hi, float fallback) {
    if (!std::isfinite(value)) return fallback;
    return std::min(std::max(value, lo), hi);
}

int Index(sculpt::BrushType type) {
    const int i = static_cast<int>(type);
    return (i >= 0 && i < static_cast<int>(sculpt::BrushType::Count)) ? i : 0;
}

}  // namespace

SculptSettings& SculptSettings::Get() {
    static SculptSettings settings;
    return settings;
}

SculptSettings::SculptSettings() {
    strength_[Index(sculpt::BrushType::Sculpt)] = 0.5f;
    strength_[Index(sculpt::BrushType::Smooth)] = 0.5f;
    strength_[Index(sculpt::BrushType::Inflate)] = 0.3f;
    strength_[Index(sculpt::BrushType::Pinch)] = 0.3f;
}

float SculptSettings::StrengthOf(sculpt::BrushType type) const { return strength_[Index(type)]; }

void SculptSettings::SetBrush(sculpt::BrushType type) {
    if (static_cast<int>(type) < 0 || type >= sculpt::BrushType::Count || type == brush_) return;
    brush_ = type;
    Changed();
}

void SculptSettings::SetSize(float pixels) {
    const float v = ClampOr(pixels, kMinSize, kMaxSize, size_);
    if (v == size_) return;
    size_ = v;
    Changed();
}

void SculptSettings::SetStrength(float strength) { SetStrengthOf(brush_, strength); }

void SculptSettings::SetStrengthOf(sculpt::BrushType type, float strength) {
    float& slot = strength_[Index(type)];
    const float v = ClampOr(strength, 0.0f, 1.0f, slot);
    if (v == slot) return;
    slot = v;
    Changed();
}

void SculptSettings::SetSpacing(float spacing) {
    const float v = ClampOr(spacing, kMinSpacing, kMaxSpacing, spacing_);
    if (v == spacing_) return;
    spacing_ = v;
    Changed();
}

void SculptSettings::SetSubtract(bool on) {
    if (on == subtract_) return;
    subtract_ = on;
    Changed();
}

void SculptSettings::SetUseFalloff(bool on) {
    if (on == useFalloff_) return;
    useFalloff_ = on;
    Changed();
}

void SculptSettings::SetBackfaceCull(bool on) {
    if (on == backfaceCull_) return;
    backfaceCull_ = on;
    Changed();
}

sculpt::BrushSettings SculptSettings::MakeBrush(sculpt::BrushType type) const {
    sculpt::BrushSettings s;
    s.type = type;
    s.strength = StrengthOf(type);
    s.subtract = subtract_;
    s.useFalloff = useFalloff_;
    s.backfaceCull = backfaceCull_;
    return s;
}

void SculptSettings::AddListener(Listener* listener) {
    if (listener && std::find(listeners_.begin(), listeners_.end(), listener) == listeners_.end())
        listeners_.push_back(listener);
}

void SculptSettings::RemoveListener(Listener* listener) {
    listeners_.erase(std::remove(listeners_.begin(), listeners_.end(), listener), listeners_.end());
}

void SculptSettings::Changed() {
    if (notifying_) return;  // A listener writing settings back must not recurse.
    notifying_ = true;
    const std::vector<Listener*> snapshot = listeners_;  // Listeners may unregister.
    for (Listener* l : snapshot)
        if (std::find(listeners_.begin(), listeners_.end(), l) != listeners_.end()) l->OnSculptSettingsChanged();
    notifying_ = false;
}
