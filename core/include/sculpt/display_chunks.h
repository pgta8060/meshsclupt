// SculptCore — splits a mesh into spatially compact display chunks so a host
// can keep one GPU buffer per chunk and re-upload only the chunks a stroke touched.
#pragma once

#include <cstdint>
#include <vector>

#include "sculpt/mesh.h"
#include "sculpt/span.h"

namespace sculpt {

struct DisplayChunk {
    std::vector<std::uint32_t> vertices;   // Mesh vertex of each local vertex.
    std::vector<std::uint32_t> triangles;  // Local vertex indices, 3 per triangle.
    std::vector<std::uint32_t> edges;      // Local vertex indices, 2 per polygon edge (no diagonals).
};

class DisplayChunks {
public:
    static constexpr std::uint32_t kDefaultTrianglesPerChunk = 16384;

    // Groups whole polygons (in Morton order of their centroids) into chunks
    // of about `trianglesPerChunk` triangles. Polygons flagged non-zero in
    // `hiddenFaces` (indexed by polygon, may be null) are left out.
    void build(const Mesh& mesh, std::uint32_t trianglesPerChunk = kDefaultTrianglesPerChunk,
               const std::vector<std::uint8_t>* hiddenFaces = nullptr);
    void clear();

    const std::vector<DisplayChunk>& chunks() const { return chunks_; }

    // Chunks that contain a copy of vertex v.
    Span<std::uint32_t> chunksOfVertex(std::uint32_t v) const {
        return {vertexChunks_.data() + vertexChunkOffsets_[v], vertexChunkOffsets_[v + 1] - vertexChunkOffsets_[v]};
    }

    // Dirty tracking for incremental uploads.
    void markVertices(Span<std::uint32_t> vertices);
    void markAll();
    bool anyDirty() const { return !dirty_.empty(); }
    // Returns the dirty chunks (each once) and clears the dirty state.
    std::vector<std::uint32_t> takeDirty();

private:
    void markChunk(std::uint32_t c);

    std::vector<DisplayChunk> chunks_;
    std::vector<std::uint32_t> vertexChunkOffsets_;  // vertexCount + 1
    std::vector<std::uint32_t> vertexChunks_;
    std::vector<std::uint8_t> dirtyFlag_;
    std::vector<std::uint32_t> dirty_;
};

}  // namespace sculpt
