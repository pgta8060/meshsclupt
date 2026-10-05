// Specialised brushes of the viewport mode: Density (remeshes on release),
// Clip / Cutter / Slice (screen-space shapes), Pose, Cloth and Curve Tube,
// plus the Curve Tube section picker.
#include <algorithm>
#include <cmath>

#include "SculptCommands.h"
#include "SculptMeshObject.h"
#include "SculptMode.h"
#include "SculptUI.h"
#include "sculpt/cut.h"
#include "sculpt/remesh.h"

using sculpt::BrushType;
using sculpt::Vec3;

namespace {

const Point3 kCutColor(1.0f, 0.85f, 0.25f);
const Point3 kCutSideColor(1.0f, 0.35f, 0.25f);
const Point3 kGuideColor(0.3f, 0.9f, 1.0f);
const Point3 kTubeColor(1.0f, 0.72f, 0.3f);
constexpr float kTwoPi = 6.28318530718f;
constexpr float kFar = 1e6f;          // Field value for points that cannot be projected.
constexpr int kTubePickRadius = 12;   // Pixels around a control point.
constexpr int kTubeEndRadius = 40;    // Pixels around a tube end (taper).
constexpr int kMinShapePixels = 3;

bool IsCutBrush(BrushType b) { return b == BrushType::Clip || b == BrushType::Cutter || b == BrushType::Slice; }

sculpt::Mat3 Transposed(const sculpt::Mat3& m) {
    return {{m.r0.x, m.r1.x, m.r2.x}, {m.r0.y, m.r1.y, m.r2.y}, {m.r0.z, m.r1.z, m.r2.z}};
}

float Cross2(float ax, float ay, float bx, float by) { return ax * by - ay * bx; }

// Nearest point of the open path to (x, y); the first and last segments run
// on to infinity. `side` < 0 on the right of the drawing direction.
bool PathNearest(const std::vector<IPoint2>& path, float x, float y, float& qx, float& qy, float& side) {
    float best = kFar * kFar;
    bool found = false;
    for (std::size_t i = 0; i + 1 < path.size(); ++i) {
        const float ax = static_cast<float>(path[i].x), ay = static_cast<float>(path[i].y);
        const float dx = static_cast<float>(path[i + 1].x) - ax, dy = static_cast<float>(path[i + 1].y) - ay;
        const float len2 = dx * dx + dy * dy;
        if (len2 < 1e-6f) continue;
        float t = ((x - ax) * dx + (y - ay) * dy) / len2;
        if (i > 0) t = std::max(t, 0.0f);
        if (i + 2 < path.size()) t = std::min(t, 1.0f);
        const float px = ax + dx * t, py = ay + dy * t;
        const float d2 = (x - px) * (x - px) + (y - py) * (y - py);
        if (d2 < best) {
            best = d2;
            qx = px;
            qy = py;
            side = Cross2(dx, dy, x - ax, y - ay) / std::sqrt(len2);
            found = true;
        }
    }
    return found;
}

// Signed distance to an axis-aligned rectangle (negative inside).
float RectDistance(float x, float y, float x0, float y0, float x1, float y1) {
    const float dx = std::max(x0 - x, x - x1), dy = std::max(y0 - y, y - y1);
    if (dx <= 0.0f && dy <= 0.0f) return std::max(dx, dy);
    const float ox = std::max(dx, 0.0f), oy = std::max(dy, 0.0f);
    return std::sqrt(ox * ox + oy * oy);
}

std::int32_t NextGroup(const sculpt::PolyData& poly) {
    std::int32_t next = 1;
    for (std::int32_t g : poly.groups) next = std::max(next, g + 1);
    return next;
}

// --- Curve Tube section picker ---------------------------------------------------------

bool IsClosedShape(INode* node, TimeValue t) {
    if (!node) return false;
    const ObjectState os = node->EvalWorldState(t);
    if (!os.obj || os.obj->SuperClassID() != SHAPE_CLASS_ID) return false;
    ShapeObject* shape = static_cast<ShapeObject*>(os.obj);
    return shape->NumberOfCurves(t) > 0 && shape->CurveClosed(t, 0);
}

class SectionPick : public PickModeCallback, public PickNodeCallback {
public:
    BOOL HitTest(IObjParam* ip, HWND hwnd, ViewExp* /*vpt*/, IPoint2 m, int /*flags*/) override {
        return ip->PickNode(hwnd, m, this) ? TRUE : FALSE;
    }
    BOOL Pick(IObjParam* /*ip*/, ViewExp* vpt) override {
        if (INode* node = vpt ? vpt->GetClosestHit() : nullptr) SculptMode::Get().SetSectionNode(node->GetHandle());
        return TRUE;  // Ends the pick mode.
    }
    BOOL RightClick(IObjParam* /*ip*/, ViewExp* /*vpt*/) override { return TRUE; }
    void EnterMode(IObjParam* ip) override {
        active = true;
        ip->PushPrompt(GetString(IDS_PROMPT_PICK_SECTION));
        SculptUI::Refresh();
    }
    void ExitMode(IObjParam* ip) override {
        active = false;
        ip->PopPrompt();
        SculptUI::Refresh();
    }
    PickNodeCallback* GetFilter() override { return this; }
    BOOL Filter(INode* node) override {
        Interface* core = GetCOREInterface();
        return core && IsClosedShape(node, core->GetTime()) ? TRUE : FALSE;
    }
    bool active = false;
};

SectionPick& Picker() {
    static SectionPick pick;
    return pick;
}

}  // namespace

namespace SectionPicker {

void Toggle() {
    Interface* core = GetCOREInterface();
    if (!core) return;
    if (Picker().active)
        core->ClearPickMode();
    else
        core->SetPickMode(&Picker());
}

bool Active() { return Picker().active; }

bool Cancel() {
    if (!Picker().active) return false;
    if (Interface* core = GetCOREInterface()) core->ClearPickMode();
    return true;
}

}  // namespace SectionPicker

// --- ScreenProjector ------------------------------------------------------------------------

