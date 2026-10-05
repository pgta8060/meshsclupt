#include "SculptMode.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "AlphaLibrary.h"
#include "SculptCommands.h"
#include "SculptMeshObject.h"
#include "SculptUI.h"
#include "sculpt/symmetry.h"

using sculpt::BrushType;

namespace {

constexpr int kCircleSegments = 64;
constexpr float kTwoPi = 6.28318530718f;
constexpr int kDragThreshold = 4;  // Pixels before a click counts as a drag.
constexpr int kMaxSnakeSteps = 64;

const Point3 kSculptColor(1.0f, 0.22f, 0.2f);
const Point3 kInvertColor(0.25f, 0.55f, 1.0f);
const Point3 kMaskColor(0.95f, 0.95f, 0.95f);
const Point3 kEraseColor(0.5f, 0.5f, 0.5f);
const Point3 kGroupColor(1.0f, 0.78f, 0.2f);

// Intersects a world ray with the plane through `point` facing `normal`.
bool IntersectPlane(const Ray& ray, const Point3& point, const Point3& normal, Point3& out) {
    const float denom = DotProd(ray.dir, normal);
    if (std::fabs(denom) < 1e-12f) return false;
    const float t = DotProd(point - ray.p, normal) / denom;
    out = ray.p + ray.dir * t;
    return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.z);
}

bool FarApart(IPoint2 a, IPoint2 b, int threshold = kDragThreshold) {
    return std::abs(a.x - b.x) > threshold || std::abs(a.y - b.y) > threshold;
}

bool PointInPolygon(const std::vector<IPoint2>& poly, float x, float y) {
    bool inside = false;
    for (std::size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
        const float xi = static_cast<float>(poly[i].x), yi = static_cast<float>(poly[i].y);
        const float xj = static_cast<float>(poly[j].x), yj = static_cast<float>(poly[j].y);
        if ((yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi) inside = !inside;
    }
    return inside;
}

// Brushes that can draw Ctrl+Alt straight lines.
bool IsLineBrush(BrushType b) {
    return sculpt::isGeometryBrush(b) && b != BrushType::Move && b != BrushType::SnakeHook &&
           b != BrushType::FaceGroups;
}

bool IsGrabBrush(BrushType b) { return b == BrushType::Move || b == BrushType::SnakeHook; }

int UndoNameFor(BrushType b) {
    if (b == BrushType::MaskPaint) return IDS_UNDO_MASK;
    if (b == BrushType::FaceGroups) return IDS_UNDO_GROUPS;
    return IDS_UNDO_STROKE;
}

}  // namespace

// --- SculptForegroundCallback ---------------------------------------------------

void SculptForegroundCallback::callback(TimeValue t, IScene* /*scene*/) {
    if (object_) object_->FlagDependents(t);
}

// --- SculptOverlay ----------------------------------------------------------------

void SculptOverlay::ShowCircle(IPoint2 center, float radiusPx, bool falloffRing, const Point3& color) {
    kind_ = Kind::Circle;
    center_ = center;
    radius_ = std::max(radiusPx, 1.0f);
    falloffRing_ = falloffRing;
    color_ = color;
}

void SculptOverlay::ShowShape(const std::vector<IPoint2>& points, bool closed, const Point3& color) {
    kind_ = points.size() >= 2 ? Kind::Shape : Kind::None;
    points_ = points;
    closed_ = closed;
    color_ = color;
}

void SculptOverlay::Display(TimeValue /*t*/, ViewExp* vpt, int /*flags*/) {
    if (kind_ == Kind::None || !vpt || !vpt->IsAlive() || vpt->GetHWnd() != hwnd_) return;
    GraphicsWindow* gw = vpt->getGW();
    if (!gw) return;

    const IPoint2 reference = kind_ == Kind::Circle ? center_ : points_.front();
    Ray centerRay;
    vpt->MapScreenToWorldRay(static_cast<float>(reference.x), static_cast<float>(reference.y), centerRay);
    const Point3 normal = Normalize(centerRay.dir);
    auto toWorld = [&](float x, float y, Point3& out) {
        Ray ray;
        vpt->MapScreenToWorldRay(x, y, ray);
        return IntersectPlane(ray, anchor_, normal, out);
    };

    std::vector<Point3> segments;  // Pairs of points.
    if (kind_ == Kind::Circle) {
        const int rings = falloffRing_ ? 2 : 1;
        const float scales[2] = {1.0f, 0.5f};  // Rim and the falloff half-way ring.
        for (int r = 0; r < rings; ++r) {
            Point3 ring[kCircleSegments];
            for (int i = 0; i < kCircleSegments; ++i) {
                const float a = kTwoPi * static_cast<float>(i) / static_cast<float>(kCircleSegments);
                if (!toWorld(static_cast<float>(center_.x) + radius_ * scales[r] * std::cos(a),
                             static_cast<float>(center_.y) + radius_ * scales[r] * std::sin(a), ring[i]))
                    return;
            }
            for (int i = 0; i < kCircleSegments; ++i) {
                segments.push_back(ring[i]);
                segments.push_back(ring[(i + 1) % kCircleSegments]);
            }
        }
    } else {
        std::vector<Point3> world(points_.size());
        for (std::size_t i = 0; i < points_.size(); ++i)
            if (!toWorld(static_cast<float>(points_[i].x), static_cast<float>(points_[i].y), world[i])) return;
        const std::size_t count = closed_ ? world.size() : world.size() - 1;
        for (std::size_t i = 0; i < count; ++i) {
            segments.push_back(world[i]);
            segments.push_back(world[(i + 1) % world.size()]);
        }
    }

    const DWORD limits = gw->getRndLimits();
    gw->setRndLimits(limits & ~GW_Z_BUFFER);  // Always on top of the mesh.
    Matrix3 identity;
    identity.IdentityMatrix();
    gw->setTransform(identity);
    gw->setColor(LINE_COLOR, color_.x, color_.y, color_.z);
    gw->startSegments();
    for (std::size_t i = 0; i + 1 < segments.size(); i += 2) gw->segment(&segments[i], 1);
    gw->endSegments();
    gw->setRndLimits(limits);
}

