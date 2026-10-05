#include "SculptSessionBridge.h"

#include <string>

bool SculptSessionBridge::Build(const MNMesh& mesh, MSTR& error) {
    session_.clear();
    coreToMax_.clear();
    maxToCore_.assign(mesh.numv > 0 ? static_cast<std::size_t>(mesh.numv) : 0u, -1);
    numVerts_ = mesh.numv;
    numFaces_ = mesh.numf;

    sculpt::MeshInput input;
    input.positions.reserve(maxToCore_.size());
    for (int i = 0; i < mesh.numv; ++i) {
        if (mesh.v[i].GetFlag(MN_DEAD)) continue;
        maxToCore_[i] = static_cast<int>(input.positions.size());
        coreToMax_.push_back(i);
        input.positions.push_back(ToVec3(mesh.v[i].p));
    }

    Tab<int> corners;
    for (int f = 0; f < mesh.numf; ++f) {
        const MNFace& face = mesh.f[f];
        if (face.GetFlag(MN_DEAD) || face.deg < 3 || !face.vtx) continue;

        // Skip faces that reference dead or out-of-range vertices rather
        // than trusting a damaged mesh.
        bool valid = true;
        for (int k = 0; k < face.deg && valid; ++k) {
            const int v = face.vtx[k];
            valid = v >= 0 && v < mesh.numv && maxToCore_[v] >= 0;
        }
        if (!valid) continue;

        const auto polyIndex = static_cast<std::uint32_t>(input.faceSizes.size());
        input.faceSizes.push_back(static_cast<std::uint32_t>(face.deg));
        for (int k = 0; k < face.deg; ++k) input.faceVerts.push_back(static_cast<std::uint32_t>(maxToCore_[face.vtx[k]]));

        // Prefer the MNMesh triangulation (handles concave polygons); fall
        // back to a fan if it looks unusable.
        face.GetTriangles(corners);
        const int expected = 3 * (face.deg - 2);
        bool cornersOk = corners.Count() == expected;
        for (int k = 0; k < corners.Count() && cornersOk; ++k) cornersOk = corners[k] >= 0 && corners[k] < face.deg;
        for (int t = 0; t < face.deg - 2; ++t) {
            for (int c = 0; c < 3; ++c) {
                const int corner = cornersOk ? corners[3 * t + c] : (c == 0 ? 0 : t + c);
                input.triVerts.push_back(static_cast<std::uint32_t>(maxToCore_[face.vtx[corner]]));
            }
            input.triFaces.push_back(polyIndex);
        }
    }

    std::string coreError;
    if (!session_.build(std::move(input), &coreError)) {
        error = MSTR::FromUTF8(coreError.c_str());
        coreToMax_.clear();
        maxToCore_.clear();
        numVerts_ = numFaces_ = 0;
        return false;
    }
    return true;
}

bool SculptSessionBridge::Matches(const MNMesh& mesh) const {
    return session_.valid() && mesh.numv == numVerts_ && mesh.numf == numFaces_;
}

bool SculptSessionBridge::SyncFromMesh(const MNMesh& mesh) {
    if (!Matches(mesh)) return false;
    std::vector<sculpt::Vec3> positions;
    positions.reserve(coreToMax_.size());
    for (int maxIndex : coreToMax_) positions.push_back(ToVec3(mesh.v[maxIndex].p));
    return session_.setPositions(positions);
}

std::size_t SculptSessionBridge::PushDirty(MNMesh& mesh) {
    const std::vector<std::uint32_t>& dirty = session_.dirtyVertices();
    const std::size_t count = dirty.size();
    const sculpt::Mesh& core = session_.mesh();
    for (std::uint32_t v : dirty) {
        const int maxIndex = coreToMax_[v];
        if (maxIndex >= 0 && maxIndex < mesh.numv) mesh.v[maxIndex].p = ToPoint3(core.position(v));
    }
    session_.clearDirty();
    return count;
}

bool SculptSessionBridge::ApplyMaxPositions(const std::vector<int>& maxIndices, const std::vector<Point3>& positions) {
    if (!session_.valid() || maxIndices.size() != positions.size()) return false;
    sculpt::StrokeDelta delta;
    delta.vertices.reserve(maxIndices.size());
    delta.before.reserve(maxIndices.size());
    for (std::size_t i = 0; i < maxIndices.size(); ++i) {
        const int maxIndex = maxIndices[i];
        if (maxIndex < 0 || maxIndex >= static_cast<int>(maxToCore_.size()) || maxToCore_[maxIndex] < 0) return false;
        delta.vertices.push_back(static_cast<std::uint32_t>(maxToCore_[maxIndex]));
        delta.before.push_back(ToVec3(positions[i]));
    }
    delta.after = delta.before;
    if (!session_.applyDelta(delta, true)) return false;
    session_.clearDirty();  // The caller already wrote these positions into the mesh.
    return true;
}
