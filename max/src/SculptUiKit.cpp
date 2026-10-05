#include "SculptUiKit.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <map>
#include <tuple>

#include <commctrl.h>
#include <commdlg.h>
#include <custcont.h>
#include <icolorman.h>
#include <winutil.h>

#include "SculptMeshPlugin.h"

namespace ui {

namespace {

const wchar_t kWindowClass[] = L"SculptMeshFloatingWindow";
LRESULT CALLBACK FloatingWindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

Theme theme;
bool themeReady = false;
HFONT boldFont = nullptr;

COLORREF Luma(COLORREF c, float amount) {  // amount > 0 lightens, < 0 darkens.
    const COLORREF target = amount > 0.0f ? RGB(255, 255, 255) : RGB(0, 0, 0);
    return Blend(c, target, std::fabs(amount));
}

float Luminance(COLORREF c) {
    return (0.299f * GetRValue(c) + 0.587f * GetGValue(c) + 0.114f * GetBValue(c)) / 255.0f;
}

HWND MaxWindow() {
    Interface* core = GetCOREInterface();
    return core ? core->GetMAXHWnd() : nullptr;
}

std::wstring FormatValue(float v, int decimals) {
    wchar_t buffer[64];
    std::swprintf(buffer, 64, L"%.*f", std::max(0, std::min(decimals, 4)), static_cast<double>(v));
    return buffer;
}

// --- Procedural brush icons ------------------------------------------------------------

float Gauss(float x, float y, float cx, float cy, float s) {
    const float dx = x - cx, dy = y - cy;
    return std::exp(-(dx * dx + dy * dy) / s);
}

float Smoothstep(float e0, float e1, float x) {
    const float t = std::min(std::max((x - e0) / (e1 - e0), 0.0f), 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// Height relief drawn on the sphere for each brush (u right, v down, -1..1).
float IconHeight(sculpt::BrushType b, float u, float v) {
    using sculpt::BrushType;
    const float r2 = u * u + v * v;
    const float d = (u + v) * 0.70710678f;  // Diagonal distance.
    switch (b) {
        case BrushType::Sculpt: return 0.32f * Gauss(u, v, 0.0f, 0.0f, 0.12f);
        case BrushType::Inflate: return 0.42f * Gauss(u, v, 0.0f, 0.0f, 0.35f);
        case BrushType::Pinch: return -0.22f * std::exp(-u * u / 0.006f) * (1.0f - Smoothstep(0.5f, 0.9f, std::fabs(v)));
        case BrushType::Clay: return 0.14f * (1.0f - Smoothstep(0.38f, 0.5f, std::sqrt(r2)));
        case BrushType::ClayBuildup: return 0.18f * (1.0f - Smoothstep(0.36f, 0.42f, std::max(std::fabs(u), std::fabs(v))));
        case BrushType::Carve: return -0.24f * std::exp(-d * d / 0.012f) * (1.0f - Smoothstep(0.6f, 0.95f, std::fabs(u - v) * 0.7071f));
        case BrushType::Knife: return -0.3f * std::max(0.0f, 1.0f - std::fabs(d) / 0.07f) * (1.0f - Smoothstep(0.6f, 0.95f, std::fabs(u - v) * 0.7071f));
        case BrushType::Contrast: return 0.09f * std::sin(9.0f * u) * std::sin(9.0f * v) * (1.0f - Smoothstep(0.4f, 0.8f, std::sqrt(r2)));
        case BrushType::Scrape: return -0.05f * std::fabs(std::sin(11.0f * v)) * (1.0f - Smoothstep(0.35f, 0.7f, std::sqrt(r2)));
        case BrushType::Polish: return -0.12f * (1.0f - Smoothstep(0.3f, 0.55f, std::sqrt(r2))) * (r2 * 0.5f + 0.5f);
        case BrushType::Move: return 0.3f * Gauss(u, v, 0.28f, -0.28f, 0.1f) + 0.12f * Gauss(u, v, 0.1f, -0.1f, 0.08f);
        case BrushType::SnakeHook: {
            float h = 0.0f;
            for (int i = 0; i < 6; ++i) {
                const float t = static_cast<float>(i) / 5.0f;
                h = std::max(h, (0.34f - 0.18f * t) * Gauss(u, v, -0.25f + 0.6f * t, 0.25f - 0.6f * t, 0.03f + 0.02f * (1.0f - t)));
            }
            return h;
        }
        case BrushType::SmoothGroupBorder: return 0.0f;
        case BrushType::Revert: {  // A ridge fading back to the plain surface.
            const float rr = std::sqrt(r2);
            return 0.16f * std::exp(-(rr - 0.42f) * (rr - 0.42f) / 0.006f) * (0.4f + 0.6f * Smoothstep(-0.6f, 0.6f, u));
        }
        default: return 0.0f;
    }
}

void IconTint(sculpt::BrushType b, float u, float v, float rgb[3]) {
    using sculpt::BrushType;
    if (b == BrushType::FaceGroups) {
        const int q = (u < -0.1f ? 0 : 1) + (v < 0.15f ? 0 : 2);
        const float colors[4][3] = {{0.85f, 0.55f, 0.45f}, {0.5f, 0.75f, 0.55f}, {0.5f, 0.6f, 0.85f}, {0.85f, 0.78f, 0.45f}};
        for (int k = 0; k < 3; ++k) rgb[k] = colors[q][k];
    } else if (b == BrushType::SmoothGroupBorder) {
        const float t = Smoothstep(-0.12f, 0.12f, u + 0.2f * v);
        const float a[3] = {0.85f, 0.6f, 0.45f}, c[3] = {0.5f, 0.65f, 0.85f};
        for (int k = 0; k < 3; ++k) rgb[k] = a[k] + (c[k] - a[k]) * t;
    } else if (b == BrushType::MaskPaint) {
        const float m = 1.0f - Smoothstep(-0.05f, 0.05f, u);
        for (int k = 0; k < 3; ++k) rgb[k] = 0.72f - 0.5f * m;
    } else {
        rgb[0] = 0.74f;
        rgb[1] = 0.70f;
        rgb[2] = 0.66f;
    }
}

std::vector<std::uint32_t> RenderBrushIcon(sculpt::BrushType b, int size, COLORREF background) {
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(size) * size);
    const float bgR = GetRValue(background) / 255.0f, bgG = GetGValue(background) / 255.0f, bgB = GetBValue(background) / 255.0f;
    const float radius = size * 0.5f - 1.0f, center = size * 0.5f;
    const float lx = -0.45f, ly = -0.55f, lz = 0.70f;  // Light from the upper left.
    const float eps = 2.0f / static_cast<float>(size);
    const float spec = b == sculpt::BrushType::Polish ? 0.55f : 0.25f;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const float px = (static_cast<float>(x) + 0.5f - center) / radius;
            const float py = (static_cast<float>(y) + 0.5f - center) / radius;
            const float r = std::sqrt(px * px + py * py);
            const float coverage = std::min(std::max((1.0f - r) * radius + 0.5f, 0.0f), 1.0f);  // Anti-aliased rim.
            float out[3] = {bgR, bgG, bgB};
            if (coverage > 0.0f) {
                const float nz0 = std::sqrt(std::max(0.0f, 1.0f - std::min(r, 1.0f) * std::min(r, 1.0f)));
                const float hx = (IconHeight(b, px + eps, py) - IconHeight(b, px - eps, py)) / (2.0f * eps);
                const float hy = (IconHeight(b, px, py + eps) - IconHeight(b, px, py - eps)) / (2.0f * eps);
                float nx = px - hx * nz0, ny = py - hy * nz0, nz = nz0;
                const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
                if (len > 0.0f) {
                    nx /= len;
                    ny /= len;
                    nz /= len;
                }
                const float diffuse = std::max(0.0f, nx * lx + ny * ly + nz * lz);
                // Blinn specular with the viewer along +z.
                float hxv = lx, hyv = ly, hzv = lz + 1.0f;
                const float hl = std::sqrt(hxv * hxv + hyv * hyv + hzv * hzv);
                hxv /= hl;
                hyv /= hl;
                hzv /= hl;
                const float highlight = spec * std::pow(std::max(0.0f, nx * hxv + ny * hyv + nz * hzv), 24.0f);
                float base[3];
                IconTint(b, px, py, base);
                for (int k = 0; k < 3; ++k) {
                    const float lit = std::min(1.0f, base[k] * (0.22f + 0.85f * diffuse) + highlight);
                    out[k] = out[k] + (lit - out[k]) * coverage;
                }
            }
            const auto to8 = [](float c) { return static_cast<std::uint32_t>(std::lround(std::min(std::max(c, 0.0f), 1.0f) * 255.0f)); };
            pixels[static_cast<std::size_t>(y) * size + x] = (to8(out[0]) << 16) | (to8(out[1]) << 8) | to8(out[2]);
        }
    }
    return pixels;
}

std::map<std::tuple<int, int, COLORREF>, std::vector<std::uint32_t>>& IconCache() {
    static std::map<std::tuple<int, int, COLORREF>, std::vector<std::uint32_t>> cache;
    return cache;
}

}  // namespace

