// SculptCore — a sculpting session: the mesh, its BVH, per-vertex mask,
// per-polygon SculptGroups and visibility, symmetry, and undo/dirty bookkeeping.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "sculpt/brush.h"
#include "sculpt/bvh.h"
#include "sculpt/mesh.h"

namespace sculpt {

// Exact record of one stroke or operation: enough to undo (before) and redo (after) it.
struct StrokeDelta {
    std::vector<std::uint32_t> vertices;  // Positions.
    std::vector<Vec3> before;
    std::vector<Vec3> after;

    std::vector<std::uint32_t> maskVertices;  // Mask values.
    std::vector<float> maskBefore;
    std::vector<float> maskAfter;

    std::vector<std::uint32_t> groupFaces;  // SculptGroup ids.
    std::vector<std::int32_t> groupBefore;
    std::vector<std::int32_t> groupAfter;

    bool hasHidden = false;  // Polygon visibility (whole arrays).
    std::vector<std::uint8_t> hiddenBefore;
    std::vector<std::uint8_t> hiddenAfter;

    bool empty() const { return vertices.empty() && maskVertices.empty() && groupFaces.empty() && !hasHidden; }
    bool consistent() const;
    std::size_t memoryBytes() const;
};

// Optional per-element data handed over with the mesh.
struct SessionAttributes {
    std::vector<std::int32_t> faceGroups;  // Per polygon (empty: all 0).
    std::vector<float> mask;               // Per vertex 0..1 (empty: unmasked).
    std::vector<std::uint8_t> hiddenFaces; // Per polygon (empty: all visible).
};

enum class AutoGroupMode : int { Curvature = 0, Angle, SmoothGroups, UVIslands, MaterialIDs, Elements };

class SculptSession {
public:
    static constexpr float kRebuildThreshold = 2.0f;

    bool build(MeshInput input, std::string* error = nullptr, SessionAttributes attributes = {});
    void clear();
    bool valid() const { return !mesh_.empty(); }

    const Mesh& mesh() const { return mesh_; }
    const Bvh& bvh() const { return bvh_; }

    // --- Attributes -----------------------------------------------------------
    const std::vector<float>& mask() const { return mask_; }
    bool hasMask() const { return hasMask_; }
    const std::vector<std::int32_t>& faceGroups() const { return faceGroups_; }
    std::int32_t nextGroupId() const;
    std::int32_t groupOfTriangle(std::uint32_t t) const { return faceGroups_[mesh_.triangleFace(t)]; }
    const std::vector<std::uint8_t>& hiddenFaces() const { return hiddenFaces_; }
    bool anyHidden() const { return anyHidden_; }
    const std::uint8_t* hiddenTriangles() const { return anyHidden_ ? hiddenTris_.data() : nullptr; }
    // Per-vertex flag: vertex touches polygons of two or more SculptGroups.
    const std::vector<std::uint8_t>& groupBorderVertices();

    // --- Surface Snapshot (Revert brush) -------------------------------------------
    // Positions the Revert brush pulls toward; empty or wrong size: Revert does nothing.
    void setReferencePositions(std::vector<Vec3> positions) { reference_ = std::move(positions); }
    bool hasReference() const { return reference_.size() == mesh_.vertexCount() && !reference_.empty(); }

    // --- Symmetry ---------------------------------------------------------------
    // Dabs are repeated once per transform; the first should be identity.
    void setSymmetry(std::vector<Mat3> transforms);
    const std::vector<Mat3>& symmetry() const { return symmetry_; }

    // Visible triangles only.
    bool raycast(const Ray& ray, RayHit& hit, bool cullBackfaces = false) const;

    // --- Strokes ----------------------------------------------------------------
    void beginStroke();
    bool strokeActive() const { return recorder_.active(); }
    // Applies one dab (all symmetry copies): geometry brushes move vertices,
    // MaskPaint edits the mask, FaceGroups paints `settings.faceGroupId`.
    std::size_t applyDab(const BrushSettings& settings, const Dab& dab);
    // Locally blurs the mask under a dab (Paint Mask double-click / Ctrl-click).
    std::size_t applyMaskBlurDab(const Dab& dab, float strength);
    // Moves vertices to explicit targets inside the current stroke (Clip, Pose,
    // Cloth, deformers). Targets are blended by (1 - mask) unless the caller
    // already weighted them (`useMask` false); hidden vertices stay. Returns
    // the number of vertices moved.
    std::size_t applyTargets(const std::vector<std::uint32_t>& vertices, const std::vector<Vec3>& targets,
                             bool useMask = true);

    // Displace brush: `height` maps an object-space point (on the stroke's
    // side of any mirror) to the stencil height relative to Height Mid
    // (about -1..1), false outside the stencil. `fade` 0 gives a hard edge,
    // 1 a full radial falloff. Layer Mode settles at one height per stroke.
    std::size_t applyDisplaceDab(const BrushSettings& settings, const Dab& dab,
                                 const std::function<bool(const Vec3&, float&)>& height, float fade);

    // Density brush: dabs only paint a per-vertex weight (0..1); the host
    // remeshes the painted region when the stroke ends.
    const std::vector<float>& densityWeights() const { return densityWeight_; }
    float densityRadius() const { return densityRadius_; }
    bool hasDensityWeights() const { return densityPainted_; }
    void clearDensityWeights();
    StrokeDelta endStroke();
    void cancelStroke();

