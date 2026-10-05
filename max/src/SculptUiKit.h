// Small owner-drawn UI toolkit for the floating Sculpt Mesh menus: theme
// colours taken from 3ds Max, DPI scaling, procedural brush icons, a
// non-activating popup window base class and a property-row list (sections,
// sliders, check boxes, buttons, choices, colour swatches).
//
// The floating windows never take keyboard focus (WS_EX_NOACTIVATE), so the
// viewport keeps receiving shortcuts while the menus are used.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <max.h>

#include "sculpt/brush.h"

namespace ui {

// --- Metrics & theme -------------------------------------------------------------------
int Px(int value);  // Scales design pixels by the 3ds Max UI scale.

struct Theme {
    COLORREF background;  // Window background.
    COLORREF panel;       // Row background / section body.
    COLORREF header;      // Section header.
    COLORREF text;
    COLORREF textDim;
    COLORREF button;
    COLORREF buttonHot;
    COLORREF accent;      // Active / checked.
    COLORREF accentText;
    COLORREF border;
    COLORREF track;       // Slider track.
};
const Theme& GetTheme();
void RefreshTheme();  // Re-reads the 3ds Max colour scheme.
HFONT Font();
HFONT BoldFont();
COLORREF Blend(COLORREF a, COLORREF b, float t);

// --- Drawing helpers ---------------------------------------------------------------------
void Fill(HDC dc, const RECT& r, COLORREF color);
void Frame(HDC dc, const RECT& r, COLORREF color);
void RoundBox(HDC dc, const RECT& r, int radius, COLORREF fill, COLORREF border);
void Text(HDC dc, const RECT& r, const std::wstring& text, COLORREF color, UINT format, HFONT font = nullptr);

enum class Glyph {
    Select, PaintMask, Rectangle, Lasso, Draw, Stamp, Drag, ColorMix, Scatter, NoAlpha, Swap, DropDown,
};
void DrawGlyph(HDC dc, const RECT& r, Glyph glyph, COLORREF color);

// Shaded clay sphere showing the brush's effect (palette and toolbar icons).
void DrawBrushIcon(HDC dc, const RECT& r, sculpt::BrushType brush, COLORREF background);
// Grayscale image (size*size bytes), e.g. an alpha thumbnail.
void DrawGrayImage(HDC dc, const RECT& r, const unsigned char* gray, int size);
// 0x00RRGGBB pixels, top row first.
void DrawPixels(HDC dc, int x, int y, int width, int height, const std::uint32_t* pixels);
void ClearIconCache();

// Pop-up menu at a screen point; returns the chosen id or 0.
struct MenuItem {
    int id;  // 0 = separator.
    std::wstring text;
    bool checked = false;
    bool enabled = true;
};
int PopupMenu(const std::vector<MenuItem>& items, POINT screen);

// Win32 colour dialog; returns false if cancelled.
bool PickColor(float rgb[3]);

// --- Window base ---------------------------------------------------------------------------
class FloatingWindow {
public:
    FloatingWindow() = default;
    FloatingWindow(const FloatingWindow&) = delete;
    FloatingWindow& operator=(const FloatingWindow&) = delete;
    virtual ~FloatingWindow();

    bool Create(HWND owner);
    void Destroy();
    HWND Hwnd() const { return hwnd_; }
    bool Visible() const { return hwnd_ && IsWindowVisible(hwnd_); }
    void Show(bool show);
    void Invalidate();
    void SetBounds(int x, int y, int width, int height);
    RECT ClientRect() const;

protected:
    virtual void Paint(HDC dc, const RECT& client) = 0;
    virtual void MouseDown(int /*x*/, int /*y*/, bool /*doubleClick*/) {}
    virtual void MouseMove(int /*x*/, int /*y*/, bool /*captured*/) {}
    virtual void MouseUp(int /*x*/, int /*y*/) {}
    virtual void RightClick(int /*x*/, int /*y*/) {}
    virtual void MouseWheel(int /*delta*/, int /*x*/, int /*y*/) {}
    virtual void MouseLeave() {}
    virtual void CaptureLost() {}
    virtual void Timer(UINT_PTR /*id*/) {}
    // Other messages; set `handled` to true to stop default processing.
    virtual LRESULT Message(UINT /*msg*/, WPARAM /*wp*/, LPARAM /*lp*/, bool& handled) {
        handled = false;
        return 0;
    }

