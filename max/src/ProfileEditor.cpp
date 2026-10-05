#include "ProfileEditor.h"

#include <algorithm>
#include <cmath>

#include "SculptSettings.h"

using sculpt::ProfileCurve;
using sculpt::ProfilePoint;
using sculpt::ProfilePointType;
using ui::Px;

namespace {

constexpr int kHitRadius = 6;

void Line(HDC dc, POINT a, POINT b) {
    MoveToEx(dc, a.x, a.y, nullptr);
    LineTo(dc, b.x, b.y);
}

bool HasHandles(const ProfilePoint& p) {
    return p.type == ProfilePointType::Bezier || p.type == ProfilePointType::BezierCorner;
}

}  // namespace

ui::RowList::CustomSpec ProfileEditor::Spec() {
    ui::RowList::CustomSpec spec;
    spec.height = [](int width) { return std::max(Px(110), std::min(width * 2 / 3, Px(200))); };
    spec.paint = [this](HDC dc, const RECT& r) { Paint(dc, r); };
    spec.mouseDown = [this](HWND host, const RECT& r, int x, int y, bool dbl) { return MouseDown(host, r, x, y, dbl); };
    spec.mouseDrag = [this](const RECT& r, int x, int y) { return MouseDrag(r, x, y); };
    spec.mouseUp = [this](const RECT&, int, int) {
        if (dragIndex_ >= 0) Store(dragCurve_);
        dragIndex_ = -1;
        dragPart_ = Part::None;
        return true;
    };
    spec.rightClick = [this](HWND host, const RECT& r, int x, int y) { return RightClick(host, r, x, y); };
    return spec;
}

ProfileEditor::Graph ProfileEditor::GraphOf(const RECT& row) const {
    Graph g;
    g.area = RECT{row.left + Px(8), row.top + Px(6), row.right - Px(8), row.bottom - Px(6)};
    g.yMax = yMax_;
    return g;
}

POINT ProfileEditor::ToScreen(const Graph& g, float x, float y) {
    const float w = static_cast<float>(g.area.right - g.area.left), h = static_cast<float>(g.area.bottom - g.area.top);
    return POINT{g.area.left + static_cast<LONG>(std::lround(x * w)),
                 g.area.bottom - static_cast<LONG>(std::lround(y / g.yMax * h))};
}

void ProfileEditor::FromScreen(const Graph& g, int sx, int sy, float& x, float& y) {
    const float w = static_cast<float>(std::max<LONG>(1, g.area.right - g.area.left));
    const float h = static_cast<float>(std::max<LONG>(1, g.area.bottom - g.area.top));
    x = static_cast<float>(sx - g.area.left) / w;
    y = static_cast<float>(g.area.bottom - sy) / h * g.yMax;
}

ProfileCurve ProfileEditor::Load() {
    ProfileCurve curve;
    const std::string& text = SculptSettings::Get().ProfileCurveText();
    if (!text.empty()) curve.fromText(text);
    return curve;
}

void ProfileEditor::Store(const ProfileCurve& curve) { SculptSettings::Get().SetProfileCurveText(curve.toText()); }

bool ProfileEditor::Hit(const Graph& g, const ProfileCurve& curve, int x, int y, int& index, Part& part) const {
    const int r2 = Px(kHitRadius) * Px(kHitRadius);
    auto close = [&](POINT p) { return (p.x - x) * (p.x - x) + (p.y - y) * (p.y - y) <= r2; };
    const auto& points = curve.points();
    for (int i = 0; i < static_cast<int>(points.size()); ++i) {
        const ProfilePoint& p = points[static_cast<std::size_t>(i)];
        if (close(ToScreen(g, p.x, p.y))) {
            index = i;
            part = Part::Point;
            return true;
        }
        if (!HasHandles(p)) continue;
        if (i > 0 && close(ToScreen(g, p.x + p.inX, p.y + p.inY))) {
            index = i;
            part = Part::In;
            return true;
        }
        if (i + 1 < static_cast<int>(points.size()) && close(ToScreen(g, p.x + p.outX, p.y + p.outY))) {
            index = i;
            part = Part::Out;
            return true;
        }
    }
    return false;
}

