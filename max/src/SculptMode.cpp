#include "SculptMode.h"

#include <algorithm>
#include <cmath>

#include "SculptMeshObject.h"
#include "SculptPanel.h"
#include "SculptSettings.h"

namespace {

constexpr int kCircleSegments = 64;
constexpr float kTwoPi = 6.28318530718f;

// Intersects a world ray with the plane through `point` facing `normal`.
bool IntersectPlane(const Ray& ray, const Point3& point, const Point3& normal, Point3& out) {
    const float denom = DotProd(ray.dir, normal);
    if (std::fabs(denom) < 1e-12f) return false;
    const float t = DotProd(point - ray.p, normal) / denom;
    out = ray.p + ray.dir * t;
    return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.z);
}

}  // namespace

// --- SculptForegroundCallback ---------------------------------------------------

void SculptForegroundCallback::callback(TimeValue t, IScene* /*scene*/) {
    if (object_) object_->FlagDependents(t);
}

// --- SculptBrushCursor ----------------------------------------------------------

void SculptBrushCursor::Display(TimeValue /*t*/, ViewExp* vpt, int /*flags*/) {
    if (!visible_ || !hasAnchor_ || !vpt || !vpt->IsAlive() || vpt->GetHWnd() != hwnd_) return;
    GraphicsWindow* gw = vpt->getGW();
    if (!gw) return;

    // The circle lives on a camera-facing plane through the anchor, so it
    // matches the on-screen brush radius used for the dabs.
    Ray centerRay;
    vpt->MapScreenToWorldRay(static_cast<float>(screen_.x), static_cast<float>(screen_.y), centerRay);
    const Point3 normal = Normalize(centerRay.dir);
    const float radiusPx = SculptSettings::Get().Size();

    Point3 ring[2][kCircleSegments];
    const float scales[2] = {1.0f, 0.5f};  // Outer rim and falloff half-way ring.
    const int ringCount = SculptSettings::Get().UseFalloff() ? 2 : 1;
    for (int r = 0; r < ringCount; ++r) {
        for (int i = 0; i < kCircleSegments; ++i) {
            const float a = kTwoPi * static_cast<float>(i) / static_cast<float>(kCircleSegments);
            Ray ray;
            vpt->MapScreenToWorldRay(static_cast<float>(screen_.x) + radiusPx * scales[r] * std::cos(a),
                                     static_cast<float>(screen_.y) + radiusPx * scales[r] * std::sin(a), ray);
            if (!IntersectPlane(ray, anchor_, normal, ring[r][i])) return;
        }
    }

    const DWORD limits = gw->getRndLimits();
    gw->setRndLimits(limits & ~GW_Z_BUFFER);  // Always on top of the mesh.
    Matrix3 identity;
    identity.IdentityMatrix();
    gw->setTransform(identity);
    if (inverted_)
        gw->setColor(LINE_COLOR, 0.25f, 0.55f, 1.0f);
    else
        gw->setColor(LINE_COLOR, 1.0f, 0.2f, 0.2f);
    gw->startSegments();
    for (int r = 0; r < ringCount; ++r) {
        for (int i = 0; i < kCircleSegments; ++i) {
            Point3 segment[2] = {ring[r][i], ring[r][(i + 1) % kCircleSegments]};
            gw->segment(segment, 1);
        }
    }
    gw->endSegments();
    gw->setRndLimits(limits);
}

void SculptBrushCursor::GetViewportRect(TimeValue /*t*/, ViewExp* vpt, Rect* rect) {
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

// --- SculptMode -------------------------------------------------------------------

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
    CancelStroke();
    IObjParam* ip = ip_;
    ip->DeleteMode(this);  // Calls ExitMode() if the mode is on the stack.
    if (active_) ExitMode();  // Defensive: never leave a registered callback behind.
    object_ = nullptr;
    foreground_.SetObject(nullptr);
    ip_ = nullptr;
}

void SculptMode::Toggle(IObjParam* ip, SculptMeshObject* object) {
    if (active_ && object_ == object)
        Stop();
    else
        Start(ip, object);
}

void SculptMode::ForgetObject(SculptMeshObject* object) {
    if (object_ != object) return;
    // No UI or undo calls here: the object is being destroyed.
    strokeActive_ = false;
    holdOpen_ = false;
    object_ = nullptr;
    foreground_.SetObject(nullptr);
    cursor_.Hide();
}

