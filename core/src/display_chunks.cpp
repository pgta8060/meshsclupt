#include "sculpt/display_chunks.h"

#include <algorithm>
#include <utility>

namespace sculpt {
namespace {

// Spreads the low 10 bits of x so they occupy every third bit.
std::uint32_t spreadBits(std::uint32_t x) {
    x &= 0x3ffu;
    x = (x | (x << 16)) & 0x030000ffu;
    x = (x | (x << 8)) & 0x0300f00fu;
    x = (x | (x << 4)) & 0x030c30c3u;
    x = (x | (x << 2)) & 0x09249249u;
    return x;
}

std::uint32_t mortonCode(const Vec3& p, const Aabb& box) {
    const Vec3 e = box.extent();
    auto quantize = [](float v, float lo, float extent) {
        if (!(extent > 0.0f)) return 0u;
        const float t = clamp01((v - lo) / extent);
        return static_cast<std::uint32_t>(t * 1023.0f);
    };
    return (spreadBits(quantize(p.x, box.lo.x, e.x)) << 2) | (spreadBits(quantize(p.y, box.lo.y, e.y)) << 1) |
           spreadBits(quantize(p.z, box.lo.z, e.z));
}

}  // namespace

void DisplayChunks::clear() {
    chunks_.clear();
    vertexChunkOffsets_.clear();
    vertexChunks_.clear();
    dirtyFlag_.clear();
    dirty_.clear();
}

void DisplayChunks::build(const Mesh& mesh, std::uint32_t trianglesPerChunk, const std::vector<std::uint8_t>* hiddenFaces) {
    clear();
    const std::uint32_t V = mesh.vertexCount();
    const std::uint32_t F = mesh.faceCount();
    const std::uint32_t T = mesh.triangleCount();
    vertexChunkOffsets_.assign(static_cast<std::size_t>(V) + 1, 0u);
    if (V == 0 || F == 0) return;
    if (trianglesPerChunk == 0) trianglesPerChunk = kDefaultTrianglesPerChunk;

    // Polygon -> triangles (CSR); triangles of a polygon need not be contiguous.
    std::vector<std::uint32_t> faceTriOffsets(static_cast<std::size_t>(F) + 1, 0u);
    for (std::uint32_t t = 0; t < T; ++t) ++faceTriOffsets[mesh.triangleFace(t) + 1];
    for (std::uint32_t f = 0; f < F; ++f) faceTriOffsets[f + 1] += faceTriOffsets[f];
    std::vector<std::uint32_t> faceTris(T);
    {
        std::vector<std::uint32_t> cursor(faceTriOffsets.begin(), faceTriOffsets.end() - 1);
        for (std::uint32_t t = 0; t < T; ++t) faceTris[cursor[mesh.triangleFace(t)]++] = t;
    }

    // Order visible polygons along a Morton curve so chunks are compact.
    std::vector<Vec3> centroids(F);
    Aabb centroidBox;
    for (std::uint32_t f = 0; f < F; ++f) {
        Vec3 c;
        const Span<std::uint32_t> poly = mesh.faceVertices(f);
        for (std::uint32_t v : poly) c += mesh.position(v);
        centroids[f] = c / static_cast<float>(poly.size());
        centroidBox.expand(centroids[f]);
    }
    std::vector<std::pair<std::uint32_t, std::uint32_t>> order;  // (code, face)
    order.reserve(F);
    for (std::uint32_t f = 0; f < F; ++f) {
        if (hiddenFaces && f < hiddenFaces->size() && (*hiddenFaces)[f]) continue;
        order.emplace_back(mortonCode(centroids[f], centroidBox), f);
    }
    std::sort(order.begin(), order.end());

    std::vector<std::uint32_t> localOf(V, 0u);
    std::vector<std::uint32_t> localStamp(V, 0u);  // Chunk index + 1 that owns localOf[v].
    std::vector<std::uint64_t> edgeKeys;

    std::size_t i = 0;
    while (i < order.size()) {
        DisplayChunk chunk;
        const auto stamp = static_cast<std::uint32_t>(chunks_.size() + 1);
        auto local = [&](std::uint32_t v) {
            if (localStamp[v] != stamp) {
                localStamp[v] = stamp;
                localOf[v] = static_cast<std::uint32_t>(chunk.vertices.size());
                chunk.vertices.push_back(v);
            }
            return localOf[v];
        };
        edgeKeys.clear();
        std::uint32_t triCount = 0;
        while (i < order.size() && (triCount == 0 || triCount < trianglesPerChunk)) {
            const std::uint32_t f = order[i++].second;
            for (std::uint32_t k = faceTriOffsets[f]; k < faceTriOffsets[f + 1]; ++k) {
                for (std::uint32_t v : mesh.triangle(faceTris[k])) chunk.triangles.push_back(local(v));
                ++triCount;
            }
            const Span<std::uint32_t> poly = mesh.faceVertices(f);
            for (std::size_t c = 0; c < poly.size(); ++c) {
                std::uint32_t a = local(poly[c]);
                std::uint32_t b = local(poly[(c + 1) % poly.size()]);
                if (a == b) continue;
                if (a > b) std::swap(a, b);
                edgeKeys.push_back((static_cast<std::uint64_t>(a) << 32) | b);
            }
        }
        std::sort(edgeKeys.begin(), edgeKeys.end());
        edgeKeys.erase(std::unique(edgeKeys.begin(), edgeKeys.end()), edgeKeys.end());
        chunk.edges.reserve(edgeKeys.size() * 2);
        for (std::uint64_t key : edgeKeys) {
            chunk.edges.push_back(static_cast<std::uint32_t>(key >> 32));
            chunk.edges.push_back(static_cast<std::uint32_t>(key & 0xffffffffu));
        }
        if (!chunk.triangles.empty()) chunks_.push_back(std::move(chunk));
    }

    // Vertex -> chunks (CSR).
    for (const DisplayChunk& c : chunks_)
        for (std::uint32_t v : c.vertices) ++vertexChunkOffsets_[v + 1];
    for (std::uint32_t v = 0; v < V; ++v) vertexChunkOffsets_[v + 1] += vertexChunkOffsets_[v];
    vertexChunks_.resize(vertexChunkOffsets_[V]);
    std::vector<std::uint32_t> cursor(vertexChunkOffsets_.begin(), vertexChunkOffsets_.end() - 1);
    for (std::uint32_t c = 0; c < chunks_.size(); ++c)
        for (std::uint32_t v : chunks_[c].vertices) vertexChunks_[cursor[v]++] = c;

    dirtyFlag_.assign(chunks_.size(), 0u);
    markAll();
}

void DisplayChunks::markChunk(std::uint32_t c) {
    if (!dirtyFlag_[c]) {
        dirtyFlag_[c] = 1u;
        dirty_.push_back(c);
    }
}

void DisplayChunks::markVertices(Span<std::uint32_t> vertices) {
    const auto V = static_cast<std::uint32_t>(vertexChunkOffsets_.empty() ? 0 : vertexChunkOffsets_.size() - 1);
    for (std::uint32_t v : vertices) {
        if (v >= V) continue;
        for (std::uint32_t c : chunksOfVertex(v)) markChunk(c);
    }
}

void DisplayChunks::markAll() {
    for (std::uint32_t c = 0; c < chunks_.size(); ++c) markChunk(c);
}

std::vector<std::uint32_t> DisplayChunks::takeDirty() {
    std::vector<std::uint32_t> out;
    out.swap(dirty_);
    for (std::uint32_t c : out) dirtyFlag_[c] = 0u;
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace sculpt