bool ScreenProjector::Capture(ViewExp& vpt, const Matrix3& objectToWorld, const Matrix3& worldToObject, float x, float y) {
    objectToWorld_ = objectToWorld;
    worldToObject_ = worldToObject;
    cx_ = x;
    cy_ = y;
    Matrix3 affine;
    vpt.GetAffineTM(affine);
    const Matrix3 camera = Inverse(affine);
    forward_ = -Normalize(camera.GetRow(2));  // Views look down their -Z.
    eye_ = camera.GetTrans();
    perspective_ = vpt.IsPerspView() != FALSE;
    Ray r0, r1, r2;
    vpt.MapScreenToWorldRay(x, y, r0);
    vpt.MapScreenToWorldRay(x + 1.0f, y, r1);
    vpt.MapScreenToWorldRay(x, y + 1.0f, r2);
    if (perspective_) {
        // Points on the plane one unit in front of the eye move linearly with the pixel.
        Point3 a[3];
        const Ray* rays[3] = {&r0, &r1, &r2};
        for (int i = 0; i < 3; ++i) {
            const float d = DotProd(rays[i]->dir, forward_);
            if (!(d > 1e-6f)) return false;
            a[i] = rays[i]->dir / d;
        }
        base_ = a[0];
        right_ = a[1] - a[0];
        down_ = a[2] - a[0];
    } else {
        auto flat = [this](const Point3& v) { return v - forward_ * DotProd(v, forward_); };
        base_ = r0.p;
        right_ = flat(r1.p - r0.p);
        down_ = flat(r2.p - r0.p);
    }
    rr_ = DotProd(right_, right_);
    rd_ = DotProd(right_, down_);
    dd_ = DotProd(down_, down_);
    det_ = rr_ * dd_ - rd_ * rd_;
    return std::isfinite(det_) && std::fabs(det_) > 1e-30f;
}

bool ScreenProjector::Project(const Vec3& objectPoint, float& x, float& y, float& depth) const {
    const Point3 w = objectToWorld_.PointTransform(ToPoint3(objectPoint));
    Point3 u;
    if (perspective_) {
        const Point3 q = w - eye_;
        depth = DotProd(q, forward_);
        if (!(depth > 1e-6f)) return false;  // Behind the eye.
        u = q / depth - base_;
    } else {
        u = w - base_;
        depth = DotProd(u, forward_);
        u = u - forward_ * depth;
    }
    const float a = DotProd(u, right_), b = DotProd(u, down_);
    x = cx_ + (a * dd_ - b * rd_) / det_;
    y = cy_ + (b * rr_ - a * rd_) / det_;
    return std::isfinite(x) && std::isfinite(y);
}

Vec3 ScreenProjector::Unproject(float x, float y, float depth) const {
    const Point3 offset = right_ * (x - cx_) + down_ * (y - cy_);
    const Point3 w = perspective_ ? eye_ + (base_ + offset) * depth : base_ + offset + forward_ * depth;
    return ToVec3(worldToObject_.PointTransform(w));
}

// --- Dispatch -----------------------------------------------------------------------------------

bool SculptMode::PressTool(HWND hwnd, IPoint2 m, int flags) {
    const SculptSettings& settings = SculptSettings::Get();
    const BrushType brush = settings.Brush();
    const bool ctrl = (flags & MOUSE_CTRL) != 0, shift = (flags & MOUSE_SHIFT) != 0, alt = (flags & MOUSE_ALT) != 0;
    ViewExp& vpt = ip_->GetViewExp(hwnd);
    if (!vpt.IsAlive()) return false;
    const float mx = static_cast<float>(m.x), my = static_cast<float>(m.y);

    if (IsCutBrush(brush)) {
        if (!projector_.Capture(vpt, objectToWorld_, worldToObject_, mx, my)) {
            gesture_ = Gesture::Consumed;
            return true;
        }
        strokeBrush_ = settings.MakeBrush(brush);
        cutShape_ = CutShape::Line;
        cutInvert_ = false;
        if (brush == BrushType::Clip) {
            if (ctrl) cutShape_ = CutShape::Rect;
            else if (shift) cutShape_ = CutShape::Circle;
            else if (alt) cutShape_ = CutShape::Curve;
            cutInvert_ = alt && (ctrl || shift);
        } else if (brush == BrushType::Cutter) {
            if (ctrl) cutShape_ = CutShape::Rect;
            else if (shift) cutShape_ = CutShape::Circle;
            cutInvert_ = alt;
        }
        cutPoints_.assign(2, m);
        cutMouse_ = m;
        gesture_ = Gesture::Cut;
        ShowCutOverlay();
        Redraw(REDRAW_INTERACTIVE);
        return true;
    }

    if (brush == BrushType::CurveTube) {
        TubePress(vpt, m, flags);
        return true;
    }

    if (brush == BrushType::Pose && !ctrl && !shift) {
        pressHit_ = Raycast(vpt, mx, my, true, pressHitInfo_);
        strokeBrush_ = settings.MakeBrush(BrushType::Pose);
        projector_.Capture(vpt, objectToWorld_, worldToObject_, mx, my);
        if (settings.Int(Prop::PoseRotationOrigins) == 0 && !poseGuide_) {
            if (!pressHit_) {
                gesture_ = Gesture::Consumed;
                return true;
            }
            poseA_ = poseB_ = pressHitInfo_.position;
            gesture_ = Gesture::PoseGuide;
            UpdatePoseOverlay();
            return true;
        }
        if (!BeginPose(vpt, m)) {
            gesture_ = Gesture::Consumed;
            ip_->ReplacePrompt(GetString(IDS_PROMPT_POSE_GUIDE));
        }
        return true;
    }

    if (brush == BrushType::Cloth && !ctrl && !shift) {
        SculptSessionBridge* bridge = object_->Bridge();
        sculpt::RayHit hit;
        Vec3 dir;
        strokeBrush_ = settings.MakeBrush(BrushType::Cloth);
        if (!Raycast(vpt, mx, my, strokeBrush_.backfaceCull, hit, &dir)) {
            gesture_ = Gesture::Consumed;
            return true;
        }
        const Point3 world = objectToWorld_.PointTransform(ToPoint3(hit.position));
        clothRadius_ = settings.Size() * WorldPerPixel(vpt, world) / objectScale_;
        sculpt::ClothSettings cloth;
        cloth.iterations = settings.Int(Prop::ClothIterations);
        cloth.damping = settings.Value(Prop::ClothDamping);
        cloth.plasticity = settings.Value(Prop::ClothPlasticity);
        cloth.bendiness = settings.Value(Prop::ClothBendiness);
        cloth.foldSize = settings.Value(Prop::ClothFoldSize);
        cloth.foldStrength = settings.Value(Prop::ClothFoldStrength);
        cloth.bendStiffness = settings.Value(Prop::ClothBendStiffness);
        cloth.simulationArea = settings.Value(Prop::ClothSimulationArea);
        cloth.moveStrength = settings.Value(Prop::ClothMoveStrength);
        cloth.gravity = settings.Value(Prop::ClothGravity);
        cloth.pressure = settings.Value(Prop::ClothPressure);
        cloth.pinBoundary = settings.Bool(Prop::ClothPinBoundary);
        if (!(clothRadius_ > 0.0f) || !cloth_.begin(bridge->Session().mesh(), hit.position, clothRadius_, cloth)) {
            gesture_ = Gesture::Consumed;
            return true;
        }
        clothCenter_ = hit.position;
        clothNormal_ = dir;
        clothShrink_ = alt;
        bridge->Session().beginStroke();
        overlay_.SetAnchor(world);
        gesture_ = Gesture::Cloth;
        return true;
    }
    return false;
}

