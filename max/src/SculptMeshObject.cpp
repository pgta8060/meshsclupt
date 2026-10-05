#include "SculptMeshObject.h"

#include <algorithm>

#include <MNNormalSpec.h>

#include "SculptMode.h"
#include "SculptPanel.h"

namespace {

// Chunk IDs inside a Sculpt Mesh object. Append new IDs; never reuse.
constexpr USHORT kVersionChunk = 0x5C00;
constexpr USHORT kPolyDataChunk = 0x5C10;

}  // namespace

SculptMeshObject* SculptMeshObject::editedObject_ = nullptr;
IObjParam* SculptMeshObject::editInterface_ = nullptr;

// --- Class descriptor -------------------------------------------------------------

class SculptMeshObjectClassDesc : public ClassDesc2 {
public:
    int IsPublic() override { return FALSE; }  // Created by conversion, not from the Create panel.
    void* Create(BOOL /*loading*/) override { return new SculptMeshObject(); }
    const TCHAR* ClassName() override { return GetString(IDS_CLASS_NAME); }
    const TCHAR* NonLocalizedClassName() override { return _T("Sculpt Mesh"); }
    SClass_ID SuperClassID() override { return GEOMOBJECT_CLASS_ID; }
    Class_ID ClassID() override { return SCULPTMESH_CLASS_ID; }
    const TCHAR* Category() override { return GetString(IDS_CATEGORY); }
    const TCHAR* InternalName() override { return _T("SculptMeshObject"); }
    HINSTANCE HInstance() override { return hInstance; }
};

ClassDesc2* GetSculptMeshObjectDesc() {
    static SculptMeshObjectClassDesc desc;
    return &desc;
}

// --- SculptMeshObject ---------------------------------------------------------------

SculptMeshObject::SculptMeshObject() = default;

SculptMeshObject::~SculptMeshObject() {
    SculptMode::Get().ForgetObject(this);
    if (editedObject_ == this) {
        editedObject_ = nullptr;
        editInterface_ = nullptr;
    }
}

void SculptMeshObject::GetClassName(MSTR& s, bool localized) const {
    s = localized ? GetString(IDS_CLASS_NAME) : _T("Sculpt Mesh");
}

BOOL SculptMeshObject::IsSubClassOf(Class_ID classID) {
    // A Sculpt Mesh is a PolyObject, so tools that accept poly objects accept it.
    return classID == ClassID() || classID == polyObjectClassID;
}

const MCHAR* SculptMeshObject::GetObjectName(bool localized) const {
    return localized ? GetString(IDS_OBJECT_NAME) : _T("Sculpt Mesh");
}

void SculptMeshObject::InitNodeName(MSTR& s) { s = _T("SculptMesh"); }

void SculptMeshObject::InitFromPoly(const PolyObject& source) {
    SetDisplacementDisable(source.GetDisplacementDisable());
    SetDisplacementSplit(source.GetDisplacementSplit());
    SetDisplacementParameters(source.GetDisplacementParameters());
    SetDisplacement(source.GetDisplacement());
}

RefTargetHandle SculptMeshObject::Clone(RemapDir& remap) {
    SculptMeshObject* copy = new SculptMeshObject();
    copy->mm = mm;
    copy->InitFromPoly(*this);
    BaseClone(this, copy, remap);
    return copy;
}

IOResult SculptMeshObject::Save(ISave* isave) {
    ULONG written = 0;
    DWORD version = kFileVersion;
    isave->BeginChunk(kVersionChunk);
    IOResult res = isave->Write(&version, sizeof(version), &written);
    isave->EndChunk();
    if (res != IO_OK) return res;

    // The PolyObject data (geometry, UVs, materials, ...) lives in its own
    // container chunk so future versions can add sibling chunks safely.
    isave->BeginChunk(kPolyDataChunk);
    res = PolyObject::Save(isave);
    isave->EndChunk();
    return res;
}