void DrawPixels(HDC dc, int x, int y, int width, int height, const std::uint32_t* pixels) {
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;  // Top-down.
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    SetDIBitsToDevice(dc, x, y, static_cast<DWORD>(width), static_cast<DWORD>(height), 0, 0, 0,
                      static_cast<UINT>(height), pixels, &info, DIB_RGB_COLORS);
}

namespace {

// --- Value edit box -------------------------------------------------------------------------

LRESULT CALLBACK EditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR /*id*/, DWORD_PTR data) {
    HWND host = reinterpret_cast<HWND>(data);
    switch (msg) {
        case WM_GETDLGCODE:
            return DLGC_WANTALLKEYS;
        case WM_KEYDOWN:
            if (wp == VK_RETURN || wp == VK_ESCAPE) {
                PostMessage(host, RowList::kFinishEditMessage, wp == VK_RETURN ? 1 : 0, 0);
                return 0;
            }
            break;
        case WM_CHAR:
            if (wp == VK_RETURN || wp == VK_ESCAPE) return 0;  // No beep.
            break;
        case WM_KILLFOCUS:
            PostMessage(host, RowList::kFinishEditMessage, 1, 0);
            break;
        default:
            break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

}  // namespace

// --- Metrics & theme -------------------------------------------------------------------------

int Px(int value) { return static_cast<int>(std::lround(static_cast<float>(value) * MaxSDK::GetUIScaleFactor())); }

COLORREF Blend(COLORREF a, COLORREF b, float t) {
    t = std::min(std::max(t, 0.0f), 1.0f);
    const auto mix = [t](int x, int y) { return static_cast<int>(std::lround(x + (y - x) * t)); };
    return RGB(mix(GetRValue(a), GetRValue(b)), mix(GetGValue(a), GetGValue(b)), mix(GetBValue(a), GetBValue(b)));
}

void RefreshTheme() {
    IColorManager* cm = ColorMan();
    const COLORREF background = cm ? cm->GetColor(kBackgroundOdd) : RGB(68, 68, 68);
    const COLORREF text = cm ? cm->GetColor(kText) : RGB(220, 220, 220);
    const COLORREF button = cm ? cm->GetColor(kButton) : RGB(88, 88, 88);
    const COLORREF accent = cm ? cm->GetColor(kActiveCommand) : RGB(86, 157, 229);
    const bool dark = Luminance(background) < 0.5f;
    theme.background = Luma(background, dark ? -0.12f : -0.04f);
    theme.panel = background;
    theme.header = Luma(background, dark ? 0.07f : -0.08f);
    theme.text = text;
    theme.textDim = Blend(text, background, 0.45f);
    theme.button = button;
    theme.buttonHot = Luma(button, dark ? 0.12f : -0.08f);
    theme.accent = accent;
    theme.accentText = Luminance(accent) > 0.6f ? RGB(20, 20, 20) : RGB(255, 255, 255);
    theme.border = Luma(background, -0.35f);
    theme.track = Luma(background, dark ? -0.3f : -0.15f);
    themeReady = true;
    if (boldFont) {
        DeleteObject(boldFont);
        boldFont = nullptr;
    }
    ClearIconCache();
}

const Theme& GetTheme() {
    if (!themeReady) RefreshTheme();
    return theme;
}

HFONT Font() {
    Interface* core = GetCOREInterface();
    HFONT font = core ? core->GetAppHFont() : nullptr;
    return font ? font : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
}

HFONT BoldFont() {
    if (!boldFont) {
        LOGFONTW lf = {};
        GetObjectW(Font(), sizeof(lf), &lf);
        lf.lfWeight = FW_BOLD;
        boldFont = CreateFontIndirectW(&lf);
    }
    return boldFont ? boldFont : Font();
}

// --- Drawing helpers -----------------------------------------------------------------------------

void Fill(HDC dc, const RECT& r, COLORREF color) {
    SetDCBrushColor(dc, color);
    FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

void Frame(HDC dc, const RECT& r, COLORREF color) {
    SetDCBrushColor(dc, color);
    FrameRect(dc, &r, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

void RoundBox(HDC dc, const RECT& r, int radius, COLORREF fill, COLORREF border) {
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(DC_BRUSH));
    HGDIOBJ oldPen = SelectObject(dc, GetStockObject(DC_PEN));
    SetDCBrushColor(dc, fill);
    SetDCPenColor(dc, border);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
}

void Text(HDC dc, const RECT& r, const std::wstring& text, COLORREF color, UINT format, HFONT font) {
    HGDIOBJ old = SelectObject(dc, font ? font : Font());
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    RECT copy = r;
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &copy, format | DT_NOPREFIX);
    SelectObject(dc, old);
}

void DrawGlyph(HDC dc, const RECT& r, Glyph glyph, COLORREF color) {
    const int w = r.right - r.left, h = r.bottom - r.top;
    const int cx = r.left + w / 2, cy = r.top + h / 2;
    const int s = std::min(w, h);
    const int u = std::max(1, s / 16);  // Glyph unit.
    HPEN pen = CreatePen(PS_SOLID, std::max(1, s / 14), color);
    HPEN dotted = CreatePen(PS_DOT, 1, color);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    SetDCBrushColor(dc, color);
    SetBkMode(dc, TRANSPARENT);
    switch (glyph) {
        case Glyph::Select: {
            const POINT arrow[] = {{cx - 4 * u, cy - 6 * u}, {cx - 4 * u, cy + 5 * u}, {cx - 1 * u, cy + 2 * u},
                                   {cx + 1 * u, cy + 7 * u}, {cx + 3 * u, cy + 6 * u}, {cx + 1 * u, cy + 1 * u},
                                   {cx + 5 * u, cy + 1 * u}};
            SelectObject(dc, GetStockObject(DC_BRUSH));
            Polygon(dc, arrow, 7);
            break;
        }
        case Glyph::PaintMask:
            Ellipse(dc, cx - 6 * u, cy - 6 * u, cx + 6 * u, cy + 6 * u);
            SelectObject(dc, GetStockObject(DC_BRUSH));
            Pie(dc, cx - 6 * u, cy - 6 * u, cx + 6 * u, cy + 6 * u, cx, cy + 6 * u, cx, cy - 6 * u);
            break;
        case Glyph::Rectangle:
            SelectObject(dc, dotted);
            Rectangle(dc, cx - 6 * u, cy - 5 * u, cx + 6 * u, cy + 5 * u);
            break;
        case Glyph::Lasso: {
            SelectObject(dc, dotted);
            Ellipse(dc, cx - 6 * u, cy - 5 * u, cx + 6 * u, cy + 3 * u);
            SelectObject(dc, pen);
            const POINT tail[] = {{cx - 2 * u, cy + 3 * u}, {cx - 3 * u, cy + 6 * u}, {cx - 1 * u, cy + 7 * u}};
            Polyline(dc, tail, 3);
            break;
        }
        case Glyph::Draw: {
            POINT wave[13];
            for (int i = 0; i < 13; ++i)
                wave[i] = {cx - 6 * u + i * u, cy + static_cast<int>(std::lround(3.0 * u * std::sin(i * 0.6)))};
            Polyline(dc, wave, 13);
            break;
        }
        case Glyph::Stamp:
            Ellipse(dc, cx - 6 * u, cy - 6 * u, cx + 6 * u, cy + 6 * u);
            SelectObject(dc, GetStockObject(DC_BRUSH));
            Ellipse(dc, cx - 2 * u, cy - 2 * u, cx + 2 * u, cy + 2 * u);
            break;
        case Glyph::Drag:
            Ellipse(dc, cx - 6 * u, cy - 6 * u, cx + 6 * u, cy + 6 * u);
            MoveToEx(dc, cx, cy, nullptr);
            LineTo(dc, cx + 4 * u, cy - 4 * u);
            LineTo(dc, cx + 1 * u, cy - 4 * u);
            MoveToEx(dc, cx + 4 * u, cy - 4 * u, nullptr);
            LineTo(dc, cx + 4 * u, cy - 1 * u);
            break;
        case Glyph::ColorMix:
            Ellipse(dc, cx - 6 * u, cy - 4 * u, cx + 2 * u, cy + 4 * u);
            Ellipse(dc, cx - 2 * u, cy - 4 * u, cx + 6 * u, cy + 4 * u);
            break;
        case Glyph::Scatter: {
            SelectObject(dc, GetStockObject(DC_BRUSH));
            const int dots[5][2] = {{-4, -3}, {2, -5}, {5, 1}, {-1, 2}, {-5, 5}};
            for (const auto& d : dots)
                Ellipse(dc, cx + d[0] * u - u - 1, cy + d[1] * u - u - 1, cx + d[0] * u + u + 1, cy + d[1] * u + u + 1);
            break;
        }
        case Glyph::NoAlpha:
            Ellipse(dc, cx - 6 * u, cy - 6 * u, cx + 6 * u, cy + 6 * u);
            MoveToEx(dc, cx - 4 * u, cy + 4 * u, nullptr);
            LineTo(dc, cx + 4 * u, cy - 4 * u);
            break;
        case Glyph::Swap:
            MoveToEx(dc, cx - 5 * u, cy - 2 * u, nullptr);
            LineTo(dc, cx + 5 * u, cy - 2 * u);
            LineTo(dc, cx + 3 * u, cy - 4 * u);
            MoveToEx(dc, cx + 5 * u, cy + 2 * u, nullptr);
            LineTo(dc, cx - 5 * u, cy + 2 * u);
            LineTo(dc, cx - 3 * u, cy + 4 * u);
            break;
        case Glyph::DropDown: {
            const POINT tri[] = {{cx - 3 * u, cy - u}, {cx + 3 * u, cy - u}, {cx, cy + 2 * u}};
            SelectObject(dc, GetStockObject(DC_BRUSH));
            Polygon(dc, tri, 3);
            break;
        }
    }
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
    DeleteObject(dotted);
}

void DrawBrushIcon(HDC dc, const RECT& r, sculpt::BrushType brush, COLORREF background) {
    const int size = std::min(r.right - r.left, r.bottom - r.top);
    if (size <= 2) return;
    auto& cache = IconCache();
    const auto key = std::make_tuple(static_cast<int>(brush), size, background);
    auto it = cache.find(key);
    if (it == cache.end()) {
        if (cache.size() > 256) cache.clear();
        it = cache.emplace(key, RenderBrushIcon(brush, size, background)).first;
    }
    const int x = r.left + (r.right - r.left - size) / 2, y = r.top + (r.bottom - r.top - size) / 2;
    DrawPixels(dc, x, y, size, size, it->second.data());
}

void DrawGrayImage(HDC dc, const RECT& r, const unsigned char* gray, int size) {
    if (!gray || size <= 0) return;
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(size) * size);
    for (std::size_t i = 0; i < pixels.size(); ++i) {
        const std::uint32_t g = gray[i];
        pixels[i] = (g << 16) | (g << 8) | g;
    }
    const int w = r.right - r.left, h = r.bottom - r.top;
    if (w == size && h == size) {
        DrawPixels(dc, r.left, r.top, size, size, pixels.data());
        return;
    }
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = size;
    info.bmiHeader.biHeight = -size;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    SetStretchBltMode(dc, HALFTONE);
    StretchDIBits(dc, r.left, r.top, w, h, 0, 0, size, size, pixels.data(), &info, DIB_RGB_COLORS, SRCCOPY);
}

void ClearIconCache() { IconCache().clear(); }

int PopupMenu(const std::vector<MenuItem>& items, POINT screen) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return 0;
    for (const MenuItem& item : items) {
        if (item.id == 0) {
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            continue;
        }
        UINT flags = MF_STRING;
        if (item.checked) flags |= MF_CHECKED;
        if (!item.enabled) flags |= MF_GRAYED;
        AppendMenuW(menu, flags, static_cast<UINT_PTR>(item.id), item.text.c_str());
    }
    HWND owner = MaxWindow();
    const int chosen = static_cast<int>(TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON,
                                                         screen.x, screen.y, owner, nullptr));
    DestroyMenu(menu);
    if (owner) PostMessage(owner, WM_NULL, 0, 0);
    return chosen;
}

