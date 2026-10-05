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
#include "sculpt/layers.h"
#include "sculpt/multires.h"
#include "sculpt/paint.h"

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
    //   3: + Multires stack, Surface Snapshot.  4: + sculpt layers.
    static constexpr DWORD kFileVersion = 4;

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

    // --- Multires (each operation is one undo step) ----------------------------------
    int MultiresTopLevel() const { return multires_ ? multires_->topLevel() : 0; }
    int MultiresLevel() const { return level_; }
    bool SetMultiresLevel(int level, MSTR& error);
    bool SubdivideLevel(MSTR& error);      // From the top level.
    bool DeleteLowerLevels(MSTR& error);   // The current level becomes level 0.
    bool DeleteHigherLevels(MSTR& error);  // Levels above the current one are removed.
    bool ReverseSubdivision(MSTR& error);  // Rebuilds a coarser level 0.
    // Auto Smooth (or faceted) from the settings; call after the settings changed.
    void RefreshSmoothing();

    // --- Topology tools (Density, Cutter, Slice, Curve Tube) ----------------------------
    // Runs `edit` on the current mesh as one undo step. The result is a plain
    // mesh again: Multires levels and sculpt layers are dropped (layers are
    // baked into the shape).
    bool RunTopologyEdit(const std::function<bool(sculpt::PolyData&, MSTR&)>& edit, int undoName, MSTR& error);

    // --- Surface Snapshot (Revert brush) -------------------------------------------------
    void CaptureSurface();
    void ClearSurface();
    bool HasSurface() const { return !surface_.positions.empty(); }
    // The snapshot fits the current level and topology (the Revert brush can use it).
    bool SurfaceMatches() const;
    MSTR SurfaceStatus() const;

    // --- Sculpt layers (live on one Multires level) ---------------------------------------
    const sculpt::LayerStack& Layers() const { return layers_; }
    int LayersLevel() const { return layersLevel_; }
    // Layers (if any) fit the current level and topology, so they can be edited.
    bool LayersUsable() const;
    bool NewLayer();
    bool DeleteLayer();       // The selected layer.
    bool ClearLayer();        // The selected layer's content.
    bool ClearAllLayers();
    bool MoveLayer(int direction);  // -1 up, +1 down.
    bool BakeAllLayers();     // Merge every layer into the base sculpt.
    void SelectLayer(int index);    // -1: strokes go to the base sculpt.
    void SetLayerEnabled(int index, bool on);
    void SetLayerStrengthLive(int index, float strength);  // While dragging (no undo yet)...
    void FinishLayerStrength();                             // ...one undo step for the drag.
    void SetLayerName(int index, const std::string& name);
    // Displace rollout: (re)computes the Displace layer on the top level.
    bool ApplyDisplace(MSTR& error, bool undoable = true);
    bool RemoveDisplace();
    bool HasDisplaceLayer() const { return layers_.displaceLayer() >= 0; }

    // Used by undo records.
    void SetLayersInternal(const sculpt::LayerStack& stack, bool moveVertices, int level, std::uint64_t topologyId);
    void SetLayerValues(int layer, const std::vector<std::uint32_t>& vertices, const std::vector<sculpt::Vec3>& values);

    // --- Texture painting (SculptMeshPaint.cpp) -----------------------------------------
    // Paint data lives while 3ds Max runs; Save / Save As write it to an image
    // file, which is what the scene keeps.
    struct PaintState;
    // Creates the canvas on first use and (re)builds the texel map for the
    // current UVs (map channel 1). Returns nullptr and fills `error` if the
    // mesh cannot be painted.
    PaintState* AcquirePaint(MSTR& error);
    PaintState* Paint() { return paint_.get(); }
    const sculpt::PaintCanvas* Canvas() const;
    // The canvas changed: recomposite and refresh the viewport texture (at
    // most every 40 ms while `interactive`).
    void PaintChanged(bool interactive);
    void PutPaintUndo(sculpt::PaintDelta delta, const MCHAR* undoName);  // Inside an open hold.
    void ApplyPaintDelta(const sculpt::PaintDelta& delta, bool before);
    // Paint layers (one undo step each).
    bool NewPaintLayer();
    bool DeletePaintLayer();
    bool ClearPaintLayer();
    bool ClearAllPaintLayers();
    bool MovePaintLayer(int direction);
    bool MergePaintLayers();  // Bake All: into the base texture.
    void SelectPaintLayer(int index);
    void SetPaintLayerEnabled(int index, bool on);
    void SetPaintLayerOpacityLive(int index, float opacity);
    void FinishPaintLayerOpacity();
    void SetPaintLayerBlend(int index, sculpt::PaintBlend blend);
    void SetPaintLayerName(int index, const std::string& name);
    void SetPaintLayersInternal(const std::vector<sculpt::PaintLayer>& layers, int active);
    bool ImportPaintTexture(int index, MSTR& error);  // Asks for the file.
    // Layer adjustments with live preview: Begin, Preview (any number), End.
    enum class Adjustment { HueSaturation, BrightnessContrast, Levels };
    bool BeginPaintAdjustment();
    void PreviewPaintAdjustment(Adjustment kind, const float values[5]);
    void EndPaintAdjustment(bool commit);
    // Material / Paint rollout.
    bool SavePaintTexture(bool chooseFile, MSTR& error);
    bool ReplacePaintTexture(MSTR& error);
    bool ResizePaintTexture(int size, MSTR& error);  // Generated texture resolution.
    bool RestoreMaterial();
    bool HasOriginalMaterial() const;
    MSTR PaintStatus() const;
    // Paint mode on this object while sculpting: the viewport shows the paint texture.
    bool PaintDisplayWanted() const;

    // Identifies the current topology + level; undo records only apply to it.
    std::uint64_t TopologyStamp() const { return (topologyId_ << 4) | static_cast<std::uint64_t>(level_ & 0xf); }

    // Full object state for undo of topology-changing operations.
    struct State;
    std::shared_ptr<State> CaptureState() const;
    void RestoreState(const State& state);
    // Level change without an undo record (used by undo/redo itself).
    bool SetLevelInternal(int level, MSTR& error);

    // The object currently shown in the Modify panel (nullptr if none).
    static SculptMeshObject* EditedObject() { return editedObject_; }
    static IObjParam* EditInterface() { return editInterface_; }

