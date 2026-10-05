// SculptCore — bounding volume hierarchy over a Mesh's triangles.
// Used for picking (ray casts) and for gathering the vertices under a brush.
#pragma once

#include <cstdint>
#include <vector>

#include "sculpt/math.h"
#include "sculpt/mesh.h"
#include "sculpt/span.h"
#include "sculpt/visit_set.h"

namespace sculpt {

struct RayHit {
    float t = kInfinity;          // Distance along the ray, in units of |ray.dir|.
    std::uint32_t triangle = 0;
    float u = 0.0f;               // Barycentric weight of the triangle's 2nd vertex.
    float v = 0.0f;               // Barycentric weight of the triangle's 3rd vertex.
    Vec3 position;
    Vec3 geometricNormal;         // Unit normal of the hit triangle (winding order).
};

class Bvh {
public:
    static constexpr std::uint32_t kLeafSize = 8;

    void build(const Mesh& mesh);
    void clear();
    bool empty() const { return nodes_.empty(); }

    // Closest hit with t in (0, tMax). Returns false when nothing is hit.
    bool raycast(const Mesh& mesh, const Ray& ray, RayHit& hit, bool cullBackfaces = false,
                 float tMax = kInfinity) const;

    // Appends every vertex (used by at least one triangle) within `radius` of
    // `center`, without duplicates.
    void gatherVertices(const Mesh& mesh, const Vec3& center, float radius, std::vector<std::uint32_t>& out) const;

    // Updates node bounds after the given triangles moved (topology unchanged).
    void refit(const Mesh& mesh, Span<std::uint32_t> movedTriangles);
    void refitAll(const Mesh& mesh);

    // Sum of leaf surface areas relative to the value right after build().
    // Grows as refits loosen the tree; callers rebuild past a threshold.
    float degradation() const;

    std::size_t nodeCount() const { return nodes_.size(); }
    std::size_t memoryBytes() const;

    // Debug helper for tests: verifies that every node encloses its contents.
    bool validate(const Mesh& mesh) const;

private:
    struct Node {
        Aabb box;
        std::uint32_t first = 0;   // Leaf: first entry in triIndices_. Inner: left child.
        std::uint32_t count = 0;   // Leaf: triangle count (> 0). Inner: 0.
        std::uint32_t right = 0;   // Inner: right child.
        std::uint32_t parent = 0;  // Root points to itself.
        bool isLeaf() const { return count > 0; }
    };

    Aabb triangleBounds(const Mesh& mesh, std::uint32_t t) const;
    Aabb leafBounds(const Mesh& mesh, const Node& node) const;
    float leafAreaSum() const;

    std::vector<Node> nodes_;
    std::vector<std::uint32_t> triIndices_;
    std::vector<std::uint32_t> triLeaf_;  // Leaf node holding each triangle.
    float builtLeafArea_ = 0.0f;

    mutable VisitSet vertexVisit_;
    mutable VisitSet nodeVisit_;
    std::vector<std::uint32_t> scratchLeaves_;
};

}  // namespace sculpt