bool PickColor(float rgb[3]) {
    static COLORREF custom[16] = {};
    const auto to8 = [](float c) { return static_cast<BYTE>(std::lround(std::min(std::max(c, 0.0f), 1.0f) * 255.0f)); };
    CHOOSECOLORW cc = {};
    cc.lStructSize = sizeof(cc);
    cc.hwndOwner = MaxWindow();
    cc.rgbResult = RGB(to8(rgb[0]), to8(rgb[1]), to8(rgb[2]));
    cc.lpCustColors = custom;
    cc.Flags = CC_RGBINIT | CC_FULLOPEN;
    DisableAccelerators();
    const BOOL ok = ChooseColorW(&cc);
    EnableAccelerators();
    if (!ok) return false;
    rgb[0] = GetRValue(cc.rgbResult) / 255.0f;
    rgb[1] = GetGValue(cc.rgbResult) / 255.0f;
    rgb[2] = GetBValue(cc.rgbResult) / 255.0f;
    return true;
}

// --- FloatingWindow --------------------------------------------------------------------------------

FloatingWindow::~FloatingWindow() { Destroy(); }

namespace {

bool RegisterWindowClass() {
    static bool registered = false;
    if (registered) return true;
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = &FloatingWindowProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kWindowClass;
    registered = RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    return registered;
}

}  // namespace

