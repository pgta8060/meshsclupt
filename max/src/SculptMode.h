// Viewport sculpting: the command mode that turns mouse input into strokes,
// mask gestures and SculptGroup gestures, and the overlay drawn under the
// cursor (brush circle, mask rectangle/lasso).
#pragma once

#include <memory>
#include <vector>

#include <sceneapi.h>

#include "SculptMeshPlugin.h"
#include "SculptSettings.h"
#include "sculpt/alpha.h"
#include "sculpt/brush.h"
#include "sculpt/stroke.h"

class SculptMeshObject;
class SculptSessionBridge;

// Puts the sculpted node in the viewport foreground during strokes.
class SculptForegroundCallback : public ChangeForegroundCallback {
public:
    void SetObject(ReferenceTarget* object) { object_ = object; }
    BOOL IsValid() override { return valid_; }
    void Invalidate() override { valid_ = FALSE; }
    void Validate() override { valid_ = TRUE; }
    void callback(TimeValue t, IScene* scene) override;

private:
    ReferenceTarget* object_ = nullptr;
    BOOL valid_ = TRUE;
};

// Draws the brush circle or the mask selection shape over the viewport.
// Screen positions are drawn on a camera-facing plane through an anchor
// point, so the overlay works in perspective and orthographic views.
class SculptOverlay : public ViewportDisplayCallback {
public:
    void Display(TimeValue t, ViewExp* vpt, int flags) override;
    void GetViewportRect(TimeValue t, ViewExp* vpt, Rect* rect) override;
    BOOL Foreground() override { return TRUE; }

    void SetViewport(HWND hwnd) { hwnd_ = hwnd; }
    void SetAnchor(const Point3& world) { anchor_ = world; }
    void ShowCircle(IPoint2 center, float radiusPx, bool falloffRing, const Point3& color);
    void ShowShape(const std::vector<IPoint2>& points, bool closed, const Point3& color);
    void Hide() { kind_ = Kind::None; }

private:
    enum class Kind { None, Circle, Shape };
    HWND hwnd_ = nullptr;
    Kind kind_ = Kind::None;
    Point3 anchor_{0.0f, 0.0f, 0.0f};
    IPoint2 center_{0, 0};
    float radius_ = 0.0f;
    bool falloffRing_ = false;
    bool closed_ = false;
    std::vector<IPoint2> points_;
    Point3 color_{1.0f, 0.2f, 0.2f};
};

class SculptMode : public CommandMode, public MouseCallBack {
public:
    static SculptMode& Get();

    // Starts sculpting `object` (the object shown in the Modify panel).
    bool Start(IObjParam* ip, SculptMeshObject* object);
    // Leaves sculpt mode; an unfinished gesture is cancelled.
    void Stop();
    bool IsActive() const { return active_; }
    bool StrokeActive() const { return gesture_ != Gesture::None; }
    SculptMeshObject* Target() const { return object_; }

    // Called from the object's destructor: drop every pointer to it without
    // touching the UI (which may already be gone).
    void ForgetObject(SculptMeshObject* object);

    // Redraws the cursor after the brush size or tool changed elsewhere.
    void RefreshCursor();

    // --- CommandMode ----------------------------------------------------------
    int Class() override { return MODIFY_COMMAND; }
    int ID() override { return SCULPTMESH_CID_SCULPT_MODE; }
    MouseCallBack* MouseProc(int* numPoints) override {
        *numPoints = 2;  // Button down (0) and button up (1).
        return this;
    }
    ChangeForegroundCallback* ChangeFGProc() override { return &foreground_; }
    BOOL ChangeFG(CommandMode* oldMode) override { return oldMode->ChangeFGProc() != &foreground_; }
    void EnterMode() override;
    void ExitMode() override;

    // --- MouseCallBack --------------------------------------------------------
    int proc(HWND hwnd, int msg, int point, int flags, IPoint2 m) override;

private:
    enum class Gesture {
        None,
        Stroke,      // Draw / Scatter / Stamp dabs with a sculpt brush or Paint Mask.
        Grab,        // Move and Snake Hook.
        DragDab,     // Drag stroke mode: size and orient one dab.
        Shape,       // Rectangle / Lasso mask.
        EmptyMask,   // Paint Mask started in empty space.
        Visibility,  // Ctrl+Shift+Alt SculptGroup visibility.
        Resize,      // Ctrl+Shift horizontal drag.
        Chord,       // Ctrl+Alt: straight line on click, mask erase on drag.
        Consumed,    // Already handled (double-click blur); wait for release.
    };

    SculptMode() = default;

    bool ResolveTargetNode(INode*& node) const;
    bool CaptureTransform(INode* node, TimeValue t);
    bool PrepareSession(HWND hwnd);

