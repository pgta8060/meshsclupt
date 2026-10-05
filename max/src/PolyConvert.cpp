#include "PolyConvert.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr std::uint32_t kNone = 0xffffffffu;
constexpr int kFirstMapChannel = -2;  // Alpha (-2) and illumination (-1) are hidden channels.

sculpt::Vec3 SafeValue(const UVVert& v) {
    return {std::isfinite(v.x) ? v.x : 0.0f, std::isfinite(v.y) ? v.y : 0.0f, std::isfinite(v.z) ? v.z : 0.0f};
}

}  // namespace

std::vector<sculpt::Vec3> AlivePositions(const MNMesh& mesh) {
    std::vector<sculpt::Vec3> out;
    out.reserve(static_cast<std::size_t>(std::max<int>(mesh.numv, 0)));
    for (int v = 0; v < mesh.numv; ++v)
        if (!mesh.v[v].GetFlag(MN_DEAD)) out.push_back(ToVec3(mesh.v[v].p));
    return out;
}

bool MeshToPoly(const MNMesh& mesh, const SculptAttributes& attributes, sculpt::PolyData& out, MSTR* error) {
    out = sculpt::PolyData();
    const int numv = mesh.numv, numf = mesh.numf;
    const bool haveAttributes = attributes.Matches(mesh);
    std::vector<std::uint32_t> index(static_cast<std::size_t>(std::max(numv, 0)), kNone);
    for (int v = 0; v < numv; ++v) {
        if (mesh.v[v].GetFlag(MN_DEAD)) continue;
        index[v] = static_cast<std::uint32_t>(out.positions.size());
        out.positions.push_back(ToVec3(mesh.v[v].p));
        out.mask.push_back(haveAttributes ? attributes.mask[v] : 0.0f);
    }
    std::vector<int> usedFaces;
    for (int f = 0; f < numf; ++f) {
        const MNFace& face = mesh.f[f];
        if (face.GetFlag(MN_DEAD) || face.deg < 3 || !face.vtx) continue;
        bool valid = true;
        for (int k = 0; k < face.deg && valid; ++k)
            valid = face.vtx[k] >= 0 && face.vtx[k] < numv && index[face.vtx[k]] != kNone;
        if (!valid) continue;
        usedFaces.push_back(f);
        out.faceSizes.push_back(static_cast<std::uint32_t>(face.deg));
        for (int k = 0; k < face.deg; ++k) out.faceVerts.push_back(index[face.vtx[k]]);
        out.material.push_back(static_cast<std::uint16_t>(face.material));
        out.smoothing.push_back(static_cast<std::uint32_t>(face.smGroup));
        out.groups.push_back(haveAttributes ? attributes.groups[f] : 0);
        out.hidden.push_back(haveAttributes ? attributes.hidden[f] : 0u);
    }
    if (out.faceSizes.empty()) {
        if (error) *error = _T("The mesh has no polygons.");
        return false;
    }
    // Map channels with one map face per polygon of matching size.
    for (int channel = kFirstMapChannel; channel < mesh.MNum(); ++channel) {
        MNMap* map = mesh.M(channel);
        if (!map || map->GetFlag(MN_DEAD) || map->numf != numf || map->numv <= 0) continue;
        sculpt::CornerMap corners;
        corners.channel = channel;
        corners.values.reserve(out.faceVerts.size());
        bool consistent = true;
        for (int f : usedFaces) {
            const MNMapFace& mf = map->f[f];
            if (mf.deg != mesh.f[f].deg || !mf.tv) {
                consistent = false;
                break;
            }
            for (int k = 0; k < mf.deg; ++k) {
                const int tv = mf.tv[k];
                if (tv < 0 || tv >= map->numv) {
                    consistent = false;
                    break;
                }
                corners.values.push_back(SafeValue(map->v[tv]));
            }
            if (!consistent) break;
        }
        if (consistent) out.maps.push_back(std::move(corners));
    }
    std::string coreError;
    if (!out.valid(&coreError)) {
        if (error) *error = MSTR::FromUTF8(coreError.c_str());
        return false;
    }
    return true;
}

