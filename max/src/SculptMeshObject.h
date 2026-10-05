// The "Sculpt Mesh" geometry object. It is a PolyObject, so 3ds Max displays,
// renders, snaps and converts it exactly like an editable poly base object.
// On top it stores the sculpt mask, SculptGroups and polygon visibility, and
// owns the runtime sculpt session and fast display while sculpting.
#pragma once

#include <functional>
#include <memory>
#include <vector>

#include "SculptDisplay.h"
#include "SculptMeshPlugin.h"
#include "SculptSessionBridge.h"

class MoveModBoxCMode;
class RotateModBoxCMode;
class UScaleModBoxCMode;
class NUScaleModBoxCMode;
class SquashModBoxCMode;

class SculptMeshObject : public PolyObject {
public:
    // Version written into every saved object. Bump when the format changes
    // and keep loading every older version.
    //   1: PolyObject data.  2: + mask, SculptGroups, hidden polygons.
    static constexpr DWORD kFileVersion = 2;

    SculptMeshObject();
    ~SculptMeshObject() override;

    // --- Animatable / ReferenceTarget ---------------------------------------
    Class_ID ClassID() override { return SCULPTMESH_CLASS_ID; }
    void GetClassName(MSTR& s, bool localized = true) const override;
    BOOL IsSubClassOf(Class_ID classID) override;
    RefTargetHandle Clone(RemapDir& remap) override;
    void DeleteThis() override { delete this; }

    using PolyObject::Load;
    using PolyObject::Save;
    IOResult Save(ISave* isave) override;
    IOResult Load(ILoad* iload) override;

    // --- BaseObject / Object -----------------------------------------------
    const MCHAR* GetObjectName(bool localized = true) const override;
    void InitNodeName(MSTR& s) override;
    void BeginEditParams(IObjParam* ip, ULONG flags, Animatable* prev) override;
    void EndEditParams(IObjParam* ip, ULONG flags, Animatable* next) override;

    // Geometry edits that bypass the sculpt session mark it stale so it
    // re-reads positions before the next stroke.
    void SetPoint(int i, const Point3& p) override;
    void PointsWereChanged() override;
    void Deform(Deformer* defProc, int useSel = 0) override;

    // --- Display ------------------------------------------------------------------
    unsigned long GetObjectDisplayRequirement() const override;
    bool PrepareDisplay(const MaxSDK::Graphics::UpdateDisplayContext& prepareDisplayContext) override;
    bool UpdatePerNodeItems(const MaxSDK::Graphics::UpdateDisplayContext& updateDisplayContext,
                            MaxSDK::Graphics::UpdateNodeContext& nodeContext,
                            MaxSDK::Graphics::IRenderItemContainer& targetRenderItemContainer) override;
    void GetLocalBoundBox(TimeValue t, INode* inode, ViewExp* vpt, Box3& box) override;
    void GetWorldBoundBox(TimeValue t, INode* inode, ViewExp* vpt, Box3& box) override;
    void GetDeformBBox(TimeValue t, Box3& box, Matrix3* tm = nullptr, BOOL useSel = FALSE) override;

    // --- Sub-object "Transform" level: W/E/R move the unmasked region --------------
    int NumSubObjTypes() override { return 1; }
    ISubObjType* GetSubObjType(int i) override;
    void ActivateSubobjSel(int level, XFormModes& modes) override;
    int HitTest(TimeValue t, INode* inode, int type, int crossing, int flags, IPoint2* p, ViewExp* vpt,
                ModContext* mc) override;
    using PolyObject::HitTest;
    void SelectSubComponent(HitRecord* /*hitRec*/, BOOL /*selected*/, BOOL /*all*/, BOOL /*invert*/ = FALSE) override {}
    void ClearSelection(int /*selLevel*/) override {}
    void GetSubObjectCenters(SubObjAxisCallback* cb, TimeValue t, INode* node, ModContext* mc) override;
    void GetSubObjectTMs(SubObjAxisCallback* cb, TimeValue t, INode* node, ModContext* mc) override;
    void Move(TimeValue t, Matrix3& partm, Matrix3& tmAxis, Point3& val, BOOL localOrigin = FALSE) override;
    void Rotate(TimeValue t, Matrix3& partm, Matrix3& tmAxis, Quat& val, BOOL localOrigin = FALSE) override;
    void Scale(TimeValue t, Matrix3& partm, Matrix3& tmAxis, Point3& val, BOOL localOrigin = FALSE) override;
    void TransformStart(TimeValue t) override;
    void TransformHoldingFinish(TimeValue t) override;
    void TransformFinish(TimeValue t) override;
    void TransformCancel(TimeValue t) override;
    int SubObjectLevel() const { return subLevel_; }

