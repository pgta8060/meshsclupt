// Texture painting in the viewport mode: Paint / Smudge / Blur / Erase
// strokes (through the normal stroke machinery: spacing, Lazy Mouse,
// Stamp, Drag, Scatter, mirror), Fill and Gradient, Color Mix, the stencil,
// and the Displace brush (stencil as height).
#include <algorithm>
#include <cmath>

#include "SculptCommands.h"
#include "SculptMeshObject.h"
#include "SculptMode.h"
#include "Stencil.h"

using sculpt::BrushType;
using sculpt::Vec3;

namespace {

const Point3 kGradientColor(0.95f, 0.85f, 0.35f);

float Luminance(const Vec3& c) { return 0.299f * c.x + 0.587f * c.y + 0.114f * c.z; }

Vec3 ColorA() {
    const SculptSettings& s = SculptSettings::Get();
    return {s.Value(Prop::ColorAR), s.Value(Prop::ColorAG), s.Value(Prop::ColorAB)};
}

Vec3 ColorB() {
    const SculptSettings& s = SculptSettings::Get();
    return {s.Value(Prop::ColorBR), s.Value(Prop::ColorBG), s.Value(Prop::ColorBB)};
}

const MCHAR* ToolUndoName(int tool) {
    static const MCHAR* const names[] = {_T("Paint"), _T("Smudge"), _T("Fill"), _T("Blur"), _T("Erase"), _T("Gradient")};
    return tool >= 0 && tool < 6 ? names[tool] : _T("Paint");
}

}  // namespace

bool SculptMode::StencilAt(const Vec3& objectPoint, Vec3& rgb, float& alpha) const {
    float x, y, depth;
    if (!projector_.Project(objectPoint, x, y, depth)) return false;
    return Stencil::Get().Sample(x, y, rgb, alpha);
}

bool SculptMode::PressPaint(HWND hwnd, IPoint2 m, int flags) {
    const bool shift = (flags & MOUSE_SHIFT) != 0, alt = (flags & MOUSE_ALT) != 0;
    MSTR error;
    SculptMeshObject::PaintState* state = object_->AcquirePaint(error);
    if (!state) {
        ip_->ReplacePrompt(error.data());
        gesture_ = Gesture::Consumed;
        return true;
    }
    if (!object_->FastDisplay()) object_->SetFastDisplay(true);  // The paint texture is shown by the fast display.
    object_->RefreshDisplayOptions();
    ViewExp& vpt = ip_->GetViewExp(hwnd);
    SculptSessionBridge* bridge = object_->Bridge();
    if (!vpt.IsAlive() || !bridge) return false;
    const SculptSettings& s = SculptSettings::Get();
    const float mx = static_cast<float>(m.x), my = static_cast<float>(m.y);

    paintTool_ = s.Int(Prop::PaintTool);
    if (shift && paintTool_ != static_cast<int>(sculpt::PaintTool::Fill) &&
        paintTool_ != static_cast<int>(sculpt::PaintTool::Gradient))
        paintTool_ = static_cast<int>(sculpt::PaintTool::Blur);  // Shift: temporary Blur.
    paintSettings_ = sculpt::PaintSettings();
    paintSettings_.tool = static_cast<sculpt::PaintTool>(paintTool_);
    paintSettings_.colorA = ColorA();
    paintSettings_.colorB = ColorB();
    paintSettings_.opacity = s.Value(Prop::PaintOpacity);
    paintSettings_.hardness = s.Value(Prop::PaintHardness);
    paintSettings_.blurStrength = s.Value(Prop::PaintBlurStrength);
    paintSettings_.useFalloff = s.Bool(Prop::UseFalloff);
    paintSettings_.backfaceCull = true;
    paintSettings_.alphaMid = s.Value(Prop::AlphaMid);
    paintSettings_.alphaFade = s.Value(Prop::AlphaFade);

    projector_.Capture(vpt, objectToWorld_, worldToObject_, mx, my);
    Stencil::Get().SetViewport(hwnd);
    useStencil_ = Stencil::Get().Loaded() &&
                  (paintTool_ == static_cast<int>(sculpt::PaintTool::Paint));
    const int stencilMode = s.Int(Prop::StencilMode);
    stencil_ = [this, stencilMode](const Vec3& p, Vec3& rgb, float& alpha) {
        if (!StencilAt(p, rgb, alpha)) return false;
        if (stencilMode == 1) alpha *= Luminance(rgb);  // The stencil only masks Color A.
        return true;
    };

    sculpt::SculptSession& session = bridge->Session();
    state->map.updateBounds(session.mesh());  // Sculpting may have moved the surface.
    paintStroke_.begin(state->canvas, state->map, session.mesh());
    const std::vector<float> noMask;
    const std::vector<float>& mask = session.hasMask() ? session.mask() : noMask;

    if (paintSettings_.tool == sculpt::PaintTool::Fill) {
        paintActive_ = true;
        paintStroke_.fill(paintSettings_, mask);
        FinishPaintStroke(nullptr);
        gesture_ = Gesture::Consumed;
        return true;
    }
    if (paintSettings_.tool == sculpt::PaintTool::Gradient) {
        sculpt::RayHit hit;
        if (!Raycast(vpt, mx, my, true, hit)) {
            paintStroke_.cancel();
            gesture_ = Gesture::Consumed;
            return true;
        }
        gradientFrom_ = hit.position;
        paintActive_ = true;
        gesture_ = Gesture::Gradient;
        return true;
    }

    paintErase_ = alt && paintSettings_.tool == sculpt::PaintTool::Paint;  // Alt: temporary erase.
    colorMix_ = static_cast<StrokeMode>(s.Int(Prop::StrokeMode)) == StrokeMode::ColorMix;
    paintActive_ = true;
    sculpt::BrushSettings footprint;  // Stroke placement only (alpha, culling); no sculpt kernel runs.
    footprint.type = BrushType::Sculpt;
    footprint.useFalloff = s.Bool(Prop::UseFalloff);
    footprint.backfaceCull = true;
    footprint.alphaMid = s.Value(Prop::AlphaMid);
    footprint.alphaFade = s.Value(Prop::AlphaFade);
    pressHit_ = Raycast(vpt, mx, my, true, pressHitInfo_);
    BeginStroke(hwnd, m, footprint, false, false, false);
    if (gesture_ != Gesture::Stroke && gesture_ != Gesture::DragDab) CancelPaintStroke();
    return true;
}