bool FloatingWindow::Create(HWND owner) {
    if (hwnd_) return true;
    if (!RegisterWindowClass()) return false;
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kWindowClass, L"", WS_POPUP | WS_CLIPCHILDREN, 0, 0,
                            10, 10, owner, nullptr, hInstance, this);
    return hwnd_ != nullptr;
}

bool FloatingWindow::CreateChild(HWND parent) {
    if (hwnd_) return true;
    if (!RegisterWindowClass()) return false;
    hwnd_ = CreateWindowExW(0, kWindowClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, 10, 10, parent, nullptr,
                            hInstance, this);
    return hwnd_ != nullptr;
}

void FloatingWindow::Destroy() {
    if (hwnd_) {
        HWND hwnd = hwnd_;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        hwnd_ = nullptr;
        DestroyWindow(hwnd);
    }
}

void FloatingWindow::Show(bool show) {
    if (!hwnd_) return;
    if (show != (IsWindowVisible(hwnd_) != FALSE)) ShowWindow(hwnd_, show ? SW_SHOWNOACTIVATE : SW_HIDE);
}

void FloatingWindow::Invalidate() {
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void FloatingWindow::SetBounds(int x, int y, int width, int height) {
    if (!hwnd_) return;
    RECT current;
    GetWindowRect(hwnd_, &current);
    const bool sameSize = current.right - current.left == width && current.bottom - current.top == height;
    if (current.left == x && current.top == y && sameSize) return;
    SetWindowPos(hwnd_, nullptr, x, y, width, height, SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER);
    if (!sameSize && cornerRadius_ > 0)  // The window owns the region afterwards.
        SetWindowRgn(hwnd_, CreateRoundRectRgn(0, 0, width + 1, height + 1, cornerRadius_, cornerRadius_), TRUE);
    Invalidate();
}

RECT FloatingWindow::ClientRect() const {
    RECT r = {0, 0, 0, 0};
    if (hwnd_) GetClientRect(hwnd_, &r);
    return r;
}

LRESULT CALLBACK FloatingWindow::Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        const CREATESTRUCTW* cs = reinterpret_cast<const CREATESTRUCTW*>(lp);
        FloatingWindow* created = static_cast<FloatingWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(created));
        if (created) created->hwnd_ = hwnd;
    }
    FloatingWindow* self = reinterpret_cast<FloatingWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self || self->hwnd_ != hwnd) return DefWindowProcW(hwnd, msg, wp, lp);
    return self->Handle(msg, wp, lp);
}