void PolyToMesh(const sculpt::PolyData& poly, MNMesh& mesh, SculptAttributes& attributes) {
    const int V = static_cast<int>(poly.positions.size()), F = static_cast<int>(poly.faceSizes.size());
    mesh.ClearAndFree();
    mesh.setNumVerts(V);
    for (int v = 0; v < V; ++v) {
        mesh.v[v].p = ToPoint3(poly.positions[v]);
        mesh.v[v].ClearFlag(MN_DEAD);
    }
    mesh.setNumFaces(F);
    std::size_t c = 0;
    for (int f = 0; f < F; ++f) {
        MNFace& face = mesh.f[f];
        const int n = static_cast<int>(poly.faceSizes[f]);
        face.Init();
        face.SetDeg(n);
        for (int k = 0; k < n; ++k) face.vtx[k] = static_cast<int>(poly.faceVerts[c + k]);
        face.material = poly.material.empty() ? 0 : poly.material[f];
        face.smGroup = poly.smoothing.empty() ? 0u : poly.smoothing[f];
        c += static_cast<std::size_t>(n);
    }

    // Maps: weld corners that share a vertex and a value.
    int highest = 1;
    for (const sculpt::CornerMap& map : poly.maps) highest = std::max(highest, map.channel);
    mesh.SetMapNum(highest + 1);
    for (int channel = 0; channel <= highest; ++channel) mesh.ClearMap(channel);
    for (const sculpt::CornerMap& corners : poly.maps) {
        if (corners.channel < kFirstMapChannel || corners.values.size() != poly.faceVerts.size()) continue;
        MNMap* map = mesh.M(corners.channel);
        if (!map) continue;
        std::vector<std::uint32_t> first(static_cast<std::size_t>(V), kNone);
        std::vector<std::uint32_t> next;
        std::vector<sculpt::Vec3> values;
        std::vector<int> cornerIndex(corners.values.size());
        for (std::size_t corner = 0; corner < poly.faceVerts.size(); ++corner) {
            const std::uint32_t v = poly.faceVerts[corner];
            const sculpt::Vec3& value = corners.values[corner];
            std::uint32_t found = kNone;
            for (std::uint32_t i = first[v]; i != kNone; i = next[i])
                if (values[i] == value) {
                    found = i;
                    break;
                }
            if (found == kNone) {
                found = static_cast<std::uint32_t>(values.size());
                values.push_back(value);
                next.push_back(first[v]);
                first[v] = found;
            }
            cornerIndex[corner] = static_cast<int>(found);
        }
        map->ClearFlag(MN_DEAD);
        map->setNumVerts(static_cast<int>(values.size()));
        for (std::size_t i = 0; i < values.size(); ++i) map->v[i] = UVVert(values[i].x, values[i].y, values[i].z);
        map->setNumFaces(F);
        c = 0;
        for (int f = 0; f < F; ++f) {
            const int n = static_cast<int>(poly.faceSizes[f]);
            map->f[f].SetSize(n);
            for (int k = 0; k < n; ++k) map->f[f].tv[k] = cornerIndex[c + k];
            c += static_cast<std::size_t>(n);
        }
    }

    mesh.FillInMesh();
    for (int f = 0; f < F; ++f) mesh.RetriangulateFace(f);
    mesh.InvalidateGeomCache();
    mesh.InvalidateTopoCache();

    attributes.mask = poly.mask.size() == static_cast<std::size_t>(V) ? poly.mask : std::vector<float>(V, 0.0f);
    attributes.groups = poly.groups.size() == static_cast<std::size_t>(F) ? poly.groups : std::vector<std::int32_t>(F, 0);
    attributes.hidden = poly.hidden.size() == static_cast<std::size_t>(F) ? poly.hidden : std::vector<std::uint8_t>(F, 0u);
}
