// The paint / Displace stencil: an image placed over the viewport in screen
// space. It is drawn by a click-through layered window above the viewport,
// and while S is held the mouse transforms it (LMB rotate, RMB scale,
// MMB move) through a thread mouse hook, so 3ds Max never sees those clicks.
#pragma once

#include <string>

#include <max.h>

#include "sculpt/paint.h"

class Stencil {
public:
    static Stencil& Get();

    bool Load(const std::wstring& path, MSTR* error = nullptr);
    void Clear();
    bool Loaded() const { return !image_.empty(); }
    const std::wstring& Path() const { return path_; }
    void ResetTransform();

    // Shown while sculpt mode runs; the hook only exists while shown.
    void SetActive(bool active);
    // The viewport the stencil sits over (follows the viewport being used).
    void SetViewport(HWND viewport);
    HWND Viewport() const { return viewport_; }
    // Re-reads opacity and the viewport placement.
    void Refresh();

    // Stencil at viewport pixel (x, y): colour and coverage; false outside the image.
    bool Sample(float x, float y, sculpt::Vec3& rgb, float& alpha) const;
    // True while S is held (the hook owns the mouse buttons then).
    static bool KeyHeld();

    // Hook entry (public for the hook procedure).
    bool HandleMouse(WPARAM message, const MOUSEHOOKSTRUCT& info);

private:
    Stencil() = default;
    void EnsureWindow();
    void Redraw();
    void UpdateHook();

    sculpt::Image image_;
    std::wstring path_;
    float centerX_ = 0.0f, centerY_ = 0.0f;  // Viewport pixels.
    float width_ = 0.0f;                     // Image width on screen (pixels).
    float angle_ = 0.0f;                     // Radians.
    bool placed_ = false;
    bool active_ = false;
    HWND viewport_ = nullptr;
    HWND window_ = nullptr;
    HHOOK hook_ = nullptr;
    enum class Drag { None, Rotate, Scale, Move } drag_ = Drag::None;
    POINT dragStart_ = {0, 0};
    float startAngle_ = 0.0f, startWidth_ = 0.0f, startX_ = 0.0f, startY_ = 0.0f;
};
