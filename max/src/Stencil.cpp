#include "Stencil.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "PaintImageIO.h"
#include "SculptSettings.h"

namespace {

const wchar_t kClassName[] = L"SculptMeshStencil";

LRESULT CALLBACK StencilWindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK MouseHookProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && lp && Stencil::Get().HandleMouse(wp, *reinterpret_cast<const MOUSEHOOKSTRUCT*>(lp)))
        return 1;  // Eaten: 3ds Max does not see S + click.
    return CallNextHookEx(nullptr, code, wp, lp);
}

HINSTANCE ModuleInstance() {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&StencilWindowProc), &module);
    return module;
}

}  // namespace

Stencil& Stencil::Get() {
    static Stencil stencil;
    return stencil;
}

bool Stencil::KeyHeld() { return (GetKeyState('S') & 0x8000) != 0; }

bool Stencil::Load(const std::wstring& path, MSTR* error) {
    sculpt::Image image;
    if (!PaintImageIO::Load(path, image, error)) return false;
    const int longest = std::max(image.width, image.height);
    if (longest > 2048) image = sculpt::resampleImage(image, image.width * 2048 / longest, image.height * 2048 / longest);
    image_ = std::move(image);
    path_ = path;
    if (!placed_) ResetTransform();
    Redraw();
    return true;
}

void Stencil::Clear() {
    image_ = sculpt::Image();
    path_.clear();
    drag_ = Drag::None;
    if (window_) ShowWindow(window_, SW_HIDE);
    UpdateHook();
}

void Stencil::ResetTransform() {
    RECT client = {0, 0, 800, 600};
    if (viewport_ && IsWindow(viewport_)) GetClientRect(viewport_, &client);
    centerX_ = 0.5f * static_cast<float>(client.right - client.left);
    centerY_ = 0.5f * static_cast<float>(client.bottom - client.top);
    width_ = 0.6f * static_cast<float>(std::min(client.right - client.left, client.bottom - client.top));
    angle_ = 0.0f;
    placed_ = true;
    Redraw();
}

void Stencil::SetActive(bool active) {
    active_ = active;
    if (!active_ && window_) ShowWindow(window_, SW_HIDE);
    UpdateHook();
    if (active_) Redraw();
}

void Stencil::SetViewport(HWND viewport) {
    if (viewport == viewport_ || !viewport) return;
    viewport_ = viewport;
    if (!placed_) ResetTransform();
    Redraw();
}

void Stencil::Refresh() {
    if (!viewport_) {
        if (Interface* core = GetCOREInterface()) viewport_ = core->GetActiveViewExp().GetHWnd();
    }
    Redraw();
}

void Stencil::UpdateHook() {
    const bool want = active_ && Loaded();
    if (want && !hook_) hook_ = SetWindowsHookExW(WH_MOUSE, MouseHookProc, nullptr, GetCurrentThreadId());
    if (!want && hook_) {
        UnhookWindowsHookEx(hook_);
        hook_ = nullptr;
    }
}

bool Stencil::Sample(float x, float y, sculpt::Vec3& rgb, float& alpha) const {
    if (image_.empty() || !(width_ > 0.0f)) return false;
    const float c = std::cos(-angle_), s = std::sin(-angle_);
    const float dx = x - centerX_, dy = y - centerY_;
    const float lx = dx * c - dy * s, ly = dx * s + dy * c;
    const float height = width_ * static_cast<float>(image_.height) / static_cast<float>(image_.width);
    const float u = lx / width_ + 0.5f, v = ly / height + 0.5f;
    if (u < 0.0f || v < 0.0f || u >= 1.0f || v >= 1.0f) return false;
    const float sx = std::min(std::max(u * image_.width - 0.5f, 0.0f), static_cast<float>(image_.width - 1));
    const float sy = std::min(std::max(v * image_.height - 0.5f, 0.0f), static_cast<float>(image_.height - 1));
    const int x0 = static_cast<int>(sx), y0 = static_cast<int>(sy);
    const int x1 = std::min(x0 + 1, image_.width - 1), y1 = std::min(y0 + 1, image_.height - 1);
    const float fx = sx - static_cast<float>(x0), fy = sy - static_cast<float>(y0);
    float out[4];
    for (int k = 0; k < 4; ++k) {
        const float a = image_.pixel(x0, y0)[k] * (1 - fx) + image_.pixel(x1, y0)[k] * fx;
        const float b = image_.pixel(x0, y1)[k] * (1 - fx) + image_.pixel(x1, y1)[k] * fx;
        out[k] = (a * (1 - fy) + b * fy) / 255.0f;
    }
    rgb = {out[0], out[1], out[2]};
    alpha = out[3];
    return true;
}

void Stencil::EnsureWindow() {
    if (window_) return;
    static bool registered = false;
    HINSTANCE instance = ModuleInstance();
    if (!registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = StencilWindowProc;
        wc.hInstance = instance;
        wc.lpszClassName = kClassName;
        registered = RegisterClassExW(&wc) != 0;
    }
    Interface* core = GetCOREInterface();
    window_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, kClassName, L"",
                              WS_POPUP, 0, 0, 1, 1, core ? core->GetMAXHWnd() : nullptr, nullptr, instance, nullptr);
}

