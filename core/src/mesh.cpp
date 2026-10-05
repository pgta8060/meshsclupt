#include "sculpt/mesh.h"

#include "sculpt/parallel.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace sculpt {
namespace {

constexpr std::uint64_t kMaxIndexCount = std::numeric_limits<std::uint32_t>::max() - 1u;

bool fail(std::string* error, const char* message) {
    if (error) *error = message;
    return false;
}

// Builds a CSR (offsets + flat values) from per-key counts, then lets the
// caller scatter values through the returned cursor array.
void prefixSum(const std::vector<std::uint32_t>& counts, std::vector<std::uint32_t>& offsets) {
    offsets.assign(counts.size() + 1, 0u);
    for (std::size_t i = 0; i < counts.size(); ++i) offsets[i + 1] = offsets[i] + counts[i];
}

template <class T>
std::size_t bytesOf(const std::vector<T>& v) {
    return v.capacity() * sizeof(T);
}

}  // namespace

void Mesh::clear() {
    *this = Mesh();
}

bool Mesh::build(MeshInput in, std::string* error) {
    clear();

    const std::uint64_t vertexCount = in.positions.size();
    const std::uint64_t faceCount = in.faceSizes.size();
    if (vertexCount == 0) return fail(error, "Mesh has no vertices.");
    if (faceCount == 0) return fail(error, "Mesh has no polygons.");
    if (vertexCount > kMaxIndexCount || faceCount > kMaxIndexCount || in.faceVerts.size() > kMaxIndexCount)
        return fail(error, "Mesh is too large.");

    for (const Vec3& p : in.positions)
        if (!isFinite(p)) return fail(error, "Mesh contains a vertex with an invalid (NaN/infinite) position.");

    std::uint64_t cornerTotal = 0;
    for (std::uint32_t size : in.faceSizes) {
        if (size < 3) return fail(error, "Mesh contains a polygon with fewer than 3 vertices.");
        cornerTotal += size;
    }
    if (cornerTotal != in.faceVerts.size()) return fail(error, "Polygon sizes do not match the polygon vertex list.");
    for (std::uint32_t v : in.faceVerts)
        if (v >= vertexCount) return fail(error, "Polygon references a vertex index out of range.");

    const bool explicitTriangles = !in.triVerts.empty();
    if (explicitTriangles) {
        if (in.triVerts.size() % 3 != 0) return fail(error, "Triangle index count is not a multiple of 3.");
        if (in.triFaces.size() != in.triVerts.size() / 3)
            return fail(error, "Triangle-to-polygon map does not match the triangle count.");
        for (std::uint32_t v : in.triVerts)
            if (v >= vertexCount) return fail(error, "Triangle references a vertex index out of range.");
        for (std::uint32_t f : in.triFaces)
            if (f >= faceCount) return fail(error, "Triangle references a polygon index out of range.");
    } else if (!in.triFaces.empty()) {
        return fail(error, "Triangle-to-polygon map given without triangles.");
    }

    const auto V = static_cast<std::uint32_t>(vertexCount);
    const auto F = static_cast<std::uint32_t>(faceCount);

    positions_ = std::move(in.positions);
    faceVerts_ = std::move(in.faceVerts);
    prefixSum(in.faceSizes, faceOffsets_);

    // --- Triangulation -----------------------------------------------------
    if (explicitTriangles) {
        triVerts_ = std::move(in.triVerts);
        triFaces_ = std::move(in.triFaces);
    } else {
        std::uint64_t triCount = cornerTotal - 2ull * faceCount;
        if (triCount * 3ull > kMaxIndexCount) {
            clear();
            return fail(error, "Mesh is too large.");
        }
        triVerts_.reserve(static_cast<std::size_t>(triCount * 3));
        triFaces_.reserve(static_cast<std::size_t>(triCount));
        for (std::uint32_t f = 0; f < F; ++f) {
            const Span<std::uint32_t> poly = faceVertices(f);
            for (std::size_t i = 1; i + 1 < poly.size(); ++i) {
                triVerts_.push_back(poly[0]);
                triVerts_.push_back(poly[i]);
                triVerts_.push_back(poly[i + 1]);
                triFaces_.push_back(f);
            }
        }
    }
    const std::uint32_t T = triangleCount();

    // --- Edges: adjacency, open borders and polygon adjacency ---------------
    {
        std::vector<std::uint64_t> edges;
        std::vector<std::pair<std::uint64_t, std::uint32_t>> edgeFaces;  // (edge key, polygon)
        edgeFaces.reserve(faceVerts_.size());
        for (std::uint32_t f = 0; f < F; ++f) {
            const Span<std::uint32_t> poly = faceVertices(f);
            for (std::size_t i = 0; i < poly.size(); ++i) {
                std::uint32_t a = poly[i];
                std::uint32_t b = poly[(i + 1) % poly.size()];
                if (a == b) continue;  // Degenerate edge (repeated vertex).
                if (a > b) std::swap(a, b);
                edgeFaces.emplace_back((static_cast<std::uint64_t>(a) << 32) | b, f);
            }
        }
        std::sort(edgeFaces.begin(), edgeFaces.end());
        edges.reserve(edgeFaces.size());
        for (const auto& ef : edgeFaces) edges.push_back(ef.first);

        // Polygon adjacency: polygons sharing an edge key.
        std::vector<std::uint32_t> faceAdjCount(F, 0u);
        for (std::size_t i = 0; i < edgeFaces.size();) {
            std::size_t j = i;
            while (j < edgeFaces.size() && edgeFaces[j].first == edgeFaces[i].first) ++j;
            for (std::size_t a = i; a < j; ++a)
                for (std::size_t b = i; b < j; ++b)
                    if (edgeFaces[a].second != edgeFaces[b].second) ++faceAdjCount[edgeFaces[a].second];
            i = j;
        }
        prefixSum(faceAdjCount, faceAdjacencyOffsets_);
        faceAdjacency_.resize(faceAdjacencyOffsets_[F]);
        {
            std::vector<std::uint32_t> cursor(faceAdjacencyOffsets_.begin(), faceAdjacencyOffsets_.end() - 1);
            for (std::size_t i = 0; i < edgeFaces.size();) {
                std::size_t j = i;
                while (j < edgeFaces.size() && edgeFaces[j].first == edgeFaces[i].first) ++j;
                for (std::size_t a = i; a < j; ++a)
                    for (std::size_t b = i; b < j; ++b)
                        if (edgeFaces[a].second != edgeFaces[b].second)
                            faceAdjacency_[cursor[edgeFaces[a].second]++] = edgeFaces[b].second;
                i = j;
            }
        }

        std::vector<std::uint32_t> adjCount(V, 0u);
        std::vector<std::uint32_t> borderCount(V, 0u);
        for (std::size_t i = 0; i < edges.size();) {
            std::size_t j = i;
            while (j < edges.size() && edges[j] == edges[i]) ++j;
            const auto a = static_cast<std::uint32_t>(edges[i] >> 32);
            const auto b = static_cast<std::uint32_t>(edges[i] & 0xffffffffu);
            ++adjCount[a];
            ++adjCount[b];
            if (j - i == 1) {  // Used by exactly one polygon: open border.
                ++borderCount[a];
                ++borderCount[b];
            }
            i = j;
        }
        prefixSum(adjCount, adjacencyOffsets_);
        prefixSum(borderCount, borderOffsets_);
        adjacency_.resize(adjacencyOffsets_[V]);
        border_.resize(borderOffsets_[V]);

        std::vector<std::uint32_t> adjCursor(adjacencyOffsets_.begin(), adjacencyOffsets_.end() - 1);
        std::vector<std::uint32_t> borderCursor(borderOffsets_.begin(), borderOffsets_.end() - 1);
        for (std::size_t i = 0; i < edges.size();) {
            std::size_t j = i;
            while (j < edges.size() && edges[j] == edges[i]) ++j;
            const auto a = static_cast<std::uint32_t>(edges[i] >> 32);
            const auto b = static_cast<std::uint32_t>(edges[i] & 0xffffffffu);
            adjacency_[adjCursor[a]++] = b;
            adjacency_[adjCursor[b]++] = a;
            if (j - i == 1) {
                border_[borderCursor[a]++] = b;
                border_[borderCursor[b]++] = a;
            }
            i = j;
        }
    }

    // --- Vertex -> polygon and polygon -> triangle incidence ---------------
    {
        std::vector<std::uint32_t> count(V, 0u);
        for (std::uint32_t f = 0; f < F; ++f) {
            const Span<std::uint32_t> poly = faceVertices(f);
            for (std::size_t i = 0; i < poly.size(); ++i)
                if (std::find(poly.begin(), poly.begin() + i, poly[i]) == poly.begin() + i) ++count[poly[i]];
        }
        prefixSum(count, vertexFaceOffsets_);
        vertexFaces_.resize(vertexFaceOffsets_[V]);
        std::vector<std::uint32_t> cursor(vertexFaceOffsets_.begin(), vertexFaceOffsets_.end() - 1);
        for (std::uint32_t f = 0; f < F; ++f) {
            const Span<std::uint32_t> poly = faceVertices(f);
            for (std::size_t i = 0; i < poly.size(); ++i)
                if (std::find(poly.begin(), poly.begin() + i, poly[i]) == poly.begin() + i)
                    vertexFaces_[cursor[poly[i]]++] = f;
        }

        std::vector<std::uint32_t> triCount(F, 0u);
        for (std::uint32_t t = 0; t < T; ++t) ++triCount[triFaces_[t]];
        prefixSum(triCount, faceTriOffsets_);
        faceTris_.resize(faceTriOffsets_[F]);
        std::vector<std::uint32_t> triCursor(faceTriOffsets_.begin(), faceTriOffsets_.end() - 1);
        for (std::uint32_t t = 0; t < T; ++t) faceTris_[triCursor[triFaces_[t]]++] = t;
    }

    // --- Vertex -> triangle incidence ---------------------------------------
    {
        std::vector<std::uint32_t> count(V, 0u);
        auto forEachUniqueCorner = [this](std::uint32_t t, auto&& fn) {
            const std::uint32_t a = triVerts_[3 * t], b = triVerts_[3 * t + 1], c = triVerts_[3 * t + 2];
            fn(a);
            if (b != a) fn(b);
            if (c != a && c != b) fn(c);
        };
        for (std::uint32_t t = 0; t < T; ++t) forEachUniqueCorner(t, [&](std::uint32_t v) { ++count[v]; });
        prefixSum(count, vertexTriOffsets_);
        vertexTris_.resize(vertexTriOffsets_[V]);
        std::vector<std::uint32_t> cursor(vertexTriOffsets_.begin(), vertexTriOffsets_.end() - 1);
        for (std::uint32_t t = 0; t < T; ++t)
            forEachUniqueCorner(t, [&](std::uint32_t v) { vertexTris_[cursor[v]++] = t; });
    }

    triNormals_.resize(T);
    normals_.resize(V);
    recomputeAllNormals();
    return true;
}