void SculptOverlay::GetViewportRect(TimeValue /*t*/, ViewExp* vpt, Rect* rect) {
    if (!rect) return;
    if (vpt && vpt->IsAlive() && vpt->GetHWnd()) {
        RECT client;
        GetClientRect(vpt->GetHWnd(), &client);
        rect->left = client.left;
        rect->top = client.top;
        rect->right = client.right;
        rect->bottom = client.bottom;
    } else {
        rect->left = rect->top = rect->right = rect->bottom = 0;
    }
}

// --- SculptMode: lifetime ------------------------------------------------------------

SculptMode& SculptMode::Get() {
    static SculptMode mode;
    return mode;
}

bool SculptMode::Start(IObjParam* ip, SculptMeshObject* object) {
    if (!ip || !object) return false;
    if (object_ && object_ != object) Stop();
    ip_ = ip;
    object_ = object;
    foreground_.SetObject(object);
    if (ip_->GetCommandMode() == this) return true;
    ip_->DeleteMode(this);  // Drop a stale entry deeper in the stack before re-pushing.
    ip_->PushCommandMode(this);
    return true;
}

void SculptMode::Stop() {
    if (!ip_) return;
    Cancel();
    IObjParam* ip = ip_;
    ip->DeleteMode(this);     // Calls ExitMode() if the mode is on the stack.
    if (active_) ExitMode();  // Defensive: never leave a registered callback behind.
    object_ = nullptr;
    foreground_.SetObject(nullptr);
    ip_ = nullptr;
    SculptUI::Refresh();
}

void SculptMode::ForgetObject(SculptMeshObject* object) {
    if (object_ != object) return;
    // No UI or undo calls here: the object is being destroyed.
    gesture_ = Gesture::None;
    object_ = nullptr;
    foreground_.SetObject(nullptr);
    overlay_.Hide();
    anchorValid_ = false;
}

void SculptMode::EnterMode() {
    active_ = true;
    if (ip_ && !overlayRegistered_) {
        ip_->RegisterViewportDisplayCallback(FALSE, &overlay_);
        overlayRegistered_ = true;
    }
    if (ip_) ip_->PushPrompt(GetString(IDS_PROMPT_SCULPT));
    // Build the session up front so the first click does not stall.
    if (object_) {
        MSTR error;
        HCURSOR previous = SetCursor(LoadCursor(nullptr, IDC_WAIT));
        if (object_->AcquireSession(error))
            object_->SetFastDisplay(true);
        else if (ip_)
            ip_->ReplacePrompt(error.data());
        SetCursor(previous);
    }
    SculptUI::Refresh();
}

void SculptMode::ExitMode() {
    Cancel();
    overlay_.Hide();
    if (object_) object_->SetFastDisplay(false);
    if (ip_) {
        if (overlayRegistered_) {
            ip_->UnRegisterViewportDisplayCallback(FALSE, &overlay_);
            overlayRegistered_ = false;
        }
        if (active_) ip_->PopPrompt();
    }
    active_ = false;
    SculptUI::Refresh();
    if (ip_) ip_->RedrawViews(ip_->GetTime());
}

void SculptMode::RefreshCursor() {
    if (!active_ || !ip_ || gesture_ != Gesture::None || !gestureHwnd_ || !IsWindow(gestureHwnd_)) return;
    int flags = 0;
    if (GetKeyState(VK_CONTROL) & 0x8000) flags |= MOUSE_CTRL;
    if (GetKeyState(VK_SHIFT) & 0x8000) flags |= MOUSE_SHIFT;
    if (GetKeyState(VK_MENU) & 0x8000) flags |= MOUSE_ALT;
    UpdateOverlay(gestureHwnd_, last_, flags);
}

// --- Helpers -------------------------------------------------------------------------

bool SculptMode::ResolveTargetNode(INode*& node) const {
    node = nullptr;
    if (!ip_ || !object_ || ip_->GetSelNodeCount() != 1) return false;
    INode* selected = ip_->GetSelNode(0);
    if (!selected) return false;
    Object* ref = selected->GetObjectRef();
    if (!ref || ref->FindBaseObject() != object_) return false;
    node = selected;
    return true;
}

bool SculptMode::CaptureTransform(INode* node, TimeValue t) {
    objectToWorld_ = node->GetObjectTM(t);
    const float scale =
        (Length(objectToWorld_.GetRow(0)) + Length(objectToWorld_.GetRow(1)) + Length(objectToWorld_.GetRow(2))) / 3.0f;
    if (!(scale > 1e-12f) || !std::isfinite(scale)) return false;  // Degenerate (e.g. zero-scaled) node.
    objectScale_ = scale;
    worldToObject_ = Inverse(objectToWorld_);
    return true;
}

bool SculptMode::PrepareSession(HWND hwnd) {
    INode* node = nullptr;
    if (!ResolveTargetNode(node)) {
        ip_->ReplacePrompt(GetString(IDS_ERR_SELECT_ONE));
        return false;
    }
    MSTR error;
    if (!object_->AcquireSession(error)) {
        ip_->ReplacePrompt(error.data());
        return false;
    }
    if (!object_->FastDisplay()) object_->SetFastDisplay(true);
    if (!CaptureTransform(node, ip_->GetTime())) return false;
    ViewExp& vpt = ip_->GetViewExp(hwnd);
    return vpt.IsAlive();
}

bool SculptMode::Raycast(ViewExp& vpt, float x, float y, bool cull, sculpt::RayHit& hit, sculpt::Vec3* rayDir) const {
    SculptSessionBridge* bridge = object_ ? object_->Bridge() : nullptr;
    if (!bridge) return false;
    Ray worldRay;
    vpt.MapScreenToWorldRay(x, y, worldRay);
    const sculpt::Ray ray{ToVec3(worldToObject_.PointTransform(worldRay.p)),
                          ToVec3(worldToObject_.VectorTransform(worldRay.dir))};
    if (rayDir) *rayDir = sculpt::normalizedOrZero(ray.dir);
    return bridge->Session().raycast(ray, hit, cull);
}