void Stencil::Redraw() {
    if (!active_ || image_.empty() || !viewport_ || !IsWindow(viewport_) || !IsWindowVisible(viewport_)) {
        if (window_) ShowWindow(window_, SW_HIDE);
        return;
    }
    EnsureWindow();
    if (!window_) return;
    RECT client;
    GetClientRect(viewport_, &client);
    const int w = client.right - client.left, h = client.bottom - client.top;
    if (w <= 0 || h <= 0) return;
    POINT origin = {0, 0};
    ClientToScreen(viewport_, &origin);

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib || !bits) {
        if (dib) DeleteObject(dib);
        DeleteDC(dc);
        ReleaseDC(nullptr, screen);
        return;
    }
    HGDIOBJ old = SelectObject(dc, dib);
    std::uint32_t* pixels = static_cast<std::uint32_t*>(bits);
    std::fill(pixels, pixels + static_cast<std::size_t>(w) * h, 0u);
    // Only the image's screen bounding box needs sampling.
    const float height = width_ * static_cast<float>(image_.height) / static_cast<float>(image_.width);
    const float reach = 0.5f * std::sqrt(width_ * width_ + height * height) + 1.0f;
    const int x0 = std::max(0, static_cast<int>(centerX_ - reach)), x1 = std::min(w - 1, static_cast<int>(centerX_ + reach));
    const int y0 = std::max(0, static_cast<int>(centerY_ - reach)), y1 = std::min(h - 1, static_cast<int>(centerY_ + reach));
    const float opacity = SculptSettings::Get().Value(Prop::StencilOpacity);
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            sculpt::Vec3 rgb;
            float a = 0.0f;
            if (!Sample(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f, rgb, a)) continue;
            a *= opacity;
            const auto premul = [a](float c) { return static_cast<std::uint32_t>(std::lround(std::min(std::max(c, 0.0f), 1.0f) * a * 255.0f)); };
            pixels[static_cast<std::size_t>(y) * w + x] = (static_cast<std::uint32_t>(std::lround(a * 255.0f)) << 24) |
                                                         (premul(rgb.x) << 16) | (premul(rgb.y) << 8) | premul(rgb.z);
        }
    POINT source = {0, 0};
    SIZE size = {w, h};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(window_, screen, &origin, &size, dc, &source, 0, &blend, ULW_ALPHA);
    SelectObject(dc, old);
    DeleteObject(dib);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    if (!IsWindowVisible(window_)) ShowWindow(window_, SW_SHOWNOACTIVATE);
}

bool Stencil::HandleMouse(WPARAM message, const MOUSEHOOKSTRUCT& info) {
    if (!active_ || image_.empty()) return false;
    const bool down = message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN || message == WM_MBUTTONDOWN;
    const bool up = message == WM_LBUTTONUP || message == WM_RBUTTONUP || message == WM_MBUTTONUP;
    if (drag_ == Drag::None) {
        if (!down || !KeyHeld()) return false;
        // Only over a viewport (the stencil moves to the one clicked).
        HWND target = info.hwnd;
        Interface7* core = GetCOREInterface7();
        bool isViewport = false;
        if (core)
            for (int i = 0; i < core->getNumViewports() && !isViewport; ++i) {
                ViewExp& view = core->getViewExp(i);
                isViewport = view.IsAlive() && view.GetHWnd() == target;
            }
        if (!isViewport) return false;
        SetViewport(target);
        drag_ = message == WM_LBUTTONDOWN ? Drag::Rotate : (message == WM_RBUTTONDOWN ? Drag::Scale : Drag::Move);
        dragStart_ = info.pt;
        startAngle_ = angle_;
        startWidth_ = width_;
        startX_ = centerX_;
        startY_ = centerY_;
        return true;
    }
    if (up) {
        drag_ = Drag::None;
        return true;
    }
    if (message != WM_MOUSEMOVE && message != WM_NCMOUSEMOVE) return down;  // Other buttons during a drag.
    POINT origin = {0, 0};
    ClientToScreen(viewport_, &origin);
    const float cx = static_cast<float>(origin.x) + centerX_, cy = static_cast<float>(origin.y) + centerY_;
    switch (drag_) {
        case Drag::Rotate: {
            const float a0 = std::atan2(static_cast<float>(dragStart_.y) - startY_ - origin.y, static_cast<float>(dragStart_.x) - startX_ - origin.x);
            const float a1 = std::atan2(static_cast<float>(info.pt.y) - cy, static_cast<float>(info.pt.x) - cx);
            angle_ = startAngle_ + (a1 - a0);
            break;
        }
        case Drag::Scale:
            width_ = std::max(16.0f, startWidth_ * std::exp(static_cast<float>(info.pt.x - dragStart_.x) * 0.005f));
            break;
        case Drag::Move:
            centerX_ = startX_ + static_cast<float>(info.pt.x - dragStart_.x);
            centerY_ = startY_ + static_cast<float>(info.pt.y - dragStart_.y);
            break;
        default:
            break;
    }
    Redraw();
    return true;
}
