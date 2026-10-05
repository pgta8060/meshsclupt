// SculptCore — a complete polygon mesh with its per-face, per-vertex and
// per-corner attributes. Used by everything that changes topology
// (Multires levels, cutting, remeshing) so those operations keep UVs,
// material IDs, smoothing groups, SculptGroups, visibility and the mask.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "sculpt/math.h"
#include "sculpt/mesh.h"

namespace sculpt {

// A host map channel (UVs, vertex colours, ...) stored per face corner,
// parallel to PolyData::faceVerts. Seams are simply corners of the same
// vertex with different values.
struct CornerMap {
    int channel = 1;
    std::vector<Vec3> values;
};

struct PolyData {
    std::vector<Vec3> positions;
    std::vector<std::uint32_t> faceSizes;  // Corner count per polygon (>= 3).
    std::vector<std::uint32_t> faceVerts;  // Polygon corners, concatenated.

    // Per polygon. Empty arrays mean "all default".
    std::vector<std::uint16_t> material;
    std::vector<std::uint32_t> smoothing;
    std::vector<std::int32_t> groups;
    std::vector<std::uint8_t> hidden;

    // Per vertex. Empty means "unmasked".
    std::vector<float> mask;

    std::vector<CornerMap> maps;

    std::size_t vertexCount() const { return positions.size(); }
    std::size_t faceCount() const { return faceSizes.size(); }
    std::size_t cornerCount() const { return faceVerts.size(); }

    // Start corner of every face plus a final entry (= cornerCount).
    std::vector<std::uint32_t> faceStarts() const;
    // Checks sizes and indices. Every array must be empty or full size.
    bool valid(std::string* error = nullptr) const;
    // Resizes the optional arrays to full size (defaults: 0, mask 0).
    void fillDefaults();
    // Fan-triangulated mesh input for a SculptSession.
    MeshInput toMeshInput() const;
    std::size_t memoryBytes() const;
};

// Undirected edges of a polygon mesh.
struct EdgeTable {
    std::vector<std::array<std::uint32_t, 2>> verts;  // verts[e][0] < verts[e][1]
    std::vector<std::uint32_t> cornerEdge;             // Edge from corner c to the next corner of its face.
    std::vector<std::uint32_t> faceUses;               // Faces using each edge.
    std::vector<std::array<std::uint32_t, 2>> faces;   // First two faces (kNone if absent).
    static constexpr std::uint32_t kNone = 0xffffffffu;

    void build(const PolyData& poly);
    std::size_t size() const { return verts.size(); }
};

// Compressed-row adjacency: vertices sharing a polygon edge.
struct VertexAdjacency {
    std::vector<std::uint32_t> offsets;  // vertexCount + 1
    std::vector<std::uint32_t> neighbors;
    void build(const PolyData& poly);
    std::size_t count(std::uint32_t v) const { return offsets[v + 1] - offsets[v]; }
};

// Area-weighted vertex normals of a polygon mesh (Newell face normals).
void polyVertexNormals(const PolyData& topology, const std::vector<Vec3>& positions, std::vector<Vec3>& normals);

}  // namespace sculpt
