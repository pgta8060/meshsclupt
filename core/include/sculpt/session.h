// SculptCore — a sculpting session: the mesh, its BVH, and stroke bookkeeping
// (undo deltas and the set of vertices the host must re-upload).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "sculpt/brush.h"
#include "sculpt/bvh.h"
#include "sculpt/mesh.h"

namespace sculpt {

// Exact record of one stroke: enough to undo (before) and redo (after) it.
struct StrokeDelta {
    std::vector<std::uint32_t> vertices;
    std::vector<Vec3> before;
    std::vector<Vec3> after;

    bool empty() const { return vertices.empty(); }
    bool consistent() const { return before.size() == vertices.size() && after.size() == vertices.size(); }
    std::size_t memoryBytes() const {
        return vertices.capacity() * sizeof(std::uint32_t) + (before.capacity() + after.capacity()) * sizeof(Vec3);
    }
};

class SculptSession {
public:
    // The BVH is rebuilt at the end of a stroke once refits have inflated the
    // leaf boxes past this factor (keeps picking/gathering fast).
    static constexpr float kRebuildThreshold = 2.0f;

    bool build(MeshInput input, std::string* error = nullptr);
    void clear();
    bool valid() const { return !mesh_.empty(); }

    const Mesh& mesh() const { return mesh_; }
    const Bvh& bvh() const { return bvh_; }

    bool raycast(const Ray& ray, RayHit& hit, bool cullBackfaces = false) const;

    void beginStroke();
    bool strokeActive() const { return recorder_.active(); }

    // Applies one dab and refreshes normals and the BVH for the touched area.
    // Starts a stroke implicitly if none is active. Returns moved vertex count.
    std::size_t applyDab(const BrushSettings& settings, const Dab& dab);

    // Finishes the stroke and returns its delta (empty if nothing moved).
    StrokeDelta endStroke();

    // Restores every vertex the current stroke moved and ends the stroke.
    void cancelStroke();

    // Writes a delta's `before` (undo) or `after` (redo) positions. Fails if a
    // stroke is active or the delta does not fit this mesh.
    bool applyDelta(const StrokeDelta& delta, bool useBefore);

    // Replaces every position (the host changed the geometry; same topology).
    bool setPositions(const std::vector<Vec3>& positions);

    // Vertices moved since the last clearDirty(), without duplicates.
    // The host copies these positions back into its own mesh.
    const std::vector<std::uint32_t>& dirtyVertices() const { return dirty_; }
    void clearDirty();

    // Vertices whose position or normal changed since clearDisplayDirty()
    // (a superset of dirtyVertices()); the host refreshes their display data.
    // displayAllDirty() means "everything changed" (e.g. after setPositions).
    const std::vector<std::uint32_t>& displayDirtyVertices() const { return displayDirty_; }
    bool displayAllDirty() const { return displayAllDirty_; }
    void clearDisplayDirty();

    std::size_t memoryBytes() const;

private:
    void commitMoved();  // Normals + BVH refit + dirty tracking for moved_.

    Mesh mesh_;
    Bvh bvh_;
    StrokeRecorder recorder_;
    DabScratch scratch_;
    std::vector<std::uint32_t> moved_;
    std::vector<std::uint32_t> movedTris_;
    std::vector<std::uint32_t> dirty_;
    std::vector<std::uint8_t> dirtyFlag_;  // Bit 0: dirty_, bit 1: displayDirty_.
    std::vector<std::uint32_t> displayDirty_;
    bool displayAllDirty_ = false;
};

}  // namespace sculpt
