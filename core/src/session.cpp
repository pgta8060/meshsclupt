#include "sculpt/session.h"

#include <utility>

namespace sculpt {

bool SculptSession::build(MeshInput input, std::string* error) {
    clear();
    if (!mesh_.build(std::move(input), error)) return false;
    bvh_.build(mesh_);
    dirtyFlag_.assign(mesh_.vertexCount(), 0u);
    return true;
}

void SculptSession::clear() {
    mesh_.clear();
    bvh_.clear();
    recorder_ = StrokeRecorder();
    scratch_ = DabScratch();
    moved_.clear();
    movedTris_.clear();
    dirty_.clear();
    dirtyFlag_.clear();
    displayDirty_.clear();
    displayAllDirty_ = false;
}

bool SculptSession::raycast(const Ray& ray, RayHit& hit, bool cullBackfaces) const {
    if (!valid() || !isFinite(ray.origin) || !isFinite(ray.dir) || lengthSq(ray.dir) == 0.0f) return false;
    return bvh_.raycast(mesh_, ray, hit, cullBackfaces);
}

void SculptSession::beginStroke() {
    if (!valid()) return;
    recorder_.begin(mesh_.vertexCount());
}

std::size_t SculptSession::applyDab(const BrushSettings& settings, const Dab& dab) {
    if (!valid()) return 0;
    if (!recorder_.active()) beginStroke();
    moved_.clear();
    const std::size_t count = sculpt::applyDab(mesh_, bvh_, settings, dab, recorder_, scratch_, moved_);
    commitMoved();
    return count;
}

void SculptSession::commitMoved() {
    if (moved_.empty()) return;
    mesh_.updateNormals(moved_);
    movedTris_.clear();
    mesh_.collectTriangles(moved_, movedTris_);
    bvh_.refit(mesh_, movedTris_);
    for (std::uint32_t v : moved_) {
        if (!(dirtyFlag_[v] & 1u)) {
            dirtyFlag_[v] |= 1u;
            dirty_.push_back(v);
        }
    }
    for (std::uint32_t v : mesh_.lastUpdatedNormals()) {
        if (!(dirtyFlag_[v] & 2u)) {
            dirtyFlag_[v] |= 2u;
            displayDirty_.push_back(v);
        }
    }
    moved_.clear();
}

StrokeDelta SculptSession::endStroke() {
    StrokeDelta delta;
    if (!recorder_.active()) return delta;
    recorder_.take(delta.vertices, delta.before);
    recorder_.end();
    delta.after.reserve(delta.vertices.size());
    for (std::uint32_t v : delta.vertices) delta.after.push_back(mesh_.position(v));
    if (bvh_.degradation() > kRebuildThreshold) bvh_.build(mesh_);
    return delta;
}

void SculptSession::cancelStroke() {
    if (!recorder_.active()) return;
    std::vector<std::uint32_t> vertices;
    std::vector<Vec3> originals;
    recorder_.take(vertices, originals);
    recorder_.end();
    moved_.clear();
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        mesh_.setPosition(vertices[i], originals[i]);
        moved_.push_back(vertices[i]);
    }
    commitMoved();
}

bool SculptSession::applyDelta(const StrokeDelta& delta, bool useBefore) {
    if (!valid() || recorder_.active() || !delta.consistent()) return false;
    const std::uint32_t V = mesh_.vertexCount();
    const std::vector<Vec3>& source = useBefore ? delta.before : delta.after;
    for (std::size_t i = 0; i < delta.vertices.size(); ++i)
        if (delta.vertices[i] >= V || !isFinite(source[i])) return false;

    moved_.clear();
    for (std::size_t i = 0; i < delta.vertices.size(); ++i) {
        mesh_.setPosition(delta.vertices[i], source[i]);
        moved_.push_back(delta.vertices[i]);
    }
    commitMoved();
    return true;
}

bool SculptSession::setPositions(const std::vector<Vec3>& positions) {
    if (!valid() || recorder_.active() || positions.size() != mesh_.vertexCount()) return false;
    for (const Vec3& p : positions)
        if (!isFinite(p)) return false;
    const std::uint32_t V = mesh_.vertexCount();
    for (std::uint32_t v = 0; v < V; ++v) mesh_.setPosition(v, positions[v]);
    mesh_.recomputeAllNormals();
    bvh_.build(mesh_);
    displayAllDirty_ = true;
    return true;
}

void SculptSession::clearDirty() {
    for (std::uint32_t v : dirty_) dirtyFlag_[v] &= static_cast<std::uint8_t>(~1u);
    dirty_.clear();
}

void SculptSession::clearDisplayDirty() {
    for (std::uint32_t v : displayDirty_) dirtyFlag_[v] &= static_cast<std::uint8_t>(~2u);
    displayDirty_.clear();
    displayAllDirty_ = false;
}

std::size_t SculptSession::memoryBytes() const {
    return mesh_.memoryBytes() + bvh_.memoryBytes() + (dirty_.capacity() + displayDirty_.capacity()) * sizeof(std::uint32_t) +
           dirtyFlag_.capacity() + moved_.capacity() * sizeof(std::uint32_t) +
           movedTris_.capacity() * sizeof(std::uint32_t);
}

}  // namespace sculpt
