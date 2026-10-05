#include "SculptUI.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <shellapi.h>

#include "AlphaLibrary.h"
#include "DisplaceRollout.h"
#include "MultiresRollout.h"
#include "ProfileEditor.h"
#include "SculptCommands.h"
#include "SculptMeshObject.h"
#include "SculptMode.h"
#include "SculptSettings.h"
#include "SculptUiKit.h"
#include "Stencil.h"

using sculpt::BrushType;
using namespace ui;

namespace {

constexpr UINT_PTR kLayoutTimer = 1;
constexpr UINT_PTR kQuickMenuTimer = 2;

SculptSettings& S() { return SculptSettings::Get(); }

std::wstring Widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(std::max(n, 0)), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &out[0], n);
    return out;
}

std::string Narrow(const std::wstring& s) {
    if (s.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(std::max(n, 0)), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &out[0], n, nullptr, nullptr);
    return out;
}

std::wstring BrushName(BrushType b) { return Widen(sculpt::brushInfo(b).name); }

bool MaskDirect() { return S().Bool(Prop::MaskDirect); }
MaskTool CurrentMaskTool() { return static_cast<MaskTool>(S().Int(Prop::MaskTool)); }
bool MaskShapeDirect() { return MaskDirect() && CurrentMaskTool() != MaskTool::PaintMask; }

// The brush whose settings Brush Settings and the Quick Menu show.
BrushType ContextBrush() { return MaskDirect() ? BrushType::MaskPaint : S().Brush(); }
bool BrushContext() { return !MaskShapeDirect(); }
bool AlphaActive() { return S().Bool(Prop::UseAlpha) && !S().AlphaId().empty(); }
bool PaintContext() { return !MaskDirect() && S().Mode() == ToolMode::Paint; }
bool SculptContext() { return BrushContext() && !PaintContext(); }
bool Is(BrushType b) { return SculptContext() && ContextBrush() == b; }
bool PaintToolIs(sculpt::PaintTool tool) { return PaintContext() && S().Int(Prop::PaintTool) == static_cast<int>(tool); }
bool CutBrush() { return Is(BrushType::Clip) || Is(BrushType::Cutter) || Is(BrushType::Slice); }
// Brushes driven by their own gesture rather than spaced dabs.
bool GestureBrush() { return CutBrush() || Is(BrushType::Pose) || Is(BrushType::Cloth) || Is(BrushType::CurveTube); }
bool DabContext() {
    return BrushContext() && !GestureBrush() && !PaintToolIs(sculpt::PaintTool::Fill) && !PaintToolIs(sculpt::PaintTool::Gradient);
}
bool SizeContext() { return BrushContext() && !CutBrush(); }
bool StrengthContext() {
    return SculptContext() && !CutBrush() && !Is(BrushType::FaceGroups) && !Is(BrushType::Pose) && !Is(BrushType::CurveTube);
}
// Layers show the stack of the working mode.
bool PaintLayersShown() { return S().Mode() == ToolMode::Paint; }
std::wstring SectionName() {
    Interface* core = GetCOREInterface();
    const ULONG handle = SculptMode::Get().SectionNode();
    INode* node = handle && core ? core->GetINodeByHandle(handle) : nullptr;
    return node ? std::wstring(L"Section: ") + node->GetName() : std::wstring(L"Section: circle (Tube Sides)");
}
bool ScatterContext() {
    const BrushType b = ContextBrush();
    return BrushContext() && b != BrushType::Move && b != BrushType::SnakeHook &&
           static_cast<StrokeMode>(S().Int(Prop::StrokeMode)) == StrokeMode::Scatter;
}

RowList::SliderSpec PropSlider(const wchar_t* label, Prop prop, int decimals, float sliderMax = -1.0f,
                               bool quadratic = false) {
    const PropInfo& info = propInfo(prop);
    RowList::SliderSpec s;
    s.label = label;
    s.get = [prop] { return S().Value(prop); };
    s.set = [prop](float v) { S().Set(prop, v); };
    s.min = info.minValue;
    s.max = sliderMax > 0.0f ? sliderMax : info.maxValue;
    s.typeMax = info.maxValue;
    s.defaultValue = info.defaultValue;
    s.decimals = decimals;
    s.quadratic = quadratic;
    return s;
}

RowList::SliderSpec StrengthSlider() {
    RowList::SliderSpec s;
    s.label = L"Brush Strength";
    s.get = [] { return S().BrushValue(ContextBrush(), BrushProp::Strength); };
    s.set = [](float v) { S().SetBrushValue(ContextBrush(), BrushProp::Strength, v); };
    s.min = 0.0f;
    s.max = 1.0f;
    s.typeMax = 1.0f;
    s.defaultValue = 0.5f;
    s.decimals = 2;
    return s;
}

void AddPropCheck(RowList& rows, const wchar_t* label, Prop prop, RowList::Visible visible = {}) {
    rows.Check(label, [prop] { return S().Bool(prop); }, [prop](bool on) { S().SetBool(prop, on); }, std::move(visible));
}

void AddBrushCheck(RowList& rows, const wchar_t* label, BrushProp prop, RowList::Visible visible = {}) {
    rows.Check(
        label, [prop] { return S().BrushValue(ContextBrush(), prop) != 0.0f; },
        [prop](bool on) { S().SetBrushValue(ContextBrush(), prop, on ? 1.0f : 0.0f); }, std::move(visible));
}

void GetColor(int which, float rgb[3]) {
    const int base = static_cast<int>(which == 0 ? Prop::ColorAR : Prop::ColorBR);
    for (int k = 0; k < 3; ++k) rgb[k] = S().Value(static_cast<Prop>(base + k));
}

void SetColor(int which, const float rgb[3]) {
    const int base = static_cast<int>(which == 0 ? Prop::ColorAR : Prop::ColorBR);
    for (int k = 0; k < 3; ++k) S().Set(static_cast<Prop>(base + k), rgb[k]);
}

void SwapColors() {
    float a[3], b[3];
    GetColor(0, a);
    GetColor(1, b);
    SetColor(0, b);
    SetColor(1, a);
}

void RgbToHsv(const float rgb[3], float& h, float& s, float& v) {
    const float mx = std::max({rgb[0], rgb[1], rgb[2]}), mn = std::min({rgb[0], rgb[1], rgb[2]});
    v = mx;
    const float d = mx - mn;
    s = mx > 0.0f ? d / mx : 0.0f;
    if (d <= 0.0f) {
        h = 0.0f;
        return;
    }
    if (mx == rgb[0])
        h = (rgb[1] - rgb[2]) / d / 6.0f;
    else if (mx == rgb[1])
        h = ((rgb[2] - rgb[0]) / d + 2.0f) / 6.0f;
    else
        h = ((rgb[0] - rgb[1]) / d + 4.0f) / 6.0f;
    if (h < 0.0f) h += 1.0f;
}

void HsvToRgb(float h, float s, float v, float rgb[3]) {
    h = (h - std::floor(h)) * 6.0f;
    const int i = static_cast<int>(h) % 6;
    const float f = h - std::floor(h);
    const float p = v * (1.0f - s), q = v * (1.0f - s * f), t = v * (1.0f - s * (1.0f - f));
    const float table[6][3] = {{v, t, p}, {q, v, p}, {p, v, t}, {p, q, v}, {t, p, v}, {v, p, q}};
    for (int k = 0; k < 3; ++k) rgb[k] = table[i][k];
}

std::uint32_t Pack(const float rgb[3]) {
    const auto to8 = [](float c) { return static_cast<std::uint32_t>(std::lround(std::min(std::max(c, 0.0f), 1.0f) * 255.0f)); };
    return (to8(rgb[0]) << 16) | (to8(rgb[1]) << 8) | to8(rgb[2]);
}

void RunOp(SculptCommands::Op op) { SculptCommands::Run(op); }

SculptMeshObject* Edited() { return SculptMeshObject::EditedObject(); }
int ActiveLayer() { return Edited() ? Edited()->Layers().active : -1; }
bool LayersEditable() { return Edited() && Edited()->LayersUsable(); }
bool LayerSelected() { return LayersEditable() && ActiveLayer() >= 0; }
const sculpt::SculptLayer* SelectedLayer() {
    const int a = ActiveLayer();
    return a >= 0 && a < static_cast<int>(Edited()->Layers().layers.size()) ? &Edited()->Layers().layers[static_cast<std::size_t>(a)] : nullptr;
}
void LayerCommand(bool (SculptMeshObject::*command)()) {
    if (SculptMeshObject* object = Edited()) (object->*command)();
    if (Interface* core = GetCOREInterface()) core->RedrawViews(core->GetTime());
}

void LayoutWindows();

// --- Left mode toolbar ---------------------------------------------------------------------------

class ToolbarWindow : public FloatingWindow {
public:
    static constexpr int kSlots = 5;
    ToolbarWindow() { cornerRadius_ = Px(10); }
    int Width() const { return Px(52); }
    int Height() const { return Px(8) + kSlots * Px(46); }

protected:
    RECT SlotRect(int i) const { return RECT{Px(6), Px(6) + i * Px(46), Px(46), Px(6) + i * Px(46) + Px(40)}; }

    int SlotAt(int x, int y) const {
        for (int i = 0; i < kSlots; ++i) {
            const RECT r = SlotRect(i);
            if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return i;
        }
        return -1;
    }

    bool SlotActive(int i) const {
        const bool sculpting = SculptCommands::IsSculpting();
        if (i == 0) return !sculpting;
        if (i == 1) return sculpting && !MaskDirect();
        if (i == 2) return sculpting && MaskDirect();
        return false;
    }

