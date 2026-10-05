#include "SculptMeshObject.h"

#include <algorithm>

#include <MNNormalSpec.h>
#include <objmode.h>

#include "MultiresRollout.h"
#include "SculptActions.h"
#include "SculptMode.h"
#include "SculptSettings.h"
#include "SculptUI.h"

namespace {

// Chunk IDs inside a Sculpt Mesh object. Append new IDs; never reuse.
constexpr USHORT kVersionChunk = 0x5C00;
constexpr USHORT kPolyDataChunk = 0x5C10;
constexpr USHORT kMaskChunk = 0x5C20;
constexpr USHORT kGroupsChunk = 0x5C21;
constexpr USHORT kHiddenChunk = 0x5C22;

template <class T>
IOResult WriteArray(ISave* isave, USHORT id, const std::vector<T>& values) {
    ULONG written = 0;
    const DWORD count = static_cast<DWORD>(values.size());
    isave->BeginChunk(id);
    IOResult res = isave->Write(&count, sizeof(count), &written);
    if (res == IO_OK && count > 0)
        res = isave->Write(values.data(), static_cast<ULONG>(sizeof(T) * values.size()), &written);
    isave->EndChunk();
    return res;
}

template <class T>
IOResult ReadArray(ILoad* iload, std::vector<T>& values) {
    ULONG read = 0;
    DWORD count = 0;
    IOResult res = iload->Read(&count, sizeof(count), &read);
    if (res != IO_OK) return res;
    if (count > 0x7fffffffu / sizeof(T)) return IO_ERROR;  // Corrupt size.
    values.resize(count);
    if (count > 0) res = iload->Read(values.data(), static_cast<ULONG>(sizeof(T) * count), &read);
    return res;
}

GenSubObjType& TransformSubObjType() {
    static GenSubObjType type(MSTR(_T("Transform")), MSTR(_T("SubObjectIcons")), 1);
    return type;
}

}  // namespace

SculptMeshObject* SculptMeshObject::editedObject_ = nullptr;
IObjParam* SculptMeshObject::editInterface_ = nullptr;
MoveModBoxCMode* SculptMeshObject::moveMode_ = nullptr;
RotateModBoxCMode* SculptMeshObject::rotateMode_ = nullptr;
UScaleModBoxCMode* SculptMeshObject::uscaleMode_ = nullptr;
NUScaleModBoxCMode* SculptMeshObject::nuscaleMode_ = nullptr;
SquashModBoxCMode* SculptMeshObject::squashMode_ = nullptr;

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
    int NumActionTables() override { return SculptActions::TableCount(); }
    ActionTable* GetActionTable(int i) override { return SculptActions::Table(i); }
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
    copy->attributes_ = attributes_;
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
    // container chunk so versions can add sibling chunks safely.
    isave->BeginChunk(kPolyDataChunk);
    res = PolyObject::Save(isave);
    isave->EndChunk();
    if (res != IO_OK) return res;

    if (attributes_.Matches(mm)) {
        const auto& a = attributes_;
        if (std::any_of(a.mask.begin(), a.mask.end(), [](float m) { return m > 0.0f; }) &&
            (res = WriteArray(isave, kMaskChunk, a.mask)) != IO_OK)
            return res;
        if (std::any_of(a.groups.begin(), a.groups.end(), [](std::int32_t g) { return g != 0; }) &&
            (res = WriteArray(isave, kGroupsChunk, a.groups)) != IO_OK)
            return res;
        if (std::any_of(a.hidden.begin(), a.hidden.end(), [](std::uint8_t h) { return h != 0; }) &&
            (res = WriteArray(isave, kHiddenChunk, a.hidden)) != IO_OK)
            return res;
    }
    return IO_OK;
}