float SculptMode::WorldPerPixel(ViewExp& vpt, const Point3& world) const {
    GraphicsWindow* gw = vpt.getGW();
    const int width = gw ? gw->getWinSizeX() : 0;
    if (width <= 0) return 0.0f;
    const float w = vpt.GetVPWorldWidth(world) / static_cast<float>(width);
    return std::isfinite(w) && w > 0.0f ? w : 0.0f;
}

bool SculptMode::ScreenToPlane(ViewExp& vpt, float x, float y, const sculpt::Vec3& point, const sculpt::Vec3& normal,
                               sculpt::Vec3& out) const {
    Ray worldRay;
    vpt.MapScreenToWorldRay(x, y, worldRay);
    const sculpt::Vec3 o = ToVec3(worldToObject_.PointTransform(worldRay.p));
    const sculpt::Vec3 d = ToVec3(worldToObject_.VectorTransform(worldRay.dir));
    const float denom = sculpt::dot(d, normal);
    if (std::fabs(denom) < 1e-20f) return false;
    const float t = sculpt::dot(point - o, normal) / denom;
    out = o + d * t;
    return sculpt::isFinite(out);
}

sculpt::Vec3 SculptMode::ScreenVector(ViewExp& vpt, float x, float y, float dx, float dy, const sculpt::Vec3& at) const {
    Ray worldRay;
    vpt.MapScreenToWorldRay(x, y, worldRay);
    const sculpt::Vec3 normal = sculpt::normalizedOrZero(ToVec3(worldToObject_.VectorTransform(worldRay.dir)));
    sculpt::Vec3 a, b;
    if (!ScreenToPlane(vpt, x, y, at, normal, a) || !ScreenToPlane(vpt, x + dx * 8.0f, y + dy * 8.0f, at, normal, b))
        return sculpt::Vec3{0.0f, 0.0f, 1.0f};
    const sculpt::Vec3 v = sculpt::normalizedOrZero(b - a);
    return sculpt::lengthSq(v) > 0.0f ? v : sculpt::Vec3{0.0f, 0.0f, 1.0f};
}

bool SculptMode::NearestVertexInBrush(ViewExp& vpt, IPoint2 m, sculpt::Vec3& out) const {
    // Move may start beside the surface: grab the vertex closest to the cursor
    // ray if it lies inside the on-screen brush circle.
    SculptSessionBridge* bridge = object_ ? object_->Bridge() : nullptr;
    GraphicsWindow* gw = vpt.getGW();
    if (!bridge || !gw) return false;
    const sculpt::SculptSession& session = bridge->Session();
    const sculpt::Mesh& mesh = session.mesh();
    gw->setTransform(objectToWorld_);
    const float radius = SculptSettings::Get().Size();
    float best = radius * radius;
    bool found = false;
    const std::vector<std::uint8_t>& hidden = session.hiddenFaces();
    for (std::uint32_t v = 0; v < mesh.vertexCount(); ++v) {
        if (session.anyHidden()) {
            bool visible = false;
            for (std::uint32_t f : mesh.vertexFaces(v)) visible = visible || !hidden[f];
            if (!visible) continue;
        }
        Point3 p = ToPoint3(mesh.position(v));
        IPoint3 s;
        if (gw->wTransPoint(&p, &s) != 0) continue;
        const float dx = static_cast<float>(s.x - m.x), dy = static_cast<float>(s.y - m.y);
        const float d2 = dx * dx + dy * dy;
        if (d2 <= best) {
            best = d2;
            out = mesh.position(v);
            found = true;
        }
    }
    return found;
}

void SculptMode::ApplyStrokeSymmetry() {
    SculptSessionBridge* bridge = object_ ? object_->Bridge() : nullptr;
    if (!bridge) return;
    const SculptSettings& s = SculptSettings::Get();
    sculpt::SymmetrySettings symmetry;
    symmetry.mirrorX = s.Bool(Prop::MirrorX);
    symmetry.mirrorY = s.Bool(Prop::MirrorY);
    symmetry.mirrorZ = s.Bool(Prop::MirrorZ);
    symmetry.radial = s.Bool(Prop::RadialMirror);
    symmetry.radialCount = s.Int(Prop::RadialCount);
    symmetry.radialAxis = s.Int(Prop::RadialAxis);
    bridge->Session().setSymmetry(sculpt::symmetryTransforms(symmetry));
}

void SculptMode::Redraw(DWORD flags) {
    if (!ip_) return;
    if (overlayRegistered_) ip_->NotifyViewportDisplayCallbackChanged(FALSE, &overlay_);
    ip_->RedrawViews(ip_->GetTime(), flags);
}

// --- Mouse ----------------------------------------------------------------------------------

int SculptMode::proc(HWND hwnd, int msg, int point, int flags, IPoint2 m) {
    if (!ip_ || !object_) return FALSE;
    switch (msg) {
        case MOUSE_POINT:
            if (point == 0)
                Press(hwnd, m, flags, false);
            else
                Release(hwnd, m, flags);
            break;
        case MOUSE_DBLCLICK:
            if (gesture_ == Gesture::None) Press(hwnd, m, flags, true);
            break;
        case MOUSE_MOVE:
            Drag(hwnd, m, flags);
            break;
        case MOUSE_ABORT:  // Right-click or Esc while dragging.
            Cancel();
            break;
        case MOUSE_FREEMOVE:
            if (gesture_ != Gesture::None) Release(hwnd, m, flags);  // The button-up was missed.
            UpdateOverlay(hwnd, m, flags);
            break;
        case MOUSE_PROPCLICK:  // Right-click while idle: Quick Menu.
            if (gesture_ == Gesture::None) {
                POINT p = {m.x, m.y};
                ClientToScreen(hwnd, &p);
                SculptUI::ShowQuickMenu(p, false);
            }
            break;
        default:
            break;
    }
    return TRUE;
}