bool SculptMode::PaintDab(const sculpt::Dab& dab) {
    SculptSessionBridge* bridge = object_ ? object_->Bridge() : nullptr;
    if (!bridge || !paintStroke_.active()) return false;
    const SculptSettings& s = SculptSettings::Get();
    paintSettings_.alpha = strokeBrush_.alpha;  // The stroke's alpha (set by BeginStroke).
    sculpt::PaintDab d;
    d.center = dab.center;
    d.radius = dab.radius;
    d.pressure = dab.pressure;
    d.amount = dab.amount;
    d.viewDir = dab.viewDir;
    d.up = dab.up;
    d.erase = paintErase_;
    auto random01 = [this]() {  // xorshift: per-dab colour variation.
        paintRandom_ ^= paintRandom_ << 13;
        paintRandom_ ^= paintRandom_ >> 17;
        paintRandom_ ^= paintRandom_ << 5;
        return static_cast<float>(paintRandom_ & 0xffffff) / static_cast<float>(0xffffff);
    };
    d.color = paintSettings_.colorA;
    if (colorMix_) {
        d.color = sculpt::lerp(paintSettings_.colorA, paintSettings_.colorB, random01());
    } else if (strokeMode_ == StrokeMode::Scatter && s.Value(Prop::PaintColorJitter) > 0.0f) {
        d.color = sculpt::lerp(paintSettings_.colorA, paintSettings_.colorB, random01() * s.Value(Prop::PaintColorJitter));
    }
    if (useStencil_) {
        d.stencil = &stencil_;
        d.stencilColors = s.Int(Prop::StencilMode) == 0;
    }
    sculpt::SculptSession& session = bridge->Session();
    const std::vector<float> noMask;
    // The stencil lives in screen space: mirrored copies would sample it wrongly.
    const std::vector<sculpt::Mat3> identity = {sculpt::Mat3::identity()};
    const std::size_t changed = paintStroke_.dab(paintSettings_, d, session.hasMask() ? session.mask() : noMask,
                                                 useStencil_ ? identity : session.symmetry());
    if (changed > 0) object_->PaintChanged(true);
    return changed > 0;
}

void SculptMode::FinishPaintStroke(const MCHAR* /*undoName*/) {
    if (!paintActive_) return;
    paintActive_ = false;
    sculpt::PaintDelta delta = paintStroke_.end();
    if (!object_) return;
    object_->PaintChanged(false);
    if (!delta.empty()) {
        const MCHAR* name = ToolUndoName(paintTool_);
        theHold.Begin();
        object_->PutPaintUndo(std::move(delta), name);
        theHold.Accept(name);
    }
}

void SculptMode::CancelPaintStroke() {
    if (!paintActive_) return;
    paintActive_ = false;
    paintStroke_.cancel();
    if (object_) object_->PaintChanged(false);
}

void SculptMode::DragGradient(ViewExp& /*vpt*/, IPoint2 m) {
    overlay_.ShowSegments({press_, m}, kGradientColor);
    Redraw(REDRAW_INTERACTIVE);
}

void SculptMode::ApplyGradient(ViewExp& vpt, IPoint2 m) {
    overlay_.Hide();
    if (!paintActive_ || !object_ || !object_->Bridge()) return;
    Vec3 to;
    sculpt::RayHit hit;
    if (Raycast(vpt, static_cast<float>(m.x), static_cast<float>(m.y), false, hit)) {
        to = hit.position;
    } else {
        float x, y, depth;
        if (!projector_.Project(gradientFrom_, x, y, depth)) {
            CancelPaintStroke();
            return;
        }
        to = projector_.Unproject(static_cast<float>(m.x), static_cast<float>(m.y), depth);
    }
    sculpt::SculptSession& session = object_->Bridge()->Session();
    const std::vector<float> noMask;
    paintStroke_.gradient(paintSettings_, gradientFrom_, to, session.hasMask() ? session.mask() : noMask);
    FinishPaintStroke(nullptr);
}

bool SculptMode::DisplaceDab(const sculpt::Dab& dab) {
    SculptSessionBridge* bridge = object_ ? object_->Bridge() : nullptr;
    if (!bridge) return false;
    if (!Stencil::Get().Loaded()) {
        ip_->ReplacePrompt(_T("Displace brush: load a stencil in Material / Paint first."));
        return false;
    }
    const SculptSettings& s = SculptSettings::Get();
    const float mid = s.Value(Prop::DisplaceHeightMid);
    const std::function<bool(const Vec3&, float&)> height = [this, mid](const Vec3& p, float& h) {
        Vec3 rgb;
        float alpha = 0.0f;
        if (!StencilAt(p, rgb, alpha)) return false;
        h = (Luminance(rgb) * alpha - mid) * 2.0f;
        return true;
    };
    return bridge->Session().applyDisplaceDab(strokeBrush_, dab, height, s.Value(Prop::DisplaceFade)) > 0;
}
