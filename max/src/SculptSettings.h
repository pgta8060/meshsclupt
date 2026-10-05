// Global brush/tool settings shared by the panel, the viewport mode and MAXScript.
#pragma once

#include <vector>

#include "sculpt/brush.h"

class SculptSettings {
public:
    // Notified after any setting changes (from UI, MAXScript or code).
    class Listener {
    public:
        virtual ~Listener() = default;
        virtual void OnSculptSettingsChanged() = 0;
    };

    static constexpr float kMinSize = 1.0f;     // Brush radius in screen pixels.
    static constexpr float kMaxSize = 2000.0f;
    static constexpr float kMinSpacing = 0.02f;  // Fraction of the brush radius.
    static constexpr float kMaxSpacing = 4.0f;

    static SculptSettings& Get();

    sculpt::BrushType Brush() const { return brush_; }
    float Size() const { return size_; }
    float Strength() const { return StrengthOf(brush_); }
    float StrengthOf(sculpt::BrushType type) const;
    float Spacing() const { return spacing_; }
    bool Subtract() const { return subtract_; }
    bool UseFalloff() const { return useFalloff_; }
    bool BackfaceCull() const { return backfaceCull_; }

    // Setters clamp to the valid range and ignore NaN.
    void SetBrush(sculpt::BrushType type);
    void SetSize(float pixels);
    void SetStrength(float strength);  // Strength of the active brush.
    void SetStrengthOf(sculpt::BrushType type, float strength);
    void SetSpacing(float spacing);
    void SetSubtract(bool on);
    void SetUseFalloff(bool on);
    void SetBackfaceCull(bool on);

    // Core brush settings for one stroke. `type` may differ from the active
    // brush (e.g. Shift = temporary Smooth).
    sculpt::BrushSettings MakeBrush(sculpt::BrushType type) const;

    void AddListener(Listener* listener);
    void RemoveListener(Listener* listener);

private:
    SculptSettings();
    void Changed();

    sculpt::BrushType brush_ = sculpt::BrushType::Sculpt;
    float size_ = 50.0f;
    float spacing_ = 0.15f;
    bool subtract_ = false;
    bool useFalloff_ = true;
    bool backfaceCull_ = true;
    float strength_[static_cast<int>(sculpt::BrushType::Count)];
    std::vector<Listener*> listeners_;
    bool notifying_ = false;
};