void ProfileEditor::Paint(HDC dc, const RECT& row) const {
    const ui::Theme& t = ui::GetTheme();
    ui::Fill(dc, row, t.panel);
    const Graph g = GraphOf(row);
    ui::Fill(dc, g.area, ui::Blend(t.background, RGB(0, 0, 0), 0.25f));
    const ProfileCurve curve = dragIndex_ >= 0 ? dragCurve_ : Load();

    HPEN grid = CreatePen(PS_SOLID, 1, ui::Blend(t.background, t.text, 0.15f));
    HPEN unit = CreatePen(PS_DOT, 1, ui::Blend(t.background, t.text, 0.35f));
    HPEN curvePen = CreatePen(PS_SOLID, std::max(1, Px(2)), t.accent);
    HPEN handlePen = CreatePen(PS_SOLID, 1, t.textDim);
    HGDIOBJ old = SelectObject(dc, grid);
    for (int i = 1; i < 4; ++i) {  // Quarter lines.
        const float f = static_cast<float>(i) / 4.0f;
        Line(dc, ToScreen(g, f, 0.0f), ToScreen(g, f, g.yMax));
    }
    SelectObject(dc, unit);
    Line(dc, ToScreen(g, 0.0f, 1.0f), ToScreen(g, 1.0f, 1.0f));  // Value 1 = no change.

    SelectObject(dc, curvePen);
    const int samples = std::max(16, static_cast<int>(g.area.right - g.area.left) / 2);
    for (int i = 0; i <= samples; ++i) {
        const float x = static_cast<float>(i) / static_cast<float>(samples);
        const POINT p = ToScreen(g, x, std::min(curve.evaluate(x), g.yMax));
        if (i == 0)
            MoveToEx(dc, p.x, p.y, nullptr);
        else
            LineTo(dc, p.x, p.y);
    }

    SelectObject(dc, handlePen);
    const auto& points = curve.points();
    const int k = Px(3);
    for (std::size_t i = 0; i < points.size(); ++i) {
        const ProfilePoint& p = points[i];
        const POINT c = ToScreen(g, p.x, p.y);
        if (HasHandles(p)) {
            for (int side = 0; side < 2; ++side) {
                if ((side == 0 && i == 0) || (side == 1 && i + 1 == points.size())) continue;
                const POINT h = side == 0 ? ToScreen(g, p.x + p.inX, p.y + p.inY) : ToScreen(g, p.x + p.outX, p.y + p.outY);
                Line(dc, c, h);
                RECT dot = {h.x - k + 1, h.y - k + 1, h.x + k - 1, h.y + k - 1};
                ui::Fill(dc, dot, t.textDim);
            }
        }
        RECT box = {c.x - k, c.y - k, c.x + k, c.y + k};
        const bool active = dragIndex_ == static_cast<int>(i);
        ui::Fill(dc, box, active ? t.accent : t.text);
    }
    SelectObject(dc, old);
    DeleteObject(grid);
    DeleteObject(unit);
    DeleteObject(curvePen);
    DeleteObject(handlePen);
    ui::Frame(dc, g.area, t.border);
}