void SculptMode::DragTool(HWND hwnd, IPoint2 m, int /*flags*/) {
    ViewExp& vpt = ip_->GetViewExp(hwnd);
    if (!vpt.IsAlive()) return;
    switch (gesture_) {
        case Gesture::Cut: {
            const bool pan = (GetKeyState(VK_SPACE) & 0x8000) != 0;
            const int dx = m.x - cutMouse_.x, dy = m.y - cutMouse_.y;
            cutMouse_ = m;
            if (pan) {  // Space: move the whole shape.
                for (IPoint2& p : cutPoints_) p = IPoint2(p.x + dx, p.y + dy);
            } else if (cutShape_ == CutShape::Curve) {
                const IPoint2 last = cutPoints_.back();
                if (std::abs(m.x - last.x) + std::abs(m.y - last.y) >= 3) cutPoints_.push_back(m);
            } else {
                cutPoints_.back() = m;
            }
            ShowCutOverlay();
            Redraw(REDRAW_INTERACTIVE);
            break;
        }
        case Gesture::PoseGuide: {
            sculpt::RayHit hit;
            if (Raycast(vpt, static_cast<float>(m.x), static_cast<float>(m.y), false, hit)) {
                poseB_ = hit.position;
            } else {
                float x, y, depth;
                if (projector_.Project(poseA_, x, y, depth))
                    poseB_ = projector_.Unproject(static_cast<float>(m.x), static_cast<float>(m.y), depth);
            }
            UpdatePoseOverlay();
            Redraw(REDRAW_INTERACTIVE);
            break;
        }
        case Gesture::Pose:
            DragPose(vpt, m);
            break;
        case Gesture::Cloth:
            DragCloth(vpt, m);
            break;
        case Gesture::Tube:
            TubeDrag(vpt, m);
            break;
        case Gesture::Gradient:
            DragGradient(vpt, m);
            break;
        default:
            break;
    }
}

void SculptMode::ReleaseTool(HWND hwnd, IPoint2 m, int /*flags*/) {
    switch (gesture_) {
        case Gesture::Gradient: {
            ViewExp& vpt = ip_->GetViewExp(hwnd);
            if (vpt.IsAlive()) ApplyGradient(vpt, m);
            break;
        }
        case Gesture::Cut:
            gesture_ = Gesture::None;
            overlay_.Hide();
            ApplyCut();
            break;
        case Gesture::PoseGuide:
            gesture_ = Gesture::None;
            poseGuide_ = moved_ && sculpt::lengthSq(poseB_ - poseA_) > 0.0f;
            UpdatePoseOverlay();
            ip_->ReplacePrompt(GetString(poseGuide_ ? IDS_PROMPT_POSE_READY : IDS_PROMPT_POSE_GUIDE));
            break;
        case Gesture::Pose: {
            FinishStroke(GetString(IDS_UNDO_POSE));
            const SculptSettings& settings = SculptSettings::Get();
            if (settings.Int(Prop::PoseRotationOrigins) == 1 || !settings.Bool(Prop::PoseKeepAnchor)) {
                poseGuide_ = false;  // The next drag starts a new guide / picks a group.
            } else if (poseSettings_.deformation == sculpt::PoseDeformation::Rotate) {
                poseB_ = poseTarget_;  // B follows the new handle position.
            }
            poseWeights_.clear();
            poseVertices_.clear();
            poseOriginal_.clear();
            UpdatePoseOverlay();
            break;
        }
        case Gesture::Cloth:
            FinishStroke(GetString(IDS_UNDO_CLOTH));
            cloth_.end();
            break;
        case Gesture::Tube:
            gesture_ = Gesture::None;
            UpdateTubeOverlay();
            break;
        default:
            break;
    }
}

void SculptMode::CancelTool() {
    switch (gesture_) {
        case Gesture::Cut:
            overlay_.Hide();
            break;
        case Gesture::PoseGuide:
            poseGuide_ = false;
            UpdatePoseOverlay();
            break;
        case Gesture::Pose:
            poseWeights_.clear();
            poseVertices_.clear();
            poseOriginal_.clear();
            UpdatePoseOverlay();
            break;
        case Gesture::Cloth:
            cloth_.end();
            break;
        default:
            break;
    }
}

void SculptMode::UpdateToolOverlay() {
    const BrushType brush = SculptSettings::Get().Brush();
    if (tube_.active)
        UpdateTubeOverlay();
    else if (brush == BrushType::Pose && poseGuide_)
        UpdatePoseOverlay();
    else
        overlay_.ClearWorld();
}

bool SculptMode::CancelPending() {
    if (SectionPicker::Cancel()) return true;
    if (!tube_.active) return false;
    tube_ = TubeState();
    overlay_.ClearWorld();
    Redraw(REDRAW_NORMAL);
    return true;
}

void SculptMode::ClearPoseGuide() {
    poseGuide_ = false;
    overlay_.ClearWorld();
    if (ip_) ip_->ReplacePrompt(GetString(IDS_PROMPT_POSE_GUIDE));
    Redraw(REDRAW_NORMAL);
}

void SculptMode::SetSectionNode(ULONG handle) {
    sectionNode_ = handle;
    if (tube_.active) {
        UpdateTubeOverlay();
        Redraw(REDRAW_NORMAL);
    }
    SculptUI::Refresh();
}

// --- Clip / Cutter / Slice ------------------------------------------------------------------------

