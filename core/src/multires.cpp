#include "sculpt/multires.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "sculpt/half.h"

namespace sculpt {

namespace {

constexpr std::uint32_t kMagic = 0x534d5253u;  // "SMRS"
constexpr std::uint32_t kVersion = 1u;

PolyData TopologyOnly(const PolyData& in, bool withMaps) {
    PolyData out;
    out.positions = in.positions;
    out.faceSizes = in.faceSizes;
    out.faceVerts = in.faceVerts;
    out.material = in.material;
    out.smoothing = in.smoothing;
    if (withMaps) out.maps = in.maps;
    return out;
}

Vec3 ToFrame(const TangentFrame& f, const Vec3& d) { return {dot(d, f.t), dot(d, f.b), dot(d, f.n)}; }
Vec3 FromFrame(const TangentFrame& f, const Vec3& d) { return f.t * d.x + f.b * d.y + f.n * d.z; }

// Two passes of neighbour averaging: keeps large-scale motion, drops fine detail.
void LowPass(const PolyData& topology, std::vector<Vec3>& field) {
    VertexAdjacency adjacency;
    adjacency.build(topology);
    std::vector<Vec3> next(field.size());
    for (int pass = 0; pass < 2; ++pass) {
        for (std::size_t v = 0; v < field.size(); ++v) {
            const std::uint32_t begin = adjacency.offsets[v], end = adjacency.offsets[v + 1];
            if (begin == end) {
                next[v] = field[v];
                continue;
            }
            Vec3 sum;
            for (std::uint32_t i = begin; i < end; ++i) sum += field[adjacency.neighbors[i]];
            next[v] = field[v] * 0.5f + sum * (0.5f / static_cast<float>(end - begin));
        }
        field.swap(next);
    }
}

// Detail of every vertex of `fine` relative to the subdivision of `coarse`.
void ComputeDetail(const PolyData& coarse, const SubdivOptions& options, const PolyData& fineTopology,
                   const std::vector<Vec3>& finePositions, std::vector<Vec3>& detail) {
    EdgeTable edges;
    edges.build(coarse);
    std::vector<Vec3> baseline;
    subdividePositions(coarse, edges, options, coarse.positions, baseline);
    std::vector<TangentFrame> frames;
    computeTangentFrames(fineTopology, baseline, frames);
    detail.resize(finePositions.size());
    for (std::size_t v = 0; v < finePositions.size(); ++v) detail[v] = ToFrame(frames[v], finePositions[v] - baseline[v]);
}

// `fine.positions` holds the subdivided baseline; adds the stored detail.
void ApplyDetail(PolyData& fine, const std::vector<Vec3>& detail) {
    std::vector<TangentFrame> frames;
    computeTangentFrames(fine, fine.positions, frames);
    for (std::size_t v = 0; v < fine.positions.size() && v < detail.size(); ++v)
        fine.positions[v] += FromFrame(frames[v], detail[v]);
}

// First descendant of face `f` of level `from` at level `to` (> from).
std::uint64_t FirstDescendant(std::uint64_t f, int from, int to, const std::vector<std::uint32_t>& baseStarts) {
    for (int level = from; level < to; ++level) f = level == 0 ? baseStarts[f] : f * 4u;
    return f;
}

// --- Binary IO -----------------------------------------------------------------------

class Writer {
public:
    template <class T>
    void put(const T& v) {
        const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
        bytes.insert(bytes.end(), p, p + sizeof(T));
    }
    template <class T>
    void putArray(const std::vector<T>& v) {
        put(static_cast<std::uint32_t>(v.size()));
        const auto* p = reinterpret_cast<const std::uint8_t*>(v.data());
        bytes.insert(bytes.end(), p, p + v.size() * sizeof(T));
    }
    std::vector<std::uint8_t> bytes;
};

class Reader {
public:
    Reader(const std::uint8_t* d, std::size_t n) : data(d), size(n) {}
    template <class T>
    bool get(T& v) {
        if (size - pos < sizeof(T)) return false;
        std::memcpy(&v, data + pos, sizeof(T));
        pos += sizeof(T);
        return true;
    }
    template <class T>
    bool getArray(std::vector<T>& v, std::size_t maxCount = 0x7fffffffu) {
        std::uint32_t n = 0;
        if (!get(n) || n > maxCount || (size - pos) / sizeof(T) < n) return false;
        v.resize(n);
        if (n) std::memcpy(v.data(), data + pos, n * sizeof(T));
        pos += n * sizeof(T);
        return true;
    }
    const std::uint8_t* data;
    std::size_t size;
    std::size_t pos = 0;
};

}  // namespace

void computeTangentFrames(const PolyData& topology, const std::vector<Vec3>& positions, std::vector<TangentFrame>& frames) {
    const std::size_t V = positions.size();
    std::vector<Vec3> normals;
    polyVertexNormals(topology, positions, normals);
    std::vector<std::uint32_t> first(V, EdgeTable::kNone);
    std::size_t c = 0;
    for (std::uint32_t n : topology.faceSizes) {
        for (std::uint32_t k = 0; k < n; ++k) {
            const std::uint32_t v = topology.faceVerts[c + k];
            const std::uint32_t next = topology.faceVerts[c + (k + 1) % n];
            if (first[v] == EdgeTable::kNone && next != v) first[v] = next;
        }
        c += n;
    }
    frames.resize(V);
    for (std::size_t v = 0; v < V; ++v) {
        TangentFrame& f = frames[v];
        const Vec3 n = normals[v];
        if (lengthSq(n) == 0.0f) {
            f = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
            continue;
        }
        Vec3 t;
        if (first[v] != EdgeTable::kNone) {
            const Vec3 d = positions[first[v]] - positions[v];
            t = normalizedOrZero(d - n * dot(d, n));
        }
        if (lengthSq(t) == 0.0f) {  // Any direction perpendicular to n.
            const Vec3 axis = std::fabs(n.x) < 0.9f ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
            t = normalizedOrZero(cross(axis, n));
        }
        f.n = n;
        f.t = t;
        f.b = cross(n, t);
    }
}

bool Multires::reset(PolyData base, std::string* error) {
    if (!base.valid(error)) return false;
    attributes_ = Attributes();
    attributes_.mask = std::move(base.mask);
    attributes_.groups = std::move(base.groups);
    attributes_.hidden = std::move(base.hidden);
    base.mask.clear();
    base.groups.clear();
    base.hidden.clear();
    base_ = std::move(base);
    baseEdgeCount_ = kUnknownCount;
    levels_.clear();
    return true;
}

SubdivOptions Multires::levelOptions(int level) const {
    if (level < 1 || level > topLevel()) return SubdivOptions();
    return levels_[static_cast<std::size_t>(level - 1)].options;
}

std::uint64_t Multires::vertexCount(int level) const {
    if (level <= 0) return base_.positions.size();
    if (baseEdgeCount_ == kUnknownCount) {
        EdgeTable edges;
        edges.build(base_);
        baseEdgeCount_ = edges.size();
    }
    std::uint64_t V = base_.positions.size(), E = baseEdgeCount_, F = base_.faceSizes.size(), C = base_.faceVerts.size();
    for (int i = 0; i < level; ++i) {
        const std::uint64_t v = V + E + F, e = 2 * E + C, f = C, c = 4 * C;
        V = v;
        E = e;
        F = f;
        C = c;
    }
    return V;
}

std::uint64_t Multires::faceCount(int level) const {
    if (level <= 0) return base_.faceSizes.size();
    std::uint64_t F = base_.faceVerts.size();  // Level 1: one quad per corner.
    for (int i = 1; i < level; ++i) F *= 4u;
    return F;
}

void Multires::injectAttributes(PolyData& p) const {
    if (attributes_.mask.size() == p.positions.size()) p.mask = attributes_.mask;
    if (attributes_.groups.size() == p.faceSizes.size()) p.groups = attributes_.groups;
    if (attributes_.hidden.size() == p.faceSizes.size()) p.hidden = attributes_.hidden;
}

bool Multires::buildChain(int level, bool withMaps, std::vector<PolyData>& chain, std::string* error) const {
    if (level < 0 || level > topLevel()) {
        if (error) *error = "level out of range";
        return false;
    }
    chain.clear();
    chain.push_back(TopologyOnly(base_, withMaps));
    if (attributes_.level == 0) injectAttributes(chain.back());
    for (int i = 1; i <= level; ++i) {
        const Level& data = levels_[static_cast<std::size_t>(i - 1)];
        PolyData next;
        if (!subdivide(chain.back(), data.options, next, error)) return false;
        if (next.positions.size() != data.detail.size()) {
            if (error) *error = "multires level does not match its topology";
            return false;
        }
        ApplyDetail(next, data.detail);
        if (attributes_.level == i) injectAttributes(next);
        chain.push_back(std::move(next));
    }
    attributesForLevel(level, chain.back());
    return true;
}

void Multires::attributesForLevel(int level, PolyData& out) const {
    const std::size_t V = out.positions.size(), F = out.faceSizes.size();
    const Attributes& a = attributes_;
    if (a.level > level) {
        // Down: vertices keep their index across levels; faces take their first child's value.
        out.mask.clear();
        if (a.mask.size() >= V && !a.mask.empty()) out.mask.assign(a.mask.begin(), a.mask.begin() + static_cast<std::ptrdiff_t>(V));
        const std::vector<std::uint32_t> baseStarts = base_.faceStarts();
        const std::uint64_t fineFaces = faceCount(a.level);
        out.groups.clear();
        out.hidden.clear();
        if (a.groups.size() == fineFaces) {
            out.groups.resize(F);
            for (std::size_t f = 0; f < F; ++f) out.groups[f] = a.groups[FirstDescendant(f, level, a.level, baseStarts)];
        }
        if (a.hidden.size() == fineFaces) {
            out.hidden.resize(F);
            for (std::size_t f = 0; f < F; ++f) out.hidden[f] = a.hidden[FirstDescendant(f, level, a.level, baseStarts)];
        }
    }
    // a.level <= level: already injected while building (subdivision carries them up).
}

bool Multires::build(int level, PolyData& out, std::string* error) const {
    if (level < 0 || level > topLevel()) {
        if (error) *error = "level out of range";
        return false;
    }
    PolyData current = TopologyOnly(base_, true);
    if (attributes_.level == 0) injectAttributes(current);
    for (int i = 1; i <= level; ++i) {
        const Level& data = levels_[static_cast<std::size_t>(i - 1)];
        PolyData next;
        if (!subdivide(current, data.options, next, error)) return false;
        if (next.positions.size() != data.detail.size()) {
            if (error) *error = "multires level does not match its topology";
            return false;
        }
        ApplyDetail(next, data.detail);
        current = std::move(next);
        if (attributes_.level == i) injectAttributes(current);
    }
    attributesForLevel(level, current);
    out = std::move(current);
    return true;
}

bool Multires::commit(int level, const PolyData& edited, std::string* error) {
    std::vector<PolyData> chain;
    if (!buildChain(level, false, chain, error)) return false;
    PolyData& top = chain.back();
    if (edited.positions.size() != top.positions.size() || edited.faceSizes.size() != top.faceSizes.size()) {
        if (error) *error = "edited mesh does not match the multires level";
        return false;
    }
    // Down-propagate the large-scale part of the edit.
    std::vector<Vec3> delta(top.positions.size());
    bool moved = false;
    for (std::size_t v = 0; v < delta.size(); ++v) {
        delta[v] = edited.positions[v] - top.positions[v];
        moved = moved || lengthSq(delta[v]) > 0.0f;
    }
    if (moved) {
        top.positions = edited.positions;
        for (int i = level; i >= 1; --i) {
            LowPass(chain[static_cast<std::size_t>(i)], delta);
            PolyData& lower = chain[static_cast<std::size_t>(i - 1)];
            delta.resize(lower.positions.size());  // Vertex points keep the parent index.
            for (std::size_t v = 0; v < delta.size(); ++v) lower.positions[v] += delta[v];
        }
        // Re-express every level up to `level` against its new baseline.
        for (int i = 1; i <= level; ++i) {
            Level& data = levels_[static_cast<std::size_t>(i - 1)];
            ComputeDetail(chain[static_cast<std::size_t>(i - 1)], data.options, chain[static_cast<std::size_t>(i)],
                          chain[static_cast<std::size_t>(i)].positions, data.detail);
        }
        base_.positions = chain[0].positions;
    }

    // Attributes: keep the stored ones unless they were edited at this level
    // (`top` holds what build() produced for this level).
    const PolyData& current = top;
    auto same = [](const auto& a, const auto& b, std::size_t n, auto zero) {
        for (std::size_t i = 0; i < n; ++i) {
            const auto x = i < a.size() ? a[i] : zero;
            const auto y = i < b.size() ? b[i] : zero;
            if (x != y) return false;
        }
        return true;
    };
    const std::size_t V = top.positions.size(), F = top.faceSizes.size();
    const bool attributesChanged = !same(edited.mask, current.mask, V, 0.0f) ||
                                   !same(edited.groups, current.groups, F, std::int32_t(0)) ||
                                   !same(edited.hidden, current.hidden, F, std::uint8_t(0));
    if (attributesChanged) {
        attributes_.level = level;
        attributes_.mask = edited.mask;
        attributes_.groups = edited.groups;
        attributes_.hidden = edited.hidden;
    }
    return true;
}

bool Multires::addLevel(const SubdivOptions& options, std::string* error) {
    if (topLevel() >= kMaxLevel) {
        if (error) *error = "the Multires stack is limited to 6 levels";
        return false;
    }
    const std::uint64_t vertices = vertexCount(topLevel() + 1);
    if (vertices > 0x7fffffffu || faceCount(topLevel() + 1) > 0x7fffffffu) {
        if (error) *error = "the next level would be too large";
        return false;
    }
    Level level;
    level.options = options;
    level.detail.assign(static_cast<std::size_t>(vertices), Vec3());
    levels_.push_back(std::move(level));
    return true;
}

bool Multires::deleteHigher(int level) {
    if (level < 0 || level >= topLevel()) return false;
    if (attributes_.level > level) {
        PolyData converted;
        converted.positions.resize(static_cast<std::size_t>(vertexCount(level)));
        converted.faceSizes.resize(static_cast<std::size_t>(faceCount(level)), 4u);
        attributesForLevel(level, converted);
        attributes_.level = level;
        attributes_.mask = std::move(converted.mask);
        attributes_.groups = std::move(converted.groups);
        attributes_.hidden = std::move(converted.hidden);
    }
    levels_.resize(static_cast<std::size_t>(level));
    return true;
}

bool Multires::deleteLower(int level, std::string* error) {
    if (level <= 0 || level > topLevel()) return false;
    PolyData newBase;
    if (!build(level, newBase, error)) return false;
    if (attributes_.level >= level) {
        attributes_.level -= level;  // Same topology, new index.
    } else {
        attributes_.level = 0;
        attributes_.mask = newBase.mask;
        attributes_.groups = newBase.groups;
        attributes_.hidden = newBase.hidden;
    }
    newBase.mask.clear();
    newBase.groups.clear();
    newBase.hidden.clear();
    base_ = std::move(newBase);
    baseEdgeCount_ = kUnknownCount;
    levels_.erase(levels_.begin(), levels_.begin() + level);
    return true;
}

bool Multires::reverseSubdivision(std::string* error) {
    if (topLevel() >= kMaxLevel) {
        if (error) *error = "the Multires stack is limited to 6 levels";
        return false;
    }
    std::vector<PolyData> oldChain;
    if (!buildChain(topLevel(), false, oldChain, error)) return false;

    PolyData coarse;
    std::vector<std::uint32_t> vertexMap, faceMap;
    std::vector<std::uint8_t> rotation;
    PolyData oldBase = base_;  // With maps, for the coarse corners.
    if (!unsubdivide(oldBase, coarse, error, &vertexMap, &faceMap, &rotation)) return false;
    coarse.mask.clear();
    coarse.groups.clear();
    coarse.hidden.clear();

    // New level i + 1 is old level i, renumbered by subdivision order.
    const int oldTop = topLevel();
    std::vector<PolyData> newChain;
    newChain.push_back(TopologyOnly(coarse, false));
    std::vector<std::vector<std::uint32_t>> vertexMaps, faceMaps;
    for (int i = 0; i <= oldTop; ++i) {
        PolyData next;
        const SubdivOptions options = i == 0 ? SubdivOptions() : levels_[static_cast<std::size_t>(i - 1)].options;
        if (!subdivide(newChain.back(), options, next, error)) return false;
        if (next.positions.size() != oldChain[static_cast<std::size_t>(i)].positions.size()) {
            if (error) *error = "reverse subdivision produced a different topology";
            return false;
        }
        for (std::size_t v = 0; v < next.positions.size(); ++v)
            next.positions[v] = oldChain[static_cast<std::size_t>(i)].positions[vertexMap[v]];
        vertexMaps.push_back(vertexMap);
        faceMaps.push_back(faceMap);
        if (i < oldTop) subdivisionCorrespondence(next, oldChain[static_cast<std::size_t>(i)], vertexMap, faceMap, rotation);
        newChain.push_back(std::move(next));
    }

    std::vector<Level> levels(static_cast<std::size_t>(oldTop + 1));
    for (int i = 1; i <= oldTop + 1; ++i) {
        Level& data = levels[static_cast<std::size_t>(i - 1)];
        data.options = i == 1 ? SubdivOptions() : levels_[static_cast<std::size_t>(i - 2)].options;
        ComputeDetail(newChain[static_cast<std::size_t>(i - 1)], data.options, newChain[static_cast<std::size_t>(i)],
                      newChain[static_cast<std::size_t>(i)].positions, data.detail);
    }
    // Attributes move up one level and follow the renumbering.
    Attributes attributes;
    attributes.level = attributes_.level + 1;
    const std::vector<std::uint32_t>& vmap = vertexMaps[static_cast<std::size_t>(attributes_.level)];
    const std::vector<std::uint32_t>& fmap = faceMaps[static_cast<std::size_t>(attributes_.level)];
    if (attributes_.mask.size() == oldChain[static_cast<std::size_t>(attributes_.level)].positions.size()) {
        attributes.mask.resize(vmap.size());
        for (std::size_t v = 0; v < vmap.size(); ++v) attributes.mask[v] = attributes_.mask[vmap[v]];
    }
    const std::size_t oldFaces = oldChain[static_cast<std::size_t>(attributes_.level)].faceSizes.size();
    if (attributes_.groups.size() == oldFaces) {
        attributes.groups.resize(fmap.size());
        for (std::size_t f = 0; f < fmap.size(); ++f) attributes.groups[f] = attributes_.groups[fmap[f]];
    }
    if (attributes_.hidden.size() == oldFaces) {
        std::vector<std::uint8_t> hidden(fmap.size(), 0u);
        for (std::size_t f = 0; f < fmap.size(); ++f) hidden[f] = attributes_.hidden[fmap[f]];
        attributes.hidden.swap(hidden);
    }

    base_ = std::move(coarse);
    baseEdgeCount_ = kUnknownCount;
    levels_ = std::move(levels);
    attributes_ = std::move(attributes);
    return true;
}

std::size_t Multires::memoryBytes() const {
    std::size_t bytes = base_.memoryBytes();
    for (const Level& level : levels_) bytes += level.detail.size() * sizeof(Vec3);
    bytes += attributes_.mask.size() * 4 + attributes_.groups.size() * 4 + attributes_.hidden.size();
    return bytes;
}

std::vector<std::uint8_t> Multires::serialize() const {
    Writer w;
    w.put(kMagic);
    w.put(kVersion);
    w.putArray(base_.positions);
    w.putArray(base_.faceSizes);
    w.putArray(base_.faceVerts);
    w.putArray(base_.material);
    w.putArray(base_.smoothing);
    w.put(static_cast<std::uint32_t>(base_.maps.size()));
    for (const CornerMap& map : base_.maps) {
        w.put(static_cast<std::int32_t>(map.channel));
        w.putArray(map.values);
    }
    w.put(static_cast<std::uint32_t>(levels_.size()));
    for (const Level& level : levels_) {
        const std::uint8_t flags = static_cast<std::uint8_t>((level.options.creaseMaterials ? 1u : 0u) |
                                                             (level.options.creaseSmoothing ? 2u : 0u));
        w.put(flags);
        float scale = 0.0f;
        for (const Vec3& d : level.detail)
            scale = std::max({scale, std::fabs(d.x), std::fabs(d.y), std::fabs(d.z)});
        if (!(scale > 0.0f) || !std::isfinite(scale)) scale = 1.0f;
        w.put(scale);
        std::vector<std::uint16_t> half(level.detail.size() * 3);
        for (std::size_t v = 0; v < level.detail.size(); ++v) {
            half[3 * v + 0] = floatToHalf(level.detail[v].x / scale);
            half[3 * v + 1] = floatToHalf(level.detail[v].y / scale);
            half[3 * v + 2] = floatToHalf(level.detail[v].z / scale);
        }
        w.putArray(half);
    }
    w.put(static_cast<std::int32_t>(attributes_.level));
    std::vector<std::uint16_t> mask(attributes_.mask.size());
    for (std::size_t v = 0; v < mask.size(); ++v)
        mask[v] = static_cast<std::uint16_t>(std::lround(clamp01(attributes_.mask[v]) * 65535.0f));
    w.putArray(mask);
    w.putArray(attributes_.groups);
    w.putArray(attributes_.hidden);
    return std::move(w.bytes);
}

bool Multires::deserialize(const std::uint8_t* data, std::size_t size, std::string* error) {
    auto fail = [error](const char* message) {
        if (error) *error = message;
        return false;
    };
    Reader r(data, size);
    std::uint32_t magic = 0, version = 0;
    if (!r.get(magic) || magic != kMagic) return fail("not Multires data");
    if (!r.get(version) || version != kVersion) return fail("unsupported Multires data version");
    PolyData base;
    std::uint32_t mapCount = 0;
    if (!r.getArray(base.positions) || !r.getArray(base.faceSizes) || !r.getArray(base.faceVerts) ||
        !r.getArray(base.material) || !r.getArray(base.smoothing) || !r.get(mapCount) || mapCount > 128)
        return fail("damaged Multires base");
    base.maps.resize(mapCount);
    for (CornerMap& map : base.maps) {
        std::int32_t channel = 0;
        if (!r.get(channel) || !r.getArray(map.values)) return fail("damaged Multires map");
        map.channel = channel;
    }
    if (!base.valid(error)) return false;
    std::uint32_t levelCount = 0;
    if (!r.get(levelCount) || levelCount > static_cast<std::uint32_t>(kMaxLevel)) return fail("damaged Multires levels");
    std::vector<Level> levels(levelCount);
    for (Level& level : levels) {
        std::uint8_t flags = 0;
        float scale = 1.0f;
        std::vector<std::uint16_t> half;
        if (!r.get(flags) || !r.get(scale) || !r.getArray(half) || half.size() % 3 != 0 || !std::isfinite(scale))
            return fail("damaged Multires level");
        level.options.creaseMaterials = (flags & 1u) != 0;
        level.options.creaseSmoothing = (flags & 2u) != 0;
        level.detail.resize(half.size() / 3);
        for (std::size_t v = 0; v < level.detail.size(); ++v)
            level.detail[v] = Vec3{halfToFloat(half[3 * v]), halfToFloat(half[3 * v + 1]), halfToFloat(half[3 * v + 2])} * scale;
    }
    Attributes attributes;
    std::int32_t attributeLevel = 0;
    std::vector<std::uint16_t> mask;
    if (!r.get(attributeLevel) || attributeLevel < 0 || attributeLevel > static_cast<std::int32_t>(levelCount) ||
        !r.getArray(mask) || !r.getArray(attributes.groups) || !r.getArray(attributes.hidden))
        return fail("damaged Multires attributes");
    attributes.level = attributeLevel;
    attributes.mask.resize(mask.size());
    for (std::size_t v = 0; v < mask.size(); ++v) attributes.mask[v] = static_cast<float>(mask[v]) / 65535.0f;

    Multires loaded;
    loaded.base_ = std::move(base);
    loaded.levels_ = std::move(levels);
    loaded.attributes_ = std::move(attributes);
    // Every level must match the size its topology predicts.
    for (int i = 1; i <= loaded.topLevel(); ++i)
        if (loaded.levels_[static_cast<std::size_t>(i - 1)].detail.size() != loaded.vertexCount(i))
            return fail("Multires level size mismatch");
    loaded.baseEdgeCount_ = kUnknownCount;
    *this = std::move(loaded);
    return true;
}

}  // namespace sculpt