void SculptMode::Press(HWND hwnd, IPoint2 m, int flags, bool doubleClick) {
    if (gesture_ == Gesture::Consumed) {
        gesture_ = Gesture::None;  // Button-up of a handled double-click reported as a press.
        return;
    }
    if (gesture_ != Gesture::None) Release(hwnd, m, flags);
    gestureHwnd_ = hwnd;
    press_ = last_ = m;
    moved_ = false;
    pressHit_ = false;
    eraseGesture_ = false;
    overlay_.SetViewport(hwnd);
    if (!PrepareSession(hwnd)) return;
    ViewExp& vpt = ip_->GetViewExp(hwnd);

    const SculptSettings& settings = SculptSettings::Get();
    const bool ctrl = (flags & MOUSE_CTRL) != 0, shift = (flags & MOUSE_SHIFT) != 0, alt = (flags & MOUSE_ALT) != 0;
    const bool maskDirect = settings.Bool(Prop::MaskDirect);
    const MaskTool maskTool = static_cast<MaskTool>(settings.Int(Prop::MaskTool));
    const float mx = static_cast<float>(m.x), my = static_cast<float>(m.y);

    // Paint Mask double-click (or two quick clicks): local mask blur.
    const DWORD now = GetTickCount();
    const bool quickSecond = now - lastClickTime_ <= GetDoubleClickTime() && !FarApart(m, lastClickPos_);
    lastClickTime_ = 0;
    if (maskDirect && maskTool == MaskTool::PaintMask && !ctrl && !shift && (doubleClick || quickSecond)) {
        MaskBlurAt(hwnd, m);
        gesture_ = Gesture::Consumed;
        return;
    }

    if (ctrl && shift && alt) {
        gesture_ = Gesture::Visibility;
        pressHit_ = Raycast(vpt, mx, my, false, pressHitInfo_);
        return;
    }
    if (ctrl && shift) {
        gesture_ = Gesture::Resize;
        resizeStart_ = settings.Size();
        overlay_.ShowCircle(m, resizeStart_, false, kSculptColor);
        return;
    }

    const BrushType brush = settings.Brush();
    if (ctrl && alt) {
        // Click: straight line from the previous stroke. Drag: unmask.
        const bool lineTool = maskDirect ? maskTool == MaskTool::PaintMask : IsLineBrush(brush);
        if (lineTool) {
            gesture_ = Gesture::Chord;
            chordMaskDirect_ = maskDirect;
            const BrushType lineBrush = maskDirect ? BrushType::MaskPaint : brush;
            pressHit_ = Raycast(vpt, mx, my, settings.MakeBrush(lineBrush).backfaceCull, pressHitInfo_);
            return;
        }
    }

    if (maskDirect || ctrl) {  // Mask tool, selected directly or held with Ctrl.
        const bool erase = alt;
        if (maskTool == MaskTool::Rectangle || maskTool == MaskTool::Lasso) {
            gesture_ = Gesture::Shape;
            shapeLasso_ = maskTool == MaskTool::Lasso;
            eraseGesture_ = erase;
            shape_.assign(1, m);
            return;
        }
        const sculpt::BrushSettings maskBrush = settings.MakeBrush(BrushType::MaskPaint);
        pressHit_ = Raycast(vpt, mx, my, maskBrush.backfaceCull, pressHitInfo_);
        if (!pressHit_) {
            gesture_ = Gesture::EmptyMask;
            eraseGesture_ = erase;
            return;
        }
        // Temporary (Ctrl) Paint Mask: a stationary click blurs instead of painting.
        BeginStroke(hwnd, m, maskBrush, erase, !maskDirect, false);
        return;
    }

    // Sculpt brushes. Shift: temporary Smooth (Face groups: extend the group under the cursor).
    BrushType type = brush;
    if (shift && brush != BrushType::FaceGroups) type = BrushType::Smooth;
    sculpt::BrushSettings strokeBrush = settings.MakeBrush(type);
    pressHit_ = Raycast(vpt, mx, my, strokeBrush.backfaceCull, pressHitInfo_);
    if (type == BrushType::FaceGroups) {
        SculptSessionBridge* bridge = object_->Bridge();
        if (alt)
            strokeBrush.faceGroupId = 0;
        else if (shift && pressHit_)
            strokeBrush.faceGroupId = bridge->Session().groupOfTriangle(pressHitInfo_.triangle);
        else
            strokeBrush.faceGroupId = bridge->Session().nextGroupId();
    }
    BeginStroke(hwnd, m, strokeBrush, alt && type != BrushType::FaceGroups, false, false);
}

void SculptMode::Drag(HWND hwnd, IPoint2 m, int /*flags*/) {
    if (gesture_ == Gesture::None || hwnd != gestureHwnd_) return;
    if (!moved_ && FarApart(m, press_)) moved_ = true;
    ViewExp& vpt = ip_->GetViewExp(hwnd);
    if (!vpt.IsAlive()) return;
    SculptSettings& settings = SculptSettings::Get();

    switch (gesture_) {
        case Gesture::Stroke:
            ContinueStroke(hwnd, m);
            break;
        case Gesture::Grab:
            ApplyGrab(vpt, m);
            object_->CommitSessionChanges(true);
            overlay_.ShowCircle(m, settings.Size(), settings.Bool(Prop::UseFalloff), kSculptColor);
            Redraw(REDRAW_INTERACTIVE);
            break;
        case Gesture::DragDab: {
            const float dx = static_cast<float>(m.x - press_.x), dy = static_cast<float>(m.y - press_.y);
            dragRadiusPx_ = std::sqrt(dx * dx + dy * dy);
            if (dragRadiusPx_ > 0.5f) {
                lastDirX_ = dx / dragRadiusPx_;
                lastDirY_ = dy / dragRadiusPx_;
            }
            overlay_.ShowCircle(press_, std::max(dragRadiusPx_, 2.0f), false, strokeInvert_ ? kInvertColor : kSculptColor);
            Redraw(REDRAW_INTERACTIVE);
            break;
        }
        case Gesture::Shape:
            if (shapeLasso_) {
                if (FarApart(m, shape_.back(), 2)) shape_.push_back(m);
                overlay_.ShowShape(shape_, true, eraseGesture_ ? kEraseColor : kMaskColor);
            } else {
                const std::vector<IPoint2> rect = {press_, IPoint2(m.x, press_.y), m, IPoint2(press_.x, m.y)};
                shape_ = {press_, m};
                overlay_.ShowShape(rect, true, eraseGesture_ ? kEraseColor : kMaskColor);
            }
            Redraw(REDRAW_INTERACTIVE);
            break;
        case Gesture::Resize:
            settings.Set(Prop::BrushSize, resizeStart_ + static_cast<float>(m.x - press_.x));
            overlay_.ShowCircle(press_, settings.Size(), false, kSculptColor);
            Redraw(REDRAW_INTERACTIVE);
            break;
        case Gesture::Chord:
            if (!moved_) break;
            // Dragging the Ctrl+Alt chord unmasks with the last mask tool.
            {
                const MaskTool maskTool = static_cast<MaskTool>(settings.Int(Prop::MaskTool));
                if (maskTool != MaskTool::PaintMask && !chordMaskDirect_) {
                    gesture_ = Gesture::Shape;
                    shapeLasso_ = maskTool == MaskTool::Lasso;
                    eraseGesture_ = true;
                    shape_.assign(1, press_);
                    Drag(hwnd, m, 0);
                } else if (pressHit_) {
                    BeginStroke(hwnd, press_, settings.MakeBrush(BrushType::MaskPaint), true, false, false);
                    moved_ = true;
                    ContinueStroke(hwnd, m);
                } else {
                    gesture_ = Gesture::EmptyMask;
                    eraseGesture_ = true;
                }
            }
            break;
        default:
            break;
    }
    last_ = m;
}