    HWND hwnd_ = nullptr;
    int cornerRadius_ = 0;  // Rounded window corners (pixels), applied by SetBounds.

private:
    static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);
    bool tracking_ = false;
};

// --- Property rows -----------------------------------------------------------------------------
class RowList {
public:
    using Visible = std::function<bool()>;
    struct Button {
        std::wstring label;
        std::function<void()> click;
        std::function<bool()> checked;  // Optional: toggle look.
        std::function<bool()> enabled;  // Optional.
    };
    struct SliderSpec {
        std::wstring label;
        std::function<float()> get;
        std::function<void(float)> set;
        float min = 0.0f;
        float max = 1.0f;        // Slider range (typed values may exceed it up to typeMax).
        float typeMax = 1.0f;
        float defaultValue = 0.0f;  // Right-click resets.
        int decimals = 2;
        bool quadratic = false;  // Finer control near the minimum (brush size).
    };

    void Clear() { rows_.clear(); }
    // Rows added after a section belong to it until the next section.
    void Section(const std::wstring& title, bool expanded);
    void Buttons(std::vector<Button> buttons, Visible visible = {});
    void Slider(SliderSpec spec, Visible visible = {});
    void Check(const std::wstring& label, std::function<bool()> get, std::function<void(bool)> set, Visible visible = {});
    void Choice(const std::wstring& label, std::vector<std::wstring> options, std::function<int()> get,
                std::function<void(int)> set, Visible visible = {});
    void Colors(std::function<void(int, float[3])> get, std::function<void(int, const float[3])> set,
                std::function<void()> swap, Visible visible = {});
    void Note(std::function<std::wstring()> text, Visible visible = {});

    int ContentHeight(int width) const;
    // Paints into `area` of the host window; input coordinates below are
    // host-client coordinates relative to the last painted area.
    void Paint(HDC dc, const RECT& area, int scroll) const;

    // Return true when the host must redraw.
    bool MouseDown(HWND host, int x, int y, bool doubleClick, int scroll);
    bool MouseMove(int x, int y, bool captured, int scroll);
    bool MouseUp(int x, int y, int scroll);
    bool RightClick(HWND host, int x, int y, int scroll);
    bool MouseLeave();
    void CaptureLost() { drag_ = -1; }
    // The host forwards kFinishEditMessage here.
    static constexpr UINT kFinishEditMessage = WM_APP + 0x5C1;
    void FinishEdit(bool commit);
    bool Editing() const { return edit_ != nullptr; }

private:
    enum class Kind { Section, Buttons, Slider, Check, Choice, Colors, Note };
    struct Row {
        Kind kind;
        std::wstring label;
        Visible visible;
        int section = -1;          // Index of the owning section row.
        mutable bool expanded = true;
        std::vector<Button> buttons;
        SliderSpec slider;
        std::function<bool()> getBool;
        std::function<void(bool)> setBool;
        std::vector<std::wstring> options;
        std::function<int()> getIndex;
        std::function<void(int)> setIndex;
        std::function<void(int, float[3])> getColor;
        std::function<void(int, const float[3])> setColor;
        std::function<void()> swap;
        std::function<std::wstring()> text;
    };
    struct Placed {
        int row;
        RECT rect;
    };

    void Add(Row row);
    std::vector<Placed> Layout(int width) const;
    int RowHeight(const Row& row) const;
    int HitRow(int x, int y, int scroll, RECT* rect) const;
    static RECT SliderTrack(const RECT& r);
    static RECT SliderValue(const RECT& r);
    static float ToT(const SliderSpec& s, float v);
    static float FromT(const SliderSpec& s, float t);
    void SetSliderFromX(const Row& row, const RECT& rect, int x);
    void BeginEdit(HWND host, int row, const RECT& valueRect);
    static int ButtonAt(const Row& row, const RECT& r, int x);
    static RECT ButtonRect(const Row& row, const RECT& r, int index);
    static RECT ColorRect(const RECT& r, int which);  // 0 = A, 1 = swap, 2 = B

    std::vector<Row> rows_;
    mutable int width_ = 0;
    mutable POINT origin_ = {0, 0};
    int drag_ = -1;          // Row whose slider is being dragged.
    RECT dragRect_{};
    int hotRow_ = -1;
    int hotButton_ = -1;
    int pressedRow_ = -1;
    int pressedButton_ = -1;
    HWND edit_ = nullptr;
    HWND editHost_ = nullptr;
    HWND editPrevFocus_ = nullptr;
    int editRow_ = -1;
};

}  // namespace ui
