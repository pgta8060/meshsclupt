// Connects a core sculpt session to an MNMesh: builds the session from the
// mesh and maps core vertex/polygon indices to MNMesh indices (MNMesh may
// contain dead slots, so the index spaces can differ).
#pragma once

#include <cstdint>
#include <vector>

#include <max.h>
#include <mnmesh.h>

#include "sculpt/session.h"

// Per-element sculpt data stored on the object, indexed like the MNMesh.
struct SculptAttributes {
    std::vector<float> mask;               // Per MNMesh vertex.
    std::vector<std::int32_t> groups;      // Per MNMesh face.
    std::vector<std::uint8_t> hidden;      // Per MNMesh face.

    // Resizes to the mesh, keeping existing values (new slots: defaults).
    void Fit(const MNMesh& mesh);
    bool Matches(const MNMesh& mesh) const;
};

class SculptSessionBridge {
public:
    bool Build(const MNMesh& mesh, const SculptAttributes& attributes, MSTR& error);

    // True when the session was built from a mesh with this vertex/face layout.
    bool Matches(const MNMesh& mesh) const;

    // Re-reads every vertex position (same topology).
    bool SyncFromMesh(const MNMesh& mesh);

    // Copies everything the session changed since the last push into the
    // mesh and attributes. Returns PushResult flags.
    enum PushResult : unsigned { kPositions = 1u, kMask = 2u, kGroups = 4u, kVisibility = 8u };
    unsigned PushDirty(MNMesh& mesh, SculptAttributes& attributes);

    // Converts deltas between session indices and MNMesh indices.
    sculpt::StrokeDelta ToMax(const sculpt::StrokeDelta& coreDelta) const;
    bool ToCore(const sculpt::StrokeDelta& maxDelta, sculpt::StrokeDelta& coreDelta) const;

    // Per-polygon keys for auto groups (smoothing groups, material IDs, UV islands).
    std::vector<std::uint64_t> FaceKeys(const MNMesh& mesh, sculpt::AutoGroupMode mode) const;

    sculpt::SculptSession& Session() { return session_; }
    const sculpt::SculptSession& Session() const { return session_; }

    int ToMaxVertex(std::uint32_t coreIndex) const { return coreToMaxVert_[coreIndex]; }
    int ToMaxFace(std::uint32_t coreIndex) const { return coreToMaxFace_[coreIndex]; }

private:
    sculpt::SculptSession session_;
    std::vector<int> coreToMaxVert_;
    std::vector<int> maxToCoreVert_;  // -1 for dead MNMesh vertices.
    std::vector<int> coreToMaxFace_;
    std::vector<int> maxToCoreFace_;  // -1 for dead/skipped MNMesh faces.
    int numVerts_ = 0;
    int numFaces_ = 0;
};

inline sculpt::Vec3 ToVec3(const Point3& p) { return {p.x, p.y, p.z}; }
inline Point3 ToPoint3(const sculpt::Vec3& v) { return Point3(v.x, v.y, v.z); }