void SculptMode::Release(HWND hwnd, IPoint2 m, int /*flags*/) {
    const Gesture gesture = gesture_;
    if (gesture == Gesture::None) return;
    if (!object_ || !object_->Bridge()) {
        gesture_ = Gesture::None;
        return;
    }
    switch (gesture) {
        case Gesture::Stroke:
            if (deferFirstDab_ && !moved_) {
                // Ctrl + stationary click: one local Blur Mask pass.
                object_->Bridge()->Session().cancelStroke();
                object_->CommitSessionChanges(false);
                gesture_ = Gesture::None;
                MaskBlurAt(hwnd, press_);
            } else {
                const bool clickedMask = strokeBrush_.type == BrushType::MaskPaint && !moved_;
                FinishStroke(GetString(UndoNameFor(strokeBrush_.type)));
                if (clickedMask) {  // Remember for double-click detection.
                    lastClickTime_ = GetTickCount();
                    lastClickPos_ = press_;
                }
            }
            break;
        case Gesture::Grab:
            FinishStroke(GetString(UndoNameFor(strokeBrush_.type)));
            break;
        case Gesture::DragDab: {
            ViewExp& vpt = ip_->GetViewExp(hwnd);
            if (vpt.IsAlive()) {
                const float size = SculptSettings::Get().Size();
                const float scale = dragRadiusPx_ >= 2.0f ? dragRadiusPx_ / size : 1.0f;
                const sculpt::StrokeSample sample{static_cast<float>(press_.x), static_cast<float>(press_.y), 1.0f};
                orientToDrag_ = dragRadiusPx_ >= 2.0f;  // The drag direction turns the alpha.
                strokeChanged_ |= ApplySample(vpt, sample, scale, 1.0f);
                orientToDrag_ = false;
            }
            FinishStroke(GetString(UndoNameFor(strokeBrush_.type)));
            break;
        }
        case Gesture::Shape:
            ApplyShape(hwnd, !moved_);
            break;
        case Gesture::EmptyMask:
            ApplyEmptyMask(moved_, eraseGesture_);
            break;
        case Gesture::Visibility:
            ApplyVisibility(moved_);
            break;
        case Gesture::Chord:
            if (pressHit_)
                ApplyLine(hwnd, m);
            else
                ApplyEmptyMask(false, true);  // Ctrl+Alt click in empty space: clear visible mask.
            break;
        default:
            break;
    }
    gesture_ = Gesture::None;
    UpdateOverlay(hwnd, m, 0);
    Redraw(REDRAW_END);
}

void SculptMode::Cancel() {
    const Gesture gesture = gesture_;
    gesture_ = Gesture::None;
    if (gesture == Gesture::None) return;
    SculptSessionBridge* bridge = object_ ? object_->Bridge() : nullptr;
    if (bridge && bridge->Session().strokeActive()) {
        bridge->Session().cancelStroke();
        object_->CommitSessionChanges(false);
    }
    strokeAlpha_.reset();
    overlay_.Hide();
    Redraw(REDRAW_END);
}

// --- Strokes ---------------------------------------------------------------------------------

