// Connects a core sculpt session to an MNMesh: builds the session from the
// mesh and maps core vertex indices to MNMesh indices (MNMesh may contain
// dead vertex slots, so the two index spaces can differ).
#pragma once

#include <cstdint>
#include <vector>

#include <max.h>
#include <mnmesh.h>

#include "sculpt/session.h"

class SculptSessionBridge {
public:
    // Builds the session from the live vertices/faces of `mesh`.
    bool Build(const MNMesh& mesh, MSTR& error);

    // True when the session was built from a mesh with this vertex/face layout.
    bool Matches(const MNMesh& mesh) const;

    // Re-reads every vertex position from `mesh` (same topology). Used when
    // the geometry was changed by something other than a sculpt stroke.
    bool SyncFromMesh(const MNMesh& mesh);

    // Writes the vertices changed in the session since the last push into
    // `mesh` and clears the session's dirty set. Returns the vertex count.
    std::size_t PushDirty(MNMesh& mesh);

    // Applies positions addressed by MNMesh vertex index (undo/redo). The
    // mesh itself is updated by the caller. Fails if an index is unknown.
    bool ApplyMaxPositions(const std::vector<int>& maxIndices, const std::vector<Point3>& positions);

    sculpt::SculptSession& Session() { return session_; }
    const sculpt::SculptSession& Session() const { return session_; }

    // MNMesh vertex index of a core vertex.
    int ToMax(std::uint32_t coreIndex) const { return coreToMax_[coreIndex]; }

private:
    sculpt::SculptSession session_;
    std::vector<int> coreToMax_;
    std::vector<int> maxToCore_;  // -1 for dead MNMesh vertices.
    int numVerts_ = 0;
    int numFaces_ = 0;
};

inline sculpt::Vec3 ToVec3(const Point3& p) { return {p.x, p.y, p.z}; }
inline Point3 ToPoint3(const sculpt::Vec3& v) { return Point3(v.x, v.y, v.z); }