void SculptMode::ShowCutOverlay() {
    std::vector<IPoint2> pairs;
    const IPoint2 a = cutPoints_.front(), b = cutPoints_.back();
    auto add = [&pairs](IPoint2 p, IPoint2 q) {
        pairs.push_back(p);
        pairs.push_back(q);
    };
    auto sideTicks = [&](IPoint2 p, IPoint2 q) {  // Short marks on the affected side.
        const float dx = static_cast<float>(q.x - p.x), dy = static_cast<float>(q.y - p.y);
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len < 1.0f) return;
        float nx = dy / len, ny = -dx / len;  // Right of the direction (side < 0).
        if (cutInvert_) {
            nx = -nx;
            ny = -ny;
        }
        const int ticks = std::max(1, static_cast<int>(len / 24.0f));
        for (int i = 0; i <= ticks; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(ticks);
            const IPoint2 s(static_cast<int>(p.x + dx * t), static_cast<int>(p.y + dy * t));
            add(s, IPoint2(static_cast<int>(s.x + nx * 8.0f), static_cast<int>(s.y + ny * 8.0f)));
        }
    };
    switch (cutShape_) {
        case CutShape::Line: {
            const float dx = static_cast<float>(b.x - a.x), dy = static_cast<float>(b.y - a.y);
            const float len = std::sqrt(dx * dx + dy * dy);
            if (len >= 1.0f) {  // The cut runs on across the view.
                const float ex = dx / len * 4000.0f, ey = dy / len * 4000.0f;
                add(IPoint2(static_cast<int>(a.x - ex), static_cast<int>(a.y - ey)),
                    IPoint2(static_cast<int>(b.x + ex), static_cast<int>(b.y + ey)));
                sideTicks(a, b);
            }
            break;
        }
        case CutShape::Curve:
            for (std::size_t i = 0; i + 1 < cutPoints_.size(); ++i) {
                add(cutPoints_[i], cutPoints_[i + 1]);
                if (i % 6 == 0) sideTicks(cutPoints_[i], cutPoints_[i + 1]);
            }
            break;
        case CutShape::Rect: {
            const IPoint2 c1(b.x, a.y), c3(a.x, b.y);
            add(a, c1);
            add(c1, b);
            add(b, c3);
            add(c3, a);
            break;
        }
        case CutShape::Circle: {
            const float r = std::sqrt(static_cast<float>((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y)));
            IPoint2 prev(static_cast<int>(a.x + r), a.y);
            for (int i = 1; i <= 64; ++i) {
                const float t = kTwoPi * static_cast<float>(i) / 64.0f;
                const IPoint2 p(static_cast<int>(a.x + r * std::cos(t)), static_cast<int>(a.y + r * std::sin(t)));
                add(prev, p);
                prev = p;
            }
            break;
        }
    }
    const BrushType type = strokeBrush_.type;
    const bool removes = type == BrushType::Cutter || (type == BrushType::Clip && cutInvert_);
    overlay_.ShowSegments(pairs, removes ? kCutSideColor : kCutColor);
}

bool SculptMode::CutField(float x, float y, float& value) const {
    const float ax = static_cast<float>(cutPoints_.front().x), ay = static_cast<float>(cutPoints_.front().y);
    const float bx = static_cast<float>(cutPoints_.back().x), by = static_cast<float>(cutPoints_.back().y);
    switch (cutShape_) {
        case CutShape::Line: {
            const float dx = bx - ax, dy = by - ay, len = std::sqrt(dx * dx + dy * dy);
            if (len < 1e-3f) return false;
            value = Cross2(dx, dy, x - ax, y - ay) / len;  // < 0: removed / second part.
            break;
        }
        case CutShape::Rect:
            value = RectDistance(x, y, std::min(ax, bx), std::min(ay, by), std::max(ax, bx), std::max(ay, by));
            break;
        case CutShape::Circle:
            value = std::sqrt((x - ax) * (x - ax) + (y - ay) * (y - ay)) - std::sqrt((bx - ax) * (bx - ax) + (by - ay) * (by - ay));
            break;
        case CutShape::Curve: {
            float qx, qy;
            if (!PathNearest(cutPoints_, x, y, qx, qy, value)) return false;
            break;
        }
    }
    if (cutInvert_) value = -value;
    return true;
}

bool SculptMode::ClipTarget(float x, float y, float& tx, float& ty) const {
    const float ax = static_cast<float>(cutPoints_.front().x), ay = static_cast<float>(cutPoints_.front().y);
    const float bx = static_cast<float>(cutPoints_.back().x), by = static_cast<float>(cutPoints_.back().y);
    switch (cutShape_) {
        case CutShape::Line: {
            const float dx = bx - ax, dy = by - ay, len = std::sqrt(dx * dx + dy * dy);
            if (len < 1e-3f) return false;
            const float side = Cross2(dx, dy, x - ax, y - ay) / len;
            if (side >= 0.0f) return false;
            // Onto the line: move against the side normal.
            tx = x + (-dy / len) * side * -1.0f;
            ty = y + (dx / len) * side * -1.0f;
            return true;
        }
        case CutShape::Curve: {
            float side;
            if (!PathNearest(cutPoints_, x, y, tx, ty, side)) return false;
            return side < 0.0f;
        }
        case CutShape::Rect: {
            const float x0 = std::min(ax, bx), x1 = std::max(ax, bx), y0 = std::min(ay, by), y1 = std::max(ay, by);
            const bool inside = x >= x0 && x <= x1 && y >= y0 && y <= y1;
            if (!cutInvert_) {  // Keep the inside: flatten what is outside onto the border.
                if (inside) return false;
                tx = std::min(std::max(x, x0), x1);
                ty = std::min(std::max(y, y0), y1);
                return true;
            }
            if (!inside) return false;  // Alt: push the inside out to the nearest side.
            const float d[4] = {x - x0, x1 - x, y - y0, y1 - y};
            const int k = static_cast<int>(std::min_element(d, d + 4) - d);
            tx = k == 0 ? x0 : (k == 1 ? x1 : x);
            ty = k == 2 ? y0 : (k == 3 ? y1 : y);
            return true;
        }
        case CutShape::Circle: {
            const float r = std::sqrt((bx - ax) * (bx - ax) + (by - ay) * (by - ay));
            const float dx = x - ax, dy = y - ay, d = std::sqrt(dx * dx + dy * dy);
            if (d < 1e-4f || (d <= r) != cutInvert_) return false;
            tx = ax + dx / d * r;
            ty = ay + dy / d * r;
            return true;
        }
    }
    return false;
}