void SculptMode::BeginStroke(HWND hwnd, IPoint2 m, const sculpt::BrushSettings& brush, bool invert, bool deferFirstDab,
                             bool forceDraw) {
    SculptSessionBridge* bridge = object_->Bridge();
    ViewExp& vpt = ip_->GetViewExp(hwnd);
    if (!bridge || !vpt.IsAlive()) return;
    const SculptSettings& settings = SculptSettings::Get();

    strokeBrush_ = brush;
    strokeInvert_ = invert;
    strokeAlpha_ = settings.Bool(Prop::UseAlpha) ? AlphaLibrary::Get().Active() : nullptr;
    strokeBrush_.alpha = strokeAlpha_ ? strokeAlpha_.get() : nullptr;
    deferFirstDab_ = deferFirstDab;
    strokeChanged_ = false;
    haveLastSample_ = false;
    haveLastDab_ = false;
    lastDirX_ = 0.0f;
    lastDirY_ = -1.0f;
    ApplyStrokeSymmetry();
    bridge->Session().beginStroke();

    strokeMode_ = static_cast<StrokeMode>(settings.Int(Prop::StrokeMode));
    if (forceDraw || strokeMode_ == StrokeMode::ColorMix || IsGrabBrush(brush.type)) strokeMode_ = StrokeMode::Draw;

    if (IsGrabBrush(brush.type)) {
        gesture_ = Gesture::Grab;
        sculpt::Vec3 point;
        bool ok = pressHit_;
        if (ok) point = pressHitInfo_.position;
        if (!ok && brush.type == BrushType::Move) ok = NearestVertexInBrush(vpt, m, point);
        if (!ok) {
            bridge->Session().cancelStroke();
            gesture_ = Gesture::Consumed;
            return;
        }
        Ray worldRay;
        vpt.MapScreenToWorldRay(static_cast<float>(m.x), static_cast<float>(m.y), worldRay);
        grabNormal_ = sculpt::normalizedOrZero(ToVec3(worldToObject_.VectorTransform(worldRay.dir)));
        grabPoint_ = grabCenter_ = point;
        // The first dab captures the region (no movement yet).
        const Point3 world = objectToWorld_.PointTransform(ToPoint3(point));
        sculpt::Dab dab;
        dab.center = point;
        dab.radius = settings.Size() * WorldPerPixel(vpt, world) / objectScale_;
        dab.viewDir = grabNormal_;
        if (dab.radius > 0.0f) bridge->Session().applyDab(strokeBrush_, dab);
        overlay_.SetAnchor(world);
        return;
    }
    if (strokeMode_ == StrokeMode::Drag) {
        gesture_ = Gesture::DragDab;
        dragRadiusPx_ = 0.0f;
        return;
    }

    gesture_ = Gesture::Stroke;
    const sculpt::StrokeSample first{static_cast<float>(m.x), static_cast<float>(m.y), 1.0f};
    lazyOn_ = settings.Bool(Prop::LazyMouse) && strokeMode_ != StrokeMode::Stamp;
    if (lazyOn_) lazy_.reset(first);
    scatter_ = sculpt::Scatter();
    samples_.clear();
    spacer_.begin(first, samples_);
    if (deferFirstDab_) {
        samples_.clear();  // Applied once the mouse really moves.
        return;
    }
    ApplySpacedSamples(vpt);
    object_->CommitSessionChanges(true);
    Redraw(REDRAW_INTERACTIVE);
}

void SculptMode::ContinueStroke(HWND hwnd, IPoint2 m) {
    if (strokeMode_ == StrokeMode::Stamp) return;
    ViewExp& vpt = ip_->GetViewExp(hwnd);
    if (!vpt.IsAlive()) return;
    if (deferFirstDab_) {
        if (!moved_) return;
        deferFirstDab_ = false;
        const sculpt::StrokeSample first{static_cast<float>(press_.x), static_cast<float>(press_.y), 1.0f};
        samples_.assign(1, first);
        ApplySpacedSamples(vpt);
    }
    FeedPointer(vpt, {static_cast<float>(m.x), static_cast<float>(m.y), 1.0f});
    object_->CommitSessionChanges(true);
    const SculptSettings& settings = SculptSettings::Get();
    overlay_.ShowCircle(m, settings.Size(), settings.Bool(Prop::UseFalloff),
                        strokeBrush_.type == BrushType::MaskPaint ? (strokeInvert_ ? kEraseColor : kMaskColor)
                                                                   : (strokeInvert_ ? kInvertColor : kSculptColor));
    Redraw(REDRAW_INTERACTIVE);
}

void SculptMode::FeedPointer(ViewExp& vpt, const sculpt::StrokeSample& pointer) {
    if (!lazyOn_) {
        const SculptSettings& settings = SculptSettings::Get();
        const float spacing = std::max(sculpt::StrokeSpacer::kMinSpacing, settings.Size() * settings.Spacing());
        spacer_.moveTo(pointer, spacing, samples_);
        ApplySpacedSamples(vpt);
        return;
    }
    const SculptSettings& settings = SculptSettings::Get();
    sculpt::StrokeSample brush;
    if (lazy_.update(pointer, settings.Value(Prop::LazyAmount) * settings.Size() * 2.0f, brush)) {
        const float spacing = std::max(sculpt::StrokeSpacer::kMinSpacing, settings.Size() * settings.Spacing());
        spacer_.moveTo(brush, spacing, samples_);
        ApplySpacedSamples(vpt);
    }
}

void SculptMode::ApplySpacedSamples(ViewExp& vpt) {
    const SculptSettings& settings = SculptSettings::Get();
    for (const sculpt::StrokeSample& sample : samples_) {
        if (haveLastSample_) {
            const float dx = sample.x - lastSample_.x, dy = sample.y - lastSample_.y;
            const float len = std::sqrt(dx * dx + dy * dy);
            if (len > 0.5f) {
                lastDirX_ = dx / len;
                lastDirY_ = dy / len;
            }
        }
        lastSample_ = sample;
        haveLastSample_ = true;

        if (strokeMode_ == StrokeMode::Scatter) {
            sculpt::ScatterSettings scatter;
            scatter.density = settings.Int(Prop::ScatterDensity);
            scatter.radius = settings.Value(Prop::ScatterRadius);
            scatter.sizeJitter = settings.Value(Prop::SizeJitter);
            scatter.amountJitter = settings.Value(Prop::AmountJitter);
            scatterDabs_.clear();
            scatter_.generate(sample, settings.Size(), scatter, scatterDabs_);
            for (const sculpt::ScatterDab& d : scatterDabs_) strokeChanged_ |= ApplySample(vpt, d.sample, d.sizeScale, d.amount);
        } else {
            strokeChanged_ |= ApplySample(vpt, sample, 1.0f, 1.0f);
        }
    }
    samples_.clear();
}

bool SculptMode::ApplySample(ViewExp& vpt, const sculpt::StrokeSample& sample, float sizeScale, float amount) {
    SculptSessionBridge* bridge = object_ ? object_->Bridge() : nullptr;
    if (!bridge) return false;
    sculpt::RayHit hit;
    sculpt::Vec3 dir;
    if (!Raycast(vpt, sample.x, sample.y, strokeBrush_.backfaceCull, hit, &dir)) return false;
    const Point3 hitWorld = objectToWorld_.PointTransform(ToPoint3(hit.position));
    const float wpp = WorldPerPixel(vpt, hitWorld);
    if (!(wpp > 0.0f)) return false;

    sculpt::Dab dab;
    dab.center = hit.position;
    dab.radius = SculptSettings::Get().Size() * sizeScale * wpp / objectScale_;
    dab.pressure = sample.pressure;
    dab.amount = amount;
    dab.viewDir = dir;
    dab.invert = strokeInvert_;
    // Alpha orientation: along the stroke (Follow Path) or screen up.
    const bool follow = orientToDrag_ || SculptSettings::Get().Bool(Prop::FollowPath);
    dab.up = ScreenVector(vpt, sample.x, sample.y, follow ? lastDirX_ : 0.0f, follow ? lastDirY_ : -1.0f, hit.position);
    overlay_.SetAnchor(hitWorld);
    lastDabCenter_ = hit.position;
    haveLastDab_ = true;
    return bridge->Session().applyDab(strokeBrush_, dab) > 0;
}

