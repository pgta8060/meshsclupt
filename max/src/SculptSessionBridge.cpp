#include "SculptSessionBridge.h"

#include <algorithm>
#include <string>

void SculptAttributes::Fit(const MNMesh& mesh) {
    mask.resize(static_cast<std::size_t>(std::max<int>(mesh.numv, 0)), 0.0f);
    groups.resize(static_cast<std::size_t>(std::max<int>(mesh.numf, 0)), 0);
    hidden.resize(static_cast<std::size_t>(std::max<int>(mesh.numf, 0)), 0u);
}

bool SculptAttributes::Matches(const MNMesh& mesh) const {
    return mask.size() == static_cast<std::size_t>(std::max<int>(mesh.numv, 0)) &&
           groups.size() == static_cast<std::size_t>(std::max<int>(mesh.numf, 0)) &&
           hidden.size() == static_cast<std::size_t>(std::max<int>(mesh.numf, 0));
}

bool SculptSessionBridge::Build(const MNMesh& mesh, const SculptAttributes& attributes, MSTR& error) {
    session_.clear();
    coreToMaxVert_.clear();
    coreToMaxFace_.clear();
    maxToCoreVert_.assign(static_cast<std::size_t>(std::max<int>(mesh.numv, 0)), -1);
    maxToCoreFace_.assign(static_cast<std::size_t>(std::max<int>(mesh.numf, 0)), -1);
    numVerts_ = mesh.numv;
    numFaces_ = mesh.numf;
    const bool haveAttributes = attributes.Matches(mesh);

    sculpt::MeshInput input;
    sculpt::SessionAttributes coreAttributes;
    for (int i = 0; i < mesh.numv; ++i) {
        if (mesh.v[i].GetFlag(MN_DEAD)) continue;
        maxToCoreVert_[i] = static_cast<int>(input.positions.size());
        coreToMaxVert_.push_back(i);
        input.positions.push_back(ToVec3(mesh.v[i].p));
        coreAttributes.mask.push_back(haveAttributes ? attributes.mask[i] : 0.0f);
    }

    Tab<int> corners;
    for (int f = 0; f < mesh.numf; ++f) {
        const MNFace& face = mesh.f[f];
        if (face.GetFlag(MN_DEAD) || face.deg < 3 || !face.vtx) continue;
        // Skip faces that reference dead or out-of-range vertices rather than
        // trusting a damaged mesh.
        bool valid = true;
        for (int k = 0; k < face.deg && valid; ++k) {
            const int v = face.vtx[k];
            valid = v >= 0 && v < mesh.numv && maxToCoreVert_[v] >= 0;
        }
        if (!valid) continue;

        const auto polyIndex = static_cast<std::uint32_t>(input.faceSizes.size());
        maxToCoreFace_[f] = static_cast<int>(polyIndex);
        coreToMaxFace_.push_back(f);
        coreAttributes.faceGroups.push_back(haveAttributes ? attributes.groups[f] : 0);
        coreAttributes.hiddenFaces.push_back(haveAttributes ? attributes.hidden[f] : 0u);
        input.faceSizes.push_back(static_cast<std::uint32_t>(face.deg));
        for (int k = 0; k < face.deg; ++k)
            input.faceVerts.push_back(static_cast<std::uint32_t>(maxToCoreVert_[face.vtx[k]]));

        // Prefer the MNMesh triangulation (handles concave polygons); fall
        // back to a fan if it looks unusable.
        face.GetTriangles(corners);
        bool cornersOk = corners.Count() == 3 * (face.deg - 2);
        for (int k = 0; k < corners.Count() && cornersOk; ++k) cornersOk = corners[k] >= 0 && corners[k] < face.deg;
        for (int t = 0; t < face.deg - 2; ++t) {
            for (int c = 0; c < 3; ++c) {
                const int corner = cornersOk ? corners[3 * t + c] : (c == 0 ? 0 : t + c);
                input.triVerts.push_back(static_cast<std::uint32_t>(maxToCoreVert_[face.vtx[corner]]));
            }
            input.triFaces.push_back(polyIndex);
        }
    }

    std::string coreError;
    if (!session_.build(std::move(input), &coreError, std::move(coreAttributes))) {
        error = MSTR::FromUTF8(coreError.c_str());
        coreToMaxVert_.clear();
        coreToMaxFace_.clear();
        maxToCoreVert_.clear();
        maxToCoreFace_.clear();
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
    positions.reserve(coreToMaxVert_.size());
    for (int maxIndex : coreToMaxVert_) positions.push_back(ToVec3(mesh.v[maxIndex].p));
    return session_.setPositions(positions);
}

unsigned SculptSessionBridge::PushDirty(MNMesh& mesh, SculptAttributes& attributes) {
    unsigned result = 0u;
    const sculpt::Mesh& core = session_.mesh();
    for (std::uint32_t v : session_.dirtyVertices()) {
        const int m = coreToMaxVert_[v];
        if (m >= 0 && m < mesh.numv) mesh.v[m].p = ToPoint3(core.position(v));
        result |= kPositions;
    }
    session_.clearDirty();

    attributes.Fit(mesh);
    for (std::uint32_t v : session_.maskDirtyVertices()) {
        const int m = coreToMaxVert_[v];
        if (m >= 0 && static_cast<std::size_t>(m) < attributes.mask.size()) attributes.mask[m] = session_.mask()[v];
        result |= kMask;
    }
    // Mask changes also need a display refresh: the host reads them first.
    for (std::uint32_t f : session_.groupDirtyFaces()) {
        const int m = coreToMaxFace_[f];
        if (m >= 0 && static_cast<std::size_t>(m) < attributes.groups.size()) attributes.groups[m] = session_.faceGroups()[f];
        result |= kGroups;
    }
    session_.clearGroupDirty();
    if (session_.visibilityDirty()) {
        for (std::size_t f = 0; f < coreToMaxFace_.size(); ++f) {
            const int m = coreToMaxFace_[f];
            if (m >= 0 && static_cast<std::size_t>(m) < attributes.hidden.size()) attributes.hidden[m] = session_.hiddenFaces()[f];
        }
        result |= kVisibility;
    }
    return result;
}

sculpt::StrokeDelta SculptSessionBridge::ToMax(const sculpt::StrokeDelta& d) const {
    sculpt::StrokeDelta out = d;
    for (std::uint32_t& v : out.vertices) v = static_cast<std::uint32_t>(coreToMaxVert_[v]);
    for (std::uint32_t& v : out.maskVertices) v = static_cast<std::uint32_t>(coreToMaxVert_[v]);
    for (std::uint32_t& f : out.groupFaces) f = static_cast<std::uint32_t>(coreToMaxFace_[f]);
    if (d.hasHidden) {
        out.hiddenBefore.assign(static_cast<std::size_t>(std::max(numFaces_, 0)), 0u);
        out.hiddenAfter.assign(static_cast<std::size_t>(std::max(numFaces_, 0)), 0u);
        for (std::size_t f = 0; f < coreToMaxFace_.size() && f < d.hiddenBefore.size(); ++f) {
            out.hiddenBefore[coreToMaxFace_[f]] = d.hiddenBefore[f];
            out.hiddenAfter[coreToMaxFace_[f]] = d.hiddenAfter[f];
        }
    }
    return out;
}

bool SculptSessionBridge::ToCore(const sculpt::StrokeDelta& d, sculpt::StrokeDelta& out) const {
    out = d;
    auto vert = [&](std::uint32_t& v) {
        if (v >= maxToCoreVert_.size() || maxToCoreVert_[v] < 0) return false;
        v = static_cast<std::uint32_t>(maxToCoreVert_[v]);
        return true;
    };
    for (std::uint32_t& v : out.vertices)
        if (!vert(v)) return false;
    for (std::uint32_t& v : out.maskVertices)
        if (!vert(v)) return false;
    for (std::uint32_t& f : out.groupFaces) {
        if (f >= maxToCoreFace_.size() || maxToCoreFace_[f] < 0) return false;
        f = static_cast<std::uint32_t>(maxToCoreFace_[f]);
    }
    if (d.hasHidden) {
        if (d.hiddenBefore.size() != maxToCoreFace_.size()) return false;
        out.hiddenBefore.assign(coreToMaxFace_.size(), 0u);
        out.hiddenAfter.assign(coreToMaxFace_.size(), 0u);
        for (std::size_t f = 0; f < coreToMaxFace_.size(); ++f) {
            out.hiddenBefore[f] = d.hiddenBefore[coreToMaxFace_[f]];
            out.hiddenAfter[f] = d.hiddenAfter[coreToMaxFace_[f]];
        }
    }
    return true;
}

std::vector<std::uint64_t> SculptSessionBridge::FaceKeys(const MNMesh& mesh, sculpt::AutoGroupMode mode) const {
    std::vector<std::uint64_t> keys(coreToMaxFace_.size(), 0u);
    if (mode == sculpt::AutoGroupMode::UVIslands) {
        // UV islands: polygons connected through shared map vertices of channel 1.
        MNMap* map = (mesh.MNum() > 1) ? mesh.M(1) : nullptr;
        if (!map || map->GetFlag(MN_DEAD) || map->numf != mesh.numf) return keys;
        std::vector<int> parent(static_cast<std::size_t>(std::max<int>(map->numv, 0)));
        for (std::size_t i = 0; i < parent.size(); ++i) parent[i] = static_cast<int>(i);
        auto find = [&](int x) {
            while (parent[x] != x) x = parent[x] = parent[parent[x]];
            return x;
        };
        for (int f = 0; f < map->numf; ++f) {
            const MNMapFace& mf = map->f[f];
            for (int k = 1; k < mf.deg; ++k) {
                const int a = find(mf.tv[0]), b = find(mf.tv[k]);
                if (a != b) parent[a] = b;
            }
        }
        for (std::size_t f = 0; f < coreToMaxFace_.size(); ++f) {
            const MNMapFace& mf = map->f[coreToMaxFace_[f]];
            keys[f] = mf.deg > 0 ? static_cast<std::uint64_t>(find(mf.tv[0])) + 1u : 0u;
        }
        return keys;
    }
    for (std::size_t f = 0; f < coreToMaxFace_.size(); ++f) {
        const MNFace& face = mesh.f[coreToMaxFace_[f]];
        keys[f] = mode == sculpt::AutoGroupMode::MaterialIDs ? static_cast<std::uint64_t>(face.material)
                                                             : static_cast<std::uint64_t>(face.smGroup);
    }
    return keys;
}