void SculptMode::ApplyCut() {
    if (!object_ || !object_->Bridge()) return;
    const IPoint2 a = cutPoints_.front(), b = cutPoints_.back();
    const bool tiny = std::abs(a.x - b.x) < kMinShapePixels && std::abs(a.y - b.y) < kMinShapePixels;
    if (tiny && cutShape_ != CutShape::Curve) return;
    if (cutShape_ == CutShape::Curve && cutPoints_.size() < 2) return;
    const BrushType type = strokeBrush_.type;
    if (type == BrushType::Clip) {
        ApplyClip();
        return;
    }

    ApplyStrokeSymmetry();
    std::vector<sculpt::Mat3> mirrors = {sculpt::Mat3::identity()};
    if (type == BrushType::Cutter) mirrors = object_->Bridge()->Session().symmetry();
    sculpt::CutOptions options;
    options.field = [this, &mirrors](const Vec3& p) {
        float best = kFar;
        for (const sculpt::Mat3& m : mirrors) {  // Removed where any mirrored copy is inside the shape.
            float x, y, depth, value;
            if (projector_.Project(m * p, x, y, depth) && CutField(x, y, value)) best = std::min(best, value);
        }
        return best;
    };
    const bool region = cutShape_ == CutShape::Rect || cutShape_ == CutShape::Circle;
    if (region && mirrors.size() == 1) {
        options.project = [this](const Vec3& p, float& x, float& y) {
            float depth;
            return projector_.Project(p, x, y, depth);
        };
    }
    options.slice = type == BrushType::Slice;
    options.joinTunnels = region && !cutInvert_;
    MSTR error;
    const bool ok = object_->RunTopologyEdit(
        [&options](sculpt::PolyData& poly, MSTR& e) {
            const std::int32_t next = NextGroup(poly);
            options.capGroup = next;
            options.secondGroup = options.slice ? next : 0;
            std::string coreError;
            if (!sculpt::cutMesh(poly, options, nullptr, &coreError)) {
                e = coreError.empty() ? MSTR(GetString(IDS_ERR_CUT_MISSED)) : MSTR::FromUTF8(coreError.c_str());
                return false;
            }
            return true;
        },
        type == BrushType::Slice ? IDS_UNDO_SLICE : IDS_UNDO_CUTTER, error);
    if (!ok && ip_ && error.Length() > 0) ip_->ReplacePrompt(error.data());
    anchorValid_ = false;
}

void SculptMode::ApplyClip() {
    SculptSessionBridge* bridge = object_->Bridge();
    sculpt::SculptSession& session = bridge->Session();
    ApplyStrokeSymmetry();
    const std::vector<sculpt::Mat3> mirrors = session.symmetry();
    const sculpt::Mesh& mesh = session.mesh();
    std::vector<std::uint32_t> vertices;
    std::vector<Vec3> targets;
    for (std::uint32_t v = 0; v < mesh.vertexCount(); ++v) {
        const Vec3 p = mesh.position(v);
        for (const sculpt::Mat3& m : mirrors) {
            const Vec3 q = m * p;
            float x, y, depth, tx, ty;
            if (!projector_.Project(q, x, y, depth) || !ClipTarget(x, y, tx, ty)) continue;
            vertices.push_back(v);
            targets.push_back(Transposed(m) * projector_.Unproject(tx, ty, depth));
            break;
        }
    }
    if (vertices.empty()) return;
    session.beginStroke();
    session.applyTargets(vertices, targets);
    const sculpt::StrokeDelta delta = session.endStroke();
    object_->CommitSessionChanges(false);
    if (!delta.empty()) {
        const MCHAR* name = GetString(IDS_UNDO_CLIP);
        theHold.Begin();
        object_->PutStrokeUndo(delta, name);
        theHold.Accept(name);
    }
}

// --- Density ----------------------------------------------------------------------------------------

void SculptMode::FinishDensity() {
    SculptSessionBridge* bridge = object_ ? object_->Bridge() : nullptr;
    if (!bridge || !bridge->Session().hasDensityWeights()) return;
    sculpt::SculptSession& session = bridge->Session();
    const std::vector<float> weights = session.densityWeights();
    const float radius = session.densityRadius();
    session.clearDensityWeights();
    const float strength = sculpt::clamp01(strokeBrush_.strength);
    sculpt::DensityOptions options;
    options.reduce = strokeBrush_.subtract != strokeInvert_;  // Sub or Alt: Reduce.
    // Stronger strokes make finer (or, reducing, coarser) triangles.
    options.targetLength = options.reduce ? radius * (0.1f + 0.5f * strength) : radius * (0.25f - 0.21f * strength);
    MSTR error;
    const bool ok = object_->RunTopologyEdit(
        [&](sculpt::PolyData& poly, MSTR& e) {
            std::string coreError;
            if (weights.size() != poly.positions.size() || !sculpt::remeshRegion(poly, weights, options, &coreError)) {
                e = coreError.empty() ? MSTR(GetString(IDS_ERR_DENSITY)) : MSTR::FromUTF8(coreError.c_str());
                return false;
            }
            return true;
        },
        IDS_UNDO_DENSITY, error);
    if (!ok && ip_ && error.Length() > 0) ip_->ReplacePrompt(error.data());
    anchorValid_ = false;
}

// --- Pose -----------------------------------------------------------------------------------------------

