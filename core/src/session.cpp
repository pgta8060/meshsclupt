#include "sculpt/session.h"

#include <algorithm>
#include <utility>

namespace sculpt {
namespace {

constexpr std::uint8_t kMovedBit = 1u;
constexpr std::uint8_t kDisplayBit = 2u;
constexpr std::uint8_t kMaskBit = 4u;

template <class T>
std::size_t bytesOf(const std::vector<T>& v) {
    return v.capacity() * sizeof(T);
}

}  // namespace

bool StrokeDelta::consistent() const {
    return before.size() == vertices.size() && after.size() == vertices.size() &&
           maskBefore.size() == maskVertices.size() && maskAfter.size() == maskVertices.size() &&
           groupBefore.size() == groupFaces.size() && groupAfter.size() == groupFaces.size() &&
           (!hasHidden || hiddenBefore.size() == hiddenAfter.size());
}

std::size_t StrokeDelta::memoryBytes() const {
    return bytesOf(vertices) + bytesOf(before) + bytesOf(after) + bytesOf(maskVertices) + bytesOf(maskBefore) +
           bytesOf(maskAfter) + bytesOf(groupFaces) + bytesOf(groupBefore) + bytesOf(groupAfter) +
           bytesOf(hiddenBefore) + bytesOf(hiddenAfter);
}

bool SculptSession::build(MeshInput input, std::string* error, SessionAttributes attributes) {
    clear();
    if (!mesh_.build(std::move(input), error)) return false;
    bvh_.build(mesh_);
    const std::uint32_t V = mesh_.vertexCount();
    const std::uint32_t F = mesh_.faceCount();

    mask_ = std::move(attributes.mask);
    if (mask_.size() != V) mask_.assign(V, 0.0f);
    for (float& m : mask_) m = isFinite(m) ? clamp01(m) : 0.0f;
    refreshHasMask();

    faceGroups_ = std::move(attributes.faceGroups);
    if (faceGroups_.size() != F) faceGroups_.assign(F, 0);
    for (std::int32_t& g : faceGroups_) g = std::max<std::int32_t>(0, g);

    hiddenFaces_ = std::move(attributes.hiddenFaces);
    if (hiddenFaces_.size() != F) hiddenFaces_.assign(F, 0u);
    refreshHiddenTriangles();

    dirtyFlag_.assign(V, 0u);
    groupDirtyFlag_.assign(F, 0u);
    return true;
}

void SculptSession::clear() {
    *this = SculptSession();
}

void SculptSession::refreshHasMask() {
    hasMask_ = std::any_of(mask_.begin(), mask_.end(), [](float m) { return m > 0.0f; });
}

void SculptSession::refreshHiddenTriangles() {
    const std::uint32_t T = mesh_.triangleCount();
    hiddenTris_.assign(T, 0u);
    anyHidden_ = false;
    for (std::uint32_t t = 0; t < T; ++t) {
        if (hiddenFaces_[mesh_.triangleFace(t)]) {
            hiddenTris_[t] = 1u;
            anyHidden_ = true;
        }
    }
}

bool SculptSession::vertexVisible(std::uint32_t v) const {
    if (!anyHidden_) return true;
    for (std::uint32_t f : mesh_.vertexFaces(v))
        if (!hiddenFaces_[f]) return true;
    return false;
}

std::int32_t SculptSession::nextGroupId() const {
    std::int32_t maxId = 0;
    for (std::int32_t g : faceGroups_) maxId = std::max(maxId, g);
    return maxId + 1;
}

const std::vector<std::uint8_t>& SculptSession::groupBorderVertices() {
    if (!groupBorderValid_) {
        const std::uint32_t V = mesh_.vertexCount();
        groupBorder_.assign(V, 0u);
        for (std::uint32_t v = 0; v < V; ++v) {
            const Span<std::uint32_t> faces = mesh_.vertexFaces(v);
            for (std::size_t i = 1; i < faces.size(); ++i) {
                if (faceGroups_[faces[i]] != faceGroups_[faces[0]]) {
                    groupBorder_[v] = 1u;
                    break;
                }
            }
        }
        groupBorderValid_ = true;
    }
    return groupBorder_;
}

void SculptSession::setSymmetry(std::vector<Mat3> transforms) {
    if (transforms.empty()) transforms.push_back(Mat3::identity());
    symmetry_ = std::move(transforms);
}

bool SculptSession::raycast(const Ray& ray, RayHit& hit, bool cullBackfaces) const {
    if (!valid() || !isFinite(ray.origin) || !isFinite(ray.dir) || lengthSq(ray.dir) == 0.0f) return false;
    return bvh_.raycast(mesh_, ray, hit, cullBackfaces, kInfinity, hiddenTriangles());
}

void SculptSession::beginStroke() {
    if (!valid()) return;
    recorder_.begin(mesh_.vertexCount());
    strokeStates_.assign(symmetry_.size(), StrokeState());
    maskTouched_.begin(mesh_.vertexCount());
    maskStrokeVerts_.clear();
    maskStrokeBefore_.clear();
    groupTouched_.begin(mesh_.faceCount());
    groupStrokeFaces_.clear();
    groupStrokeBefore_.clear();
}

void SculptSession::setMaskValue(std::uint32_t v, float value, bool recordInStroke) {
    value = clamp01(value);
    if (mask_[v] == value) return;
    if (recordInStroke && maskTouched_.visit(v)) {
        maskStrokeVerts_.push_back(v);
        maskStrokeBefore_.push_back(mask_[v]);
    }
    mask_[v] = value;
    if (value > 0.0f) hasMask_ = true;
    if (!(dirtyFlag_[v] & kMaskBit)) {
        dirtyFlag_[v] |= kMaskBit;
        maskDirty_.push_back(v);
    }
}

void SculptSession::setGroupValue(std::uint32_t f, std::int32_t group, bool recordInStroke) {
    if (faceGroups_[f] == group) return;
    if (recordInStroke && groupTouched_.visit(f)) {
        groupStrokeFaces_.push_back(f);
        groupStrokeBefore_.push_back(faceGroups_[f]);
    }
    faceGroups_[f] = group;
    groupBorderValid_ = false;
    if (!groupDirtyFlag_[f]) {
        groupDirtyFlag_[f] = 1u;
        groupDirty_.push_back(f);
    }
}

std::size_t SculptSession::applyDab(const BrushSettings& settings, const Dab& dab) {
    if (!valid()) return 0;
    if (!recorder_.active()) beginStroke();
    std::size_t changed = 0;

    for (std::size_t k = 0; k < symmetry_.size(); ++k) {
        const Mat3& m = symmetry_[k];
        Dab d = dab;
        d.center = m * dab.center;
        d.viewDir = m * dab.viewDir;
        d.up = m * dab.up;
        d.grabDelta = m * dab.grabDelta;

        if (settings.type == BrushType::MaskPaint) {
            applyMaskDab(settings, d, changed);
            continue;
        }
        if (settings.type == BrushType::FaceGroups) {
            applyGroupDab(settings, d, changed);
            continue;
        }
        moved_.clear();
        DabTarget target{mesh_, bvh_, recorder_, strokeStates_[k], scratch_, moved_};
        target.mask = hasMask_ ? &mask_ : nullptr;
        target.hiddenTriangles = hiddenTriangles();
        if (settings.type == BrushType::SmoothGroupBorder) target.groupBorder = &groupBorderVertices();
        changed += sculpt::applyDab(target, settings, d);
        // Refresh normals/BVH per copy: the next copy may overlap this one.
        commitMoved();
    }
    return changed;
}

void SculptSession::applyMaskDab(const BrushSettings& settings, const Dab& dab, std::size_t& changed) {
    const float strength = clamp01(settings.strength) * clamp01(dab.pressure) * std::max(0.0f, dab.amount);
    if (!(dab.radius > 0.0f) || !isFinite(dab.center) || !(strength > 0.0f)) return;
    const bool alpha = settings.alpha && !settings.alpha->empty();
    scratch_.verts.clear();
    bvh_.gatherVertices(mesh_, dab.center, dab.radius * (alpha && !settings.useFalloff ? 1.41421356f : 1.0f),
                        scratch_.verts, hiddenTriangles());
    const float sign = (settings.subtract != dab.invert) ? -1.0f : 1.0f;  // Alt erases.
    const Vec3 frameNormal = normalizedOrZero(-dab.viewDir);
    for (std::uint32_t v : scratch_.verts) {
        if (settings.backfaceCull && dot(mesh_.normal(v), dab.viewDir) > 0.0f) continue;
        const float w = dabWeight(settings, dab, frameNormal, mesh_.position(v)) * strength;
        if (w == 0.0f) continue;
        const float before = mask_[v];
        setMaskValue(v, before + sign * w, true);
        if (mask_[v] != before) ++changed;
    }
}

void SculptSession::applyGroupDab(const BrushSettings& settings, const Dab& dab, std::size_t& changed) {
    if (!(dab.radius > 0.0f) || !isFinite(dab.center)) return;
    scratch_.verts.clear();
    bvh_.gatherVertices(mesh_, dab.center, dab.radius, scratch_.verts, hiddenTriangles());
    const float r2 = dab.radius * dab.radius;
    const std::int32_t group = std::max<std::int32_t>(0, settings.faceGroupId);
    for (std::uint32_t v : scratch_.verts) {
        if (settings.backfaceCull && dot(mesh_.normal(v), dab.viewDir) > 0.0f) continue;
        for (std::uint32_t f : mesh_.vertexFaces(v)) {
            if (hiddenFaces_[f] || faceGroups_[f] == group) continue;
            if (lengthSq(mesh_.faceCenter(f) - dab.center) > r2) continue;
            setGroupValue(f, group, true);
            ++changed;
        }
    }
}

std::size_t SculptSession::applyMaskBlurDab(const Dab& dab, float strength) {
    if (!valid() || !(dab.radius > 0.0f) || !isFinite(dab.center)) return 0;
    if (!recorder_.active()) beginStroke();
    std::size_t changed = 0;
    for (const Mat3& m : symmetry_) {
        Dab d = dab;
        d.center = m * dab.center;
        scratch_.verts.clear();
        bvh_.gatherVertices(mesh_, d.center, d.radius, scratch_.verts, hiddenTriangles());
        std::vector<float> next(scratch_.verts.size());
        const float invRadius = 1.0f / d.radius;
        for (std::size_t i = 0; i < scratch_.verts.size(); ++i) {
            const std::uint32_t v = scratch_.verts[i];
            float sum = 0.0f;
            int count = 0;
            for (std::uint32_t n : mesh_.neighbors(v)) {
                sum += mask_[n];
                ++count;
            }
            const float avg = count ? sum / static_cast<float>(count) : mask_[v];
            const float w = brushFalloff(length(mesh_.position(v) - d.center) * invRadius, true) * clamp01(strength);
            next[i] = mask_[v] + (avg - mask_[v]) * w;
        }
        for (std::size_t i = 0; i < scratch_.verts.size(); ++i) {
            const float before = mask_[scratch_.verts[i]];
            setMaskValue(scratch_.verts[i], next[i], true);
            if (mask_[scratch_.verts[i]] != before) ++changed;
        }
    }
    return changed;
}

void SculptSession::commitMoved() {
    if (moved_.empty()) return;
    mesh_.updateNormals(moved_);
    movedTris_.clear();
    mesh_.collectTriangles(moved_, movedTris_);
    bvh_.refit(mesh_, movedTris_);
    for (std::uint32_t v : moved_) {
        if (!(dirtyFlag_[v] & kMovedBit)) {
            dirtyFlag_[v] |= kMovedBit;
            dirty_.push_back(v);
        }
    }
    for (std::uint32_t v : mesh_.lastUpdatedNormals()) {
        if (!(dirtyFlag_[v] & kDisplayBit)) {
            dirtyFlag_[v] |= kDisplayBit;
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

    delta.maskVertices.swap(maskStrokeVerts_);
    delta.maskBefore.swap(maskStrokeBefore_);
    for (std::uint32_t v : delta.maskVertices) delta.maskAfter.push_back(mask_[v]);
    delta.groupFaces.swap(groupStrokeFaces_);
    delta.groupBefore.swap(groupStrokeBefore_);
    for (std::uint32_t f : delta.groupFaces) delta.groupAfter.push_back(faceGroups_[f]);
    maskStrokeVerts_.clear();
    maskStrokeBefore_.clear();
    groupStrokeFaces_.clear();
    groupStrokeBefore_.clear();
    strokeStates_.clear();

    if (!delta.maskVertices.empty()) refreshHasMask();
    if (bvh_.degradation() > kRebuildThreshold) bvh_.build(mesh_);
    return delta;
}

void SculptSession::cancelStroke() {
    if (!recorder_.active()) return;
    StrokeDelta delta = endStroke();
    // Restore everything the stroke changed (recorder is inactive now).
    moved_.clear();
    for (std::size_t i = 0; i < delta.vertices.size(); ++i) {
        mesh_.setPosition(delta.vertices[i], delta.before[i]);
        moved_.push_back(delta.vertices[i]);
    }
    commitMoved();
    for (std::size_t i = 0; i < delta.maskVertices.size(); ++i)
        setMaskValue(delta.maskVertices[i], delta.maskBefore[i], false);
    for (std::size_t i = 0; i < delta.groupFaces.size(); ++i)
        setGroupValue(delta.groupFaces[i], delta.groupBefore[i], false);
    refreshHasMask();
}

bool SculptSession::applyDelta(const StrokeDelta& delta, bool useBefore) {
    if (!valid() || recorder_.active() || !delta.consistent()) return false;
    const std::uint32_t V = mesh_.vertexCount();
    const std::uint32_t F = mesh_.faceCount();
    const std::vector<Vec3>& positions = useBefore ? delta.before : delta.after;
    const std::vector<float>& masks = useBefore ? delta.maskBefore : delta.maskAfter;
    const std::vector<std::int32_t>& groups = useBefore ? delta.groupBefore : delta.groupAfter;
    for (std::size_t i = 0; i < delta.vertices.size(); ++i)
        if (delta.vertices[i] >= V || !isFinite(positions[i])) return false;
    for (std::uint32_t v : delta.maskVertices)
        if (v >= V) return false;
    for (std::uint32_t f : delta.groupFaces)
        if (f >= F) return false;
    if (delta.hasHidden && delta.hiddenBefore.size() != F) return false;

    moved_.clear();
    for (std::size_t i = 0; i < delta.vertices.size(); ++i) {
        mesh_.setPosition(delta.vertices[i], positions[i]);
        moved_.push_back(delta.vertices[i]);
    }
    commitMoved();
    for (std::size_t i = 0; i < delta.maskVertices.size(); ++i) setMaskValue(delta.maskVertices[i], masks[i], false);
    if (!delta.maskVertices.empty()) refreshHasMask();
    for (std::size_t i = 0; i < delta.groupFaces.size(); ++i) setGroupValue(delta.groupFaces[i], groups[i], false);
    if (delta.hasHidden) {
        hiddenFaces_ = useBefore ? delta.hiddenBefore : delta.hiddenAfter;
        refreshHiddenTriangles();
        visibilityDirty_ = true;
    }
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
    for (std::uint32_t v : dirty_) dirtyFlag_[v] &= static_cast<std::uint8_t>(~kMovedBit);
    dirty_.clear();
}

void SculptSession::clearDisplayDirty() {
    for (std::uint32_t v : displayDirty_) dirtyFlag_[v] &= static_cast<std::uint8_t>(~kDisplayBit);
    displayDirty_.clear();
    displayAllDirty_ = false;
}

void SculptSession::clearMaskDirty() {
    for (std::uint32_t v : maskDirty_) dirtyFlag_[v] &= static_cast<std::uint8_t>(~kMaskBit);
    maskDirty_.clear();
}

void SculptSession::clearGroupDirty() {
    for (std::uint32_t f : groupDirty_) groupDirtyFlag_[f] = 0u;
    groupDirty_.clear();
}

std::size_t SculptSession::memoryBytes() const {
    return mesh_.memoryBytes() + bvh_.memoryBytes() + bytesOf(dirty_) + bytesOf(displayDirty_) + bytesOf(maskDirty_) +
           bytesOf(dirtyFlag_) + bytesOf(moved_) + bytesOf(movedTris_) + bytesOf(mask_) + bytesOf(faceGroups_) +
           bytesOf(hiddenFaces_) + bytesOf(hiddenTris_) + bytesOf(groupBorder_) + bytesOf(groupDirtyFlag_) +
           bytesOf(groupDirty_);
}

}  // namespace sculpt