    void Paint(HDC dc, const RECT& client) override {
        const Theme& t = GetTheme();
        Fill(dc, client, t.background);
        for (int i = 0; i < kSlots; ++i) {
            const RECT r = SlotRect(i);
            const bool active = SlotActive(i);
            const COLORREF fill = active ? t.accent : (hot_ == i ? t.buttonHot : t.panel);
            RoundBox(dc, r, Px(8), pressed_ == i ? Blend(fill, t.accent, 0.4f) : fill, t.border);
            RECT inner = r;
            InflateRect(&inner, -Px(7), -Px(7));
            const COLORREF ink = active ? t.accentText : t.text;
            switch (i) {
                case 0: DrawGlyph(dc, inner, Glyph::Select, ink); break;
                case 1:
                    if (S().Mode() == ToolMode::Paint)
                        DrawPaintIcon(dc, inner, S().Int(Prop::PaintTool), fill);
                    else
                        DrawBrushIcon(dc, inner, S().Brush(), fill);
                    break;
                case 2: {
                    const Glyph g[] = {Glyph::PaintMask, Glyph::Rectangle, Glyph::Lasso};
                    DrawGlyph(dc, inner, g[std::min(std::max(S().Int(Prop::MaskTool), 0), 2)], ink);
                    break;
                }
                case 3: {
                    const Glyph g[] = {Glyph::Draw, Glyph::Stamp, Glyph::Drag, Glyph::ColorMix, Glyph::Scatter};
                    DrawGlyph(dc, inner, g[std::min(std::max(S().Int(Prop::StrokeMode), 0), 4)], ink);
                    break;
                }
                case 4: {
                    const std::string& id = S().AlphaId();
                    const int size = inner.right - inner.left;
                    const std::vector<unsigned char>* thumb = id.empty() ? nullptr : AlphaLibrary::Get().Thumbnail(id, size);
                    if (thumb)
                        DrawGrayImage(dc, inner, thumb->data(), size);
                    else
                        DrawGlyph(dc, inner, Glyph::NoAlpha, t.textDim);
                    break;
                }
                default: break;
            }
            if (i >= 1 && i <= 3) {  // Flyout marker.
                const POINT tri[] = {{r.right - Px(3), r.bottom - Px(9)}, {r.right - Px(3), r.bottom - Px(3)},
                                     {r.right - Px(9), r.bottom - Px(3)}};
                HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(DC_BRUSH));
                HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
                SetDCBrushColor(dc, ink);
                Polygon(dc, tri, 3);
                SelectObject(dc, oldBrush);
                SelectObject(dc, oldPen);
            }
        }
    }

    void MouseDown(int x, int y, bool /*doubleClick*/) override {
        pressed_ = SlotAt(x, y);
        Invalidate();
    }

    void MouseUp(int x, int y) override {
        const int slot = SlotAt(x, y), pressed = pressed_;
        pressed_ = -1;
        Invalidate();
        if (slot >= 0 && slot == pressed) Activate(slot);
    }

    void RightClick(int x, int y) override {
        const int slot = SlotAt(x, y);
        if (slot >= 1 && slot <= 3) Activate(slot);
    }

    void MouseMove(int x, int y, bool /*captured*/) override {
        const int hot = SlotAt(x, y);
        if (hot != hot_) {
            hot_ = hot;
            Invalidate();
        }
    }

    void MouseLeave() override {
        hot_ = -1;
        Invalidate();
    }

    void Timer(UINT_PTR id) override {
        if (id == kLayoutTimer) LayoutWindows();
    }

private:
    POINT FlyoutPoint(int slot) const {
        const RECT r = SlotRect(slot);
        POINT p = {r.right + Px(6), r.top};
        ClientToScreen(hwnd_, &p);
        return p;
    }

    void Activate(int slot) {
        switch (slot) {
            case 0:
                SculptCommands::StopSculpting();
                break;
            case 1: {
                const bool paint = S().Mode() == ToolMode::Paint;
                const int chosen = PopupMenu({{1, L"Sculpt", !MaskDirect() && !paint, true}, {2, L"Paint", !MaskDirect() && paint, true}},
                                             FlyoutPoint(slot));
                if (chosen == 1) SculptCommands::SelectSculptMode();
                if (chosen == 2) SculptCommands::SelectPaintMode();
                break;
            }
            case 2: {
                const int tool = S().Int(Prop::MaskTool);
                const bool direct = MaskDirect();
                const int chosen = PopupMenu({{1, L"Paint Mask", direct && tool == 0, true},
                                              {2, L"Rectangle", direct && tool == 1, true},
                                              {3, L"Lasso", direct && tool == 2, true}},
                                             FlyoutPoint(slot));
                if (chosen > 0) SculptCommands::SelectMaskTool(static_cast<MaskTool>(chosen - 1));
                break;
            }
            case 3: {
                const int mode = S().Int(Prop::StrokeMode);
                const int chosen = PopupMenu({{1, L"Draw", mode == 0, true},
                                              {2, L"Stamp", mode == 1, true},
                                              {3, L"Drag", mode == 2, true},
                                              {4, L"Color Mix  (Paint)", mode == 3, true},
                                              {5, L"Scatter", mode == 4, true}},
                                             FlyoutPoint(slot));
                if (chosen > 0) SculptCommands::SetStrokeMode(static_cast<StrokeMode>(chosen - 1));
                break;
            }
            case 4:
                SculptUI::ShowAlphasPage();
                break;
            default:
                break;
        }
        Invalidate();
    }

    int hot_ = -1;
    int pressed_ = -1;
};

// --- Bottom palette --------------------------------------------------------------------------------

class PaletteWindow : public FloatingWindow {
public:
    enum class Page { Sculpting = 0, Paint = 1, Alphas = 2 };
    PaletteWindow() { cornerRadius_ = Px(10); }
    int Height() const { return Px(100); }
    void SetPage(Page page) {
        if (page == page_) return;
        page_ = page;
        scroll_ = 0;
        Invalidate();
    }

protected:
    struct Item {
        bool isBrush = true;
        BrushType brush = BrushType::Sculpt;
        std::string alpha;  // "" = No Alpha.
        int paintTool = -1;  // Paint page.
    };

    std::vector<Item> Items() const {
        std::vector<Item> items;
        if (page_ == Page::Sculpting) {
            for (BrushType b : S().PaletteOrder()) items.push_back({true, b, std::string(), -1});
            return items;
        }
        if (page_ == Page::Paint) {
            for (int tool : S().PaintPaletteOrder()) items.push_back({false, BrushType::Sculpt, std::string(), tool});
            return items;
        }
        items.push_back({false, BrushType::Sculpt, std::string(), -1});
        for (const std::string& id : AlphaLibrary::Get().Items("builtin")) items.push_back({false, BrushType::Sculpt, id, -1});
        const std::string& category = S().AlphaCategory();
        if (category != "builtin")
            for (const std::string& id : AlphaLibrary::Get().Items(category)) items.push_back({false, BrushType::Sculpt, id, -1});
        return items;
    }
    bool Reorderable() const { return page_ == Page::Sculpting || page_ == Page::Paint; }

    int ItemWidth() const { return Px(64); }
    RECT TabRect(int i) const { return RECT{Px(8) + i * Px(84), Px(4), Px(8) + i * Px(84) + Px(80), Px(24)}; }
    RECT CategoryRect() const {
        const RECT c = ClientRect();
        return RECT{Px(268), Px(4), std::min<LONG>(Px(268) + Px(170), c.right - Px(96)), Px(24)};
    }
    RECT BrowseRect() const {
        const RECT category = CategoryRect();
        return RECT{category.right + Px(6), Px(4), category.right + Px(6) + Px(84), Px(24)};
    }
    RECT ItemsArea() const {
        const RECT c = ClientRect();
        return RECT{Px(6), Px(28), c.right - Px(6), c.bottom - Px(4)};
    }
    RECT ItemRect(int i) const {
        const RECT area = ItemsArea();
        const int x = area.left + i * ItemWidth() - scroll_;
        return RECT{x, area.top, x + ItemWidth(), area.bottom};
    }
    int ItemAt(int x, int y) const {
        const RECT area = ItemsArea();
        if (x < area.left || x >= area.right || y < area.top || y >= area.bottom) return -1;
        const int i = (x - area.left + scroll_) / ItemWidth();
        return i >= 0 && i < static_cast<int>(Items().size()) ? i : -1;
    }
    void ClampScroll() {
        const RECT area = ItemsArea();
        const int content = static_cast<int>(Items().size()) * ItemWidth();
        scroll_ = std::max(0, std::min(scroll_, content - static_cast<int>(area.right - area.left)));
    }
    bool IsActive(const Item& item) const {
        if (item.paintTool >= 0) return !MaskDirect() && S().Mode() == ToolMode::Paint && S().Int(Prop::PaintTool) == item.paintTool;
        if (item.isBrush) return !MaskDirect() && S().Mode() != ToolMode::Paint && S().Brush() == item.brush;
        return S().AlphaId() == item.alpha;
    }