bool SculptMode::BeginPose(ViewExp& vpt, IPoint2 /*m*/) {
    SculptSessionBridge* bridge = object_->Bridge();
    sculpt::SculptSession& session = bridge->Session();
    const sculpt::Mesh& mesh = session.mesh();
    const SculptSettings& s = SculptSettings::Get();
    poseSettings_.deformation = static_cast<sculpt::PoseDeformation>(s.Int(Prop::PoseDeformation));
    poseSettings_.originOffset = s.Value(Prop::PoseOriginOffset);
    poseSettings_.smoothIterations = s.Int(Prop::PoseSmoothIterations);
    poseSettings_.ikSegments = s.Int(Prop::PoseIkSegments);
    poseSettings_.connectedOnly = s.Bool(Prop::PoseConnectedOnly);
    if (s.Int(Prop::PoseRotationOrigins) == 1) {
        // SculptGroups: the clicked group turns around its border.
        if (!pressHit_) return false;
        Vec3 pivot;
        if (!sculpt::poseGroupWeights(mesh, session.faceGroups(), mesh.triangleFace(pressHitInfo_.triangle),
                                      poseSettings_.smoothIterations, poseWeights_, pivot))
            return false;
        poseA_ = pivot;
        poseB_ = pressHitInfo_.position;
        poseSettings_.originOffset = 0.0f;
        poseGuide_ = true;
    } else {
        if (!poseGuide_) return false;
        const Point3 world = objectToWorld_.PointTransform(ToPoint3(poseA_));
        const float band = s.Size() * WorldPerPixel(vpt, world) / objectScale_ * 0.5f;
        sculpt::poseWeights(mesh, poseA_, poseB_, band, poseSettings_, poseWeights_);
    }
    if (session.hasMask()) {  // The mask protects; applyTargets then skips it.
        const std::vector<float>& mask = session.mask();
        for (std::size_t v = 0; v < poseWeights_.size() && v < mask.size(); ++v)
            poseWeights_[v] *= 1.0f - sculpt::clamp01(mask[v]);
    }
    poseVertices_.clear();
    poseOriginal_.clear();
    for (std::uint32_t v = 0; v < mesh.vertexCount(); ++v) {
        if (poseWeights_[v] <= 0.0f) continue;
        poseVertices_.push_back(v);
        poseOriginal_.push_back(mesh.position(v));
    }
    if (poseVertices_.empty()) return false;
    poseTarget_ = poseB_;
    poseAmount_ = 0.0f;
    session.beginStroke();
    gesture_ = Gesture::Pose;
    UpdatePoseOverlay();
    return true;
}

void SculptMode::DragPose(ViewExp& /*vpt*/, IPoint2 m) {
    SculptSessionBridge* bridge = object_ ? object_->Bridge() : nullptr;
    if (!bridge) return;
    const float dx = static_cast<float>(m.x - press_.x), dy = static_cast<float>(m.y - press_.y);
    switch (poseSettings_.deformation) {
        case sculpt::PoseDeformation::Twist:
            poseAmount_ = dx * 0.01f;
            break;
        case sculpt::PoseDeformation::Scale:
            poseAmount_ = std::max(-0.95f, -dy * 0.005f);  // Drag up to grow.
            break;
        case sculpt::PoseDeformation::Rotate:
        default: {
            float x, y, depth;  // B follows the mouse at its own depth.
            if (projector_.Project(poseB_, x, y, depth)) poseTarget_ = projector_.Unproject(x + dx, y + dy, depth);
            break;
        }
    }
    std::vector<Vec3> out;
    sculpt::posePositions(poseVertices_, poseOriginal_, poseWeights_, poseA_, poseB_, poseTarget_, poseAmount_,
                          poseSettings_, out);
    strokeChanged_ |= bridge->Session().applyTargets(poseVertices_, out, false) > 0;
    object_->CommitSessionChanges(true);
    UpdatePoseOverlay();
    Redraw(REDRAW_INTERACTIVE);
}

void SculptMode::UpdatePoseOverlay() {
    const bool show = gesture_ == Gesture::PoseGuide || gesture_ == Gesture::Pose || poseGuide_;
    if (!show) {
        overlay_.ClearWorld();
        return;
    }
    const Point3 a = objectToWorld_.PointTransform(ToPoint3(poseA_));
    const Point3 b = objectToWorld_.PointTransform(ToPoint3(gesture_ == Gesture::Pose ? poseTarget_ : poseB_));
    overlay_.SetWorld({a, b}, {a, b}, kGuideColor);
}

// --- Cloth ------------------------------------------------------------------------------------------------

void SculptMode::DragCloth(ViewExp& vpt, IPoint2 m) {
    SculptSessionBridge* bridge = object_ ? object_->Bridge() : nullptr;
    if (!bridge || !cloth_.active()) return;
    Vec3 target;
    if (!ScreenToPlane(vpt, static_cast<float>(m.x), static_cast<float>(m.y), clothCenter_, clothNormal_, target)) return;
    const Vec3 grab = target - clothCenter_;
    // Strength runs extra relaxation steps per mouse move.
    const int steps = 1 + static_cast<int>(sculpt::clamp01(strokeBrush_.strength) * 3.0f);
    std::vector<std::uint32_t> vertices;
    std::vector<Vec3> targets;
    for (int i = 0; i < steps; ++i) {
        cloth_.step(clothCenter_, clothRadius_, i == 0 ? grab : Vec3(), clothShrink_, vertices, targets);
        strokeChanged_ |= bridge->Session().applyTargets(vertices, targets) > 0;
    }
    clothCenter_ = target;
    object_->CommitSessionChanges(true);
    const SculptSettings& settings = SculptSettings::Get();
    overlay_.SetAnchor(objectToWorld_.PointTransform(ToPoint3(clothCenter_)));
    overlay_.ShowCircle(m, settings.Size(), settings.Bool(Prop::UseFalloff), Point3(1.0f, 0.22f, 0.2f));
    Redraw(REDRAW_INTERACTIVE);
}

// --- Curve Tube ------------------------------------------------------------------------------------------

float SculptMode::TubeRadius() const { return SculptSettings::Get().Size() * tube_.unitsPerPixel; }

bool SculptMode::TubePoint(ViewExp& vpt, IPoint2 m, Vec3& out) {
    const float x = static_cast<float>(m.x), y = static_cast<float>(m.y);
    if (tube_.onPlane) return ScreenToPlane(vpt, x, y, tube_.planePoint, tube_.planeNormal, out);
    sculpt::RayHit hit;
    Vec3 dir;
    if (Raycast(vpt, x, y, true, hit, &dir)) {
        out = hit.position + hit.geometricNormal * (SculptSettings::Get().Value(Prop::TubeSurfaceOffset) * TubeRadius());
        return true;
    }
    if (tube_.controls.empty()) return false;
    // Off the surface: continue on the camera-facing plane through the last point.
    return ScreenToPlane(vpt, x, y, tube_.controls.back(), dir, out);
}

