// Viewport sculpting: the command mode that turns mouse drags into strokes,
// and the brush circle drawn under the cursor.
#pragma once

#include <vector>

#include <sceneapi.h>

#include "SculptMeshPlugin.h"
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

// Draws the brush circle in the viewport under the mouse.
class SculptBrushCursor : public ViewportDisplayCallback {
public:
    void Display(TimeValue t, ViewExp* vpt, int flags) override;
    void GetViewportRect(TimeValue t, ViewExp* vpt, Rect* rect) override;
    BOOL Foreground() override { return TRUE; }

    void SetScreen(HWND hwnd, IPoint2 position) {
        hwnd_ = hwnd;
        screen_ = position;
        visible_ = true;
    }
    // World-space point that fixes the depth of the circle.
    void SetAnchor(const Point3& world) {
        anchor_ = world;
        hasAnchor_ = true;
    }
    void SetInverted(bool inverted) { inverted_ = inverted; }
    void Hide() { visible_ = false; }

private:
    HWND hwnd_ = nullptr;
    IPoint2 screen_{0, 0};
    Point3 anchor_{0.0f, 0.0f, 0.0f};
    bool hasAnchor_ = false;
    bool visible_ = false;
    bool inverted_ = false;
};

class SculptMode : public CommandMode, public MouseCallBack {
public:
    static SculptMode& Get();

    // Starts sculpting `object` (the object shown in the Modify panel).
    bool Start(IObjParam* ip, SculptMeshObject* object);
    // Leaves sculpt mode; an unfinished stroke is cancelled.
    void Stop();
    void Toggle(IObjParam* ip, SculptMeshObject* object);
    bool IsActive() const { return active_; }
    SculptMeshObject* Target() const { return object_; }

    // Called from the object's destructor: drop every pointer to it without
    // touching the UI (which may already be gone).
    void ForgetObject(SculptMeshObject* object);

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
    SculptMode() = default;

    bool ResolveTargetNode(INode*& node) const;
    void BeginStroke(HWND hwnd, IPoint2 m, int flags);
    void ContinueStroke(HWND hwnd, IPoint2 m);
    void EndStroke();
    void CancelStroke();
    void ApplyPendingSamples(HWND hwnd);
    bool ApplyDab(ViewExp& vpt, SculptSessionBridge& bridge, const sculpt::StrokeSample& sample);
    void UpdateCursor(HWND hwnd, IPoint2 m, bool redraw);
    bool CaptureTransform(INode* node, TimeValue t);
    void Redraw(DWORD flags);

    IObjParam* ip_ = nullptr;
    SculptMeshObject* object_ = nullptr;
    SculptForegroundCallback foreground_;
    SculptBrushCursor cursor_;
    bool active_ = false;           // Between EnterMode and ExitMode.
    bool cursorRegistered_ = false;

    // Stroke state.
    bool strokeActive_ = false;
    bool holdOpen_ = false;
    sculpt::BrushSettings strokeBrush_;
    bool strokeInvert_ = false;
    sculpt::StrokeSpacer spacer_;
    std::vector<sculpt::StrokeSample> samples_;
    Matrix3 objectToWorld_;
    Matrix3 worldToObject_;
    float objectScale_ = 1.0f;
};