void SculptMode::ApplyGrab(ViewExp& vpt, IPoint2 m) {
    SculptSessionBridge* bridge = object_ ? object_->Bridge() : nullptr;
    if (!bridge) return;
    sculpt::Vec3 target;
    if (!ScreenToPlane(vpt, static_cast<float>(m.x), static_cast<float>(m.y), grabPoint_, grabNormal_, target)) return;
    const sculpt::Vec3 total = target - grabPoint_;
    if (sculpt::lengthSq(total) == 0.0f) return;

    const Point3 world = objectToWorld_.PointTransform(ToPoint3(grabPoint_));
    const float radius = SculptSettings::Get().Size() * WorldPerPixel(vpt, world) / objectScale_;
    if (!(radius > 0.0f)) return;
    // Snake Hook follows the pulled geometry in small steps; Move applies the whole delta.
    int steps = 1;
    if (strokeBrush_.type == BrushType::SnakeHook)
        steps = std::min(kMaxSnakeSteps, std::max(1, static_cast<int>(std::ceil(sculpt::length(total) / (radius * 0.25f)))));
    const sculpt::Vec3 step = total * (1.0f / static_cast<float>(steps));
    for (int i = 0; i < steps; ++i) {
        sculpt::Dab dab;
        dab.center = strokeBrush_.type == BrushType::Move ? grabCenter_ : grabPoint_;
        dab.radius = radius;
        dab.viewDir = grabNormal_;
        dab.grabDelta = step;
        strokeChanged_ |= bridge->Session().applyDab(strokeBrush_, dab) > 0;
        grabPoint_ = grabPoint_ + step;
    }
    lastDabCenter_ = grabPoint_;
    haveLastDab_ = true;
    overlay_.SetAnchor(objectToWorld_.PointTransform(ToPoint3(grabPoint_)));
}

void SculptMode::FinishStroke(const MCHAR* undoName) {
    gesture_ = Gesture::None;
    strokeAlpha_.reset();
    SculptSessionBridge* bridge = object_ ? object_->Bridge() : nullptr;
    if (!bridge || !bridge->Session().strokeActive()) return;
    const sculpt::StrokeDelta delta = bridge->Session().endStroke();
    object_->CommitSessionChanges(false);
    if (!delta.empty()) {
        theHold.Begin();
        object_->PutStrokeUndo(delta, undoName);
        theHold.Accept(undoName);
    }
    // The end of this stroke is the next Ctrl+Alt straight-line anchor.
    const BrushType type = strokeBrush_.type;
    if (haveLastDab_ && (IsLineBrush(type) || type == BrushType::MaskPaint)) {
        anchorValid_ = true;
        anchor_ = lastDabCenter_;
        anchorBrush_ = type;
    }
}

void SculptMode::ApplyLine(HWND hwnd, IPoint2 m) {
    ViewExp& vpt = ip_->GetViewExp(hwnd);
    GraphicsWindow* gw = vpt.IsAlive() ? vpt.getGW() : nullptr;
    if (!gw) return;
    const SculptSettings& settings = SculptSettings::Get();
    const BrushType type = chordMaskDirect_ ? BrushType::MaskPaint : settings.Brush();
    const sculpt::BrushSettings brush = settings.MakeBrush(type);
    sculpt::RayHit end;
    if (!Raycast(vpt, static_cast<float>(m.x), static_cast<float>(m.y), brush.backfaceCull, end)) end = pressHitInfo_;

    if (!anchorValid_ || anchorBrush_ != type) {
        // No compatible previous stroke: this click is the first anchor.
        anchorValid_ = true;
        anchor_ = end.position;
        anchorBrush_ = type;
        ip_->ReplacePrompt(GetString(IDS_PROMPT_LINE_START));
        return;
    }
    gw->setTransform(objectToWorld_);
    Point3 a = ToPoint3(anchor_);
    IPoint3 screenA;
    if (gw->wTransPoint(&a, &screenA) != 0) {  // Anchor is off-screen.
        anchor_ = end.position;
        return;
    }
    // The selected Add/Sub direction is used: Alt belongs to the chord.
    BeginStroke(hwnd, IPoint2(screenA.x, screenA.y), brush, false, false, true);
    if (gesture_ != Gesture::Stroke) return;
    lazyOn_ = false;
    FeedPointer(vpt, {static_cast<float>(m.x), static_cast<float>(m.y), 1.0f});
    FinishStroke(GetString(UndoNameFor(type)));
    anchorValid_ = true;
    anchor_ = end.position;
    anchorBrush_ = type;
}

void SculptMode::MaskBlurAt(HWND hwnd, IPoint2 m) {
    SculptSessionBridge* bridge = object_ ? object_->Bridge() : nullptr;
    ViewExp& vpt = ip_->GetViewExp(hwnd);
    if (!bridge || !vpt.IsAlive()) return;
    const sculpt::BrushSettings maskBrush = SculptSettings::Get().MakeBrush(BrushType::MaskPaint);
    sculpt::RayHit hit;
    sculpt::Vec3 dir;
    if (!Raycast(vpt, static_cast<float>(m.x), static_cast<float>(m.y), maskBrush.backfaceCull, hit, &dir)) return;
    const float wpp = WorldPerPixel(vpt, objectToWorld_.PointTransform(ToPoint3(hit.position)));
    if (!(wpp > 0.0f)) return;
    sculpt::Dab dab;
    dab.center = hit.position;
    dab.radius = SculptSettings::Get().Size() * wpp / objectScale_;
    dab.viewDir = dir;
    ApplyStrokeSymmetry();
    sculpt::SculptSession& session = bridge->Session();
    session.beginStroke();
    session.applyMaskBlurDab(dab, 1.0f);
    const sculpt::StrokeDelta delta = session.endStroke();
    object_->CommitSessionChanges(false);
    if (!delta.empty()) {
        const MCHAR* name = SculptCommands::OpDisplayName(SculptCommands::Op::MaskBlur);
        theHold.Begin();
        object_->PutStrokeUndo(delta, name);
        theHold.Accept(name);
    }
    Redraw(REDRAW_NORMAL);
}