Vec3 Mesh::computeTriangleNormal(std::uint32_t t) const {
    const Vec3& a = positions_[triVerts_[3 * t]];
    const Vec3& b = positions_[triVerts_[3 * t + 1]];
    const Vec3& c = positions_[triVerts_[3 * t + 2]];
    return cross(b - a, c - a);
}

Vec3 Mesh::accumulateVertexNormal(std::uint32_t v) const {
    Vec3 sum;
    for (std::uint32_t t : vertexTriangles(v)) sum += triNormals_[t];
    return normalizedOrZero(sum);
}

void Mesh::recomputeAllNormals() {
    // Every element is computed independently and in a fixed order, so the
    // parallel result is identical to a serial one.
    parallelFor(triangleCount(), 4096, [this](std::size_t b, std::size_t e) {
        for (std::size_t t = b; t < e; ++t) triNormals_[t] = computeTriangleNormal(static_cast<std::uint32_t>(t));
    });
    parallelFor(vertexCount(), 4096, [this](std::size_t b, std::size_t e) {
        for (std::size_t v = b; v < e; ++v) normals_[v] = accumulateVertexNormal(static_cast<std::uint32_t>(v));
    });
}

void Mesh::collectTriangles(Span<std::uint32_t> vertices, std::vector<std::uint32_t>& out) const {
    triVisit_.begin(triangleCount());
    for (std::uint32_t v : vertices)
        for (std::uint32_t t : vertexTriangles(v))
            if (triVisit_.visit(t)) out.push_back(t);
}