IOResult SculptMeshObject::Load(ILoad* iload) {
    ULONG read = 0;
    DWORD version = 0;
    IOResult res = IO_OK;
    while ((res = iload->OpenChunk()) == IO_OK) {
        switch (iload->CurChunkID()) {
            case kVersionChunk:
                res = iload->Read(&version, sizeof(version), &read);
                break;
            case kPolyDataChunk:
                res = PolyObject::Load(iload);
                break;
            default:
                break;  // Chunk from a newer version: skip it, keep the rest.
        }
        iload->CloseChunk();
        if (res != IO_OK) return res;
    }
    ReleaseSession();
    return IO_OK;
}

void SculptMeshObject::BeginEditParams(IObjParam* ip, ULONG flags, Animatable* prev) {
    PolyObject::BeginEditParams(ip, flags, prev);
    editedObject_ = this;
    editInterface_ = ip;
    SculptPanel::Open(ip, this);
}

void SculptMeshObject::EndEditParams(IObjParam* ip, ULONG flags, Animatable* next) {
    if (SculptMode::Get().Target() == this) SculptMode::Get().Stop();
    SculptPanel::Close(ip);
    if (editedObject_ == this) {
        editedObject_ = nullptr;
        editInterface_ = nullptr;
    }
    ReleaseSession();  // Frees memory; rebuilt on the next sculpt.
    PolyObject::EndEditParams(ip, flags, next);
}

void SculptMeshObject::SetPoint(int i, const Point3& p) {
    PolyObject::SetPoint(i, p);
    sessionStale_ = true;
}

void SculptMeshObject::PointsWereChanged() {
    PolyObject::PointsWereChanged();
    sessionStale_ = true;
}

void SculptMeshObject::Deform(Deformer* defProc, int useSel) {
    PolyObject::Deform(defProc, useSel);
    sessionStale_ = true;
}

SculptSessionBridge* SculptMeshObject::AcquireSession(MSTR& error) {
    if (bridge_ && !bridge_->Matches(mm)) bridge_.reset();  // Topology changed underneath.
    if (bridge_ && sessionStale_ && !bridge_->Session().strokeActive()) {
        if (!bridge_->SyncFromMesh(mm)) bridge_.reset();
        sessionStale_ = false;
    }
    if (!bridge_) {
        auto bridge = std::make_unique<SculptSessionBridge>();
        if (!bridge->Build(mm, error)) return nullptr;
        bridge_ = std::move(bridge);
        sessionStale_ = false;
        if (display_) display_->Build(bridge_->Session().mesh());  // New topology: new chunks.
    }
    return bridge_.get();
}

void SculptMeshObject::ReleaseSession() {
    if (fastDisplay_) SetFastDisplay(false);
    bridge_.reset();
    sessionStale_ = false;
}

void SculptMeshObject::SetFastDisplay(bool on) {
    if (on && bridge_) {
        if (!display_) display_ = std::make_unique<SculptDisplay>();
        display_->Build(bridge_->Session().mesh());
        bridge_->Session().clearDisplayDirty();
        fastDisplay_ = true;
    } else {
        display_.reset();
        fastDisplay_ = false;
        mm.InvalidateGeomCache();  // The PolyObject display rebuilds from mm.
    }
    NotifyDependents(FOREVER, PART_GEOM | PART_DISPLAY, REFMSG_CHANGE);
}

unsigned long SculptMeshObject::GetObjectDisplayRequirement() const {
    if (fastDisplay_ && display_ && bridge_) return 0;  // Nitrous per-node items.
    return PolyObject::GetObjectDisplayRequirement();
}

bool SculptMeshObject::PrepareDisplay(const MaxSDK::Graphics::UpdateDisplayContext& prepareDisplayContext) {
    if (FastDisplay()) {
        display_->Upload(bridge_->Session().mesh());
        return true;
    }
    return PolyObject::PrepareDisplay(prepareDisplayContext);
}