    void Press(HWND hwnd, IPoint2 m, int flags, bool doubleClick);
    void Drag(HWND hwnd, IPoint2 m, int flags);
    void Release(HWND hwnd, IPoint2 m, int flags);
    void Cancel();

    // Stroke helpers.
    void BeginStroke(HWND hwnd, IPoint2 m, const sculpt::BrushSettings& brush, bool invert, bool deferFirstDab,
                     bool forceDraw);
    void ContinueStroke(HWND hwnd, IPoint2 m);
    void FinishStroke(const MCHAR* undoName);
    void FeedPointer(ViewExp& vpt, const sculpt::StrokeSample& pointer);
    void ApplySpacedSamples(ViewExp& vpt);
    bool ApplySample(ViewExp& vpt, const sculpt::StrokeSample& sample, float sizeScale, float amount);
    void ApplyGrab(ViewExp& vpt, IPoint2 m);
    void ApplyLine(HWND hwnd, IPoint2 m);
    void MaskBlurAt(HWND hwnd, IPoint2 m);
    void ApplyShape(HWND hwnd, bool clickOnly);
    void ApplyEmptyMask(bool dragged, bool erase);
    void ApplyVisibility(bool dragged);

    // Geometry helpers (object space unless noted).
    bool Raycast(ViewExp& vpt, float x, float y, bool cull, sculpt::RayHit& hit, sculpt::Vec3* rayDir = nullptr) const;
    bool NearestVertexInBrush(ViewExp& vpt, IPoint2 m, sculpt::Vec3& out) const;
    float WorldPerPixel(ViewExp& vpt, const Point3& world) const;
    bool ScreenToPlane(ViewExp& vpt, float x, float y, const sculpt::Vec3& point, const sculpt::Vec3& normal,
                       sculpt::Vec3& out) const;
    sculpt::Vec3 ScreenVector(ViewExp& vpt, float x, float y, float dx, float dy, const sculpt::Vec3& at) const;

    void UpdateOverlay(HWND hwnd, IPoint2 m, int flags);
    void Redraw(DWORD flags);
    void ApplyStrokeSymmetry();

    IObjParam* ip_ = nullptr;
    SculptMeshObject* object_ = nullptr;
    SculptForegroundCallback foreground_;
    SculptOverlay overlay_;
    bool active_ = false;
    bool overlayRegistered_ = false;

    // Node transform captured at the start of a gesture.
    Matrix3 objectToWorld_;
    Matrix3 worldToObject_;
    float objectScale_ = 1.0f;

    // Current gesture.
    Gesture gesture_ = Gesture::None;
    HWND gestureHwnd_ = nullptr;
    IPoint2 press_{0, 0};
    IPoint2 last_{0, 0};
    bool moved_ = false;
    bool pressHit_ = false;
    sculpt::RayHit pressHitInfo_;
    bool eraseGesture_ = false;
    bool chordMaskDirect_ = false;

    // Stroke state.
    sculpt::BrushSettings strokeBrush_;
    std::shared_ptr<const sculpt::Alpha> strokeAlpha_;
    bool strokeInvert_ = false;
    bool deferFirstDab_ = false;
    bool strokeChanged_ = false;
    StrokeMode strokeMode_ = StrokeMode::Draw;
    sculpt::StrokeSpacer spacer_;
    sculpt::LazyMouse lazy_;
    bool lazyOn_ = false;
    sculpt::Scatter scatter_;
    std::vector<sculpt::StrokeSample> samples_;
    std::vector<sculpt::ScatterDab> scatterDabs_;
    float lastDirX_ = 0.0f, lastDirY_ = -1.0f;  // Screen direction of travel (alpha orientation).
    bool orientToDrag_ = false;
    sculpt::StrokeSample lastSample_;
    bool haveLastSample_ = false;
    sculpt::Vec3 lastDabCenter_;
    bool haveLastDab_ = false;

    // Move / Snake Hook.
    sculpt::Vec3 grabPoint_;
    sculpt::Vec3 grabNormal_;
    sculpt::Vec3 grabCenter_;

    // Drag stroke mode.
    float dragRadiusPx_ = 0.0f;

    // Rectangle / Lasso.
    bool shapeLasso_ = false;
    std::vector<IPoint2> shape_;

    // Ctrl+Shift resize.
    float resizeStart_ = 0.0f;

    // Ctrl+Alt straight-line anchor: end of the previous compatible stroke.
    bool anchorValid_ = false;
    sculpt::Vec3 anchor_;
    sculpt::BrushType anchorBrush_ = sculpt::BrushType::Sculpt;

    // Double-click detection (Paint Mask blur).
    DWORD lastClickTime_ = 0;
    IPoint2 lastClickPos_{-10000, -10000};
};