void SculptMode::TubePress(ViewExp& vpt, IPoint2 m, int flags) {
    const bool ctrl = (flags & MOUSE_CTRL) != 0, shift = (flags & MOUSE_SHIFT) != 0, alt = (flags & MOUSE_ALT) != 0;
    gesture_ = Gesture::Tube;
    tubeLastPoint_ = m;
    GraphicsWindow* gw = vpt.getGW();
    auto screen = [&](const Vec3& p, IPoint2& out) {
        Point3 q = ToPoint3(p);
        IPoint3 s;
        if (!gw || gw->wTransPoint(&q, &s) != 0) return false;
        out = IPoint2(s.x, s.y);
        return true;
    };
    if (gw) gw->setTransform(objectToWorld_);
    auto nearPoint = [&](const Vec3& p, int radius, int& d2) {
        IPoint2 s;
        if (!screen(p, s)) return false;
        d2 = (s.x - m.x) * (s.x - m.x) + (s.y - m.y) * (s.y - m.y);
        return d2 <= radius * radius;
    };

    if (tube_.active && ctrl && alt) {  // Taper the nearest end.
        int d0 = 0, d1 = 0;
        const bool n0 = nearPoint(tube_.controls.front(), kTubeEndRadius, d0);
        const bool n1 = tube_.controls.size() > 1 && nearPoint(tube_.controls.back(), kTubeEndRadius, d1);
        if (!n0 && !n1) {
            gesture_ = Gesture::Consumed;
            return;
        }
        tubeAction_ = TubeAction::Taper;
        tubeIndex_ = n1 && (!n0 || d1 < d0) ? 1 : 0;
        tubeTaperStart_ = tubeIndex_ == 0 ? tube_.taperStart : tube_.taperEnd;
        return;
    }
    if (tube_.active && shift) {
        tubeAction_ = TubeAction::Relax;
        return;
    }
    if (tube_.active) {
        int best = -1, bestD2 = 0;
        for (std::size_t i = 0; i < tube_.controls.size(); ++i) {
            int d2 = 0;
            if (nearPoint(tube_.controls[i], kTubePickRadius, d2) && (best < 0 || d2 < bestD2)) {
                best = static_cast<int>(i);
                bestD2 = d2;
            }
        }
        if (best >= 0) {
            tubeAction_ = TubeAction::MovePoint;
            tubeIndex_ = best;
            return;
        }
    }
    tubeAction_ = TubeAction::Draw;
    if (!tube_.active) {
        sculpt::RayHit hit;
        Vec3 dir;
        if (!Raycast(vpt, static_cast<float>(m.x), static_cast<float>(m.y), true, hit, &dir)) {
            gesture_ = Gesture::Consumed;
            return;
        }
        tube_ = TubeState();
        const Point3 world = objectToWorld_.PointTransform(ToPoint3(hit.position));
        tube_.unitsPerPixel = WorldPerPixel(vpt, world) / objectScale_;
        tube_.onPlane = ctrl;  // Ctrl: on a camera-facing plane through the first hit.
        tube_.planePoint = hit.position;
        tube_.planeNormal = dir;
        Vec3 first;
        if (!TubePoint(vpt, m, first)) first = hit.position;
        tube_.controls.push_back(first);
        tube_.active = true;
        ip_->ReplacePrompt(GetString(IDS_PROMPT_TUBE));
    } else {
        // Extend from the end nearest to the click.
        int d0 = 0, d1 = 0;
        nearPoint(tube_.controls.front(), 100000, d0);
        nearPoint(tube_.controls.back(), 100000, d1);
        if (tube_.controls.size() > 1 && d0 < d1) {
            std::reverse(tube_.controls.begin(), tube_.controls.end());
            std::swap(tube_.taperStart, tube_.taperEnd);
        }
        Vec3 p;
        if (TubePoint(vpt, m, p)) tube_.controls.push_back(p);
    }
    UpdateTubeOverlay();
    Redraw(REDRAW_INTERACTIVE);
}

void SculptMode::TubeDrag(ViewExp& vpt, IPoint2 m) {
    switch (tubeAction_) {
        case TubeAction::Draw: {
            const float spacing = SculptSettings::Get().Value(Prop::TubeSpacing);
            const float dx = static_cast<float>(m.x - tubeLastPoint_.x), dy = static_cast<float>(m.y - tubeLastPoint_.y);
            Vec3 p;
            if (dx * dx + dy * dy >= spacing * spacing && TubePoint(vpt, m, p)) {
                tube_.controls.push_back(p);
                tubeLastPoint_ = m;
            }
            break;
        }
        case TubeAction::MovePoint: {
            if (tubeIndex_ < 0 || tubeIndex_ >= static_cast<int>(tube_.controls.size())) break;
            Vec3& point = tube_.controls[static_cast<std::size_t>(tubeIndex_)];
            sculpt::RayHit hit;
            Vec3 dir;
            if (!tube_.onPlane && Raycast(vpt, static_cast<float>(m.x), static_cast<float>(m.y), true, hit, &dir)) {
                point = hit.position + hit.geometricNormal * (SculptSettings::Get().Value(Prop::TubeSurfaceOffset) * TubeRadius());
            } else {
                Ray worldRay;
                vpt.MapScreenToWorldRay(static_cast<float>(m.x), static_cast<float>(m.y), worldRay);
                const Vec3 normal = sculpt::normalizedOrZero(ToVec3(worldToObject_.VectorTransform(worldRay.dir)));
                Vec3 p;
                if (ScreenToPlane(vpt, static_cast<float>(m.x), static_cast<float>(m.y), point, normal, p)) point = p;
            }
            break;
        }
        case TubeAction::Relax: {
            GraphicsWindow* gw = vpt.getGW();
            if (!gw || tube_.controls.size() < 3) break;
            gw->setTransform(objectToWorld_);
            const float radius = SculptSettings::Get().Size();
            std::vector<Vec3> next = tube_.controls;
            for (std::size_t i = 1; i + 1 < next.size(); ++i) {
                Point3 q = ToPoint3(tube_.controls[i]);
                IPoint3 s;
                if (gw->wTransPoint(&q, &s) != 0) continue;
                const float dx = static_cast<float>(s.x - m.x), dy = static_cast<float>(s.y - m.y);
                if (dx * dx + dy * dy > radius * radius) continue;
                const Vec3 mid = (tube_.controls[i - 1] + tube_.controls[i + 1]) * 0.5f;
                next[i] = sculpt::lerp(tube_.controls[i], mid, 0.3f);
            }
            tube_.controls.swap(next);
            break;
        }
        case TubeAction::Taper: {
            const float value = std::min(std::max(tubeTaperStart_ + static_cast<float>(m.x - press_.x) * 0.01f, 0.0f), 3.0f);
            (tubeIndex_ == 0 ? tube_.taperStart : tube_.taperEnd) = value;
            break;
        }
    }
    UpdateTubeOverlay();
    Redraw(REDRAW_INTERACTIVE);
}