LRESULT FloatingWindow::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    bool handled = false;
    const LRESULT result = Message(msg, wp, lp, handled);
    if (handled) return result;
    const int x = static_cast<short>(LOWORD(lp)), y = static_cast<short>(HIWORD(lp));
    switch (msg) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd_, &ps);
            const RECT client = ClientRect();
            const int w = std::max<int>(1, client.right), h = std::max<int>(1, client.bottom);
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bitmap = CreateCompatibleBitmap(dc, w, h);
            HGDIOBJ old = SelectObject(mem, bitmap);
            Paint(mem, client);
            BitBlt(dc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bitmap);
            DeleteDC(mem);
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
            SetCapture(hwnd_);
            MouseDown(x, y, msg == WM_LBUTTONDBLCLK);
            return 0;
        case WM_MOUSEMOVE:
            if (!tracking_) {
                TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd_, 0};
                tracking_ = TrackMouseEvent(&tme) != FALSE;
            }
            MouseMove(x, y, GetCapture() == hwnd_);
            return 0;
        case WM_LBUTTONUP:
            if (GetCapture() == hwnd_) ReleaseCapture();
            MouseUp(x, y);
            return 0;
        case WM_RBUTTONUP:
            RightClick(x, y);
            return 0;
        case WM_MOUSEWHEEL: {
            POINT p = {x, y};
            ScreenToClient(hwnd_, &p);
            MouseWheel(GET_WHEEL_DELTA_WPARAM(wp), p.x, p.y);
            return 0;
        }
        case WM_MOUSELEAVE:
            tracking_ = false;
            MouseLeave();
            return 0;
        case WM_CAPTURECHANGED:
            CaptureLost();
            return 0;
        case WM_TIMER:
            Timer(wp);
            return 0;
        case WM_CLOSE:
            return 0;  // Closed through the Sculpt Mesh UI only.
        default:
            break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

// --- RowList -----------------------------------------------------------------------------------------

void RowList::Add(Row row) {
    // Attach to the latest section.
    for (int i = static_cast<int>(rows_.size()) - 1; i >= 0; --i) {
        if (rows_[static_cast<std::size_t>(i)].kind == Kind::Section) {
            row.section = i;
            break;
        }
    }
    rows_.push_back(std::move(row));
}

void RowList::Section(const std::wstring& title, bool expanded) {
    Row row;
    row.kind = Kind::Section;
    row.label = title;
    row.expanded = expanded;
    rows_.push_back(std::move(row));
}

void RowList::Buttons(std::vector<Button> buttons, Visible visible) {
    Row row;
    row.kind = Kind::Buttons;
    row.buttons = std::move(buttons);
    row.visible = std::move(visible);
    Add(std::move(row));
}

void RowList::Slider(SliderSpec spec, Visible visible) {
    Row row;
    row.kind = Kind::Slider;
    row.label = spec.label;
    row.slider = std::move(spec);
    row.visible = std::move(visible);
    Add(std::move(row));
}

void RowList::Check(const std::wstring& label, std::function<bool()> get, std::function<void(bool)> set, Visible visible) {
    Row row;
    row.kind = Kind::Check;
    row.label = label;
    row.getBool = std::move(get);
    row.setBool = std::move(set);
    row.visible = std::move(visible);
    Add(std::move(row));
}

void RowList::Choice(const std::wstring& label, std::vector<std::wstring> options, std::function<int()> get,
                     std::function<void(int)> set, Visible visible) {
    Row row;
    row.kind = Kind::Choice;
    row.label = label;
    row.options = std::move(options);
    row.getIndex = std::move(get);
    row.setIndex = std::move(set);
    row.visible = std::move(visible);
    Add(std::move(row));
}

void RowList::Colors(std::function<void(int, float[3])> get, std::function<void(int, const float[3])> set,
                     std::function<void()> swap, Visible visible) {
    Row row;
    row.kind = Kind::Colors;
    row.getColor = std::move(get);
    row.setColor = std::move(set);
    row.swap = std::move(swap);
    row.visible = std::move(visible);
    Add(std::move(row));
}

void RowList::Note(std::function<std::wstring()> text, Visible visible) {
    Row row;
    row.kind = Kind::Note;
    row.text = std::move(text);
    row.visible = std::move(visible);
    Add(std::move(row));
}

void RowList::List(std::function<int()> count, std::function<std::wstring(int)> item, std::function<int()> selected,
                   std::function<void(int)> select, Visible visible) {
    Row row;
    row.kind = Kind::List;
    row.count = std::move(count);
    row.item = std::move(item);
    row.selected = std::move(selected);
    row.select = std::move(select);
    row.visible = std::move(visible);
    Add(std::move(row));
}

void RowList::Text(const std::wstring& label, std::function<std::wstring()> get, std::function<void(const std::wstring&)> set,
                   Visible visible) {
    Row row;
    row.kind = Kind::Text;
    row.label = label;
    row.text = std::move(get);
    row.setText = std::move(set);
    row.visible = std::move(visible);
    Add(std::move(row));
}

int RowList::ListItemHeight() const { return Px(20); }

RECT RowList::TextBox(const RECT& r) {
    const int split = r.left + (r.right - r.left) * 30 / 100;
    return RECT{split, r.top + Px(2), r.right - Px(4), r.bottom - Px(2)};
}

int RowList::RowHeight(const Row& row) const {
    switch (row.kind) {
        case Kind::Section: return Px(22);
        case Kind::Buttons: return Px(25);
        case Kind::Colors: return Px(28);
        case Kind::Note: return Px(18);
        case Kind::List: {
            const int n = row.count ? row.count() : 0;
            return ListItemHeight() * std::max(n, 2) + Px(6);
        }
        default: return Px(22);
    }
}

std::vector<RowList::Placed> RowList::Layout(int width) const {
    std::vector<Placed> out;
    int y = 0;
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        const Row& row = rows_[i];
        if (row.kind != Kind::Section) {
            if (row.section >= 0 && !rows_[static_cast<std::size_t>(row.section)].expanded) continue;
            if (row.visible && !row.visible()) continue;
        } else if (!out.empty()) {
            y += Px(4);  // Gap between sections.
        }
        const int h = RowHeight(row);
        out.push_back({static_cast<int>(i), RECT{0, y, width, y + h}});
        y += h;
    }
    return out;
}

int RowList::ContentHeight(int width) const {
    const std::vector<Placed> placed = Layout(width);
    return placed.empty() ? 0 : placed.back().rect.bottom + Px(4);
}

RECT RowList::SliderValue(const RECT& r) {
    return RECT{r.right - Px(52), r.top + Px(3), r.right - Px(4), r.bottom - Px(3)};
}