bool ProfileEditor::MouseDown(HWND /*host*/, const RECT& row, int x, int y, bool doubleClick) {
    const Graph g = GraphOf(row);
    ProfileCurve curve = Load();
    int index = -1;
    Part part = Part::None;
    const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    if (Hit(g, curve, x, y, index, part)) {
        if (alt && part == Part::Point) {
            curve.removePoint(index);
            Store(curve);
            return false;  // Nothing to drag.
        }
        dragCurve_ = curve;
        dragIndex_ = index;
        dragPart_ = part;
        return true;
    }
    if (x < g.area.left || x > g.area.right || y < g.area.top || y > g.area.bottom) return false;
    if (doubleClick) {
        yMax_ = 1.5f;
        return false;
    }
    float px, py;
    FromScreen(g, x, y, px, py);
    index = curve.addPoint(px, std::max(0.0f, py));
    if (index < 0) return false;
    dragCurve_ = curve;
    dragIndex_ = index;
    dragPart_ = Part::Point;
    return true;
}

bool ProfileEditor::MouseDrag(const RECT& row, int x, int y) {
    if (dragIndex_ < 0) return false;
    const Graph g = GraphOf(row);
    auto& points = dragCurve_.points();
    if (dragIndex_ >= static_cast<int>(points.size())) return false;
    ProfilePoint& p = points[static_cast<std::size_t>(dragIndex_)];
    float px, py;
    FromScreen(g, x, y, px, py);
    py = std::min(std::max(py, 0.0f), g.yMax);
    if (dragPart_ == Part::Point) {
        const bool end = dragIndex_ == 0 || dragIndex_ + 1 == static_cast<int>(points.size());
        if (!end) {  // Interior points stay between their neighbours.
            const float lo = points[static_cast<std::size_t>(dragIndex_ - 1)].x + 0.005f;
            const float hi = points[static_cast<std::size_t>(dragIndex_ + 1)].x - 0.005f;
            p.x = std::min(std::max(px, lo), hi);
        }
        p.y = py;
    } else {
        const float hx = px - p.x, hy = py - p.y;
        if (dragPart_ == Part::In) {
            p.inX = hx;
            p.inY = hy;
        } else {
            p.outX = hx;
            p.outY = hy;
        }
        if (p.type == ProfilePointType::Bezier) {  // Keep the tangent continuous.
            float& ox = dragPart_ == Part::In ? p.outX : p.inX;
            float& oy = dragPart_ == Part::In ? p.outY : p.inY;
            const float other = std::sqrt(ox * ox + oy * oy);
            const float len = std::sqrt(hx * hx + hy * hy);
            if (len > 1e-6f) {
                ox = -hx / len * other;
                oy = -hy / len * other;
            }
        }
    }
    dragCurve_.normalize();
    return true;
}

bool ProfileEditor::RightClick(HWND host, const RECT& row, int x, int y) {
    const Graph g = GraphOf(row);
    ProfileCurve curve = Load();
    int index = -1;
    Part part = Part::None;
    POINT cursor = {x, y};
    ClientToScreen(host, &cursor);
    if (Hit(g, curve, x, y, index, part) && part == Part::Point) {
        ProfilePoint& p = curve.points()[static_cast<std::size_t>(index)];
        const int type = static_cast<int>(p.type);
        const int chosen = ui::PopupMenu({{1, L"Bezier", type == 0, true},
                                          {2, L"Bezier Corner", type == 1, true},
                                          {3, L"Smooth", type == 2, true},
                                          {4, L"Linear", type == 3, true}},
                                         cursor);
        if (chosen <= 0) return false;
        const ProfilePointType next = static_cast<ProfilePointType>(chosen - 1);
        if (!HasHandles(p) && (next == ProfilePointType::Bezier || next == ProfilePointType::BezierCorner)) {
            curve.normalize();  // Start from smooth handles.
            p.type = ProfilePointType::Smooth;
            curve.normalize();
        }
        curve.points()[static_cast<std::size_t>(index)].type = next;
        curve.normalize();
        Store(curve);
        return true;
    }
    const int chosen = ui::PopupMenu({{1, L"Reset View", false, true}, {2, L"Reset Profile", false, true}}, cursor);
    if (chosen == 1) yMax_ = 1.5f;
    if (chosen == 2) Store(ProfileCurve());
    return chosen > 0;
}