void SculptMode::EnterMode() {
    active_ = true;
    if (ip_ && !cursorRegistered_) {
        ip_->RegisterViewportDisplayCallback(FALSE, &cursor_);
        cursorRegistered_ = true;
    }
    if (ip_) ip_->PushPrompt(GetString(IDS_PROMPT_SCULPT));
    SculptPanel::Refresh();

    // Build the session up front so the first click does not stall.
    if (object_) {
        MSTR error;
        SetCursor(LoadCursor(nullptr, IDC_WAIT));
        if (object_->AcquireSession(error))
            object_->SetFastDisplay(true);
        else if (ip_)
            ip_->ReplacePrompt(error.data());
        SetCursor(LoadCursor(nullptr, IDC_ARROW));
    }
}

void SculptMode::ExitMode() {
    CancelStroke();
    cursor_.Hide();
    if (object_) object_->SetFastDisplay(false);
    if (ip_) {
        if (cursorRegistered_) {
            ip_->UnRegisterViewportDisplayCallback(FALSE, &cursor_);
            cursorRegistered_ = false;
        }
        if (active_) ip_->PopPrompt();
    }
    active_ = false;
    SculptPanel::Refresh();
    if (ip_) ip_->RedrawViews(ip_->GetTime());
}

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

int SculptMode::proc(HWND hwnd, int msg, int point, int flags, IPoint2 m) {
    if (!ip_ || !object_) return FALSE;
    switch (msg) {
        case MOUSE_POINT:
            if (point == 0)
                BeginStroke(hwnd, m, flags);
            else
                EndStroke();
            break;
        case MOUSE_MOVE:
            ContinueStroke(hwnd, m);
            break;
        case MOUSE_ABORT:  // Right-click or Esc while dragging.
            CancelStroke();
            break;
        case MOUSE_FREEMOVE:
            cursor_.SetInverted(SculptSettings::Get().Subtract() != ((flags & MOUSE_ALT) != 0));
            UpdateCursor(hwnd, m, true);
            break;
        case MOUSE_PROPCLICK:  // Right-click while idle leaves sculpt mode.
            if (!strokeActive_) ip_->PopCommandMode();
            break;
        default:
            break;
    }
    return TRUE;
}

void SculptMode::BeginStroke(HWND hwnd, IPoint2 m, int flags) {
    if (strokeActive_) EndStroke();

    INode* node = nullptr;
    if (!ResolveTargetNode(node)) {
        ip_->ReplacePrompt(GetString(IDS_ERR_SELECT_ONE));
        return;
    }
    MSTR error;
    if (!object_->AcquireSession(error)) {
        ip_->ReplacePrompt(error.data());
        return;
    }
    if (!CaptureTransform(node, ip_->GetTime())) return;

    const SculptSettings& settings = SculptSettings::Get();
    const sculpt::BrushType type = (flags & MOUSE_SHIFT) ? sculpt::BrushType::Smooth : settings.Brush();
    strokeBrush_ = settings.MakeBrush(type);
    strokeInvert_ = (flags & MOUSE_ALT) != 0;

    theHold.Begin();
    holdOpen_ = true;
    object_->Bridge()->Session().beginStroke();
    strokeActive_ = true;

    samples_.clear();
    spacer_.begin({static_cast<float>(m.x), static_cast<float>(m.y), 1.0f}, samples_);
    ApplyPendingSamples(hwnd);
    UpdateCursor(hwnd, m, false);
}

void SculptMode::ContinueStroke(HWND hwnd, IPoint2 m) {
    if (!strokeActive_) return;
    const SculptSettings& settings = SculptSettings::Get();
    const float spacingPx = std::max(sculpt::StrokeSpacer::kMinSpacing, settings.Size() * settings.Spacing());
    samples_.clear();
    spacer_.moveTo({static_cast<float>(m.x), static_cast<float>(m.y), 1.0f}, spacingPx, samples_);
    UpdateCursor(hwnd, m, false);
    ApplyPendingSamples(hwnd);
}

void SculptMode::ApplyPendingSamples(HWND hwnd) {
    SculptSessionBridge* bridge = object_ ? object_->Bridge() : nullptr;
    if (!bridge || samples_.empty()) return;
    ViewExp& vpt = ip_->GetViewExp(hwnd);
    if (!vpt.IsAlive()) return;

    bool changed = false;
    for (const sculpt::StrokeSample& sample : samples_) changed |= ApplyDab(vpt, *bridge, sample);
    samples_.clear();
    if (changed) object_->CommitSessionChanges();
    Redraw(REDRAW_INTERACTIVE);
}