private:
    bool CommitLevel(MSTR& error);
    void ReplaceMesh(const sculpt::PolyData& poly);
    void NotifyTopologyChange();
    void ApplyAutoSmooth();
    void UpdateSessionReference();
    // Runs a topology/stack change as one undo step (full state snapshots).
    // `sameMesh`: the displayed mesh is unchanged, so a matching snapshot stays valid.
    bool RunStructural(const std::function<bool(MSTR&)>& op, int undoName, MSTR& error, bool sameMesh = false,
                       bool keepLayers = false);
    void ChangeLayers(sculpt::LayerStack after, bool moveVertices, int undoName);
    void RecordIntoActiveLayer(const sculpt::StrokeDelta& maxDelta);
    void AddLayerOffsets();  // mm += layer offsets (after building the layers' level).
    bool LayersHere() const { return !layers_.empty() && LayersUsable(); }
    static std::uint64_t NewTopologyId();
    bool BuildPaintMap(MSTR& error);
    void ChangePaintLayers(const std::function<bool(std::vector<sculpt::PaintLayer>&, int&)>& edit, int undoName);
    INode* FindNode() const;
    void ShowTextureOnNode(const std::wstring& path);

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

    std::unique_ptr<sculpt::Multires> multires_;  // Null: no subdivision levels.
    int level_ = 0;
    std::uint64_t topologyId_ = NewTopologyId();
    struct Surface {
        int level = 0;
        std::uint64_t topologyId = 0;
        std::vector<Point3> positions;  // By MNMesh vertex index.
    } surface_;

    sculpt::LayerStack layers_;  // Deltas by MNMesh vertex index.
    int layersLevel_ = 0;
    std::uint64_t layersTopologyId_ = 0;
    std::unique_ptr<sculpt::LayerStack> strengthDragStart_;
    std::unique_ptr<PaintState> paint_;

    static SculptMeshObject* editedObject_;
    static IObjParam* editInterface_;
    static MoveModBoxCMode* moveMode_;
    static RotateModBoxCMode* rotateMode_;
    static UScaleModBoxCMode* uscaleMode_;
    static NUScaleModBoxCMode* nuscaleMode_;
    static SquashModBoxCMode* squashMode_;
};

// Undo record for any sculpt change (MNMesh indices). Ignored if the object's
// topology/level changed in a way the undo stack did not restore.
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
    std::uint64_t stamp_;
};

class PaintMaterialKeeper;

struct SculptMeshObject::PaintState {
    PaintState();
    ~PaintState();
    sculpt::PaintCanvas canvas;
    sculpt::TexelMap map;
    std::uint64_t mapStamp = 0;              // TopologyStamp the UV data belongs to.
    std::vector<std::uint32_t> cornerKeys;   // UV index per session polygon corner.
    std::vector<sculpt::Vec3> keyUVs;        // Map channel 1 vertices.
    std::wstring path;                       // Paint texture file ("" until saved).
    bool fromDiffuse = false;                // Base came from the material's diffuse map.
    DWORD lastUpload = 0;
    bool uploadPending = false;
    std::vector<sculpt::PaintLayer> opacityDragStart;
    bool opacityDragging = false;
    std::shared_ptr<sculpt::Image> adjustSource;  // Layer pixels before an adjustment dialog.
    std::uint64_t adjustImage = 0;
    PaintMaterialKeeper* keeper = nullptr;   // The node's material before Sculpt Mesh assigned one.
};

struct SculptMeshObject::State {
    MNMesh mesh;
    SculptAttributes attributes;
    std::unique_ptr<sculpt::Multires> multires;
    int level = 0;
    std::uint64_t topologyId = 0;
    sculpt::LayerStack layers;
    int layersLevel = 0;
    std::uint64_t layersTopologyId = 0;
};
