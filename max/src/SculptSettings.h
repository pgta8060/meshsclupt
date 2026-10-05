// Tool settings shared by the floating UI, the viewport mode, hotkeys and
// MAXScript. Plain C++ (no 3ds Max dependency) so it is unit tested.
//
// Every setting is a property with a key, a range and a default, so UI rows,
// MAXScript access (SculptMesh.SetValue "brushSize" 40) and persistence all
// use the same table. Per-brush values (strength, Add/Sub, backface cull,
// layer mode) are stored per brush.
#pragma once

#include <array>
#include <string>
#include <vector>

#include "sculpt/brush.h"

enum class ToolMode : int { Select = 0, Sculpt = 1, Paint = 2 };
enum class MaskTool : int { PaintMask = 0, Rectangle = 1, Lasso = 2 };
enum class StrokeMode : int { Draw = 0, Stamp = 1, Drag = 2, ColorMix = 3, Scatter = 4 };

enum class Prop : int {
    ToolMode,      // ToolMode
    Brush,         // sculpt::BrushType of the active sculpt brush
    MaskTool,      // MaskTool (last selected)
    MaskDirect,    // Mask tool selected directly from the toolbar (vs. Sculpt/Paint)
    StrokeMode,    // StrokeMode
    BrushSize,     // Radius in screen pixels
    StrokeSpacing, // Fraction of the radius
    UseFalloff,
    FollowPath,
    LazyMouse,
    LazyAmount,    // 0..1 of the brush radius
    UseAlpha,      // Quick menu "Use Alpha Texture"
    AlphaMid,
    AlphaFade,
    ClayBorder,
    PolishHardness,
    AccuCurve,
    ScrapeOriginalPlane,
    ScrapeOriginalNormal,
    ScatterDensity,
    ScatterRadius,
    SizeJitter,
    AmountJitter,
    MirrorX,
    MirrorY,
    MirrorZ,
    RadialMirror,
    RadialCount,
    RadialAxis,
    CavityCoverage,
    AOCoverage,
    ShowMask,
    ShowGroups,
    AutoGroupMode, // sculpt::AutoGroupMode
    SculptMaterialPreview,
    ColorAR, ColorAG, ColorAB,
    ColorBR, ColorBG, ColorBB,
    MenusOpen,     // Floating Sculpt Mesh menus are shown with the Modify panel.
    MultiresUseMaterials,   // New levels keep material-ID borders sharp.
    MultiresUseSmoothing,   // New levels keep smoothing-group borders sharp.
    Autosmooth,             // Auto Smooth after level changes / topology tools (else faceted).
    AutosmoothAngle,        // Degrees.
    Count
};

enum class BrushProp : int { Strength = 0, Subtract, BackfaceCull, LayerMode, Count };

struct PropInfo {
    const char* key;  // Stable name used by MAXScript and the settings file.
    float minValue;
    float maxValue;
    float defaultValue;
    bool isBool;
    bool isInt;
};

const PropInfo& propInfo(Prop prop);
const char* brushPropKey(BrushProp prop);
// Finds a property by key (case-insensitive). Returns false if unknown.
bool findProp(const std::string& key, Prop& out);
bool findBrushProp(const std::string& key, BrushProp& out);

class SculptSettings {
public:
    class Listener {
    public:
        virtual ~Listener() = default;
        virtual void OnSculptSettingsChanged() = 0;
    };

    static SculptSettings& Get();

    float Value(Prop prop) const { return values_[static_cast<int>(prop)]; }
    bool Bool(Prop prop) const { return Value(prop) != 0.0f; }
    int Int(Prop prop) const;
    // Clamps to the property range (ints rounded, bools 0/1); NaN is ignored.
    void Set(Prop prop, float value);
    void SetBool(Prop prop, bool value) { Set(prop, value ? 1.0f : 0.0f); }

    float BrushValue(sculpt::BrushType brush, BrushProp prop) const;
    void SetBrushValue(sculpt::BrushType brush, BrushProp prop, float value);

    // --- Convenience for the active brush ---------------------------------------
    ToolMode Mode() const { return static_cast<ToolMode>(Int(Prop::ToolMode)); }
    sculpt::BrushType Brush() const { return static_cast<sculpt::BrushType>(Int(Prop::Brush)); }
    void SetBrush(sculpt::BrushType brush);  // Ignores brushes that are not available.
    float Size() const { return Value(Prop::BrushSize); }
    float Spacing() const { return Value(Prop::StrokeSpacing); }
    float Strength() const { return BrushValue(Brush(), BrushProp::Strength); }
    void SetStrength(float v) { SetBrushValue(Brush(), BrushProp::Strength, v); }
    bool Subtract() const { return BrushValue(Brush(), BrushProp::Subtract) != 0.0f; }
    void SetSubtract(bool on) { SetBrushValue(Brush(), BrushProp::Subtract, on ? 1.0f : 0.0f); }
    bool BackfaceCull() const { return BrushValue(Brush(), BrushProp::BackfaceCull) != 0.0f; }

    // Core brush settings for one stroke of `type` (may differ from the
    // active brush, e.g. Shift = temporary Smooth). Alpha is set by the caller.
    sculpt::BrushSettings MakeBrush(sculpt::BrushType type) const;

    // --- Palette & libraries ------------------------------------------------------
    // Brushes in the order shown in the Sculpting palette (keys 1-5 = first five).
    const std::vector<sculpt::BrushType>& PaletteOrder() const { return palette_; }
    void SetPaletteOrder(std::vector<sculpt::BrushType> order);  // Sanitised: each available brush once.
    void MovePaletteItem(int from, int to);

    // Active alpha: "" none, "builtin:N", or an image file path.
    const std::string& AlphaId() const { return alphaId_; }
    void SetAlphaId(const std::string& id);
    const std::string& AlphaLibraryFolder() const { return alphaFolder_; }
    void SetAlphaLibraryFolder(const std::string& folder);
    const std::vector<std::string>& AlphaFavorites() const { return alphaFavorites_; }
    bool IsAlphaFavorite(const std::string& path) const;
    void ToggleAlphaFavorite(const std::string& path);
    // Alphas page category: "builtin", "favorites", or a library sub-folder name ("." = root).
    const std::string& AlphaCategory() const { return alphaCategory_; }
    void SetAlphaCategory(const std::string& category);

    // --- Persistence (UTF-8 "key=value" lines) -------------------------------------
    std::string ToText() const;
    void FromText(const std::string& text);  // Unknown keys are ignored.
    void ResetToDefaults();

    void AddListener(Listener* listener);
    void RemoveListener(Listener* listener);

private:
    SculptSettings();
    void Changed();

    std::array<float, static_cast<int>(Prop::Count)> values_{};
    std::array<std::array<float, static_cast<int>(BrushProp::Count)>, static_cast<int>(sculpt::BrushType::Count)> brush_{};
    std::vector<sculpt::BrushType> palette_;
    std::string alphaId_;
    std::string alphaFolder_;
    std::string alphaCategory_;
    std::vector<std::string> alphaFavorites_;
    std::vector<Listener*> listeners_;
    bool notifying_ = false;
};

// Brushes offered in the Sculpting palette by default, in documentation order.
std::vector<sculpt::BrushType> DefaultPaletteOrder();
