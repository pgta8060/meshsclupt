// SculptCore — editable polygon mesh with the derived data sculpting needs:
// a triangulation (for ray casting and normals), vertex adjacency (for
// smoothing), open-border detection and incrementally updated vertex normals.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "sculpt/math.h"
#include "sculpt/span.h"
#include "sculpt/visit_set.h"

namespace sculpt {

// Mesh description handed over by the host application.
struct MeshInput {
    std::vector<Vec3> positions;
    std::vector<std::uint32_t> faceSizes;  // Vertex count of each polygon (>= 3).
    std::vector<std::uint32_t> faceVerts;  // Polygon vertex indices, concatenated.

    // Optional explicit triangulation (e.g. the host's own polygon triangulation).
    // When empty, every polygon is fan-triangulated.
    std::vector<std::uint32_t> triVerts;  // 3 vertex indices per triangle.
    std::vector<std::uint32_t> triFaces;  // Owning polygon of each triangle.
};

class Mesh {
public:
    // Validates the input and builds all derived data. On failure the mesh is
    // left empty, false is returned and *error (if given) explains why.
    bool build(MeshInput input, std::string* error = nullptr);
    void clear();
    bool empty() const { return positions_.empty(); }

    std::uint32_t vertexCount() const { return static_cast<std::uint32_t>(positions_.size()); }
    std::uint32_t faceCount() const { return static_cast<std::uint32_t>(faceOffsets_.empty() ? 0 : faceOffsets_.size() - 1); }
    std::uint32_t triangleCount() const { return static_cast<std::uint32_t>(triVerts_.size() / 3); }

    const Vec3& position(std::uint32_t v) const { return positions_[v]; }
    const std::vector<Vec3>& positions() const { return positions_; }

    // Moves a vertex. Normals are not refreshed here: call updateNormals() with
    // the moved vertices once a batch of edits is complete.
    void setPosition(std::uint32_t v, const Vec3& p) { positions_[v] = p; }

    // Unit vertex normal (area weighted), or zero for vertices without area.
    const Vec3& normal(std::uint32_t v) const { return normals_[v]; }
    const std::vector<Vec3>& normals() const { return normals_; }

    std::array<std::uint32_t, 3> triangle(std::uint32_t t) const {
        return {triVerts_[3 * t], triVerts_[3 * t + 1], triVerts_[3 * t + 2]};
    }
    std::uint32_t triangleFace(std::uint32_t t) const { return triFaces_[t]; }
    // Unnormalised geometric normal of a triangle (length = 2 * area).
    const Vec3& triangleNormal(std::uint32_t t) const { return triNormals_[t]; }

    Span<std::uint32_t> faceVertices(std::uint32_t f) const {
        return {faceVerts_.data() + faceOffsets_[f], faceOffsets_[f + 1] - faceOffsets_[f]};
    }
    // Vertices sharing a polygon edge with v (no duplicates, no v itself).
    Span<std::uint32_t> neighbors(std::uint32_t v) const {
        return {adjacency_.data() + adjacencyOffsets_[v], adjacencyOffsets_[v + 1] - adjacencyOffsets_[v]};
    }
    // Vertices sharing an open-border edge with v. Empty for interior vertices.
    Span<std::uint32_t> borderNeighbors(std::uint32_t v) const {
        return {border_.data() + borderOffsets_[v], borderOffsets_[v + 1] - borderOffsets_[v]};
    }
    bool isBorder(std::uint32_t v) const { return borderOffsets_[v + 1] != borderOffsets_[v]; }

    Span<std::uint32_t> vertexTriangles(std::uint32_t v) const {
        return {vertexTris_.data() + vertexTriOffsets_[v], vertexTriOffsets_[v + 1] - vertexTriOffsets_[v]};
    }

    void recomputeAllNormals();

    // Refreshes every triangle/vertex normal influenced by the moved vertices.
    // The result is bit-identical to recomputeAllNormals().
    void updateNormals(Span<std::uint32_t> movedVertices);

    // Appends (without duplicates) every triangle touching one of `vertices`.
    void collectTriangles(Span<std::uint32_t> vertices, std::vector<std::uint32_t>& out) const;

    Aabb bounds() const;
    std::size_t memoryBytes() const;

private:
    Vec3 computeTriangleNormal(std::uint32_t t) const;
    Vec3 accumulateVertexNormal(std::uint32_t v) const;

    std::vector<Vec3> positions_;
    std::vector<Vec3> normals_;

    std::vector<std::uint32_t> faceOffsets_;  // faceCount + 1
    std::vector<std::uint32_t> faceVerts_;

    std::vector<std::uint32_t> triVerts_;
    std::vector<std::uint32_t> triFaces_;
    std::vector<Vec3> triNormals_;

    std::vector<std::uint32_t> adjacencyOffsets_;  // vertexCount + 1
    std::vector<std::uint32_t> adjacency_;
    std::vector<std::uint32_t> borderOffsets_;  // vertexCount + 1
    std::vector<std::uint32_t> border_;
    std::vector<std::uint32_t> vertexTriOffsets_;  // vertexCount + 1
    std::vector<std::uint32_t> vertexTris_;

    // Scratch for incremental updates (not thread-safe).
    mutable VisitSet triVisit_;
    mutable VisitSet vertVisit_;
    std::vector<std::uint32_t> scratchTris_;
};

}  // namespace sculpt