    void Paint(HDC dc, const RECT& client) override {
        const Theme& t = GetTheme();
        Fill(dc, client, t.background);
        const wchar_t* tabs[] = {L"Sculpting", L"Paint", L"Alphas"};
        for (int i = 0; i < 3; ++i) {
            const RECT r = TabRect(i);
            const bool active = static_cast<int>(page_) == i;
            RoundBox(dc, r, Px(6), active ? t.accent : (hotTab_ == i ? t.buttonHot : t.panel), t.border);
            Text(dc, r, tabs[i], active ? t.accentText : t.text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        if (page_ == Page::Alphas) {
            const RECT c = CategoryRect();
            RoundBox(dc, c, Px(6), t.button, t.border);
            std::wstring name = L"Built-in";
            for (const AlphaLibrary::Category& category : AlphaLibrary::Get().Categories())
                if (category.id == S().AlphaCategory()) name = category.name;
            RECT text = {c.left + Px(6), c.top, c.right - Px(16), c.bottom};
            Text(dc, text, name, t.text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            RECT arrow = {c.right - Px(16), c.top, c.right - Px(2), c.bottom};
            DrawGlyph(dc, arrow, Glyph::DropDown, t.text);
            const RECT b = BrowseRect();
            RoundBox(dc, b, Px(6), t.button, t.border);
            Text(dc, b, L"Browse...", t.text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else {
            RECT hint = {Px(268), Px(4), client.right - Px(8), Px(24)};
            Text(dc, hint, page_ == Page::Paint ? L"Keys 1-5 pick the first five Paint tools  \x2022  drag to reorder"
                                                : L"Keys 1-5 pick the first five brushes  \x2022  drag to reorder",
                 t.textDim,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }

        const RECT area = ItemsArea();
        const int saved = SaveDC(dc);
        IntersectClipRect(dc, area.left, area.top, area.right, area.bottom);
        const std::vector<Item> items = Items();
        const int iconSize = Px(42);
        for (int i = 0; i < static_cast<int>(items.size()); ++i) {
            const RECT cell = ItemRect(i);
            if (cell.right < area.left || cell.left > area.right) continue;
            const Item& item = items[static_cast<std::size_t>(i)];
            const bool active = IsActive(item);
            RECT box = cell;
            InflateRect(&box, -Px(2), 0);
            if (active || hot_ == i) RoundBox(dc, box, Px(8), active ? Blend(t.background, t.accent, 0.45f) : t.panel, active ? t.accent : t.border);
            const COLORREF behind = active ? Blend(t.background, t.accent, 0.45f) : (hot_ == i ? t.panel : t.background);
            RECT icon = {cell.left + (ItemWidth() - iconSize) / 2, cell.top + Px(3), cell.left + (ItemWidth() + iconSize) / 2,
                         cell.top + Px(3) + iconSize};
            std::wstring label;
            if (item.paintTool >= 0) {
                DrawPaintIcon(dc, icon, item.paintTool, behind);
                label = Widen(paintToolName(item.paintTool));
            } else if (item.isBrush) {
                DrawBrushIcon(dc, icon, item.brush, behind);
                label = BrushName(item.brush);
            } else {
                const std::vector<unsigned char>* thumb =
                    item.alpha.empty() ? nullptr : AlphaLibrary::Get().Thumbnail(item.alpha, iconSize);
                if (thumb)
                    DrawGrayImage(dc, icon, thumb->data(), iconSize);
                else
                    DrawGlyph(dc, icon, Glyph::NoAlpha, t.textDim);
                label = AlphaLibrary::DisplayName(item.alpha);
                if (!item.alpha.empty() && S().IsAlphaFavorite(item.alpha)) label = L"\x2605 " + label;
            }
            RECT text = {cell.left + Px(2), icon.bottom + Px(2), cell.right - Px(2), cell.bottom};
            Text(dc, text, label, t.text, DT_CENTER | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
            if ((item.isBrush || item.paintTool >= 0) && i < 5) {  // Shortcut badge.
                RECT badge = {icon.left - Px(4), icon.top, icon.left + Px(10), icon.top + Px(14)};
                RoundBox(dc, badge, Px(14), t.accent, t.accent);
                Text(dc, badge, std::to_wstring(i + 1), t.accentText, DT_CENTER | DT_VCENTER | DT_SINGLELINE, BoldFont());
            }
        }
        if (dragging_ && dropIndex_ >= 0) {
            const int x = area.left + dropIndex_ * ItemWidth() - scroll_;
            RECT caret = {x - Px(1), area.top, x + Px(2), area.bottom};
            Fill(dc, caret, t.accent);
        }
        RestoreDC(dc, saved);
    }

    void MouseDown(int x, int y, bool /*doubleClick*/) override {
        for (int i = 0; i < 3; ++i) {
            const RECT r = TabRect(i);
            if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) {
                SetPage(static_cast<Page>(i));
                return;
            }
        }
        if (page_ == Page::Alphas) {
            const RECT c = CategoryRect(), b = BrowseRect();
            if (x >= c.left && x < c.right && y >= c.top && y < c.bottom) {
                ChooseCategory();
                return;
            }
            if (x >= b.left && x < b.right && y >= b.top && y < b.bottom) {
                BrowseLibrary();
                return;
            }
        }
        pressed_ = ItemAt(x, y);
        pressX_ = x;
        dragging_ = false;
    }

    void MouseMove(int x, int y, bool captured) override {
        if (captured && pressed_ >= 0 && Reorderable()) {
            if (!dragging_ && std::abs(x - pressX_) > Px(6)) dragging_ = true;
            if (dragging_) {
                const RECT area = ItemsArea();
                const int n = static_cast<int>(Items().size());
                dropIndex_ = std::max(0, std::min<int>(n, (x - area.left + scroll_ + ItemWidth() / 2) / ItemWidth()));
                Invalidate();
            }
            return;
        }
        int tab = -1;
        for (int i = 0; i < 3; ++i) {
            const RECT r = TabRect(i);
            if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) tab = i;
        }
        const int hot = ItemAt(x, y);
        if (hot != hot_ || tab != hotTab_) {
            hot_ = hot;
            hotTab_ = tab;
            Invalidate();
        }
    }

    void MouseUp(int x, int y) override {
        const int pressed = pressed_;
        pressed_ = -1;
        if (dragging_) {
            dragging_ = false;
            if (dropIndex_ >= 0 && pressed >= 0) {
                const int to = dropIndex_ > pressed ? dropIndex_ - 1 : dropIndex_;
                if (page_ == Page::Paint)
                    S().MovePaintPaletteItem(pressed, to);
                else
                    S().MovePaletteItem(pressed, to);
            }
            dropIndex_ = -1;
            Invalidate();
            return;
        }
        const int index = ItemAt(x, y);
        if (index < 0 || index != pressed) return;
        const std::vector<Item> items = Items();
        const Item& item = items[static_cast<std::size_t>(index)];
        if (item.paintTool >= 0) {
            SculptCommands::ChoosePaintTool(item.paintTool);
        } else if (item.isBrush) {
            SculptCommands::ChooseBrush(item.brush);
        } else {
            S().SetAlphaId(item.alpha);
            if (!item.alpha.empty()) S().SetBool(Prop::UseAlpha, true);
        }
        Invalidate();
    }

    void RightClick(int x, int y) override {
        const int index = ItemAt(x, y);
        if (page_ != Page::Alphas || index < 0) return;
        const std::vector<Item> items = Items();
        const std::string id = items[static_cast<std::size_t>(index)].alpha;
        if (id.empty() || AlphaLibrary::IsBuiltin(id)) return;
        POINT p = {x, y};
        ClientToScreen(hwnd_, &p);
        const bool favorite = S().IsAlphaFavorite(id);
        const int chosen = PopupMenu({{1, L"Use as Alpha", false, true},
                                      {2, L"Use as Stencil", false, true},
                                      {3, L"Use as Displacement", false, true},
                                      {0, L""},
                                      {4, favorite ? L"Remove Favorite" : L"Add Favorite", false, true},
                                      {5, L"Show in Explorer", false, true}},
                                     p);
        if (chosen == 1) {
            S().SetAlphaId(id);
            S().SetBool(Prop::UseAlpha, true);
        } else if (chosen == 2) {
            SculptCommands::LoadStencil(Widen(id));
        } else if (chosen == 3) {
            S().SetDisplaceMap(id);
            DisplaceRollout::Apply();
        } else if (chosen == 4) {
            S().ToggleAlphaFavorite(id);
        } else if (chosen == 5) {
            const std::wstring args = L"/select,\"" + Widen(id) + L"\"";
            ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
        }
        Invalidate();
    }

    void MouseWheel(int delta, int /*x*/, int /*y*/) override {
        scroll_ -= (delta / WHEEL_DELTA) * ItemWidth();
        ClampScroll();
        Invalidate();
    }

    void MouseLeave() override {
        hot_ = hotTab_ = -1;
        Invalidate();
    }

    void CaptureLost() override {
        if (dragging_) {
            dragging_ = false;
            dropIndex_ = -1;
            Invalidate();
        }
    }

private:
    void ChooseCategory() {
        const std::vector<AlphaLibrary::Category> categories = AlphaLibrary::Get().Categories();
        std::vector<MenuItem> items;
        for (std::size_t i = 0; i < categories.size(); ++i)
            items.push_back({static_cast<int>(i) + 1, categories[i].name, categories[i].id == S().AlphaCategory(), true});
        const RECT c = CategoryRect();
        POINT p = {c.left, c.bottom};
        ClientToScreen(hwnd_, &p);
        if (GetCapture() == hwnd_) ReleaseCapture();
        const int chosen = PopupMenu(items, p);
        if (chosen > 0) {
            S().SetAlphaCategory(categories[static_cast<std::size_t>(chosen - 1)].id);
            scroll_ = 0;
        }
        Invalidate();
    }

    void BrowseLibrary() {
        if (GetCapture() == hwnd_) ReleaseCapture();
        Interface* core = GetCOREInterface();
        if (!core) return;
        MCHAR dir[MAX_PATH] = {};
        const std::wstring current = Widen(S().AlphaLibraryFolder());
        lstrcpynW(dir, current.c_str(), MAX_PATH);
        core->ChooseDirectory(core->GetMAXHWnd(), _T("Alpha Library Folder"), dir);
        if (dir[0] == 0) return;
        S().SetAlphaLibraryFolder(Narrow(dir));
        AlphaLibrary::Get().Rescan();
        // Show the new library right away.
        const std::vector<AlphaLibrary::Category> categories = AlphaLibrary::Get().Categories();
        S().SetAlphaCategory(categories.size() > 2 ? categories[2].id : std::string("builtin"));
        scroll_ = 0;
        Invalidate();
    }

    Page page_ = Page::Sculpting;
    int scroll_ = 0;
    int hot_ = -1;
    int hotTab_ = -1;
    int pressed_ = -1;
    int pressX_ = 0;
    bool dragging_ = false;
    int dropIndex_ = -1;
};

// --- Right-side rollouts ----------------------------------------------------------------------------

// --- Paint layers ------------------------------------------------------------------------------------

const sculpt::PaintCanvas* PaintCanvasOf() { return Edited() ? Edited()->Canvas() : nullptr; }
int PaintLayerCount() { return PaintCanvasOf() ? static_cast<int>(PaintCanvasOf()->layers().size()) : 0; }
int ActivePaintLayer() { return PaintCanvasOf() ? PaintCanvasOf()->active() : -1; }
bool PaintLayerSelected() { return PaintLayersShown() && ActivePaintLayer() >= 0; }
const sculpt::PaintLayer* SelectedPaintLayer() {
    const int a = ActivePaintLayer();
    return a >= 0 && a < PaintLayerCount() ? &PaintCanvasOf()->layers()[static_cast<std::size_t>(a)] : nullptr;
}
void RedrawViewports() {
    if (Interface* core = GetCOREInterface()) core->RedrawViews(core->GetTime());
}
void PaintLayerCommand(bool (SculptMeshObject::*command)()) {
    if (SculptMeshObject* object = Edited()) (object->*command)();
    RedrawViewports();
}

// Live-preview adjustment dialog (OK / Cancel) for the selected paint layer.
class AdjustWindow : public FloatingWindow {
public:
    using Kind = SculptMeshObject::Adjustment;
    void Open(Kind kind, POINT at) {
        if (Visible()) Finish(false);
        SculptMeshObject* object = Edited();
        if (!object || !object->BeginPaintAdjustment()) return;
        object_ = object;
        kind_ = kind;
        const float defaults[3][5] = {{0, 0, 0, 0, 0}, {0, 0, 0, 0, 0}, {0, 1, 255, 0, 255}};
        for (int k = 0; k < 5; ++k) values_[k] = defaults[static_cast<int>(kind)][k];
        Build();
        Interface* core = GetCOREInterface();
        if (!Hwnd() && !Create(core ? core->GetMAXHWnd() : nullptr)) return;
        cornerRadius_ = Px(10);
        const int width = Px(280);
        SetBounds(at.x, at.y, width, rows_.ContentHeight(width - Px(12)) + Px(12));
        Show(true);
        Invalidate();
    }
    void Finish(bool commit) {
        if (object_ && object_ == Edited()) object_->EndPaintAdjustment(commit);
        object_ = nullptr;
        Show(false);
        RedrawViewports();
        SculptUI::Refresh();
    }

protected:
    void Paint(HDC dc, const RECT& client) override {
        Fill(dc, client, GetTheme().background);
        rows_.Paint(dc, RECT{Px(6), Px(6), client.right - Px(6), client.bottom - Px(6)}, 0);
    }
    void MouseDown(int x, int y, bool doubleClick) override {
        if (rows_.MouseDown(hwnd_, x, y, doubleClick, 0)) Invalidate();
    }
    void MouseMove(int x, int y, bool captured) override {
        if (rows_.MouseMove(x, y, captured, 0)) Invalidate();
    }
    void MouseUp(int x, int y) override {
        if (rows_.MouseUp(x, y, 0)) Invalidate();
    }
    void RightClick(int x, int y) override {
        if (rows_.RightClick(hwnd_, x, y, 0)) Invalidate();
    }
    void MouseLeave() override {
        if (rows_.MouseLeave()) Invalidate();
    }
    void CaptureLost() override { rows_.CaptureLost(); }
    LRESULT Message(UINT msg, WPARAM wp, LPARAM /*lp*/, bool& handled) override {
        handled = msg == RowList::kFinishEditMessage;
        if (handled) rows_.FinishEdit(wp != 0);
        return 0;
    }

private:
    void Slider(const wchar_t* label, int index, float lo, float hi, float defaultValue, int decimals) {
        RowList::SliderSpec spec;
        spec.label = label;
        spec.get = [this, index] { return values_[index]; };
        spec.set = [this, index](float v) {
            values_[index] = v;
            if (object_ && object_ == Edited()) object_->PreviewPaintAdjustment(kind_, values_);
            RedrawViewports();
        };
        spec.min = lo;
        spec.max = hi;
        spec.typeMax = hi;
        spec.defaultValue = defaultValue;
        spec.decimals = decimals;
        rows_.Slider(spec);
    }
    void Build() {
        rows_.Clear();
        const wchar_t* titles[] = {L"Hue / Saturation / Luminosity", L"Brightness / Contrast", L"Levels"};
        rows_.Section(titles[static_cast<int>(kind_)], true);
        switch (kind_) {
            case Kind::HueSaturation:
                Slider(L"Hue", 0, -180.0f, 180.0f, 0.0f, 0);
                Slider(L"Saturation", 1, -100.0f, 100.0f, 0.0f, 0);
                Slider(L"Luminosity", 2, -100.0f, 100.0f, 0.0f, 0);
                break;
            case Kind::BrightnessContrast:
                Slider(L"Brightness", 0, -100.0f, 100.0f, 0.0f, 0);
                Slider(L"Contrast", 1, -100.0f, 100.0f, 0.0f, 0);
                break;
            case Kind::Levels:
                Slider(L"Input Black", 0, 0.0f, 255.0f, 0.0f, 0);
                Slider(L"Gamma", 1, 0.1f, 10.0f, 1.0f, 2);
                Slider(L"Input White", 2, 0.0f, 255.0f, 255.0f, 0);
                Slider(L"Output Black", 3, 0.0f, 255.0f, 0.0f, 0);
                Slider(L"Output White", 4, 0.0f, 255.0f, 255.0f, 0);
                break;
        }
        rows_.Buttons({{L"OK", [this] { Finish(true); }, {}, {}, {}}, {L"Cancel", [this] { Finish(false); }, {}, {}, {}}});
    }

    RowList rows_;
    Kind kind_ = Kind::HueSaturation;
    float values_[5] = {0, 0, 0, 0, 0};
    SculptMeshObject* object_ = nullptr;
};

AdjustWindow& Adjust() {
    static AdjustWindow window;
    return window;
}

void PaintLayerMenu(POINT screen) {
    SculptMeshObject* object = Edited();
    if (!object || ActivePaintLayer() < 0) return;
    const int chosen = PopupMenu({{1, L"Import Texture...", false, true},
                                  {0, L""},
                                  {2, L"Hue / Saturation / Luminosity...", false, true},
                                  {3, L"Brightness / Contrast...", false, true},
                                  {4, L"Levels...", false, true}},
                                 screen);
    if (chosen == 1) {
        MSTR error;
        if (!object->ImportPaintTexture(ActivePaintLayer(), error) && error.Length() > 0) {
            Interface* core = GetCOREInterface();
            MessageBoxW(core ? core->GetMAXHWnd() : nullptr, error.data(), L"Sculpt Mesh", MB_OK | MB_ICONINFORMATION);
        }
        RedrawViewports();
    } else if (chosen >= 2) {
        Adjust().Open(static_cast<AdjustWindow::Kind>(chosen - 2), screen);
    }
}

// Paint stack rows of the Layers rollout (the list shows the top layer first).
void BuildPaintLayers(RowList& r) {
    const auto shown = [] { return PaintLayersShown(); };
    const auto hasLayers = [] { return PaintLayersShown() && PaintLayerCount() > 0; };
    r.Buttons({{L"New", [] { PaintLayerCommand(&SculptMeshObject::NewPaintLayer); }, {}, {}, {}},
               {L"Delete", [] { PaintLayerCommand(&SculptMeshObject::DeletePaintLayer); }, {}, PaintLayerSelected, {}},
               {L"Clear", [] { PaintLayerCommand(&SculptMeshObject::ClearPaintLayer); }, {}, PaintLayerSelected, {}},
               {L"Clear All", [] { PaintLayerCommand(&SculptMeshObject::ClearAllPaintLayers); }, {}, hasLayers, {}}},
              shown);
    r.Buttons({{L"Move Up", [] { if (Edited()) Edited()->MovePaintLayer(1); RedrawViewports(); }, {},
                [] { return PaintLayerSelected() && ActivePaintLayer() + 1 < PaintLayerCount(); }, {}},
               {L"Move Down", [] { if (Edited()) Edited()->MovePaintLayer(-1); RedrawViewports(); }, {},
                [] { return PaintLayerSelected() && ActivePaintLayer() > 0; }, {}},
               {L"Bake All", [] { PaintLayerCommand(&SculptMeshObject::MergePaintLayers); }, {}, hasLayers, {}}},
              shown);
    r.List(
        [] { return PaintLayerCount(); },
        [](int row) {
            const sculpt::PaintLayer& layer = PaintCanvasOf()->layers()[static_cast<std::size_t>(PaintLayerCount() - 1 - row)];
            const wchar_t* blends[] = {L"Normal", L"Multiply", L"Screen", L"Overlay", L"Add", L"Subtract"};
            wchar_t text[200];
            swprintf(text, 200, L"%ls%ls   %d%%  %ls", Widen(layer.name).c_str(), layer.enabled ? L"" : L"  (off)",
                     static_cast<int>(std::lround(layer.opacity * 100.0f)), blends[std::min(std::max(static_cast<int>(layer.blend), 0), 5)]);
            return std::wstring(text);
        },
        [] { return ActivePaintLayer() < 0 ? -1 : PaintLayerCount() - 1 - ActivePaintLayer(); },
        [](int row) { if (Edited()) Edited()->SelectPaintLayer(row < 0 ? -1 : PaintLayerCount() - 1 - row); }, shown,
        [](int, POINT screen) { PaintLayerMenu(screen); });
    r.Note([] { return std::wstring(L"Strokes paint into the base texture (select a layer to paint on it)"); },
           [] { return PaintLayersShown() && ActivePaintLayer() < 0 && Edited() && Edited()->Canvas(); });
    r.Text(L"Name", [] { return SelectedPaintLayer() ? Widen(SelectedPaintLayer()->name) : std::wstring(); },
           [](const std::wstring& name) { if (Edited()) Edited()->SetPaintLayerName(ActivePaintLayer(), Narrow(name)); },
           PaintLayerSelected);
    r.Check(L"Layer Enabled", [] { return SelectedPaintLayer() && SelectedPaintLayer()->enabled; },
            [](bool on) {
                if (Edited()) Edited()->SetPaintLayerEnabled(ActivePaintLayer(), on);
                RedrawViewports();
            },
            PaintLayerSelected);
    RowList::SliderSpec opacity;
    opacity.label = L"Opacity";
    opacity.get = [] { return SelectedPaintLayer() ? SelectedPaintLayer()->opacity : 1.0f; };
    opacity.set = [](float v) {
        if (Edited()) Edited()->SetPaintLayerOpacityLive(ActivePaintLayer(), v);
        if (Interface* core = GetCOREInterface()) core->RedrawViews(core->GetTime(), REDRAW_INTERACTIVE);
    };
    opacity.released = [] { if (Edited()) Edited()->FinishPaintLayerOpacity(); };
    opacity.min = 0.0f;
    opacity.max = 1.0f;
    opacity.typeMax = 1.0f;
    opacity.defaultValue = 1.0f;
    opacity.format = [](float v) { return std::to_wstring(static_cast<int>(std::lround(v * 100.0f))) + L"%"; };
    r.Slider(opacity, PaintLayerSelected);
    r.Choice(L"Blend", {L"Normal", L"Multiply", L"Screen", L"Overlay", L"Add", L"Subtract"},
             [] { return SelectedPaintLayer() ? static_cast<int>(SelectedPaintLayer()->blend) : 0; },
             [](int i) {
                 if (Edited()) Edited()->SetPaintLayerBlend(ActivePaintLayer(), static_cast<sculpt::PaintBlend>(i));
                 RedrawViewports();
             },
             PaintLayerSelected);
    r.Buttons({{L"Adjust / Import...", [] {
                    POINT p;
                    GetCursorPos(&p);
                    PaintLayerMenu(p);
                }, {}, PaintLayerSelected, {}}},
              shown);
}

class PanelWindow : public FloatingWindow {
public:
    PanelWindow() {
        cornerRadius_ = Px(10);
        Build();
    }
    int Width() const { return Px(256); }
    int ContentHeight() const { return rows_.ContentHeight(Width() - Px(12)) + Px(10); }

protected:
    RECT Area() const {
        const RECT c = ClientRect();
        return RECT{Px(6), Px(6), c.right - Px(6), c.bottom - Px(4)};
    }

    void Paint(HDC dc, const RECT& client) override {
        Fill(dc, client, GetTheme().background);
        const RECT area = Area();
        ClampScroll();
        const int saved = SaveDC(dc);
        IntersectClipRect(dc, area.left, area.top, area.right, area.bottom);
        rows_.Paint(dc, area, scroll_);
        RestoreDC(dc, saved);
    }

    void ClampScroll() {
        const RECT area = Area();
        const int overflow = ContentHeight() - Px(10) - static_cast<int>(area.bottom - area.top);
        scroll_ = std::max(0, std::min(scroll_, overflow));
    }

    void MouseDown(int x, int y, bool doubleClick) override {
        if (rows_.MouseDown(hwnd_, x, y, doubleClick, scroll_)) {
            Invalidate();
            LayoutWindows();  // A section may have opened or closed.
        }
    }
    void MouseMove(int x, int y, bool captured) override {
        if (rows_.MouseMove(x, y, captured, scroll_)) Invalidate();
    }
    void MouseUp(int x, int y) override {
        if (rows_.MouseUp(x, y, scroll_)) Invalidate();
    }
    void RightClick(int x, int y) override {
        if (rows_.RightClick(hwnd_, x, y, scroll_)) Invalidate();
    }
    void MouseLeave() override {
        if (rows_.MouseLeave()) Invalidate();
    }
    void CaptureLost() override { rows_.CaptureLost(); }
    void MouseWheel(int delta, int /*x*/, int /*y*/) override {
        scroll_ -= delta / WHEEL_DELTA * Px(40);
        ClampScroll();
        Invalidate();
    }
    LRESULT Message(UINT msg, WPARAM wp, LPARAM /*lp*/, bool& handled) override {
        handled = msg == RowList::kFinishEditMessage;
        if (handled) rows_.FinishEdit(wp != 0);
        return 0;
    }

private:
    void Build() {
        using Op = SculptCommands::Op;
        RowList& r = rows_;
        r.Section(L"Brush Settings", true);
        r.Note([] {
            if (MaskDirect()) {
                const wchar_t* names[] = {L"Paint Mask", L"Rectangle Mask", L"Lasso Mask"};
                return std::wstring(names[std::min(std::max(S().Int(Prop::MaskTool), 0), 2)]);
            }
            if (PaintContext()) return L"Paint: " + Widen(paintToolName(S().Int(Prop::PaintTool)));
            return BrushName(S().Brush());
        });
        r.Buttons({{L"Add", [] { S().SetBrushValue(ContextBrush(), BrushProp::Subtract, 0.0f); },
                    [] { return S().BrushValue(ContextBrush(), BrushProp::Subtract) == 0.0f; }, {}},
                   {L"Sub", [] { S().SetBrushValue(ContextBrush(), BrushProp::Subtract, 1.0f); },
                    [] { return S().BrushValue(ContextBrush(), BrushProp::Subtract) != 0.0f; }, {}}},
                  [] { return SculptContext() && sculpt::isSignedBrush(ContextBrush()); });
        r.Note(
            [] {
                if (Is(BrushType::Clip))
                    return std::wstring(L"Drag line \x2022 Alt curve \x2022 Ctrl rect \x2022 Shift circle \x2022 +Alt invert");
                if (Is(BrushType::Cutter)) return std::wstring(L"Drag line \x2022 Ctrl rect \x2022 Shift circle \x2022 Alt side");
                if (Is(BrushType::Slice)) return std::wstring(L"Drag a line through the mesh \x2022 Space pans");
                if (Is(BrushType::Pose)) return std::wstring(L"Drag A \x2192 B, then drag to pose \x2022 RMB clears");
                if (Is(BrushType::CurveTube)) return std::wstring(L"Drag to draw \x2022 RMB commits \x2022 Esc cancels");
                if (Is(BrushType::Density)) return std::wstring(L"Remeshes when the stroke ends \x2022 Alt reduces");
                return std::wstring();
            },
            [] {
                return GestureBrush() || Is(BrushType::Density);
            });
        r.Slider(PropSlider(L"Brush Size", Prop::BrushSize, 1, 500.0f, true), SizeContext);
        r.Slider(StrengthSlider(), StrengthContext);
        // Paint tools: Opacity and Hardness instead of Brush Strength.
        r.Slider(PropSlider(L"Opacity", Prop::PaintOpacity, 2), PaintContext);
        r.Slider(PropSlider(L"Hardness", Prop::PaintHardness, 2),
                 [] { return PaintContext() && !PaintToolIs(sculpt::PaintTool::Fill) && !PaintToolIs(sculpt::PaintTool::Gradient); });
        r.Slider(PropSlider(L"Blur Strength", Prop::PaintBlurStrength, 2), [] { return PaintToolIs(sculpt::PaintTool::Blur); });
        r.Note([] { return std::wstring(L"Shift: Blur \x2022 Alt: erase \x2022 Ctrl: mask"); },
               [] { return PaintToolIs(sculpt::PaintTool::Paint); });
        r.Note([] { return std::wstring(L"Click the mesh to fill the active layer"); }, [] { return PaintToolIs(sculpt::PaintTool::Fill); });
        r.Note([] { return std::wstring(L"Drag across the mesh: Color A \x2192 Color B"); },
               [] { return PaintToolIs(sculpt::PaintTool::Gradient); });
        AddBrushCheck(r, L"Layer Mode", BrushProp::LayerMode,
                      [] { return SculptContext() && sculpt::brushInfo(ContextBrush()).supportsLayerMode; });
        r.Slider(PropSlider(L"Height Mid", Prop::DisplaceHeightMid, 2), [] { return Is(BrushType::Displace); });
        r.Slider(PropSlider(L"Displace Fade", Prop::DisplaceFade, 2), [] { return Is(BrushType::Displace); });
        r.Note([] { return std::wstring(L"Uses the stencil as height \x2022 Alt reverses"); }, [] { return Is(BrushType::Displace); });
        r.Slider(PropSlider(L"Stroke Spacing", Prop::StrokeSpacing, 2, 1.0f), DabContext);
        r.Slider(PropSlider(L"Alpha Mid", Prop::AlphaMid, 2), [] { return DabContext() && AlphaActive(); });
        r.Slider(PropSlider(L"Alpha Fade", Prop::AlphaFade, 2), [] { return DabContext() && AlphaActive(); });
        r.Slider(PropSlider(L"Border", Prop::ClayBorder, 2), [] { return BrushContext() && ContextBrush() == BrushType::Clay; });
        r.Slider(PropSlider(L"Polish Hardness", Prop::PolishHardness, 2),
                 [] { return BrushContext() && ContextBrush() == BrushType::Polish; });
        r.Slider(PropSlider(L"AccuCurve", Prop::AccuCurve, 2), [] { return BrushContext() && ContextBrush() == BrushType::Move; });
        AddPropCheck(r, L"Use Original Plane", Prop::ScrapeOriginalPlane,
                     [] { return BrushContext() && ContextBrush() == BrushType::Scrape; });
        AddPropCheck(r, L"Use Original Normal", Prop::ScrapeOriginalNormal,
                     [] { return BrushContext() && ContextBrush() == BrushType::Scrape; });
        r.Slider(PropSlider(L"Scatter Density", Prop::ScatterDensity, 0), ScatterContext);
        r.Slider(PropSlider(L"Scatter Radius", Prop::ScatterRadius, 2), ScatterContext);
        r.Slider(PropSlider(L"Size Jitter", Prop::SizeJitter, 2), ScatterContext);
        r.Slider(PropSlider(L"Amount Jitter", Prop::AmountJitter, 2), ScatterContext);
        r.Slider(PropSlider(L"Color Jitter A/B", Prop::PaintColorJitter, 2), [] { return ScatterContext() && PaintContext(); });
        // Cloth.
        const auto cloth = [] { return Is(BrushType::Cloth); };
        r.Slider(PropSlider(L"Cloth Iterations", Prop::ClothIterations, 0, 20.0f), cloth);
        r.Slider(PropSlider(L"Cloth Damping", Prop::ClothDamping, 2), cloth);
        r.Slider(PropSlider(L"Cloth Plasticity", Prop::ClothPlasticity, 2), cloth);
        r.Slider(PropSlider(L"Cloth Bendiness", Prop::ClothBendiness, 2), cloth);
        r.Slider(PropSlider(L"Fold Size", Prop::ClothFoldSize, 2), cloth);
        r.Slider(PropSlider(L"Fold Strength", Prop::ClothFoldStrength, 2), cloth);
        r.Slider(PropSlider(L"Bend Stiffness", Prop::ClothBendStiffness, 2), cloth);
        r.Slider(PropSlider(L"Simulation Area", Prop::ClothSimulationArea, 2), cloth);
        r.Slider(PropSlider(L"Move Strength", Prop::ClothMoveStrength, 2), cloth);
        r.Slider(PropSlider(L"Gravity", Prop::ClothGravity, 2), cloth);
        r.Slider(PropSlider(L"Pressure", Prop::ClothPressure, 2), cloth);
        AddPropCheck(r, L"Pin Boundary", Prop::ClothPinBoundary, cloth);
        // Pose.
        const auto pose = [] { return Is(BrushType::Pose); };
        r.Choice(L"Deformation", {L"Rotate", L"Twist", L"Scale"}, [] { return S().Int(Prop::PoseDeformation); },
                 [](int i) { S().Set(Prop::PoseDeformation, static_cast<float>(i)); }, pose);
        r.Choice(L"Rotation Origins", {L"Guide (A \x2192 B)", L"SculptGroups"}, [] { return S().Int(Prop::PoseRotationOrigins); },
                 [](int i) {
                     S().Set(Prop::PoseRotationOrigins, static_cast<float>(i));
                     SculptMode::Get().ClearPoseGuide();
                 },
                 pose);
        r.Slider(PropSlider(L"Pose Origin Offset", Prop::PoseOriginOffset, 2), pose);
        r.Slider(PropSlider(L"Smooth Iterations", Prop::PoseSmoothIterations, 0, 10.0f), pose);
        r.Slider(PropSlider(L"Pose IK Segments", Prop::PoseIkSegments, 0), pose);
        AddPropCheck(r, L"Keep Anchor Point", Prop::PoseKeepAnchor, pose);
        AddPropCheck(r, L"Connected Only", Prop::PoseConnectedOnly, pose);
        r.Buttons({{L"Clear Guide", [] { SculptMode::Get().ClearPoseGuide(); }, {}, {}, {}}}, pose);
        // Curve Tube.
        const auto tube = [] { return Is(BrushType::CurveTube); };
        r.Slider(PropSlider(L"Tube Sides", Prop::TubeSides, 0, 32.0f), [] { return Is(BrushType::CurveTube) && !SculptMode::Get().SectionNode(); });
        r.Slider(PropSlider(L"Point Spacing", Prop::TubeSpacing, 0, 100.0f), tube);
        r.Slider(PropSlider(L"Surface Offset", Prop::TubeSurfaceOffset, 2), tube);
        r.Buttons({{L"Pick Section Shape", [] { SectionPicker::Toggle(); }, [] { return SectionPicker::Active(); }, {}, {}},
                   {L"Clear", [] { SculptMode::Get().SetSectionNode(0); }, {}, [] { return SculptMode::Get().SectionNode() != 0; }, {}}},
                  tube);
        r.Note(SectionName, tube);
        r.Buttons({{L"Commit Tube", [] { SculptMode::Get().CommitTube(); }, {}, [] { return SculptMode::Get().TubePending(); }, {}},
                   {L"Cancel", [] { SculptMode::Get().CancelPending(); }, {}, [] { return SculptMode::Get().TubePending(); }, {}}},
                  tube);

        AddPropCheck(r, L"Use Falloff", Prop::UseFalloff, DabContext);
        AddPropCheck(r, L"Follow Path", Prop::FollowPath, DabContext);
        AddPropCheck(r, L"Lazy Mouse", Prop::LazyMouse, DabContext);
        r.Slider(PropSlider(L"Lazy Amount", Prop::LazyAmount, 2), [] { return DabContext() && S().Bool(Prop::LazyMouse); });
        AddBrushCheck(r, L"Backface Cull", BrushProp::BackfaceCull,
                      [] { return (StrengthContext() || Is(BrushType::Cloth) || MaskDirect()) && !PaintContext(); });

        r.Section(L"Material / Paint", false);
        AddPropCheck(r, L"Use Sculpt Material Preview", Prop::SculptMaterialPreview);
        r.Choice(L"Paint Source", {L"Generated Texture", L"Existing Diffuse Texture"}, [] { return S().Int(Prop::PaintSource); },
                 [](int i) { S().Set(Prop::PaintSource, static_cast<float>(i)); });
        r.Choice(L"Resolution", {L"512", L"1024", L"2048", L"4096"}, [] { return S().Int(Prop::PaintResolution); },
                 [](int i) { S().Set(Prop::PaintResolution, static_cast<float>(i)); }, [] { return S().Int(Prop::PaintSource) == 0; });
        r.Colors(GetColor, SetColor, SwapColors);
        const auto painted = [] { return Edited() && Edited()->Canvas(); };
        r.Buttons({{L"Save", [] { SculptCommands::SavePaintTexture(false); }, {}, painted, {}},
                   {L"Save As", [] { SculptCommands::SavePaintTexture(true); }, {}, painted, {}}});
        r.Buttons({{L"Replace Texture", [] { SculptCommands::ReplacePaintTexture(); }, {}, {}, {}},
                   {L"Restore Material", [] { SculptCommands::RestoreMaterial(); }, {},
                    [] { return Edited() && Edited()->HasOriginalMaterial(); }, {}}});
        r.Note([] { return Edited() ? std::wstring(Edited()->PaintStatus().data()) : std::wstring(); });
        r.Buttons({{L"Load Stencil", [] { SculptCommands::LoadStencil(); }, {}, {}, {}},
                   {L"\x21BA", [] { SculptCommands::ResetStencil(); }, {}, [] { return Stencil::Get().Loaded(); }, {}},
                   {L"\x2715", [] { SculptCommands::ClearStencil(); }, {}, [] { return Stencil::Get().Loaded(); }, {}}});
        RowList::SliderSpec stencilOpacity = PropSlider(L"Stencil Opacity", Prop::StencilOpacity, 2);
        stencilOpacity.format = [](float v) { return std::to_wstring(static_cast<int>(std::lround(v * 100.0f))) + L"%"; };
        r.Slider(stencilOpacity, [] { return Stencil::Get().Loaded(); });
        r.Choice(L"Stencil Paints", {L"Its Colours", L"Color A (mask)"}, [] { return S().Int(Prop::StencilMode); },
                 [](int i) { S().Set(Prop::StencilMode, static_cast<float>(i)); }, [] { return Stencil::Get().Loaded(); });
        r.Note([] { return std::wstring(L"Hold S: LMB rotate \x2022 RMB scale \x2022 MMB move"); }, [] { return Stencil::Get().Loaded(); });

        r.Section(L"Mask", false);
        r.Buttons({{L"Mask by Cavity", [] { RunOp(Op::MaskByCavity); }, {}, {}}});
        r.Slider(PropSlider(L"Coverage", Prop::CavityCoverage, 2));
        r.Buttons({{L"Mask by AO", [] { RunOp(Op::MaskByAO); }, {}, {}}});
        r.Slider(PropSlider(L"Coverage", Prop::AOCoverage, 2));
        r.Buttons({{L"Blur Mask", [] { RunOp(Op::MaskBlur); }, {}, {}}, {L"Sharpen Mask", [] { RunOp(Op::MaskSharpen); }, {}, {}}});
        r.Buttons({{L"Grow Mask", [] { RunOp(Op::MaskGrow); }, {}, {}}, {L"Shrink Mask", [] { RunOp(Op::MaskShrink); }, {}, {}}});
        r.Buttons({{L"Clear Mask", [] { RunOp(Op::MaskClear); }, {}, {}}, {L"Invert Mask", [] { RunOp(Op::MaskInvert); }, {}, {}}});
        AddPropCheck(r, L"Show Mask", Prop::ShowMask);
        AddPropCheck(r, L"Show SculptGroups", Prop::ShowGroups);
        r.Buttons({{L"Auto Groups", [] { RunOp(Op::AutoGroups); }, {}, {}}});
        r.Choice(L"Mode", {L"Curvature", L"Angle", L"Smooth Groups", L"UV Islands", L"Material IDs", L"Elements"},
                 [] { return S().Int(Prop::AutoGroupMode); },
                 [](int i) { S().Set(Prop::AutoGroupMode, static_cast<float>(i)); });
        r.Buttons({{L"Group from Mask", [] { RunOp(Op::GroupFromMask); }, {}, {}},
                   {L"Show All", [] { RunOp(Op::ShowAll); }, {}, {}}});

        r.Section(L"Mirror", false);
        const auto axis = [](const wchar_t* label, Prop prop) {
            return RowList::Button{label, [prop] { S().SetBool(prop, !S().Bool(prop)); }, [prop] { return S().Bool(prop); }, {}};
        };
        r.Buttons({axis(L"X", Prop::MirrorX), axis(L"Y", Prop::MirrorY), axis(L"Z", Prop::MirrorZ)});
        AddPropCheck(r, L"Radial Mirror", Prop::RadialMirror);
        r.Slider(PropSlider(L"Radial Count", Prop::RadialCount, 0), [] { return S().Bool(Prop::RadialMirror); });
        r.Choice(L"Radial Axis", {L"X", L"Y", L"Z"}, [] { return S().Int(Prop::RadialAxis); },
                 [](int i) { S().Set(Prop::RadialAxis, static_cast<float>(i)); }, [] { return S().Bool(Prop::RadialMirror); });

        r.Section(L"Profile", false);
        AddPropCheck(r, L"Use Profile", Prop::ProfileUse);
        r.Choice(L"Apply to", {L"Curve Tube", L"Active Sculpt Group"}, [] { return S().Int(Prop::ProfileTarget); },
                 [](int i) {
                     S().Set(Prop::ProfileTarget, static_cast<float>(i));
                     S().Set(Prop::ProfileMapping, i == 0 ? 0.0f : 3.0f);  // Curve Length / Local Z.
                 });
        r.Choice(L"Mapping", {L"Curve Length", L"Local X", L"Local Y", L"Local Z"}, [] { return S().Int(Prop::ProfileMapping); },
                 [](int i) { S().Set(Prop::ProfileMapping, static_cast<float>(i)); });
        r.Custom(profileEditor_.Spec());
        r.Buttons({{L"Reset Profile", [] { S().SetProfileCurveText(std::string()); }, {}, {}, {}},
                   {L"Apply to Group", [] { SculptCommands::ApplyProfile(); }, {},
                    [] { return S().Bool(Prop::ProfileUse) && S().Int(Prop::ProfileTarget) == 1; }, {}}});

        r.Section(L"Layers", true);
        // Tabs: switching also routes Sculpt Mesh into that working mode.
        r.Buttons({{L"Sculpt", [] { SculptCommands::SelectSculptMode(); }, [] { return !PaintLayersShown(); }, {}, {}},
                   {L"Paint", [] { SculptCommands::SelectPaintMode(); }, [] { return PaintLayersShown(); }, {}, {}}});
        BuildPaintLayers(r);
        const auto sculptLayers = [] { return !PaintLayersShown(); };
        r.Note(
            [] {
                wchar_t text[96];
                swprintf(text, 96, L"Layers live on level %d", Edited() ? Edited()->LayersLevel() : 0);
                return std::wstring(text);
            },
            [] { return !PaintLayersShown() && Edited() && !Edited()->LayersUsable(); });
        r.Buttons({{L"New", [] { LayerCommand(&SculptMeshObject::NewLayer); }, {}, LayersEditable, {}},
                   {L"Delete", [] { LayerCommand(&SculptMeshObject::DeleteLayer); }, {}, LayerSelected, {}},
                   {L"Clear", [] { LayerCommand(&SculptMeshObject::ClearLayer); }, {}, LayerSelected, {}},
                   {L"Clear All", [] { LayerCommand(&SculptMeshObject::ClearAllLayers); }, {},
                    [] { return LayersEditable() && !Edited()->Layers().empty(); }, {}}},
                  sculptLayers);
        r.Buttons({{L"Move Up", [] { if (Edited()) Edited()->MoveLayer(-1); }, {}, [] { return LayerSelected() && ActiveLayer() > 0; }, {}},
                   {L"Move Down", [] { if (Edited()) Edited()->MoveLayer(1); }, {},
                    [] { return LayerSelected() && ActiveLayer() + 1 < static_cast<int>(Edited()->Layers().layers.size()); }, {}},
                   {L"Bake All", [] { LayerCommand(&SculptMeshObject::BakeAllLayers); }, {},
                    [] { return LayersEditable() && !Edited()->Layers().empty(); }, {}}},
                  sculptLayers);
        r.List(
            [] { return Edited() ? static_cast<int>(Edited()->Layers().layers.size()) : 0; },
            [](int i) {
                const sculpt::SculptLayer& layer = Edited()->Layers().layers[static_cast<std::size_t>(i)];
                wchar_t text[160];
                swprintf(text, 160, L"%ls%ls   %.2fx", Widen(layer.name).c_str(), layer.enabled ? L"" : L"  (off)",
                         static_cast<double>(layer.strength));
                return std::wstring(text);
            },
            ActiveLayer, [](int i) { if (Edited()) Edited()->SelectLayer(i); }, sculptLayers);
        r.Text(L"Name", [] { return SelectedLayer() ? Widen(SelectedLayer()->name) : std::wstring(); },
               [](const std::wstring& name) { if (Edited()) Edited()->SetLayerName(ActiveLayer(), Narrow(name)); },
               [] { return !PaintLayersShown() && LayerSelected(); });
        r.Check(L"Layer Enabled", [] { return SelectedLayer() && SelectedLayer()->enabled; },
                [](bool on) {
                    if (Edited()) Edited()->SetLayerEnabled(ActiveLayer(), on);
                    if (Interface* core = GetCOREInterface()) core->RedrawViews(core->GetTime());
                },
                [] { return !PaintLayersShown() && LayerSelected(); });
        RowList::SliderSpec strength;
        strength.label = L"Strength";
        strength.get = [] { return SelectedLayer() ? SelectedLayer()->strength : 1.0f; };
        strength.set = [](float v) {
            if (Edited()) Edited()->SetLayerStrengthLive(ActiveLayer(), v);
            if (Interface* core = GetCOREInterface()) core->RedrawViews(core->GetTime(), REDRAW_INTERACTIVE);
        };
        strength.released = [] { if (Edited()) Edited()->FinishLayerStrength(); };
        strength.min = 0.0f;
        strength.max = sculpt::kMaxLayerStrength;
        strength.typeMax = sculpt::kMaxLayerStrength;
        strength.defaultValue = 1.0f;
        strength.toValue = sculpt::layerStrengthFromSlider;
        strength.toPosition = sculpt::layerSliderFromStrength;
        strength.format = [](float v) {
            wchar_t text[32];
            swprintf(text, 32, L"%.2fx", static_cast<double>(v));
            return std::wstring(text);
        };
        r.Slider(strength, [] { return !PaintLayersShown() && LayerSelected(); });

        r.Section(L"Surface Snapshot", false);
        r.Buttons({{L"Capture Surface", [] { SculptCommands::CaptureSurface(); }, {}, {}, {}},
                   {L"Clear", [] { SculptCommands::ClearSurface(); }, {},
                    [] { return SculptMeshObject::EditedObject() && SculptMeshObject::EditedObject()->HasSurface(); }, {}}});
        r.Note([] {
            SculptMeshObject* object = SculptMeshObject::EditedObject();
            return object ? std::wstring(object->SurfaceStatus().data()) : std::wstring();
        });
    }

    RowList rows_;
    ProfileEditor profileEditor_;
    int scroll_ = 0;
};

// --- Quick Menu ----------------------------------------------------------------------------------------

class QuickMenuWindow : public FloatingWindow {
public:
    QuickMenuWindow() {
        cornerRadius_ = Px(10);
        RowList& r = rows_;
        r.Colors(GetColor, SetColor, SwapColors);
        r.Slider(PropSlider(L"Brush Size", Prop::BrushSize, 1, 500.0f, true));
        r.Slider(StrengthSlider(), [] { return ContextBrush() != BrushType::FaceGroups; });
        AddPropCheck(r, L"Use Alpha Texture", Prop::UseAlpha);
        AddPropCheck(r, L"Use Falloff", Prop::UseFalloff);
        AddPropCheck(r, L"Follow Path", Prop::FollowPath);
        AddBrushCheck(r, L"Backface Cull", BrushProp::BackfaceCull);
    }

    int Width() const { return Px(240); }
    int Height() const { return RowsTop() + rows_.ContentHeight(Width() - Px(12)) + Px(4); }

    void Popup(POINT screen, bool space) {
        if (!hwnd_) return;
        space_ = space;
        float rgb[3], s, v;
        GetColor(0, rgb);
        RgbToHsv(rgb, hue_, s, v);
        // Open with the cursor just inside the top-left corner, kept on the monitor.
        int x = screen.x - Px(24), y = screen.y - Px(24);
        MONITORINFO mi = {sizeof(mi)};
        if (GetMonitorInfo(MonitorFromPoint(screen, MONITOR_DEFAULTTONEAREST), &mi)) {
            x = std::max<int>(mi.rcWork.left, std::min<int>(x, mi.rcWork.right - Width()));
            y = std::max<int>(mi.rcWork.top, std::min<int>(y, mi.rcWork.bottom - Height()));
        }
        SetBounds(x, y, Width(), Height());
        SetWindowPos(hwnd_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        Invalidate();
        SetTimer(hwnd_, kQuickMenuTimer, 30, nullptr);
    }

    void Hide() {
        if (!hwnd_) return;
        if (rows_.Editing()) rows_.FinishEdit(true);
        KillTimer(hwnd_, kQuickMenuTimer);
        if (GetCapture() == hwnd_) ReleaseCapture();
        drag_ = Drag::None;
        Show(false);
    }

protected:
    int RowsTop() const { return Px(124); }
    RECT SvRect() const { return RECT{Px(8), Px(8), Width() - Px(36), Px(116)}; }
    RECT HueRect() const { return RECT{Width() - Px(28), Px(8), Width() - Px(8), Px(116)}; }
    RECT RowsArea() const {
        const RECT c = ClientRect();
        return RECT{Px(6), RowsTop(), c.right - Px(6), c.bottom};
    }

    void Paint(HDC dc, const RECT& client) override {
        const Theme& t = GetTheme();
        Fill(dc, client, t.background);
        Frame(dc, client, t.border);
        float rgb[3], s, v, h;
        GetColor(0, rgb);
        RgbToHsv(rgb, h, s, v);
        if (s > 0.0f && v > 0.0f) hue_ = h;  // Keep the hue for grays.

        const RECT sv = SvRect();
        const int w = sv.right - sv.left, hgt = sv.bottom - sv.top;
        std::vector<std::uint32_t> pixels(static_cast<std::size_t>(w) * hgt);
        for (int y = 0; y < hgt; ++y)
            for (int x = 0; x < w; ++x) {
                float c[3];
                HsvToRgb(hue_, static_cast<float>(x) / std::max(1, w - 1), 1.0f - static_cast<float>(y) / std::max(1, hgt - 1), c);
                pixels[static_cast<std::size_t>(y) * w + x] = Pack(c);
            }
        DrawPixels(dc, sv.left, sv.top, w, hgt, pixels.data());
        const RECT hr = HueRect();
        const int hw = hr.right - hr.left, hh = hr.bottom - hr.top;
        std::vector<std::uint32_t> hue(static_cast<std::size_t>(hw) * hh);
        for (int y = 0; y < hh; ++y) {
            float c[3];
            HsvToRgb(static_cast<float>(y) / std::max(1, hh - 1), 1.0f, 1.0f, c);
            for (int x = 0; x < hw; ++x) hue[static_cast<std::size_t>(y) * hw + x] = Pack(c);
        }
        DrawPixels(dc, hr.left, hr.top, hw, hh, hue.data());
        // Markers.
        const int mx = sv.left + static_cast<int>(std::lround(s * (w - 1)));
        const int my = sv.top + static_cast<int>(std::lround((1.0f - v) * (hgt - 1)));
        HGDIOBJ oldPen = SelectObject(dc, GetStockObject(WHITE_PEN));
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Ellipse(dc, mx - Px(4), my - Px(4), mx + Px(5), my + Px(5));
        const int hy = hr.top + static_cast<int>(std::lround(hue_ * (hh - 1)));
        Rectangle(dc, hr.left - Px(1), hy - Px(2), hr.right + Px(1), hy + Px(3));
        SelectObject(dc, oldPen);
        SelectObject(dc, oldBrush);

        const RECT area = RowsArea();
        rows_.Paint(dc, area, 0);
    }

    void SetFromSv(int x, int y) {
        const RECT sv = SvRect();
        const float s = std::min(std::max(static_cast<float>(x - sv.left) / std::max<LONG>(1, sv.right - sv.left - 1), 0.0f), 1.0f);
        const float v = 1.0f - std::min(std::max(static_cast<float>(y - sv.top) / std::max<LONG>(1, sv.bottom - sv.top - 1), 0.0f), 1.0f);
        float rgb[3];
        HsvToRgb(hue_, s, v, rgb);
        SetColor(0, rgb);
    }

    void SetFromHue(int y) {
        const RECT hr = HueRect();
        hue_ = std::min(std::max(static_cast<float>(y - hr.top) / std::max<LONG>(1, hr.bottom - hr.top - 1), 0.0f), 0.999f);
        float rgb[3], h, s, v;
        GetColor(0, rgb);
        RgbToHsv(rgb, h, s, v);
        if (s <= 0.0f) s = 1.0f;  // A gray turns into the picked hue.
        if (v <= 0.0f) v = 1.0f;
        HsvToRgb(hue_, s, v, rgb);
        SetColor(0, rgb);
    }

    static bool Inside(const RECT& r, int x, int y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

    void MouseDown(int x, int y, bool doubleClick) override {
        if (Inside(SvRect(), x, y)) {
            drag_ = Drag::Sv;
            SetFromSv(x, y);
        } else if (Inside(HueRect(), x, y)) {
            drag_ = Drag::Hue;
            SetFromHue(y);
        } else {
            rows_.MouseDown(hwnd_, x, y, doubleClick, 0);
        }
        Invalidate();
    }
    void MouseMove(int x, int y, bool captured) override {
        if (captured && drag_ == Drag::Sv) {
            SetFromSv(x, y);
        } else if (captured && drag_ == Drag::Hue) {
            SetFromHue(y);
        } else if (!rows_.MouseMove(x, y, captured, 0)) {
            return;
        }
        Invalidate();
    }
    void MouseUp(int x, int y) override {
        drag_ = Drag::None;
        rows_.MouseUp(x, y, 0);
        Invalidate();
    }
    void RightClick(int x, int y) override {
        if (rows_.RightClick(hwnd_, x, y, 0)) Invalidate();
    }
    void MouseLeave() override {
        if (rows_.MouseLeave()) Invalidate();
    }
    void CaptureLost() override {
        drag_ = Drag::None;
        rows_.CaptureLost();
    }
    LRESULT Message(UINT msg, WPARAM wp, LPARAM /*lp*/, bool& handled) override {
        handled = msg == RowList::kFinishEditMessage;
        if (handled) rows_.FinishEdit(wp != 0);
        return 0;
    }

    void Timer(UINT_PTR id) override {
        if (id != kQuickMenuTimer || !Visible()) return;
        if (rows_.Editing() || drag_ != Drag::None) return;
        if (space_) {
            if (!(GetAsyncKeyState(VK_SPACE) & 0x8000)) Hide();  // Space released.
            return;
        }
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
            Hide();
            return;
        }
        POINT cursor;
        GetCursorPos(&cursor);
        RECT r;
        GetWindowRect(hwnd_, &r);
        const bool inside = PtInRect(&r, cursor) != FALSE;
        RECT zone = r;
        InflateRect(&zone, Px(48), Px(48));
        const bool buttonDown = (GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON) | GetAsyncKeyState(VK_MBUTTON)) & 0x8000;
        if (!PtInRect(&zone, cursor) || (buttonDown && !inside && GetCapture() != hwnd_)) Hide();
    }

private:
    enum class Drag { None, Sv, Hue };
    RowList rows_;
    bool space_ = false;
    float hue_ = 0.0f;
    Drag drag_ = Drag::None;
};

// --- Manager ---------------------------------------------------------------------------------------------

class Manager : public SculptSettings::Listener {
public:
    ToolbarWindow toolbar;
    PaletteWindow palette;
    PanelWindow panel;
    QuickMenuWindow quick;
    bool created = false;
    bool editing = false;
    bool shown = false;

    bool Ensure() {
        if (created) return true;
        Interface* core = GetCOREInterface();
        HWND owner = core ? core->GetMAXHWnd() : nullptr;
        if (!owner) return false;
        RefreshTheme();
        if (!toolbar.Create(owner) || !palette.Create(owner) || !panel.Create(owner) || !quick.Create(owner)) {
            Destroy();
            return false;
        }
        SetTimer(toolbar.Hwnd(), kLayoutTimer, 200, nullptr);
        S().AddListener(this);
        created = true;
        CacheDisplayState();
        return true;
    }

    void Destroy() {
        if (created) S().RemoveListener(this);
        if (toolbar.Hwnd()) KillTimer(toolbar.Hwnd(), kLayoutTimer);
        quick.Destroy();
        panel.Destroy();
        palette.Destroy();
        toolbar.Destroy();
        created = false;
        shown = false;
    }

    void HideAll() {
        quick.Hide();
        panel.Show(false);
        palette.Show(false);
        toolbar.Show(false);
        shown = false;
    }

    static bool ViewportArea(RECT& out) {
        Interface7* ip7 = GetCOREInterface7();
        if (!ip7) return false;
        bool any = false;
        RECT area = {0, 0, 0, 0};
        for (int i = 0; i < ip7->getNumViewports(); ++i) {
            ViewExp& view = ip7->getViewExp(i);
            HWND hwnd = view.IsAlive() ? view.GetHWnd() : nullptr;
            if (!hwnd || !IsWindowVisible(hwnd)) continue;
            RECT r;
            GetWindowRect(hwnd, &r);
            if (r.right - r.left < 64 || r.bottom - r.top < 64) continue;
            if (any)
                UnionRect(&area, &area, &r);
            else
                area = r;
            any = true;
        }
        out = area;
        return any;
    }

    void Layout() {
        if (!created) return;
        const bool wanted = editing && S().Bool(Prop::MenusOpen);
        Interface* core = GetCOREInterface();
        HWND max = core ? core->GetMAXHWnd() : nullptr;
        RECT area;
        if (!wanted || !max || IsIconic(max) || !IsWindowVisible(max) || !ViewportArea(area)) {
            if (shown) HideAll();
            return;
        }
        const int margin = Px(6);
        const int top = area.top + Px(30);  // Below the viewport label menus.
        const int tw = toolbar.Width(), th = toolbar.Height();
        toolbar.SetBounds(area.left + margin, top, tw, std::min(th, static_cast<int>(area.bottom - top - margin)));

        const int ph = palette.Height();
        const int pw = panel.Width();
        const int panelBottom = area.bottom - margin - ph - margin;
        const int panelHeight = std::max(Px(80), std::min(panel.ContentHeight(), panelBottom - top));
        panel.SetBounds(area.right - margin - pw, top, pw, panelHeight);

        int left = area.left + margin + tw + margin, right = area.right - margin - pw - margin;
        if (right - left < Px(260)) {
            left = area.left + margin;
            right = area.right - margin;
        }
        palette.SetBounds(left, area.bottom - margin - ph, std::max(Px(120), right - left), ph);
        toolbar.Show(true);
        palette.Show(true);
        panel.Show(true);
        shown = true;
    }

    void InvalidateAll() {
        toolbar.Invalidate();
        palette.Invalidate();
        panel.Invalidate();
        quick.Invalidate();
    }

    void CacheDisplayState() {
        showMask_ = S().Bool(Prop::ShowMask);
        showGroups_ = S().Bool(Prop::ShowGroups);
        preview_ = S().Bool(Prop::SculptMaterialPreview);
        mode_ = S().Int(Prop::ToolMode);
        stencilOpacity_ = S().Value(Prop::StencilOpacity);
        resolution_ = S().Int(Prop::PaintResolution);
    }

    void OnSculptSettingsChanged() override {
        InvalidateAll();
        if (shown) Layout();  // Rows may have appeared or disappeared.
        if (SculptMeshObject* object = SculptMeshObject::EditedObject()) {
            if (S().Bool(Prop::ShowMask) != showMask_ || S().Bool(Prop::ShowGroups) != showGroups_)
                object->RefreshDisplayOptions();
            const bool sculpting = SculptMode::Get().Target() == object && SculptMode::Get().IsActive();
            if (S().Int(Prop::ToolMode) != mode_ && sculpting) {
                // Paint mode shows the paint texture; it needs the canvas and UVs first.
                if (S().Mode() == ToolMode::Paint) {
                    MSTR error;
                    if (!object->AcquirePaint(error) && error.Length() > 0) SculptCommands::Prompt(error.data());
                }
                object->SetFastDisplay(true);
                object->RefreshDisplayOptions();
            } else if (S().Bool(Prop::SculptMaterialPreview) != preview_ && sculpting) {
                object->SetFastDisplay(S().Bool(Prop::SculptMaterialPreview));
            }
            if (S().Int(Prop::PaintResolution) != resolution_ && S().Int(Prop::PaintSource) == 0) {
                const int sizes[] = {512, 1024, 2048, 4096};
                MSTR error;
                if (!object->ResizePaintTexture(sizes[std::min(std::max(S().Int(Prop::PaintResolution), 0), 3)], error) &&
                    error.Length() > 0)
                    SculptCommands::Prompt(error.data());
            }
        }
        if (S().Value(Prop::StencilOpacity) != stencilOpacity_) Stencil::Get().Refresh();
        CacheDisplayState();
        SculptMode::Get().RefreshCursor();
    }

private:
    bool showMask_ = true;
    bool showGroups_ = false;
    bool preview_ = true;
    int mode_ = 1;
    float stencilOpacity_ = 0.5f;
    int resolution_ = 2;
};

Manager& M() {
    static Manager manager;
    return manager;
}

void LayoutWindows() { M().Layout(); }

}  // namespace

namespace SculptUI {

void OnEditBegin() {
    SculptCommands::LoadSettings();  // In case the startup notification was missed.
    M().editing = true;
    if (S().Bool(Prop::MenusOpen) && M().Ensure()) M().Layout();
    MultiresRollout::Refresh();
}

void OnEditEnd() {
    M().editing = false;
    if (M().created) M().HideAll();
}

void Open() {
    S().SetBool(Prop::MenusOpen, true);
    if (M().editing && M().Ensure()) M().Layout();
    MultiresRollout::Refresh();
}

void Close() {
    S().SetBool(Prop::MenusOpen, false);
    if (M().created) M().HideAll();
    MultiresRollout::Refresh();
}

void Toggle() {
    if (IsWanted())
        Close();
    else
        Open();
}

bool IsOpen() { return M().created && M().shown; }

bool IsWanted() { return S().Bool(Prop::MenusOpen); }

void Refresh() {
    if (M().created) M().InvalidateAll();
    MultiresRollout::Refresh();
}

void ShowQuickMenu(POINT screen, bool whileSpaceHeld) {
    if (!M().editing || !M().Ensure()) return;
    if (whileSpaceHeld && M().quick.Visible()) return;  // Key repeat while Space is held.
    M().quick.Popup(screen, whileSpaceHeld);
}

void HideQuickMenu() {
    if (M().created) M().quick.Hide();
}

bool QuickMenuVisible() { return M().created && M().quick.Visible(); }

void ShowAlphasPage() {
    if (!IsWanted()) Open();
    M().palette.SetPage(PaletteWindow::Page::Alphas);
}

void Shutdown() {
    if (Adjust().Visible()) Adjust().Finish(false);
    Adjust().Destroy();
    if (M().created) M().Destroy();
    M().editing = false;
}

}  // namespace SculptUI