RECT RowList::SliderTrack(const RECT& r) {
    const int left = r.left + (r.right - r.left) * 42 / 100;
    const RECT value = SliderValue(r);
    const int mid = (r.top + r.bottom) / 2;
    return RECT{left, mid - Px(3), value.left - Px(6), mid + Px(3)};
}

float RowList::ToT(const SliderSpec& s, float v) {
    if (s.toPosition) return std::min(std::max(s.toPosition(v), 0.0f), 1.0f);
    const float hi = MaxOf(s);
    const float t = (std::min(std::max(v, s.min), hi) - s.min) / std::max(hi - s.min, 1e-6f);
    return s.quadratic ? std::sqrt(t) : t;
}

float RowList::FromT(const SliderSpec& s, float t) {
    t = std::min(std::max(t, 0.0f), 1.0f);
    if (s.toValue) return s.toValue(t);
    if (s.quadratic) t *= t;
    float v = s.min + t * (MaxOf(s) - s.min);
    if (s.decimals == 0) v = std::round(v);
    return v;
}

RECT RowList::ButtonRect(const Row& row, const RECT& r, int index) {
    const int n = std::max<int>(1, static_cast<int>(row.buttons.size()));
    const int gap = Px(4);
    const int left = r.left + Px(6), right = r.right - Px(6);
    const int w = (right - left - gap * (n - 1)) / n;
    const int x = left + index * (w + gap);
    return RECT{x, r.top + Px(2), index == n - 1 ? right : x + w, r.bottom - Px(2)};
}

int RowList::ButtonAt(const Row& row, const RECT& r, int x) {
    for (int i = 0; i < static_cast<int>(row.buttons.size()); ++i) {
        const RECT b = ButtonRect(row, r, i);
        if (x >= b.left && x < b.right) return i;
    }
    return -1;
}

RECT RowList::ColorRect(const RECT& r, int which) {
    const int left = r.left + Px(6), right = r.right - Px(6);
    const int swapW = Px(26);
    const int w = (right - left - swapW - Px(8)) / 2;
    if (which == 0) return RECT{left, r.top + Px(3), left + w, r.bottom - Px(3)};
    if (which == 1) return RECT{left + w + Px(4), r.top + Px(3), left + w + Px(4) + swapW, r.bottom - Px(3)};
    return RECT{right - w, r.top + Px(3), right, r.bottom - Px(3)};
}

