// The "Sculpt Mesh" geometry object. It is a PolyObject, so 3ds Max displays,
// renders, snaps and converts it exactly like an editable poly base object;
// the sculpt session is a runtime-only acceleration structure on top.
#pragma once

#include <memory>
#include <vector>

#include "SculptMeshPlugin.h"
#include "SculptDisplay.h"
#include "SculptSessionBridge.h"

class SculptMeshObject : public PolyObject {
public:
    // Version written into every saved object. Bump when the format changes
    // and keep loading every older version.
    static constexpr DWORD kFileVersion = 1;

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

    // Geometry edits that bypass the sculpt session (e.g. from other tools)
    // mark the session stale so it re-reads positions before the next stroke.
    void SetPoint(int i, const Point3& p) override;
    void PointsWereChanged() override;
    void Deform(Deformer* defProc, int useSel = 0) override;

    // --- Display (fast chunked display while sculpting) ----------------------
    unsigned long GetObjectDisplayRequirement() const override;
    bool PrepareDisplay(const MaxSDK::Graphics::UpdateDisplayContext& prepareDisplayContext) override;
    bool UpdatePerNodeItems(const MaxSDK::Graphics::UpdateDisplayContext& updateDisplayContext,
                            MaxSDK::Graphics::UpdateNodeContext& nodeContext,
                            MaxSDK::Graphics::IRenderItemContainer& targetRenderItemContainer) override;
    void GetLocalBoundBox(TimeValue t, INode* inode, ViewExp* vpt, Box3& box) override;
    void GetWorldBoundBox(TimeValue t, INode* inode, ViewExp* vpt, Box3& box) override;
    void GetDeformBBox(TimeValue t, Box3& box, Matrix3* tm = nullptr, BOOL useSel = FALSE) override;

    // Switches between the fast sculpt display (clay material, partial GPU
    // updates) and the regular PolyObject display. Needs a session to enable.
    void SetFastDisplay(bool on);
    bool FastDisplay() const { return fastDisplay_ && display_ && bridge_; }

    // --- Sculpting ------------------------------------------------------------
    // Returns the sculpt session, (re)building or re-syncing it as needed.
    // Returns nullptr and fills `error` if the mesh cannot be sculpted.
    SculptSessionBridge* AcquireSession(MSTR& error);
    // Frees the session (it is rebuilt on demand).
    void ReleaseSession();
    // The current session, or nullptr (does not build one).
    SculptSessionBridge* Bridge() { return bridge_.get(); }

    // Copies vertices changed by the session into the MNMesh and notifies
    // 3ds Max that the geometry changed.
    void CommitSessionChanges();

    // Undo/redo entry point: writes positions (MNMesh indices) to the mesh
    // and the session, then notifies dependents.
    void ApplyPositions(const std::vector<int>& maxIndices, const std::vector<Point3>& positions);

    // Invalidates geometry caches and tells dependents/viewports.
    void GeometryChanged();

    // Called by the converter after `mm` was filled.
    void InitFromPoly(const PolyObject& source);

    // The object currently shown in the Modify panel (nullptr if none).
    static SculptMeshObject* EditedObject() { return editedObject_; }
    static IObjParam* EditInterface() { return editInterface_; }

private:
    Box3 SessionBounds() const;

    std::unique_ptr<SculptSessionBridge> bridge_;
    bool sessionStale_ = false;
    std::unique_ptr<SculptDisplay> display_;
    bool fastDisplay_ = false;

    static SculptMeshObject* editedObject_;
    static IObjParam* editInterface_;
};

// Undo record for one sculpt stroke (vertex positions before/after).
class SculptStrokeRestore : public RestoreObj {
public:
    SculptStrokeRestore(SculptMeshObject* object, std::vector<int> vertices, std::vector<Point3> before,
                        std::vector<Point3> after);
    void Restore(int isUndo) override;
    void Redo() override;
    int Size() override;
    MSTR Description() override;

private:
    SculptMeshObject* object_;
    std::vector<int> vertices_;
    std::vector<Point3> before_;
    std::vector<Point3> after_;
};