bool SculptMode::SectionShape(std::vector<std::array<float, 2>>& section) const {
    Interface* core = GetCOREInterface();
    INode* node = sectionNode_ && core ? core->GetINodeByHandle(sectionNode_) : nullptr;
    if (!node) return false;
    const TimeValue t = core->GetTime();
    if (!IsClosedShape(node, t)) return false;
    ShapeObject* shape = static_cast<ShapeObject*>(node->EvalWorldState(t).obj);
    constexpr int kSamples = 48;
    std::vector<Point3> points;
    Box3 box;
    box.Init();
    for (int i = 0; i < kSamples; ++i) {
        const Point3 p = shape->InterpCurve3D(t, 0, static_cast<float>(i) / static_cast<float>(kSamples), PARAM_NORMALIZED);
        points.push_back(p);
        box += p;
    }
    const Point3 center = box.Center(), size = box.Width();
    const float half = 0.5f * std::max(size.x, size.y);
    if (!(half > 0.0f)) return false;
    section.clear();
    for (const Point3& p : points) section.push_back({(p.x - center.x) / half, (p.y - center.y) / half});
    return true;
}

bool SculptMode::BuildTube(sculpt::PolyData& poly, std::int32_t group) const {
    if (tube_.controls.size() < 2) return false;
    const SculptSettings& s = SculptSettings::Get();
    const float radius = TubeRadius();
    if (!(radius > 0.0f)) return false;
    sculpt::TubeSettings settings;
    settings.sides = s.Int(Prop::TubeSides);
    settings.radius = radius;
    settings.taperStart = tube_.taperStart;
    settings.taperEnd = tube_.taperEnd;
    std::vector<std::array<float, 2>> section;
    if (SectionShape(section)) settings.section = &section;
    const int sides = settings.section ? static_cast<int>(section.size()) : settings.sides;
    const std::vector<Vec3> curve =
        sculpt::sampleCurve(tube_.controls, std::max(radius * kTwoPi / static_cast<float>(std::max(sides, 3)), radius * 0.25f));
    sculpt::ProfileCurve profile;
    std::vector<float> param;
    if (s.Bool(Prop::ProfileUse) && s.Int(Prop::ProfileTarget) == 0) {
        if (!s.ProfileCurveText().empty()) profile.fromText(s.ProfileCurveText());
        settings.profile = &profile;
        const int mapping = s.Int(Prop::ProfileMapping);
        if (mapping >= 1) {  // Local X/Y/Z of the object instead of the curve length.
            auto coord = [mapping](const Vec3& p) { return mapping == 1 ? p.x : (mapping == 2 ? p.y : p.z); };
            float lo = sculpt::kInfinity, hi = -sculpt::kInfinity;
            for (const Vec3& p : curve) {
                lo = std::min(lo, coord(p));
                hi = std::max(hi, coord(p));
            }
            for (const Vec3& p : curve) param.push_back(hi > lo ? (coord(p) - lo) / (hi - lo) : 0.0f);
            settings.profileParam = &param;
        }
    }
    return sculpt::appendTube(poly, curve, settings, group);
}

void SculptMode::UpdateTubeOverlay() {
    if (!tube_.active) {
        overlay_.ClearWorld();
        return;
    }
    std::vector<Point3> markers;
    for (const Vec3& c : tube_.controls) markers.push_back(objectToWorld_.PointTransform(ToPoint3(c)));
    std::vector<Point3> segments;
    sculpt::PolyData preview;
    if (BuildTube(preview, 0)) {
        const std::vector<std::uint32_t> starts = preview.faceStarts();
        for (std::size_t f = 0; f < preview.faceSizes.size(); ++f) {
            const std::uint32_t n = preview.faceSizes[f];
            if (n != 4) continue;  // Side quads only; the caps add clutter.
            for (std::uint32_t k = 0; k < 2; ++k) {  // Two edges per quad cover the whole wireframe.
                segments.push_back(objectToWorld_.PointTransform(ToPoint3(preview.positions[preview.faceVerts[starts[f] + k]])));
                segments.push_back(
                    objectToWorld_.PointTransform(ToPoint3(preview.positions[preview.faceVerts[starts[f] + k + 1]])));
            }
        }
    }
    overlay_.SetWorld(std::move(segments), std::move(markers), kTubeColor);
}

bool SculptMode::CommitTube() {
    if (!tube_.active || !object_) return false;
    if (tube_.controls.size() < 2) {
        CancelPending();
        return false;
    }
    MSTR error;
    const bool ok = object_->RunTopologyEdit(
        [this](sculpt::PolyData& poly, MSTR& e) {
            if (BuildTube(poly, NextGroup(poly))) return true;
            e = GetString(IDS_UNDO_TUBE);
            return false;
        },
        IDS_UNDO_TUBE, error);
    tube_ = TubeState();
    overlay_.ClearWorld();
    if (!ok && ip_ && error.Length() > 0) ip_->ReplacePrompt(error.data());
    Redraw(REDRAW_NORMAL);
    return ok;
}

// --- Profile on the active SculptGroup ------------------------------------------------------------------

bool SculptMode::ApplyProfileToGroup(MSTR& error) {
    SculptMeshObject* object = SculptMeshObject::EditedObject();
    if (!object) return false;
    if (activeGroup_ < 0) {
        error = GetString(IDS_ERR_NO_GROUP);
        return false;
    }
    const SculptSettings& s = SculptSettings::Get();
    sculpt::ProfileCurve profile;
    if (!s.ProfileCurveText().empty()) profile.fromText(s.ProfileCurveText());
    const int mapping = s.Int(Prop::ProfileMapping);
    const int axis = mapping >= 1 ? mapping - 1 : 2;  // Curve Length means Local Z for a group.
    const std::int32_t group = activeGroup_;
    return object->RunOperation(
        [group, axis, profile](SculptSessionBridge& bridge) {
            sculpt::SculptSession& session = bridge.Session();
            std::vector<std::uint32_t> vertices;
            std::vector<Vec3> targets;
            sculpt::groupProfileTargets(session.mesh(), session.faceGroups(), group, axis, profile, vertices, targets);
            session.beginStroke();
            session.applyTargets(vertices, targets);
            return session.endStroke();
        },
        GetString(IDS_UNDO_PROFILE));
}