IOResult SculptMeshObject::Load(ILoad* iload) {
    ULONG read = 0;
    DWORD version = 0;
    IOResult res = IO_OK;
    SculptAttributes loaded;
    while ((res = iload->OpenChunk()) == IO_OK) {
        switch (iload->CurChunkID()) {
            case kVersionChunk: res = iload->Read(&version, sizeof(version), &read); break;
            case kPolyDataChunk: res = PolyObject::Load(iload); break;
            case kMaskChunk: res = ReadArray(iload, loaded.mask); break;
            case kGroupsChunk: res = ReadArray(iload, loaded.groups); break;
            case kHiddenChunk: res = ReadArray(iload, loaded.hidden); break;
            default: break;  // Chunk from a newer version: skip it, keep the rest.
        }
        iload->CloseChunk();
        if (res != IO_OK) return res;
    }
    // Keep each attribute only if it fits the loaded mesh (a damaged or
    // mismatched array is dropped rather than misapplied).
    attributes_ = SculptAttributes();
    attributes_.Fit(mm);
    if (loaded.mask.size() == attributes_.mask.size()) attributes_.mask = loaded.mask;
    if (loaded.groups.size() == attributes_.groups.size()) attributes_.groups = loaded.groups;
    if (loaded.hidden.size() == attributes_.hidden.size()) attributes_.hidden = loaded.hidden;
    ReleaseSession();
    return IO_OK;
}

void SculptMeshObject::BeginEditParams(IObjParam* ip, ULONG flags, Animatable* prev) {
    PolyObject::BeginEditParams(ip, flags, prev);
    editedObject_ = this;
    editInterface_ = ip;
    if (!moveMode_) {
        moveMode_ = new MoveModBoxCMode(this, ip);
        rotateMode_ = new RotateModBoxCMode(this, ip);
        uscaleMode_ = new UScaleModBoxCMode(this, ip);
        nuscaleMode_ = new NUScaleModBoxCMode(this, ip);
        squashMode_ = new SquashModBoxCMode(this, ip);
    }
    MultiresRollout::Open(ip, this);
    SculptActions::Activate();
    SculptUI::OnEditBegin();
}