bool SculptMode::ApplyDab(ViewExp& vpt, SculptSessionBridge& bridge, const sculpt::StrokeSample& sample) {
    Ray worldRay;
    vpt.MapScreenToWorldRay(sample.x, sample.y, worldRay);
    const Point3 origin = worldToObject_.PointTransform(worldRay.p);
    const Point3 direction = worldToObject_.VectorTransform(worldRay.dir);

    const sculpt::Ray ray{ToVec3(origin), ToVec3(direction)};
    sculpt::RayHit hit;
    if (!bridge.Session().raycast(ray, hit)) return false;

    // Brush size is in screen pixels: convert it at the depth of the hit.
    const Point3 hitWorld = objectToWorld_.PointTransform(ToPoint3(hit.position));
    GraphicsWindow* gw = vpt.getGW();
    const int viewportWidth = gw ? gw->getWinSizeX() : 0;
    if (viewportWidth <= 0) return false;
    const float worldRadius = SculptSettings::Get().Size() * vpt.GetVPWorldWidth(hitWorld) / static_cast<float>(viewportWidth);

    sculpt::Dab dab;
    dab.center = hit.position;
    dab.radius = worldRadius / objectScale_;
    dab.pressure = sample.pressure;
    dab.viewDir = sculpt::normalizedOrZero(ray.dir);
    dab.invert = strokeInvert_;
    cursor_.SetAnchor(hitWorld);
    return bridge.Session().applyDab(strokeBrush_, dab) > 0;
}

void SculptMode::EndStroke() {
    if (!strokeActive_) return;
    strokeActive_ = false;

    SculptSessionBridge* bridge = object_ ? object_->Bridge() : nullptr;
    if (!bridge) {
        if (holdOpen_) theHold.Cancel();
        holdOpen_ = false;
        return;
    }

    sculpt::StrokeDelta delta = bridge->Session().endStroke();
    object_->CommitSessionChanges();

    if (holdOpen_) {
        if (delta.empty() || !delta.consistent()) {
            theHold.Cancel();
        } else {
            std::vector<int> vertices;
            std::vector<Point3> before, after;
            vertices.reserve(delta.vertices.size());
            before.reserve(delta.vertices.size());
            after.reserve(delta.vertices.size());
            for (std::size_t i = 0; i < delta.vertices.size(); ++i) {
                vertices.push_back(bridge->ToMax(delta.vertices[i]));
                before.push_back(ToPoint3(delta.before[i]));
                after.push_back(ToPoint3(delta.after[i]));
            }
            theHold.Put(new SculptStrokeRestore(object_, std::move(vertices), std::move(before), std::move(after)));
            theHold.Accept(GetString(IDS_UNDO_STROKE));
        }
        holdOpen_ = false;
    }
    Redraw(REDRAW_END);
}

void SculptMode::CancelStroke() {
    if (!strokeActive_) return;
    strokeActive_ = false;
    SculptSessionBridge* bridge = object_ ? object_->Bridge() : nullptr;
    if (bridge) {
        bridge->Session().cancelStroke();
        object_->CommitSessionChanges();
    }
    if (holdOpen_) {
        theHold.Cancel();
        holdOpen_ = false;
    }
    Redraw(REDRAW_END);
}

void SculptMode::UpdateCursor(HWND hwnd, IPoint2 m, bool redraw) {
    cursor_.SetScreen(hwnd, m);

    // Anchor the circle on the surface under the mouse (or keep the last one).
    INode* node = nullptr;
    SculptSessionBridge* bridge = object_ ? object_->Bridge() : nullptr;
    if (bridge && ResolveTargetNode(node)) {
        ViewExp& vpt = ip_->GetViewExp(hwnd);
        if (vpt.IsAlive()) {
            const TimeValue t = ip_->GetTime();
            const Matrix3 toWorld = node->GetObjectTM(t);
            const Matrix3 toObject = Inverse(toWorld);
            Ray worldRay;
            vpt.MapScreenToWorldRay(static_cast<float>(m.x), static_cast<float>(m.y), worldRay);
            const sculpt::Ray ray{ToVec3(toObject.PointTransform(worldRay.p)),
                                  ToVec3(toObject.VectorTransform(worldRay.dir))};
            sculpt::RayHit hit;
            if (bridge->Session().raycast(ray, hit))
                cursor_.SetAnchor(toWorld.PointTransform(ToPoint3(hit.position)));
            else
                cursor_.SetAnchor(toWorld.GetTrans());
        }
    }
    if (redraw) Redraw(REDRAW_NORMAL);
}

void SculptMode::Redraw(DWORD flags) {
    if (!ip_) return;
    if (cursorRegistered_) ip_->NotifyViewportDisplayCallbackChanged(FALSE, &cursor_);
    ip_->RedrawViews(ip_->GetTime(), flags);
}