    // Writes a delta's `before` (undo) or `after` (redo) state. Fails (without
    // changing anything) if a stroke is active or the delta does not fit.
    bool applyDelta(const StrokeDelta& delta, bool useBefore);

    // Replaces every position (the host changed the geometry; same topology).
    bool setPositions(const std::vector<Vec3>& positions);

    // --- Whole-mesh operations (each returns its undo delta) -------------------
    StrokeDelta maskClear(bool visibleOnly = false);
    StrokeDelta maskFill(bool visibleOnly = true);
    StrokeDelta maskToggleVisible();  // Clears the visible mask if any, else masks all visible.
    StrokeDelta maskInvert(bool visibleOnly = false);
    StrokeDelta maskBlur(int iterations = 1);
    StrokeDelta maskSharpen();
    StrokeDelta maskGrow();
    StrokeDelta maskShrink();
    StrokeDelta maskByCavity(float coverage);
    StrokeDelta maskByAO(float coverage, int rays = 16);
    // Rectangle/lasso: `inside` decides per vertex position (object space).
    StrokeDelta maskSelect(const std::function<bool(const Vec3&)>& inside, bool erase);

    StrokeDelta groupFromMask(float threshold = 0.5f);
    // keys: per-polygon host data for SmoothGroups/UVIslands/MaterialIDs.
    StrokeDelta autoGroups(AutoGroupMode mode, const std::vector<std::uint64_t>* keys = nullptr,
                           float angleDegrees = 30.0f);

    StrokeDelta showAll();
    StrokeDelta isolateOrHideGroup(std::int32_t group);  // Isolate; if already isolated, hide it.
    StrokeDelta invertVisibility();

    // --- Dirty tracking for the host --------------------------------------------
    const std::vector<std::uint32_t>& dirtyVertices() const { return dirty_; }  // Moved.
    void clearDirty();
    const std::vector<std::uint32_t>& displayDirtyVertices() const { return displayDirty_; }  // Moved or new normal.
    bool displayAllDirty() const { return displayAllDirty_; }
    void clearDisplayDirty();
    const std::vector<std::uint32_t>& maskDirtyVertices() const { return maskDirty_; }
    void clearMaskDirty();
    const std::vector<std::uint32_t>& groupDirtyFaces() const { return groupDirty_; }
    void clearGroupDirty();
    bool visibilityDirty() const { return visibilityDirty_; }
    void clearVisibilityDirty() { visibilityDirty_ = false; }

    std::size_t memoryBytes() const;

private:
    void commitMoved();
    void setMaskValue(std::uint32_t v, float value, bool recordInStroke);
    void setGroupValue(std::uint32_t f, std::int32_t group, bool recordInStroke);
    void refreshHiddenTriangles();
    void refreshHasMask();
    bool vertexVisible(std::uint32_t v) const;
    StrokeDelta finishMaskOp(const std::vector<float>& newMask);
    StrokeDelta finishGroupOp(const std::vector<std::int32_t>& newGroups);
    StrokeDelta finishHiddenOp(const std::vector<std::uint8_t>& newHidden);
    void applyMaskDab(const BrushSettings& settings, const Dab& dab, std::size_t& changed);
    void applyGroupDab(const BrushSettings& settings, const Dab& dab, std::size_t& changed);

    Mesh mesh_;
    Bvh bvh_;
    StrokeRecorder recorder_;
    std::vector<StrokeState> strokeStates_;
    DabScratch scratch_;
    std::vector<std::uint32_t> moved_;
    std::vector<std::uint32_t> movedTris_;
    std::vector<Mat3> symmetry_{Mat3::identity()};
    std::vector<Vec3> reference_;
    std::vector<float> densityWeight_;
    float densityRadius_ = 0.0f;
    bool densityPainted_ = false;

    std::vector<float> mask_;
    bool hasMask_ = false;
    std::vector<std::int32_t> faceGroups_;
    std::vector<std::uint8_t> hiddenFaces_;
    std::vector<std::uint8_t> hiddenTris_;
    bool anyHidden_ = false;
    std::vector<std::uint8_t> groupBorder_;
    bool groupBorderValid_ = false;

    // Stroke recording for mask and group painting.
    VisitSet maskTouched_;
    std::vector<std::uint32_t> maskStrokeVerts_;
    std::vector<float> maskStrokeBefore_;
    VisitSet groupTouched_;
    std::vector<std::uint32_t> groupStrokeFaces_;
    std::vector<std::int32_t> groupStrokeBefore_;

    // Dirty state. Flag bits: 1 moved, 2 display, 4 mask.
    std::vector<std::uint8_t> dirtyFlag_;
    std::vector<std::uint32_t> dirty_;
    std::vector<std::uint32_t> displayDirty_;
    std::vector<std::uint32_t> maskDirty_;
    bool displayAllDirty_ = false;
    std::vector<std::uint8_t> groupDirtyFlag_;
    std::vector<std::uint32_t> groupDirty_;
    bool visibilityDirty_ = false;
};

}  // namespace sculpt