    // --- Sculpting ------------------------------------------------------------------
    // Returns the sculpt session, (re)building or re-syncing it as needed.
    // Returns nullptr and fills `error` if the mesh cannot be sculpted.
    SculptSessionBridge* AcquireSession(MSTR& error);
    void ReleaseSession();
    SculptSessionBridge* Bridge() { return bridge_.get(); }

    // Copies everything the session changed into the MNMesh/attributes,
    // refreshes the display and notifies 3ds Max. While `interactive` (during
    // a stroke) display rebuilds for SculptGroup/visibility changes are
    // throttled; the final commit of the stroke catches up.
    void CommitSessionChanges(bool interactive = false);

    // Runs a whole-mesh operation on the session as one undo step.
    // Returns false if there is no session or nothing changed.
    bool RunOperation(const std::function<sculpt::StrokeDelta(SculptSessionBridge&)>& op, const MCHAR* undoName);

    // Puts the undo record for a finished stroke (core indices) into the
    // currently open hold (the caller owns Begin/Accept).
    void PutStrokeUndo(const sculpt::StrokeDelta& coreDelta, const MCHAR* undoName);

    // Undo/redo entry point: `maxDelta` uses MNMesh indices.
    void ApplyDelta(const sculpt::StrokeDelta& maxDelta, bool useBefore);

    // Display switches.
    void SetFastDisplay(bool on);
    bool FastDisplay() const { return fastDisplay_ && display_ && bridge_; }
    void RefreshDisplayOptions();

    // Invalidates geometry caches and tells dependents/viewports.
    void GeometryChanged();

    // Called by the converter after `mm` was filled.
    void InitFromPoly(const PolyObject& source);

    const SculptAttributes& Attributes() const { return attributes_; }

    // The object currently shown in the Modify panel (nullptr if none).
    static SculptMeshObject* EditedObject() { return editedObject_; }
    static IObjParam* EditInterface() { return editInterface_; }

private:
    Box3 SessionBounds() const;
    void Transform(TimeValue t, Matrix3& partm, Matrix3& tmAxis, const Matrix3& xfrm);
    float TransformWeight(int v) const;
    SculptDisplay::Options DisplayOptions() const;

    SculptAttributes attributes_;
    std::unique_ptr<SculptSessionBridge> bridge_;
    bool sessionStale_ = false;
    std::unique_ptr<SculptDisplay> display_;
    bool fastDisplay_ = false;
    bool displayRebuildPending_ = false;
    DWORD lastDisplayRebuild_ = 0;

    int subLevel_ = 0;
    bool xformActive_ = false;
    std::vector<Point3> xformOrigin_;

    static SculptMeshObject* editedObject_;
    static IObjParam* editInterface_;
    static MoveModBoxCMode* moveMode_;
    static RotateModBoxCMode* rotateMode_;
    static UScaleModBoxCMode* uscaleMode_;
    static NUScaleModBoxCMode* nuscaleMode_;
    static SquashModBoxCMode* squashMode_;
};

// Undo record for any sculpt change (MNMesh indices).
class SculptDeltaRestore : public RestoreObj {
public:
    SculptDeltaRestore(SculptMeshObject* object, sculpt::StrokeDelta delta, MSTR name);
    void Restore(int isUndo) override;
    void Redo() override;
    int Size() override;
    MSTR Description() override { return name_; }

private:
    SculptMeshObject* object_;
    sculpt::StrokeDelta delta_;
    MSTR name_;
};