void SculptMeshObject::EndEditParams(IObjParam* ip, ULONG flags, Animatable* next) {
    if (SculptMode::Get().Target() == this) SculptMode::Get().Stop();
    SculptUI::OnEditEnd();
    SculptActions::Deactivate();
    MultiresRollout::Close(ip);
    if (moveMode_) {
        ip->DeleteMode(moveMode_);
        ip->DeleteMode(rotateMode_);
        ip->DeleteMode(uscaleMode_);
        ip->DeleteMode(nuscaleMode_);
        ip->DeleteMode(squashMode_);
        delete moveMode_;
        delete rotateMode_;
        delete uscaleMode_;
        delete nuscaleMode_;
        delete squashMode_;
        moveMode_ = nullptr;
        rotateMode_ = nullptr;
        uscaleMode_ = nullptr;
        nuscaleMode_ = nullptr;
        squashMode_ = nullptr;
    }
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

// --- Session ------------------------------------------------------------------------

SculptDisplay::Options SculptMeshObject::DisplayOptions() const {
    return SculptDisplay::Options{SculptSettings::Get().Bool(Prop::ShowMask), SculptSettings::Get().Bool(Prop::ShowGroups)};
}

SculptSessionBridge* SculptMeshObject::AcquireSession(MSTR& error) {
    attributes_.Fit(mm);
    if (bridge_ && !bridge_->Matches(mm)) bridge_.reset();  // Topology changed underneath.
    if (bridge_ && sessionStale_ && !bridge_->Session().strokeActive()) {
        if (!bridge_->SyncFromMesh(mm)) bridge_.reset();
        sessionStale_ = false;
    }
    if (!bridge_) {
        auto bridge = std::make_unique<SculptSessionBridge>();
        if (!bridge->Build(mm, attributes_, error)) return nullptr;
        bridge_ = std::move(bridge);
        sessionStale_ = false;
        if (display_) display_->Build(bridge_->Session(), DisplayOptions());  // New session: new chunks.
    }
    return bridge_.get();
}

void SculptMeshObject::ReleaseSession() {
    if (fastDisplay_) SetFastDisplay(false);
    bridge_.reset();
    sessionStale_ = false;
}

void SculptMeshObject::CommitSessionChanges(bool interactive) {
    if (!bridge_) return;
    sculpt::SculptSession& session = bridge_->Session();
    const unsigned pushed = bridge_->PushDirty(mm, attributes_);
    if ((pushed & (SculptSessionBridge::kGroups | SculptSessionBridge::kVisibility)) != 0u)
        displayRebuildPending_ = true;
    if (display_) {
        // The chunk layout depends on groups/visibility: rebuild (at most every
        // 150 ms while painting groups on a dense mesh).
        const DWORD now = GetTickCount();
        if (displayRebuildPending_ && (!interactive || now - lastDisplayRebuild_ >= 150)) {
            display_->Build(session, DisplayOptions());
            displayRebuildPending_ = false;
            lastDisplayRebuild_ = now;
        } else {
            if (session.displayAllDirty())
                display_->MarkAll();
            else
                display_->MarkVertices(session.displayDirtyVertices());
            display_->MarkVertices(session.maskDirtyVertices());
        }
    }
    session.clearDisplayDirty();
    session.clearMaskDirty();
    session.clearVisibilityDirty();
    if (pushed != 0u) GeometryChanged();
}

bool SculptMeshObject::RunOperation(const std::function<sculpt::StrokeDelta(SculptSessionBridge&)>& op,
                                    const MCHAR* undoName) {
    MSTR error;
    SculptSessionBridge* bridge = AcquireSession(error);
    if (!bridge || bridge->Session().strokeActive()) return false;
    const sculpt::StrokeDelta delta = op(*bridge);
    if (delta.empty()) return false;
    CommitSessionChanges();
    theHold.Begin();
    PutStrokeUndo(delta, undoName);
    theHold.Accept(undoName);
    return true;
}

void SculptMeshObject::PutStrokeUndo(const sculpt::StrokeDelta& coreDelta, const MCHAR* undoName) {
    if (!bridge_ || coreDelta.empty() || !coreDelta.consistent() || !theHold.Holding()) return;
    theHold.Put(new SculptDeltaRestore(this, bridge_->ToMax(coreDelta), MSTR(undoName)));
}

void SculptMeshObject::ApplyDelta(const sculpt::StrokeDelta& d, bool useBefore) {
    if (!d.consistent()) return;
    if (bridge_ && !bridge_->Session().strokeActive()) {
        sculpt::StrokeDelta core;
        if (bridge_->ToCore(d, core) && bridge_->Session().applyDelta(core, useBefore)) {
            CommitSessionChanges();
            return;
        }
    }
    // No (usable) session: write the mesh and attributes directly.
    attributes_.Fit(mm);
    for (std::size_t i = 0; i < d.vertices.size(); ++i) {
        const int v = static_cast<int>(d.vertices[i]);
        const sculpt::Vec3& p = useBefore ? d.before[i] : d.after[i];
        if (v >= 0 && v < mm.numv) mm.v[v].p = ToPoint3(p);
    }
    for (std::size_t i = 0; i < d.maskVertices.size(); ++i)
        if (d.maskVertices[i] < attributes_.mask.size())
            attributes_.mask[d.maskVertices[i]] = useBefore ? d.maskBefore[i] : d.maskAfter[i];
    for (std::size_t i = 0; i < d.groupFaces.size(); ++i)
        if (d.groupFaces[i] < attributes_.groups.size())
            attributes_.groups[d.groupFaces[i]] = useBefore ? d.groupBefore[i] : d.groupAfter[i];
    if (d.hasHidden && d.hiddenBefore.size() == attributes_.hidden.size())
        attributes_.hidden = useBefore ? d.hiddenBefore : d.hiddenAfter;
    if (bridge_) {
        // The session no longer matches the attributes: rebuild on next use.
        const bool hadFastDisplay = fastDisplay_;
        ReleaseSession();
        if (hadFastDisplay) {
            MSTR error;
            if (AcquireSession(error)) SetFastDisplay(true);
        }
    }
    GeometryChanged();
}

void SculptMeshObject::GeometryChanged() {
    if (MNNormalSpec* normals = mm.GetSpecifiedNormals()) normals->ClearFlag(MNNORMAL_NORMALS_COMPUTED);
    mm.InvalidateGeomCache();
    NotifyDependents(FOREVER, PART_GEOM | PART_DISPLAY, REFMSG_CHANGE);
}

// --- Display --------------------------------------------------------------------------

void SculptMeshObject::RefreshDisplayOptions() {
    if (!display_) return;
    display_->SetOptions(DisplayOptions());
    NotifyDependents(FOREVER, PART_DISPLAY, REFMSG_CHANGE);
}

void SculptMeshObject::SetFastDisplay(bool on) {
    on = on && SculptSettings::Get().Bool(Prop::SculptMaterialPreview);
    if (on && bridge_) {
        if (!display_) display_ = std::make_unique<SculptDisplay>();
        display_->Build(bridge_->Session(), DisplayOptions());
        bridge_->Session().clearDisplayDirty();
        displayRebuildPending_ = false;
        fastDisplay_ = true;
    } else {
        if (!fastDisplay_ && !display_) return;
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
        display_->Upload(bridge_->Session());
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

// --- Sub-object Transform (mask-aware W/E/R) ------------------------------------------

ISubObjType* SculptMeshObject::GetSubObjType(int i) {
    if (i == -1) return subLevel_ > 0 ? &TransformSubObjType() : nullptr;
    return i == 0 ? &TransformSubObjType() : nullptr;
}

void SculptMeshObject::ActivateSubobjSel(int level, XFormModes& modes) {
    subLevel_ = level;
    if (level > 0) {
        if (SculptMode::Get().Target() == this) SculptMode::Get().Stop();
        if (moveMode_) modes = XFormModes(moveMode_, rotateMode_, nuscaleMode_, uscaleMode_, squashMode_, nullptr);
    }
    if (IObjParam* ip = editInterface_) ip->PipeSelLevelChanged();
    NotifyDependents(FOREVER, PART_SELECT | PART_DISPLAY | PART_SUBSEL_TYPE, REFMSG_CHANGE);
}

int SculptMeshObject::HitTest(TimeValue /*t*/, INode* /*inode*/, int /*type*/, int /*crossing*/, int /*flags*/,
                              IPoint2* /*p*/, ViewExp* /*vpt*/, ModContext* /*mc*/) {
    return 0;  // The Transform level has nothing to pick: the mask is the selection.
}

float SculptMeshObject::TransformWeight(int v) const {
    if (v < 0 || v >= mm.numv || mm.v[v].GetFlag(MN_DEAD)) return 0.0f;
    if (static_cast<std::size_t>(v) >= attributes_.mask.size()) return 1.0f;
    return 1.0f - std::min(std::max(attributes_.mask[v], 0.0f), 1.0f);
}

void SculptMeshObject::GetSubObjectCenters(SubObjAxisCallback* cb, TimeValue t, INode* node, ModContext* /*mc*/) {
    if (subLevel_ == 0 || !node) return;
    Point3 sum(0.0f, 0.0f, 0.0f);
    float weight = 0.0f;
    for (int v = 0; v < mm.numv; ++v) {
        const float w = TransformWeight(v);
        if (w <= 0.0f) continue;
        sum += mm.v[v].p * w;
        weight += w;
    }
    if (weight > 0.0f) cb->Center(node->GetObjectTM(t).PointTransform(sum / weight), 0);
}

void SculptMeshObject::GetSubObjectTMs(SubObjAxisCallback* cb, TimeValue t, INode* node, ModContext* mc) {
    if (subLevel_ == 0 || !node) return;
    struct CenterGrabber : SubObjAxisCallback {
        Point3 center{0, 0, 0};
        bool found = false;
        void Center(Point3 c, int) override {
            center = c;
            found = true;
        }
        void TM(Matrix3, int) override {}
        int Type() override { return SO_CENTER_PIVOT; }
    } grab;
    GetSubObjectCenters(&grab, t, node, mc);
    if (!grab.found) return;
    Matrix3 tm = node->GetObjectTM(t);
    tm.NoScale();
    tm.SetTrans(grab.center);
    cb->TM(tm, 0);
}

void SculptMeshObject::TransformStart(TimeValue /*t*/) {
    if (xformActive_) return;
    xformActive_ = true;
    xformOrigin_.resize(static_cast<std::size_t>(std::max<int>(mm.numv, 0)));
    for (int v = 0; v < mm.numv; ++v) xformOrigin_[v] = mm.v[v].p;
    if (editInterface_) editInterface_->LockAxisTripods(TRUE);
}

void SculptMeshObject::Transform(TimeValue t, Matrix3& partm, Matrix3& tmAxis, const Matrix3& xfrm) {
    if (subLevel_ == 0) return;
    if (!xformActive_) TransformStart(t);
    if (xformOrigin_.size() != static_cast<std::size_t>(mm.numv)) return;
    // Same convention as Editable Mesh: values are relative to the drag start.
    Matrix3 tm = partm * Inverse(tmAxis);
    const Matrix3 itm = Inverse(tm);
    tm *= xfrm;
    for (int v = 0; v < mm.numv; ++v) {
        const float w = TransformWeight(v);
        const Point3& old = xformOrigin_[v];
        if (w <= 0.0f) {
            mm.v[v].p = old;
            continue;
        }
        const Point3 moved = itm.PointTransform(tm.PointTransform(old));
        mm.v[v].p = old + (moved - old) * w;
    }
    sessionStale_ = true;
    GeometryChanged();
}

void SculptMeshObject::Move(TimeValue t, Matrix3& partm, Matrix3& tmAxis, Point3& val, BOOL /*localOrigin*/) {
    Transform(t, partm, tmAxis, TransMatrix(val));
}

void SculptMeshObject::Rotate(TimeValue t, Matrix3& partm, Matrix3& tmAxis, Quat& val, BOOL /*localOrigin*/) {
    Matrix3 m;
    val.MakeMatrix(m);
    Transform(t, partm, tmAxis, m);
}

void SculptMeshObject::Scale(TimeValue t, Matrix3& partm, Matrix3& tmAxis, Point3& val, BOOL /*localOrigin*/) {
    Transform(t, partm, tmAxis, ScaleMatrix(val));
}

void SculptMeshObject::TransformHoldingFinish(TimeValue /*t*/) {
    if (!xformActive_ || xformOrigin_.size() != static_cast<std::size_t>(mm.numv)) return;
    sculpt::StrokeDelta delta;
    for (int v = 0; v < mm.numv; ++v) {
        if (mm.v[v].p == xformOrigin_[v]) continue;
        delta.vertices.push_back(static_cast<std::uint32_t>(v));
        delta.before.push_back(ToVec3(xformOrigin_[v]));
        delta.after.push_back(ToVec3(mm.v[v].p));
    }
    if (!delta.empty() && theHold.Holding())
        theHold.Put(new SculptDeltaRestore(this, std::move(delta), MSTR(GetString(IDS_UNDO_TRANSFORM))));
    for (int v = 0; v < mm.numv; ++v) xformOrigin_[v] = mm.v[v].p;  // A following drag starts from here.
}

void SculptMeshObject::TransformFinish(TimeValue /*t*/) {
    xformActive_ = false;
    xformOrigin_.clear();
    if (editInterface_) editInterface_->LockAxisTripods(FALSE);
}

void SculptMeshObject::TransformCancel(TimeValue /*t*/) {
    if (xformActive_ && xformOrigin_.size() == static_cast<std::size_t>(mm.numv)) {
        for (int v = 0; v < mm.numv; ++v) mm.v[v].p = xformOrigin_[v];
        GeometryChanged();
    }
    xformActive_ = false;
    xformOrigin_.clear();
    if (editInterface_) editInterface_->LockAxisTripods(FALSE);
}

// --- Undo ---------------------------------------------------------------------------

SculptDeltaRestore::SculptDeltaRestore(SculptMeshObject* object, sculpt::StrokeDelta delta, MSTR name)
    : object_(object), delta_(std::move(delta)), name_(std::move(name)) {}

void SculptDeltaRestore::Restore(int /*isUndo*/) {
    if (object_) object_->ApplyDelta(delta_, true);
}

void SculptDeltaRestore::Redo() {
    if (object_) object_->ApplyDelta(delta_, false);
}

int SculptDeltaRestore::Size() {
    const std::size_t bytes = sizeof(*this) + delta_.memoryBytes();
    return bytes > 0x7fffffff ? 0x7fffffff : static_cast<int>(bytes);
}