bool SculptMeshObject::UpdatePerNodeItems(const MaxSDK::Graphics::UpdateDisplayContext& updateDisplayContext,
                                          MaxSDK::Graphics::UpdateNodeContext& nodeContext,
                                          MaxSDK::Graphics::IRenderItemContainer& targetRenderItemContainer) {
    if (FastDisplay()) {
        display_->AddRenderItems(nodeContext, targetRenderItemContainer);
        return true;
    }
    return PolyObject::UpdatePerNodeItems(updateDisplayContext, nodeContext, targetRenderItemContainer);
}

Box3 SculptMeshObject::SessionBounds() const {
    const sculpt::Aabb b = bridge_->Session().bvh().rootBounds();
    Box3 box;
    box.Init();
    if (!b.empty()) box += Box3(ToPoint3(b.lo), ToPoint3(b.hi));
    return box;
}

void SculptMeshObject::GetLocalBoundBox(TimeValue t, INode* inode, ViewExp* vpt, Box3& box) {
    if (FastDisplay()) {
        box = SessionBounds();
        return;
    }
    PolyObject::GetLocalBoundBox(t, inode, vpt, box);
}

void SculptMeshObject::GetWorldBoundBox(TimeValue t, INode* inode, ViewExp* vpt, Box3& box) {
    if (FastDisplay() && inode) {
        box = SessionBounds() * inode->GetObjectTM(t);
        return;
    }
    PolyObject::GetWorldBoundBox(t, inode, vpt, box);
}

void SculptMeshObject::GetDeformBBox(TimeValue t, Box3& box, Matrix3* tm, BOOL useSel) {
    if (FastDisplay() && !useSel) {
        box = tm ? SessionBounds() * (*tm) : SessionBounds();
        return;
    }
    PolyObject::GetDeformBBox(t, box, tm, useSel);
}

void SculptMeshObject::CommitSessionChanges() {
    if (!bridge_) return;
    if (bridge_->PushDirty(mm) == 0) return;
    GeometryChanged();
}

void SculptMeshObject::ApplyPositions(const std::vector<int>& maxIndices, const std::vector<Point3>& positions) {
    const std::size_t count = std::min(maxIndices.size(), positions.size());
    for (std::size_t i = 0; i < count; ++i) {
        const int v = maxIndices[i];
        if (v >= 0 && v < mm.numv) mm.v[v].p = positions[i];
    }
    if (bridge_ && !bridge_->ApplyMaxPositions(maxIndices, positions)) sessionStale_ = true;
    GeometryChanged();
}

void SculptMeshObject::GeometryChanged() {
    if (bridge_) {
        sculpt::SculptSession& session = bridge_->Session();
        if (display_) {
            if (session.displayAllDirty())
                display_->MarkAll();
            else
                display_->MarkVertices(session.displayDirtyVertices());
        }
        session.clearDisplayDirty();
    }
    if (MNNormalSpec* normals = mm.GetSpecifiedNormals()) normals->ClearFlag(MNNORMAL_NORMALS_COMPUTED);
    mm.InvalidateGeomCache();
    NotifyDependents(FOREVER, PART_GEOM, REFMSG_CHANGE);
}

// --- Undo ---------------------------------------------------------------------------

SculptStrokeRestore::SculptStrokeRestore(SculptMeshObject* object, std::vector<int> vertices,
                                         std::vector<Point3> before, std::vector<Point3> after)
    : object_(object), vertices_(std::move(vertices)), before_(std::move(before)), after_(std::move(after)) {}

void SculptStrokeRestore::Restore(int /*isUndo*/) {
    if (object_) object_->ApplyPositions(vertices_, before_);
}

void SculptStrokeRestore::Redo() {
    if (object_) object_->ApplyPositions(vertices_, after_);
}

int SculptStrokeRestore::Size() {
    const std::size_t bytes =
        sizeof(*this) + vertices_.capacity() * sizeof(int) + (before_.capacity() + after_.capacity()) * sizeof(Point3);
    return bytes > 0x7fffffff ? 0x7fffffff : static_cast<int>(bytes);
}

MSTR SculptStrokeRestore::Description() { return MSTR(GetString(IDS_UNDO_STROKE)); }
