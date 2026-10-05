#include "sculpt/poly.h"

#include <algorithm>

namespace sculpt {

std::vector<std::uint32_t> PolyData::faceStarts() const {
    std::vector<std::uint32_t> starts(faceSizes.size() + 1, 0u);
    for (std::size_t f = 0; f < faceSizes.size(); ++f) starts[f + 1] = starts[f] + faceSizes[f];
    return starts;
}

bool PolyData::valid(std::string* error) const {
    auto fail = [error](const char* message) {
        if (error) *error = message;
        return false;
    };
    const std::size_t V = positions.size(), F = faceSizes.size();
    std::size_t corners = 0;
    for (std::uint32_t n : faceSizes) {
        if (n < 3) return fail("polygon with fewer than 3 corners");
        corners += n;
    }
    if (corners != faceVerts.size()) return fail("face sizes do not match the corner list");
    for (std::uint32_t v : faceVerts)
        if (v >= V) return fail("corner references a missing vertex");
    for (const Vec3& p : positions)
        if (!isFinite(p)) return fail("vertex position is not finite");
    if (!material.empty() && material.size() != F) return fail("material array size");
    if (!smoothing.empty() && smoothing.size() != F) return fail("smoothing array size");
    if (!groups.empty() && groups.size() != F) return fail("groups array size");
    if (!hidden.empty() && hidden.size() != F) return fail("hidden array size");
    if (!mask.empty() && mask.size() != V) return fail("mask array size");
    for (const CornerMap& map : maps)
        if (map.values.size() != corners) return fail("map channel size");
    return true;
}

void PolyData::fillDefaults() {
    const std::size_t V = positions.size(), F = faceSizes.size();
    material.resize(F, 0);
    smoothing.resize(F, 0u);
    groups.resize(F, 0);
    hidden.resize(F, 0u);
    mask.resize(V, 0.0f);
}

MeshInput PolyData::toMeshInput() const {
    MeshInput input;
    input.positions = positions;
    input.faceSizes = faceSizes;
    input.faceVerts = faceVerts;
    return input;  // Fan triangulation.
}

std::size_t PolyData::memoryBytes() const {
    std::size_t bytes = positions.size() * sizeof(Vec3) + faceSizes.size() * 4 + faceVerts.size() * 4 +
                        material.size() * 2 + smoothing.size() * 4 + groups.size() * 4 + hidden.size() + mask.size() * 4;
    for (const CornerMap& map : maps) bytes += map.values.size() * sizeof(Vec3);
    return bytes;
}

void EdgeTable::build(const PolyData& poly) {
    const std::size_t V = poly.positions.size();
    const std::size_t C = poly.faceVerts.size();
    const std::vector<std::uint32_t> starts = poly.faceStarts();
    verts.clear();
    faceUses.clear();
    faces.clear();
    cornerEdge.assign(C, kNone);

    // Bucket every corner edge by its lower vertex (counting sort), then
    // number the unique edges per bucket. Deterministic and linear.
    std::vector<std::uint32_t> bucketStart(V + 1, 0u);
    auto cornerEnds = [&](std::size_t f, std::uint32_t k, std::uint32_t& a, std::uint32_t& b) {
        const std::uint32_t n = poly.faceSizes[f];
        a = poly.faceVerts[starts[f] + k];
        b = poly.faceVerts[starts[f] + (k + 1) % n];
    };
    for (std::size_t f = 0; f < poly.faceSizes.size(); ++f)
        for (std::uint32_t k = 0; k < poly.faceSizes[f]; ++k) {
            std::uint32_t a, b;
            cornerEnds(f, k, a, b);
            ++bucketStart[std::min(a, b) + 1];
        }
    for (std::size_t v = 0; v < V; ++v) bucketStart[v + 1] += bucketStart[v];
    std::vector<std::uint32_t> fill(bucketStart.begin(), bucketStart.end() - 1);
    std::vector<std::uint32_t> bucketCorner(C);
    std::vector<std::uint32_t> cornerFace(C);
    for (std::size_t f = 0; f < poly.faceSizes.size(); ++f)
        for (std::uint32_t k = 0; k < poly.faceSizes[f]; ++k) {
            std::uint32_t a, b;
            cornerEnds(f, k, a, b);
            const std::uint32_t c = starts[f] + k;
            bucketCorner[fill[std::min(a, b)]++] = c;
            cornerFace[c] = static_cast<std::uint32_t>(f);
        }

    for (std::size_t v = 0; v < V; ++v) {
        const std::size_t firstEdge = verts.size();
        for (std::uint32_t i = bucketStart[v]; i < bucketStart[v + 1]; ++i) {
            const std::uint32_t c = bucketCorner[i];
            const std::uint32_t f = cornerFace[c];
            const std::uint32_t k = c - starts[f];
            std::uint32_t a, b;
            cornerEnds(f, k, a, b);
            const std::uint32_t other = std::max(a, b);
            std::uint32_t edge = kNone;
            for (std::size_t e = firstEdge; e < verts.size(); ++e)
                if (verts[e][1] == other) {
                    edge = static_cast<std::uint32_t>(e);
                    break;
                }
            if (edge == kNone) {
                edge = static_cast<std::uint32_t>(verts.size());
                verts.push_back({static_cast<std::uint32_t>(v), other});
                faceUses.push_back(0u);
                faces.push_back({kNone, kNone});
            }
            cornerEdge[c] = edge;
            if (faceUses[edge] < 2) faces[edge][faceUses[edge]] = f;
            ++faceUses[edge];
        }
    }
}

void VertexAdjacency::build(const PolyData& poly) {
    EdgeTable edges;
    edges.build(poly);
    const std::size_t V = poly.positions.size();
    offsets.assign(V + 1, 0u);
    for (const auto& e : edges.verts) {
        if (e[0] == e[1]) continue;
        ++offsets[e[0] + 1];
        ++offsets[e[1] + 1];
    }
    for (std::size_t v = 0; v < V; ++v) offsets[v + 1] += offsets[v];
    neighbors.assign(offsets[V], 0u);
    std::vector<std::uint32_t> fill(offsets.begin(), offsets.end() - 1);
    for (const auto& e : edges.verts) {
        if (e[0] == e[1]) continue;
        neighbors[fill[e[0]]++] = e[1];
        neighbors[fill[e[1]]++] = e[0];
    }
}

void polyVertexNormals(const PolyData& topology, const std::vector<Vec3>& positions, std::vector<Vec3>& normals) {
    normals.assign(positions.size(), Vec3());
    std::size_t c = 0;
    for (std::uint32_t n : topology.faceSizes) {
        Vec3 newell;
        for (std::uint32_t k = 0; k < n; ++k) {
            const Vec3& a = positions[topology.faceVerts[c + k]];
            const Vec3& b = positions[topology.faceVerts[c + (k + 1) % n]];
            newell.x += (a.y - b.y) * (a.z + b.z);
            newell.y += (a.z - b.z) * (a.x + b.x);
            newell.z += (a.x - b.x) * (a.y + b.y);
        }
        for (std::uint32_t k = 0; k < n; ++k) normals[topology.faceVerts[c + k]] += newell;
        c += n;
    }
    for (Vec3& n : normals) n = normalizedOrZero(n);
}

}  // namespace sculpt
