#include "sculpt/bvh.h"

#include <algorithm>
#include <numeric>

namespace sculpt {
namespace {

// Median splits keep the tree balanced, so depth <= log2(2^32) + 1. Each
// level pushes at most one extra entry, which keeps traversal stacks tiny.
constexpr int kStackSize = 128;

bool contains(const Aabb& outer, const Vec3& p) {
    return p.x >= outer.lo.x && p.y >= outer.lo.y && p.z >= outer.lo.z && p.x <= outer.hi.x &&
           p.y <= outer.hi.y && p.z <= outer.hi.z;
}

bool contains(const Aabb& outer, const Aabb& inner) {
    return inner.empty() || (contains(outer, inner.lo) && contains(outer, inner.hi));
}

}  // namespace

void Bvh::clear() {
    nodes_.clear();
    triIndices_.clear();
    triLeaf_.clear();
    builtLeafArea_ = 0.0f;
}

Aabb Bvh::triangleBounds(const Mesh& mesh, std::uint32_t t) const {
    const auto tri = mesh.triangle(t);
    Aabb box;
    box.expand(mesh.position(tri[0]));
    box.expand(mesh.position(tri[1]));
    box.expand(mesh.position(tri[2]));
    return box;
}

Aabb Bvh::leafBounds(const Mesh& mesh, const Node& node) const {
    Aabb box;
    for (std::uint32_t i = node.first; i < node.first + node.count; ++i) box.expand(triangleBounds(mesh, triIndices_[i]));
    return box;
}

void Bvh::build(const Mesh& mesh) {
    clear();
    const std::uint32_t T = mesh.triangleCount();
    if (T == 0) return;

    triIndices_.resize(T);
    std::iota(triIndices_.begin(), triIndices_.end(), 0u);
    triLeaf_.assign(T, 0u);

    std::vector<Vec3> centroids(T);
    for (std::uint32_t t = 0; t < T; ++t) centroids[t] = triangleBounds(mesh, t).center();

    nodes_.reserve(2 * (T / kLeafSize + 1));
    nodes_.push_back(Node{});

    struct Task {
        std::uint32_t node, begin, end;
    };
    std::vector<Task> tasks;
    tasks.push_back({0u, 0u, T});
    while (!tasks.empty()) {
        const Task task = tasks.back();
        tasks.pop_back();
        const std::uint32_t count = task.end - task.begin;

        if (count <= kLeafSize) {
            Node& leaf = nodes_[task.node];
            leaf.first = task.begin;
            leaf.count = count;
            for (std::uint32_t i = task.begin; i < task.end; ++i) triLeaf_[triIndices_[i]] = task.node;
            continue;
        }

        Aabb centroidBox;
        for (std::uint32_t i = task.begin; i < task.end; ++i) centroidBox.expand(centroids[triIndices_[i]]);
        const int axis = centroidBox.longestAxis();
        const std::uint32_t mid = task.begin + count / 2;
        std::nth_element(triIndices_.begin() + task.begin, triIndices_.begin() + mid, triIndices_.begin() + task.end,
                         [&](std::uint32_t a, std::uint32_t b) { return centroids[a][axis] < centroids[b][axis]; });

        const auto left = static_cast<std::uint32_t>(nodes_.size());
        const std::uint32_t right = left + 1;
        nodes_.push_back(Node{});
        nodes_.push_back(Node{});
        nodes_[left].parent = task.node;
        nodes_[right].parent = task.node;
        Node& inner = nodes_[task.node];
        inner.first = left;
        inner.right = right;
        inner.count = 0;
        tasks.push_back({left, task.begin, mid});
        tasks.push_back({right, mid, task.end});
    }

    refitAll(mesh);
    builtLeafArea_ = leafAreaSum();
}

void Bvh::refitAll(const Mesh& mesh) {
    // Children always have larger indices than their parent, so a reverse
    // sweep visits every child before its parent.
    for (std::size_t i = nodes_.size(); i-- > 0;) {
        Node& node = nodes_[i];
        if (node.isLeaf()) {
            node.box = leafBounds(mesh, node);
        } else {
            Aabb box = nodes_[node.first].box;
            box.expand(nodes_[node.right].box);
            node.box = box;
        }
    }
}

void Bvh::refit(const Mesh& mesh, Span<std::uint32_t> movedTriangles) {
    if (nodes_.empty() || movedTriangles.empty()) return;
    scratchLeaves_.clear();
    nodeVisit_.begin(nodes_.size());
    for (std::uint32_t t : movedTriangles) {
        const std::uint32_t leaf = triLeaf_[t];
        if (nodeVisit_.visit(leaf)) scratchLeaves_.push_back(leaf);
    }
    for (std::uint32_t leaf : scratchLeaves_) nodes_[leaf].box = leafBounds(mesh, nodes_[leaf]);

    // All leaves are final now, so a walk may stop as soon as an ancestor's
    // box comes out unchanged: anything above it is already correct.
    for (std::uint32_t leaf : scratchLeaves_) {
        std::uint32_t n = leaf;
        while (n != 0u) {
            Node& parent = nodes_[nodes_[n].parent];
            Aabb box = nodes_[parent.first].box;
            box.expand(nodes_[parent.right].box);
            if (box == parent.box) break;
            parent.box = box;
            n = nodes_[n].parent;
        }
    }
}

bool Bvh::raycast(const Mesh& mesh, const Ray& ray, RayHit& hit, bool cullBackfaces, float tMax) const {
    if (nodes_.empty()) return false;
    const Vec3 invDir{1.0f / ray.dir.x, 1.0f / ray.dir.y, 1.0f / ray.dir.z};

    float best = tMax;
    bool found = false;
    std::uint32_t stack[kStackSize];
    int sp = 0;
    stack[sp++] = 0u;
    while (sp > 0) {
        const Node& node = nodes_[stack[--sp]];
        float tEnter = 0.0f;
        if (!intersectRayAabb(ray.origin, invDir, node.box, best, tEnter)) continue;

        if (node.isLeaf()) {
            for (std::uint32_t i = node.first; i < node.first + node.count; ++i) {
                const std::uint32_t t = triIndices_[i];
                const auto tri = mesh.triangle(t);
                float th = 0.0f, u = 0.0f, v = 0.0f;
                if (intersectRayTriangle(ray, mesh.position(tri[0]), mesh.position(tri[1]), mesh.position(tri[2]),
                                         cullBackfaces, best, th, u, v)) {
                    best = th;
                    found = true;
                    hit.t = th;
                    hit.triangle = t;
                    hit.u = u;
                    hit.v = v;
                }
            }
            continue;
        }

        // Visit the nearer child first so `best` shrinks early.
        float tLeft = kInfinity, tRight = kInfinity;
        const bool hitLeft = intersectRayAabb(ray.origin, invDir, nodes_[node.first].box, best, tLeft);
        const bool hitRight = intersectRayAabb(ray.origin, invDir, nodes_[node.right].box, best, tRight);
        if (hitLeft && hitRight) {
            const bool leftFirst = tLeft <= tRight;
            stack[sp++] = leftFirst ? node.right : node.first;
            stack[sp++] = leftFirst ? node.first : node.right;
        } else if (hitLeft) {
            stack[sp++] = node.first;
        } else if (hitRight) {
            stack[sp++] = node.right;
        }
    }

    if (found) {
        const auto tri = mesh.triangle(hit.triangle);
        const Vec3& a = mesh.position(tri[0]);
        const Vec3& b = mesh.position(tri[1]);
        const Vec3& c = mesh.position(tri[2]);
        hit.position = a * (1.0f - hit.u - hit.v) + b * hit.u + c * hit.v;
        hit.geometricNormal = normalizedOrZero(cross(b - a, c - a));
    }
    return found;
}

void Bvh::gatherVertices(const Mesh& mesh, const Vec3& center, float radius, std::vector<std::uint32_t>& out) const {
    if (nodes_.empty() || !(radius >= 0.0f) || !isFinite(center)) return;
    const float r2 = radius * radius;
    vertexVisit_.begin(mesh.vertexCount());

    std::uint32_t stack[kStackSize];
    int sp = 0;
    stack[sp++] = 0u;
    while (sp > 0) {
        const Node& node = nodes_[stack[--sp]];
        if (node.box.distanceSq(center) > r2) continue;
        if (!node.isLeaf()) {
            stack[sp++] = node.first;
            stack[sp++] = node.right;
            continue;
        }
        for (std::uint32_t i = node.first; i < node.first + node.count; ++i) {
            for (std::uint32_t v : mesh.triangle(triIndices_[i])) {
                if (!vertexVisit_.visit(v)) continue;
                if (lengthSq(mesh.position(v) - center) <= r2) out.push_back(v);
            }
        }
    }
}

float Bvh::leafAreaSum() const {
    double sum = 0.0;
    for (const Node& node : nodes_)
        if (node.isLeaf()) sum += static_cast<double>(node.box.surfaceArea());
    return static_cast<float>(sum);
}

float Bvh::degradation() const {
    if (!(builtLeafArea_ > 0.0f)) return 1.0f;
    return leafAreaSum() / builtLeafArea_;
}

std::size_t Bvh::memoryBytes() const {
    return nodes_.capacity() * sizeof(Node) + triIndices_.capacity() * sizeof(std::uint32_t) +
           triLeaf_.capacity() * sizeof(std::uint32_t) + scratchLeaves_.capacity() * sizeof(std::uint32_t);
}

bool Bvh::validate(const Mesh& mesh) const {
    const std::uint32_t T = mesh.triangleCount();
    if (nodes_.empty()) return T == 0;
    if (triIndices_.size() != T || triLeaf_.size() != T) return false;

    std::vector<std::uint8_t> seen(T, 0u);
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        const Node& node = nodes_[i];
        if (i != 0 && nodes_[node.parent].isLeaf()) return false;
        if (node.isLeaf()) {
            for (std::uint32_t k = node.first; k < node.first + node.count; ++k) {
                const std::uint32_t t = triIndices_[k];
                if (seen[t]++) return false;
                if (triLeaf_[t] != i) return false;
                if (!contains(node.box, triangleBounds(mesh, t))) return false;
            }
        } else {
            if (node.first >= nodes_.size() || node.right >= nodes_.size()) return false;
            if (nodes_[node.first].parent != i || nodes_[node.right].parent != i) return false;
            if (!contains(node.box, nodes_[node.first].box) || !contains(node.box, nodes_[node.right].box))
                return false;
        }
    }
    return std::all_of(seen.begin(), seen.end(), [](std::uint8_t s) { return s == 1u; });
}

}  // namespace sculpt