void RowList::Paint(HDC dc, const RECT& area, int scroll) const {
    const Theme& t = GetTheme();
    width_ = area.right - area.left;
    origin_ = {area.left, area.top};
    for (const Placed& p : Layout(width_)) {
        const Row& row = rows_[static_cast<std::size_t>(p.row)];
        RECT r = p.rect;
        OffsetRect(&r, area.left, area.top - scroll);
        if (r.bottom < area.top || r.top > area.bottom) continue;
        const bool hot = p.row == hotRow_;
        switch (row.kind) {
            case Kind::Section: {
                Fill(dc, r, t.header);
                RECT arrow = {r.left + Px(4), r.top, r.left + Px(18), r.bottom};
                ui::Text(dc, arrow, row.expanded ? L"\x25BE" : L"\x25B8", t.text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                RECT label = {r.left + Px(20), r.top, r.right - Px(4), r.bottom};
                ui::Text(dc, label, row.label, t.text, DT_LEFT | DT_VCENTER | DT_SINGLELINE, BoldFont());
                break;
            }
            case Kind::Slider: {
                const SliderSpec& s = row.slider;
                RECT label = {r.left + Px(8), r.top, SliderTrack(r).left - Px(4), r.bottom};
                ui::Text(dc, label, s.label, t.text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                const RECT track = SliderTrack(r);
                const float value = (drag_ == p.row && s.applyOnRelease) ? pending_ : (s.get ? s.get() : 0.0f);
                RoundBox(dc, track, Px(4), t.track, t.track);
                RECT fill = track;
                fill.right = track.left + static_cast<int>(std::lround((track.right - track.left) * ToT(s, value)));
                if (fill.right > fill.left + 1) RoundBox(dc, fill, Px(4), t.accent, t.accent);
                const int knobX = fill.right;
                RECT knob = {knobX - Px(4), track.top - Px(3), knobX + Px(4), track.bottom + Px(3)};
                RoundBox(dc, knob, Px(3), hot || drag_ == p.row ? t.buttonHot : t.button, t.border);
                const RECT box = SliderValue(r);
                Fill(dc, box, t.background);
                Frame(dc, box, t.border);
                RECT text = box;
                text.right -= Px(3);
                ui::Text(dc, text, s.format ? s.format(value) : FormatValue(value, s.decimals), t.text,
                     DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
                break;
            }
            case Kind::Check: {
                const bool on = row.getBool && row.getBool();
                const int box = Px(13);
                RECT b = {r.left + Px(8), (r.top + r.bottom - box) / 2, r.left + Px(8) + box, (r.top + r.bottom + box) / 2};
                Fill(dc, b, on ? t.accent : t.background);
                Frame(dc, b, hot ? t.text : t.border);
                if (on) ui::Text(dc, b, L"\x2713", t.accentText, DT_CENTER | DT_VCENTER | DT_SINGLELINE, BoldFont());
                RECT label = {b.right + Px(6), r.top, r.right - Px(4), r.bottom};
                ui::Text(dc, label, row.label, t.text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                break;
            }
            case Kind::Buttons: {
                for (int i = 0; i < static_cast<int>(row.buttons.size()); ++i) {
                    const Button& button = row.buttons[static_cast<std::size_t>(i)];
                    const RECT b = ButtonRect(row, r, i);
                    const bool enabled = !button.enabled || button.enabled();
                    const bool checked = button.checked && button.checked();
                    const bool pressed = pressedRow_ == p.row && pressedButton_ == i;
                    COLORREF fill = checked ? t.accent : (hot && hotButton_ == i && enabled ? t.buttonHot : t.button);
                    if (pressed) fill = Blend(fill, t.accent, 0.5f);
                    RoundBox(dc, b, Px(4), fill, t.border);
                    ui::Text(dc, b, button.text ? button.text() : button.label, enabled ? (checked ? t.accentText : t.text) : t.textDim,
                         DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                }
                break;
            }
            case Kind::Choice: {
                const int split = r.left + (r.right - r.left) * 42 / 100;
                RECT label = {r.left + Px(8), r.top, split - Px(4), r.bottom};
                ui::Text(dc, label, row.label, t.text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                RECT box = {split, r.top + Px(2), r.right - Px(4), r.bottom - Px(2)};
                RoundBox(dc, box, Px(4), hot ? t.buttonHot : t.button, t.border);
                const int index = row.getIndex ? row.getIndex() : -1;
                RECT text = {box.left + Px(6), box.top, box.right - Px(16), box.bottom};
                if (index >= 0 && index < static_cast<int>(row.options.size()))
                    ui::Text(dc, text, row.options[static_cast<std::size_t>(index)], t.text,
                         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                RECT arrow = {box.right - Px(16), box.top, box.right - Px(2), box.bottom};
                DrawGlyph(dc, arrow, Glyph::DropDown, t.text);
                break;
            }
            case Kind::Colors: {
                for (int which = 0; which < 3; which += 2) {
                    float rgb[3] = {0, 0, 0};
                    if (row.getColor) row.getColor(which / 2, rgb);
                    const auto to8 = [](float c) { return static_cast<int>(std::lround(std::min(std::max(c, 0.0f), 1.0f) * 255.0f)); };
                    const COLORREF color = RGB(to8(rgb[0]), to8(rgb[1]), to8(rgb[2]));
                    const RECT c = ColorRect(r, which);
                    Fill(dc, c, color);
                    Frame(dc, c, t.border);
                    const float l = (0.299f * rgb[0] + 0.587f * rgb[1] + 0.114f * rgb[2]);
                    ui::Text(dc, c, which == 0 ? L"A" : L"B", l > 0.5f ? RGB(20, 20, 20) : RGB(235, 235, 235),
                         DT_CENTER | DT_VCENTER | DT_SINGLELINE, BoldFont());
                }
                const RECT s = ColorRect(r, 1);
                RoundBox(dc, s, Px(4), hot && hotButton_ == 1 ? t.buttonHot : t.button, t.border);
                DrawGlyph(dc, s, Glyph::Swap, t.text);
                break;
            }
            case Kind::Note: {
                RECT text = {r.left + Px(8), r.top, r.right - Px(4), r.bottom};
                ui::Text(dc, text, row.text ? row.text() : std::wstring(), t.textDim,
                         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                break;
            }
            case Kind::List: {
                RECT box = {r.left + Px(6), r.top + Px(2), r.right - Px(6), r.bottom - Px(4)};
                Fill(dc, box, t.background);
                Frame(dc, box, t.border);
                const int n = row.count ? row.count() : 0;
                const int selected = row.selected ? row.selected() : -1;
                for (int i = 0; i < n; ++i) {
                    RECT item = {box.left + 1, box.top + 1 + i * ListItemHeight(), box.right - 1,
                                 box.top + 1 + (i + 1) * ListItemHeight()};
                    if (i == selected) Fill(dc, item, t.accent);
                    RECT label = {item.left + Px(6), item.top, item.right - Px(4), item.bottom};
                    ui::Text(dc, label, row.item ? row.item(i) : std::wstring(), i == selected ? t.accentText : t.text,
                             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                }
                break;
            }
            case Kind::Text: {
                const RECT box = TextBox(r);
                RECT label = {r.left + Px(8), r.top, box.left - Px(4), r.bottom};
                ui::Text(dc, label, row.label, t.text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                Fill(dc, box, t.background);
                Frame(dc, box, hot ? t.text : t.border);
                RECT inner = {box.left + Px(4), box.top, box.right - Px(4), box.bottom};
                ui::Text(dc, inner, row.text ? row.text() : std::wstring(), t.text,
                         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                break;
            }
        }
    }
}

int RowList::HitRow(int x, int y, int scroll, RECT* rect) const {
    for (const Placed& p : Layout(width_)) {
        RECT r = p.rect;
        OffsetRect(&r, origin_.x, origin_.y - scroll);
        if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) {
            if (rect) *rect = r;
            return p.row;
        }
    }
    return -1;
}

void RowList::SetSliderFromX(const Row& row, const RECT& rect, int x) {
    const RECT track = SliderTrack(rect);
    const float t = static_cast<float>(x - track.left) / static_cast<float>(std::max<LONG>(1, track.right - track.left));
    const float value = FromT(row.slider, t);
    if (row.slider.applyOnRelease)
        pending_ = value;
    else if (row.slider.set)
        row.slider.set(value);
}

bool RowList::MouseDown(HWND host, int x, int y, bool /*doubleClick*/, int scroll) {
    if (edit_) FinishEdit(true);
    RECT r;
    const int index = HitRow(x, y, scroll, &r);
    if (index < 0) return false;
    const Row& row = rows_[static_cast<std::size_t>(index)];
    switch (row.kind) {
        case Kind::Section:
            row.expanded = !row.expanded;
            return true;
        case Kind::Slider: {
            const RECT value = SliderValue(r);
            if (x >= value.left) {
                BeginEdit(host, index, value);
                return true;
            }
            if (x >= SliderTrack(r).left - Px(6)) {
                drag_ = index;
                dragRect_ = r;
                SetSliderFromX(row, r, x);
                return true;
            }
            return false;
        }
        case Kind::Check:
            if (row.setBool) row.setBool(!(row.getBool && row.getBool()));
            return true;
        case Kind::Buttons: {
            const int b = ButtonAt(row, r, x);
            if (b < 0) return false;
            const Button& button = row.buttons[static_cast<std::size_t>(b)];
            if (button.enabled && !button.enabled()) return false;
            pressedRow_ = index;
            pressedButton_ = b;
            return true;
        }
        case Kind::Choice: {
            const int split = r.left + (r.right - r.left) * 42 / 100;
            if (x < split || !row.setIndex) return false;
            std::vector<MenuItem> items;
            const int current = row.getIndex ? row.getIndex() : -1;
            for (int i = 0; i < static_cast<int>(row.options.size()); ++i)
                items.push_back({i + 1, row.options[static_cast<std::size_t>(i)], i == current, true});
            POINT p = {split, r.bottom};
            ClientToScreen(host, &p);
            if (GetCapture() == host) ReleaseCapture();
            const int chosen = PopupMenu(items, p);
            if (chosen > 0) row.setIndex(chosen - 1);
            return true;
        }
        case Kind::List: {
            const int n = row.count ? row.count() : 0;
            const int i = (y - (r.top + Px(3))) / ListItemHeight();
            if (i < 0 || i >= n || !row.select) return false;
            row.select(row.selected && row.selected() == i ? -1 : i);
            return true;
        }
        case Kind::Text: {
            const RECT box = TextBox(r);
            if (x < box.left || !row.setText) return false;
            BeginEdit(host, index, box);
            return true;
        }
        case Kind::Colors: {
            for (int which = 0; which < 3; ++which) {
                const RECT c = ColorRect(r, which);
                if (x < c.left || x >= c.right) continue;
                if (GetCapture() == host) ReleaseCapture();
                if (which == 1) {
                    if (row.swap) row.swap();
                } else if (row.getColor && row.setColor) {
                    float rgb[3];
                    row.getColor(which / 2, rgb);
                    if (PickColor(rgb)) row.setColor(which / 2, rgb);
                }
                return true;
            }
            return false;
        }
        default:
            return false;
    }
}

bool RowList::MouseMove(int x, int y, bool captured, int scroll) {
    if (drag_ >= 0 && captured) {
        SetSliderFromX(rows_[static_cast<std::size_t>(drag_)], dragRect_, x);
        return true;
    }
    RECT r;
    const int index = HitRow(x, y, scroll, &r);
    int button = -1;
    if (index >= 0) {
        const Row& row = rows_[static_cast<std::size_t>(index)];
        if (row.kind == Kind::Buttons) button = ButtonAt(row, r, x);
        if (row.kind == Kind::Colors) {
            const RECT s = ColorRect(r, 1);
            button = x >= s.left && x < s.right ? 1 : -1;
        }
    }
    if (index == hotRow_ && button == hotButton_) return false;
    hotRow_ = index;
    hotButton_ = button;
    return true;
}

bool RowList::MouseUp(int x, int y, int scroll) {
    if (drag_ >= 0) {
        const int row = drag_;
        drag_ = -1;
        const SliderSpec& s = rows_[static_cast<std::size_t>(row)].slider;
        if (s.applyOnRelease && s.set) s.set(pending_);
        if (s.released) s.released();
        return true;
    }
    if (pressedRow_ < 0) return false;
    const int row = pressedRow_, button = pressedButton_;
    pressedRow_ = pressedButton_ = -1;
    RECT r;
    if (HitRow(x, y, scroll, &r) == row && ButtonAt(rows_[static_cast<std::size_t>(row)], r, x) == button) {
        const Button& b = rows_[static_cast<std::size_t>(row)].buttons[static_cast<std::size_t>(button)];
        if (b.click) b.click();
    }
    return true;
}

bool RowList::RightClick(HWND /*host*/, int x, int y, int scroll) {
    RECT r;
    const int index = HitRow(x, y, scroll, &r);
    if (index < 0) return false;
    const Row& row = rows_[static_cast<std::size_t>(index)];
    if (row.kind != Kind::Slider || !row.slider.set) return false;
    row.slider.set(row.slider.defaultValue);  // Like 3ds Max spinners: right-click resets.
    if (row.slider.released) row.slider.released();
    return true;
}

bool RowList::MouseLeave() {
    if (hotRow_ < 0) return false;
    hotRow_ = hotButton_ = -1;
    return true;
}

void RowList::BeginEdit(HWND host, int row, const RECT& valueRect) {
    const Row& target = rows_[static_cast<std::size_t>(row)];
    const SliderSpec& s = target.slider;
    RECT r = valueRect;
    MapWindowPoints(host, nullptr, reinterpret_cast<POINT*>(&r), 2);
    if (GetCapture() == host) ReleaseCapture();
    const bool isText = target.kind == Kind::Text;
    const std::wstring text = isText ? (target.text ? target.text() : std::wstring())
                                     : FormatValue(s.get ? s.get() : 0.0f, s.decimals);
    edit_ = CreateWindowExW(WS_EX_TOOLWINDOW, L"EDIT", text.c_str(),
                            WS_POPUP | WS_BORDER | ES_AUTOHSCROLL | (isText ? ES_LEFT : ES_RIGHT),
                            r.left, r.top, r.right - r.left, r.bottom - r.top, host, nullptr, hInstance, nullptr);
    if (!edit_) return;
    editHost_ = host;
    editRow_ = row;
    editPrevFocus_ = GetFocus();
    SendMessageW(edit_, WM_SETFONT, reinterpret_cast<WPARAM>(Font()), TRUE);
    SetWindowSubclass(edit_, EditProc, 1, reinterpret_cast<DWORD_PTR>(host));
    DisableAccelerators();  // Typed digits must not trigger shortcuts.
    ShowWindow(edit_, SW_SHOW);
    SetFocus(edit_);
    SendMessageW(edit_, EM_SETSEL, 0, -1);
}

void RowList::FinishEdit(bool commit) {
    if (!edit_) return;
    HWND edit = edit_;
    edit_ = nullptr;
    if (commit && editRow_ >= 0 && editRow_ < static_cast<int>(rows_.size()) &&
        rows_[static_cast<std::size_t>(editRow_)].kind == Kind::Text) {
        wchar_t buffer[256] = {};
        GetWindowTextW(edit, buffer, 255);
        if (rows_[static_cast<std::size_t>(editRow_)].setText) rows_[static_cast<std::size_t>(editRow_)].setText(buffer);
    } else if (commit && editRow_ >= 0 && editRow_ < static_cast<int>(rows_.size())) {
        wchar_t buffer[64] = {};
        GetWindowTextW(edit, buffer, 63);
        wchar_t* end = nullptr;
        const float v = std::wcstof(buffer, &end);
        const SliderSpec& s = rows_[static_cast<std::size_t>(editRow_)].slider;
        if (end != buffer && std::isfinite(v) && s.set) {
            s.set(std::min(std::max(v, s.min), s.dynamicMax ? s.dynamicMax() : std::max(s.max, s.typeMax)));
            if (s.released) s.released();
        }
    }
    RemoveWindowSubclass(edit, EditProc, 1);
    DestroyWindow(edit);
    EnableAccelerators();
    if (editPrevFocus_ && IsWindow(editPrevFocus_))
        SetFocus(editPrevFocus_);
    else if (HWND max = MaxWindow())
        SetFocus(max);
    editRow_ = -1;
    if (editHost_) InvalidateRect(editHost_, nullptr, FALSE);
}

namespace {
LRESULT CALLBACK FloatingWindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    return FloatingWindow::Proc(hwnd, msg, wp, lp);
}
}  // namespace

}  // namespace ui