// --- Mask selection and visibility gestures ------------------------------------------------

void SculptMode::ApplyShape(HWND hwnd, bool clickOnly) {
    const std::size_t minimum = shapeLasso_ ? 3u : 2u;
    if (clickOnly || shape_.size() < minimum) {
        ApplyEmptyMask(false, eraseGesture_);  // A click with Rectangle/Lasso: visible-mask shortcut.
        return;
    }
    ViewExp& vpt = ip_->GetViewExp(hwnd);
    GraphicsWindow* gw = vpt.IsAlive() ? vpt.getGW() : nullptr;
    if (!gw) return;
    gw->setTransform(objectToWorld_);
    const int x0 = std::min(shape_[0].x, shape_.back().x), x1 = std::max(shape_[0].x, shape_.back().x);
    const int y0 = std::min(shape_[0].y, shape_.back().y), y1 = std::max(shape_[0].y, shape_.back().y);
    const bool lasso = shapeLasso_;
    const std::vector<IPoint2>& polygon = shape_;
    std::size_t inside = 0;
    auto test = [&](const sculpt::Vec3& p) {
        Point3 q = ToPoint3(p);
        IPoint3 s;
        if (gw->wTransPoint(&q, &s) != 0) return false;
        const bool in = lasso ? PointInPolygon(polygon, static_cast<float>(s.x), static_cast<float>(s.y))
                              : (s.x >= x0 && s.x <= x1 && s.y >= y0 && s.y <= y1);
        if (in) ++inside;
        return in;
    };
    const bool erase = eraseGesture_;
    object_->RunOperation([&](SculptSessionBridge& bridge) { return bridge.Session().maskSelect(test, erase); },
                          GetString(IDS_UNDO_MASK));
    if (inside == 0) ApplyEmptyMask(true, erase);  // The shape touched nothing: invert the visible mask.
}

void SculptMode::ApplyEmptyMask(bool dragged, bool erase) {
    if (!object_) return;
    using Op = SculptCommands::Op;
    const Op op = dragged ? Op::MaskInvert : (erase ? Op::MaskClear : Op::MaskToggleVisible);
    object_->RunOperation(
        [op](SculptSessionBridge& bridge) {
            sculpt::SculptSession& s = bridge.Session();
            if (op == Op::MaskInvert) return s.maskInvert(true);
            if (op == Op::MaskClear) return s.maskClear(true);
            return s.maskToggleVisible();
        },
        SculptCommands::OpDisplayName(op));
}

void SculptMode::ApplyVisibility(bool dragged) {
    if (!object_) return;
    using Op = SculptCommands::Op;
    if (pressHit_) {
        const sculpt::RayHit hit = pressHitInfo_;
        object_->RunOperation(
            [hit](SculptSessionBridge& bridge) {
                sculpt::SculptSession& s = bridge.Session();
                return s.isolateOrHideGroup(s.groupOfTriangle(hit.triangle));
            },
            GetString(IDS_UNDO_VISIBILITY));
        return;
    }
    const Op op = dragged ? Op::InvertVisibility : Op::ShowAll;
    object_->RunOperation(
        [op](SculptSessionBridge& bridge) {
            return op == Op::InvertVisibility ? bridge.Session().invertVisibility() : bridge.Session().showAll();
        },
        SculptCommands::OpDisplayName(op));
}

// --- Cursor --------------------------------------------------------------------------------------

void SculptMode::UpdateOverlay(HWND hwnd, IPoint2 m, int flags) {
    overlay_.SetViewport(hwnd);
    last_ = m;
    gestureHwnd_ = hwnd;
    const SculptSettings& settings = SculptSettings::Get();
    const bool ctrl = (flags & MOUSE_CTRL) != 0, shift = (flags & MOUSE_SHIFT) != 0, alt = (flags & MOUSE_ALT) != 0;
    const bool maskDirect = settings.Bool(Prop::MaskDirect);
    const MaskTool maskTool = static_cast<MaskTool>(settings.Int(Prop::MaskTool));
    const bool usesMask = maskDirect || (ctrl && !shift);

    if ((ctrl && shift && alt) || (usesMask && maskTool != MaskTool::PaintMask)) {
        overlay_.Hide();  // Visibility gesture or Rectangle/Lasso: no brush circle.
    } else {
        Point3 color = kSculptColor;
        if (usesMask) {
            color = alt ? kEraseColor : kMaskColor;
        } else if (settings.Brush() == BrushType::FaceGroups) {
            color = kGroupColor;
        } else if (sculpt::isSignedBrush(settings.Brush()) && !shift && (settings.Subtract() != alt)) {
            color = kInvertColor;
        }
        overlay_.ShowCircle(m, settings.Size(), settings.Bool(Prop::UseFalloff), color);
        // Anchor the circle on the surface under the mouse (or keep the last one).
        INode* node = nullptr;
        if (object_ && object_->Bridge() && ResolveTargetNode(node)) {
            ViewExp& vpt = ip_->GetViewExp(hwnd);
            if (vpt.IsAlive() && CaptureTransform(node, ip_->GetTime())) {
                sculpt::RayHit hit;
                if (Raycast(vpt, static_cast<float>(m.x), static_cast<float>(m.y), false, hit))
                    overlay_.SetAnchor(objectToWorld_.PointTransform(ToPoint3(hit.position)));
                else
                    overlay_.SetAnchor(node->GetObjectTM(ip_->GetTime()).GetTrans());
            }
        }
    }
    Redraw(REDRAW_NORMAL);
}