void Mesh::updateNormals(Span<std::uint32_t> movedVertices) {
    normalUpdated_.clear();
    if (movedVertices.empty()) return;
    scratchTris_.clear();
    collectTriangles(movedVertices, scratchTris_);
    parallelFor(scratchTris_.size(), 2048, [this](std::size_t b, std::size_t e) {
        for (std::size_t i = b; i < e; ++i) triNormals_[scratchTris_[i]] = computeTriangleNormal(scratchTris_[i]);
    });

    // Every vertex of an updated triangle may have a different normal now.
    vertVisit_.begin(vertexCount());
    for (std::uint32_t t : scratchTris_) {
        for (int k = 0; k < 3; ++k) {
            const std::uint32_t v = triVerts_[3 * t + k];
            if (vertVisit_.visit(v)) normalUpdated_.push_back(v);
        }
    }
    parallelFor(normalUpdated_.size(), 2048, [this](std::size_t b, std::size_t e) {
        for (std::size_t i = b; i < e; ++i) normals_[normalUpdated_[i]] = accumulateVertexNormal(normalUpdated_[i]);
    });
}

Vec3 Mesh::faceNormal(std::uint32_t f) const {
    Vec3 sum;
    for (std::uint32_t k = faceTriOffsets_[f]; k < faceTriOffsets_[f + 1]; ++k) sum += triNormals_[faceTris_[k]];
    return normalizedOrZero(sum);
}

Vec3 Mesh::faceCenter(std::uint32_t f) const {
    const Span<std::uint32_t> poly = faceVertices(f);
    Vec3 c;
    for (std::uint32_t v : poly) c += positions_[v];
    return c / static_cast<float>(poly.size());
}

Aabb Mesh::bounds() const {
    Aabb box;
    for (const Vec3& p : positions_) box.expand(p);
    return box;
}

std::size_t Mesh::memoryBytes() const {
    return bytesOf(positions_) + bytesOf(normals_) + bytesOf(faceOffsets_) + bytesOf(faceVerts_) +
           bytesOf(triVerts_) + bytesOf(triFaces_) + bytesOf(triNormals_) + bytesOf(adjacencyOffsets_) +
           bytesOf(adjacency_) + bytesOf(borderOffsets_) + bytesOf(border_) + bytesOf(vertexTriOffsets_) +
           bytesOf(vertexTris_) + bytesOf(scratchTris_) + bytesOf(normalUpdated_) +
           bytesOf(vertexFaceOffsets_) + bytesOf(vertexFaces_) + bytesOf(faceAdjacencyOffsets_) + bytesOf(faceAdjacency_) +
           bytesOf(faceTriOffsets_) + bytesOf(faceTris_);
}

}  // namespace sculpt
