#include "sculpt/remesh.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace sculpt {

namespace {

struct Tri {
    std::array<std::uint32_t, 3> v;
    std::uint32_t parent;  // Original polygon (face attributes).
    bool alive = true;
};

struct EdgeRef {
    std::uint64_t key;
    std::uint32_t tri;
    std::uint8_t k;  // Edge from corner k to corner k+1.
};

std::uint64_t Key(std::uint32_t a, std::uint32_t b) {
    if (a > b) std::swap(a, b);
    return (static_cast<std::uint64_t>(a) << 32) | b;
}

class Remesher {
public:
    Remesher(const PolyData& in, const std::vector<float>& weight) : in_(in) {
        pos_ = in.positions;
        mask_ = in.mask;
        if (mask_.size() != pos_.size()) mask_.assign(pos_.size(), 0.0f);
        weight_ = weight;
        weight_.resize(pos_.size(), 0.0f);
        corners_.resize(in.maps.size());
    }

    // Triangulates every polygon touching a weighted vertex.
    bool Collect() {
        const std::vector<std::uint32_t> starts = in_.faceStarts();
        inRegion_.assign(in_.faceSizes.size(), 0u);
        for (std::size_t f = 0; f < in_.faceSizes.size(); ++f) {
            const std::uint32_t n = in_.faceSizes[f];
            bool touched = false;
            for (std::uint32_t k = 0; k < n && !touched; ++k) touched = weight_[in_.faceVerts[starts[f] + k]] > 0.0f;
            if (!touched) continue;
            inRegion_[f] = 1u;
            for (std::uint32_t k = 1; k + 1 < n; ++k) {
                Tri t;
                t.v = {in_.faceVerts[starts[f]], in_.faceVerts[starts[f] + k], in_.faceVerts[starts[f] + k + 1]};
                t.parent = static_cast<std::uint32_t>(f);
                tris_.push_back(t);
                for (std::size_t m = 0; m < in_.maps.size(); ++m) {
                    const std::vector<Vec3>& src = in_.maps[m].values;
                    corners_[m].push_back(src[starts[f]]);
                    corners_[m].push_back(src[starts[f] + k]);
                    corners_[m].push_back(src[starts[f] + k + 1]);
                }
            }
        }
        // Vertices also used by untouched polygons must not move or disappear.
        locked_.assign(pos_.size(), 0u);
        for (std::size_t f = 0; f < in_.faceSizes.size(); ++f) {
            if (inRegion_[f]) continue;
            for (std::uint32_t k = 0; k < in_.faceSizes[f]; ++k) locked_[in_.faceVerts[starts[f] + k]] = 1u;
        }
        return !tris_.empty();
    }

    void Refine(float targetLength, std::size_t maxNew) {
        const std::size_t limit = pos_.size() + maxNew;
        for (int pass = 0; pass < 64; ++pass) {
            BuildEdges();
            struct Candidate {
                float length;
                std::size_t begin, count;
            };
            std::vector<Candidate> candidates;
            ForEachEdge([&](std::size_t begin, std::size_t count) {
                if (count > 2) return;  // Non-manifold: leave alone.
                const Tri& t = tris_[edges_[begin].tri];
                const std::uint32_t a = t.v[edges_[begin].k], b = t.v[(edges_[begin].k + 1) % 3];
                const float w = 0.5f * (weight_[a] + weight_[b]);
                if (!(w > 0.0f)) return;
                const float length = sculpt::length(pos_[a] - pos_[b]);
                if (length > Target(targetLength, w)) candidates.push_back({length, begin, count});
            });
            if (candidates.empty()) break;
            std::sort(candidates.begin(), candidates.end(), [](const Candidate& x, const Candidate& y) { return x.length > y.length; });
            std::vector<std::uint8_t> modified(tris_.size(), 0u);
            bool any = false;
            for (const Candidate& c : candidates) {
                if (pos_.size() >= limit) break;
                bool free = true;
                for (std::size_t i = 0; i < c.count; ++i) free = free && !modified[edges_[c.begin + i].tri];
                if (!free) continue;
                Split(c.begin, c.count, modified);
                any = true;
            }
            if (!any || pos_.size() >= limit) break;
        }
        Relax();
    }

    void Reduce(float targetLength) {
        for (int pass = 0; pass < 64; ++pass) {
            BuildEdges();
            BuildVertexTris();
            std::vector<std::uint8_t> fixed = locked_;
            fixed.resize(pos_.size(), 0u);
            ForEachEdge([&](std::size_t begin, std::size_t count) {
                if (count != 2) {  // Open border or non-manifold edge: its vertices stay.
                    const Tri& t = tris_[edges_[begin].tri];
                    fixed[t.v[edges_[begin].k]] = 1u;
                    fixed[t.v[(edges_[begin].k + 1) % 3]] = 1u;
                }
            });
            struct Candidate {
                float length;
                std::size_t begin;
            };
            std::vector<Candidate> candidates;
            ForEachEdge([&](std::size_t begin, std::size_t count) {
                if (count != 2) return;
                const Tri& t = tris_[edges_[begin].tri];
                const std::uint32_t a = t.v[edges_[begin].k], b = t.v[(edges_[begin].k + 1) % 3];
                if (fixed[a] || fixed[b]) return;
                const float w = std::min(weight_[a], weight_[b]);
                if (!(w > 0.0f)) return;
                const float length = sculpt::length(pos_[a] - pos_[b]);
                if (length < targetLength * std::min(w, 1.0f)) candidates.push_back({length, begin});  // Less where faintly painted.
            });
            if (candidates.empty()) break;
            std::sort(candidates.begin(), candidates.end(), [](const Candidate& x, const Candidate& y) { return x.length < y.length; });
            std::vector<std::uint8_t> touched(pos_.size(), 0u);
            bool any = false;
            for (const Candidate& c : candidates)
                if (Collapse(c.begin, touched)) any = true;
            if (!any) break;
        }
    }

    bool Assemble(PolyData& out, std::string* error) {
        const std::vector<std::uint32_t> starts = in_.faceStarts();
        PolyData result;
        std::vector<std::uint32_t> remap(pos_.size(), 0xffffffffu);
        auto use = [&](std::uint32_t v) {
            if (remap[v] == 0xffffffffu) {
                remap[v] = static_cast<std::uint32_t>(result.positions.size());
                result.positions.push_back(pos_[v]);
                result.mask.push_back(mask_[v]);
            }
            return remap[v];
        };
        // Number the surviving vertices in their original order (new ones last).
        std::vector<std::uint8_t> used(pos_.size(), 0u);
        for (std::size_t f = 0; f < in_.faceSizes.size(); ++f)
            if (!inRegion_[f])
                for (std::uint32_t k = 0; k < in_.faceSizes[f]; ++k) used[in_.faceVerts[starts[f] + k]] = 1u;
        for (const Tri& tri : tris_)
            if (tri.alive)
                for (std::uint32_t v : tri.v) used[v] = 1u;
        for (std::size_t v = 0; v < pos_.size(); ++v)
            if (used[v]) use(static_cast<std::uint32_t>(v));
        result.maps.resize(in_.maps.size());
        for (std::size_t m = 0; m < in_.maps.size(); ++m) result.maps[m].channel = in_.maps[m].channel;
        auto addAttributes = [&](std::uint32_t parent) {
            if (!in_.material.empty()) result.material.push_back(in_.material[parent]);
            if (!in_.smoothing.empty()) result.smoothing.push_back(in_.smoothing[parent]);
            if (!in_.groups.empty()) result.groups.push_back(in_.groups[parent]);
            if (!in_.hidden.empty()) result.hidden.push_back(in_.hidden[parent]);
        };
        for (std::size_t f = 0; f < in_.faceSizes.size(); ++f) {
            if (inRegion_[f]) continue;
            const std::uint32_t n = in_.faceSizes[f];
            result.faceSizes.push_back(n);
            for (std::uint32_t k = 0; k < n; ++k) {
                result.faceVerts.push_back(use(in_.faceVerts[starts[f] + k]));
                for (std::size_t m = 0; m < in_.maps.size(); ++m) result.maps[m].values.push_back(in_.maps[m].values[starts[f] + k]);
            }
            addAttributes(static_cast<std::uint32_t>(f));
        }
        for (std::size_t t = 0; t < tris_.size(); ++t) {
            const Tri& tri = tris_[t];
            if (!tri.alive || tri.v[0] == tri.v[1] || tri.v[1] == tri.v[2] || tri.v[0] == tri.v[2]) continue;
            result.faceSizes.push_back(3u);
            for (int k = 0; k < 3; ++k) {
                result.faceVerts.push_back(use(tri.v[k]));
                for (std::size_t m = 0; m < in_.maps.size(); ++m) result.maps[m].values.push_back(corners_[m][3 * t + k]);
            }
            addAttributes(tri.parent);
        }
        if (in_.mask.empty()) result.mask.clear();
        if (!result.valid(error)) return false;
        out = std::move(result);
        return true;
    }

private:
    static float Target(float targetLength, float w) { return targetLength / std::min(std::max(w, 0.1f), 1.0f); }

    void BuildEdges() {
        edges_.clear();
        for (std::size_t t = 0; t < tris_.size(); ++t) {
            if (!tris_[t].alive) continue;
            for (std::uint8_t k = 0; k < 3; ++k)
                edges_.push_back({Key(tris_[t].v[k], tris_[t].v[(k + 1) % 3]), static_cast<std::uint32_t>(t), k});
        }
        std::sort(edges_.begin(), edges_.end(), [](const EdgeRef& x, const EdgeRef& y) { return x.key < y.key; });
    }

    template <class Fn>
    void ForEachEdge(Fn fn) {
        for (std::size_t i = 0; i < edges_.size();) {
            std::size_t j = i + 1;
            while (j < edges_.size() && edges_[j].key == edges_[i].key) ++j;
            fn(i, j - i);
            i = j;
        }
    }

    void BuildVertexTris() {
        vtStart_.assign(pos_.size() + 1, 0u);
        for (const Tri& t : tris_)
            if (t.alive)
                for (std::uint32_t v : t.v) ++vtStart_[v + 1];
        for (std::size_t v = 0; v < pos_.size(); ++v) vtStart_[v + 1] += vtStart_[v];
        vt_.assign(vtStart_.back(), 0u);
        std::vector<std::uint32_t> fill(vtStart_.begin(), vtStart_.end() - 1);
        for (std::size_t t = 0; t < tris_.size(); ++t)
            if (tris_[t].alive)
                for (std::uint32_t v : tris_[t].v) vt_[fill[v]++] = static_cast<std::uint32_t>(t);
    }

    void Split(std::size_t begin, std::size_t count, std::vector<std::uint8_t>& modified) {
        const Tri& first = tris_[edges_[begin].tri];
        const std::uint32_t a = first.v[edges_[begin].k], b = first.v[(edges_[begin].k + 1) % 3];
        const std::uint32_t m = static_cast<std::uint32_t>(pos_.size());
        pos_.push_back((pos_[a] + pos_[b]) * 0.5f);
        mask_.push_back(0.5f * (mask_[a] + mask_[b]));
        weight_.push_back(0.5f * (weight_[a] + weight_[b]));
        locked_.push_back(0u);
        created_.push_back(m);
        for (std::size_t i = 0; i < count; ++i) {
            const std::uint32_t t = edges_[begin + i].tri;
            const std::uint8_t k = edges_[begin + i].k;
            const std::array<std::uint32_t, 3> v = tris_[t].v;
            const std::uint32_t x = v[k], y = v[(k + 1) % 3], c = v[(k + 2) % 3];
            Tri second;
            second.v = {m, y, c};
            second.parent = tris_[t].parent;
            tris_[t].v = {x, m, c};
            for (std::size_t mp = 0; mp < corners_.size(); ++mp) {
                std::vector<Vec3>& cv = corners_[mp];
                const Vec3 ux = cv[3 * t + k], uy = cv[3 * t + (k + 1) % 3], uc = cv[3 * t + (k + 2) % 3];
                const Vec3 um = (ux + uy) * 0.5f;
                cv[3 * t + 0] = ux;
                cv[3 * t + 1] = um;
                cv[3 * t + 2] = uc;
                cv.push_back(um);
                cv.push_back(uy);
                cv.push_back(uc);
            }
            tris_.push_back(second);
            modified[t] = 1u;
            modified.push_back(1u);
        }
    }

    bool Collapse(std::size_t begin, std::vector<std::uint8_t>& touched) {
        const std::uint32_t t0 = edges_[begin].tri, t1 = edges_[begin + 1].tri;
        if (!tris_[t0].alive || !tris_[t1].alive) return false;
        const std::uint32_t a = tris_[t0].v[edges_[begin].k], b = tris_[t0].v[(edges_[begin].k + 1) % 3];
        if (touched[a] || touched[b]) return false;
        auto opposite = [&](std::uint32_t t) {
            for (std::uint32_t v : tris_[t].v)
                if (v != a && v != b) return v;
            return a;
        };
        const std::uint32_t c = opposite(t0), d = opposite(t1);
        if (c == d) return false;
        // Link condition: a and b share exactly the neighbours c and d.
        auto neighbors = [&](std::uint32_t v, std::vector<std::uint32_t>& out) {
            out.clear();
            for (std::uint32_t i = vtStart_[v]; i < vtStart_[v + 1]; ++i) {
                const Tri& t = tris_[vt_[i]];
                if (!t.alive) continue;
                for (std::uint32_t u : t.v)
                    if (u != v) out.push_back(u);
            }
            std::sort(out.begin(), out.end());
            out.erase(std::unique(out.begin(), out.end()), out.end());
        };
        std::vector<std::uint32_t> na, nb, common;
        neighbors(a, na);
        neighbors(b, nb);
        std::set_intersection(na.begin(), na.end(), nb.begin(), nb.end(), std::back_inserter(common));
        if (common.size() != 2) return false;
        const Vec3 p = (pos_[a] + pos_[b]) * 0.5f;
        // No triangle may flip.
        for (std::uint32_t v : {a, b}) {
            for (std::uint32_t i = vtStart_[v]; i < vtStart_[v + 1]; ++i) {
                const std::uint32_t t = vt_[i];
                if (t == t0 || t == t1 || !tris_[t].alive) continue;
                std::array<Vec3, 3> before, after;
                for (int k = 0; k < 3; ++k) {
                    const std::uint32_t u = tris_[t].v[k];
                    before[k] = pos_[u];
                    after[k] = (u == a || u == b) ? p : pos_[u];
                }
                const Vec3 n0 = cross(before[1] - before[0], before[2] - before[0]);
                const Vec3 n1 = cross(after[1] - after[0], after[2] - after[0]);
                if (dot(n0, n1) <= 0.1f * length(n0) * length(n1)) return false;
            }
        }
        pos_[a] = p;
        mask_[a] = 0.5f * (mask_[a] + mask_[b]);
        weight_[a] = 0.5f * (weight_[a] + weight_[b]);
        tris_[t0].alive = false;
        tris_[t1].alive = false;
        for (std::uint32_t i = vtStart_[b]; i < vtStart_[b + 1]; ++i) {
            Tri& t = tris_[vt_[i]];
            for (std::uint32_t& u : t.v)
                if (u == b) u = a;
        }
        for (std::uint32_t v : na) touched[v] = 1u;
        for (std::uint32_t v : nb) touched[v] = 1u;
        touched[a] = touched[b] = 1u;
        return true;
    }

    // Tangential smoothing of the new vertices: evens out the triangles
    // without shrinking the surface.
    void Relax() {
        if (created_.empty()) return;
        BuildEdges();
        std::vector<std::uint8_t> border(pos_.size(), 0u);
        ForEachEdge([&](std::size_t begin, std::size_t count) {
            if (count == 2) return;
            const Tri& t = tris_[edges_[begin].tri];
            border[t.v[edges_[begin].k]] = border[t.v[(edges_[begin].k + 1) % 3]] = 1u;
        });
        BuildVertexTris();
        // Map values move with the vertex (same blend), unless the vertex sits
        // on a seam where its corners disagree.
        std::vector<std::uint8_t> seam(created_.size(), 0u);
        for (std::size_t i = 0; i < created_.size(); ++i) {
            const std::uint32_t v = created_[i];
            for (std::size_t m = 0; m < corners_.size() && !seam[i]; ++m) {
                bool first = true;
                Vec3 value;
                for (std::uint32_t j = vtStart_[v]; j < vtStart_[v + 1]; ++j) {
                    const std::uint32_t t = vt_[j];
                    for (int k = 0; k < 3; ++k) {
                        if (tris_[t].v[k] != v) continue;
                        if (first) value = corners_[m][3 * t + k];
                        else if (corners_[m][3 * t + k] != value) seam[i] = 1u;
                        first = false;
                    }
                }
            }
        }
        std::vector<Vec3> next;
        std::vector<std::vector<Vec3>> nextValues(corners_.size());
        for (int iteration = 0; iteration < 3; ++iteration) {
            next.assign(created_.size(), Vec3());
            for (auto& values : nextValues) values.assign(created_.size(), Vec3());
            std::vector<std::uint8_t> moved(created_.size(), 0u);
            for (std::size_t i = 0; i < created_.size(); ++i) {
                const std::uint32_t v = created_[i];
                next[i] = pos_[v];
                if (border[v] || seam[i]) continue;
                Vec3 normal, sum;
                std::vector<Vec3> valueSum(corners_.size());
                float count = 0.0f;
                for (std::uint32_t j = vtStart_[v]; j < vtStart_[v + 1]; ++j) {
                    const std::uint32_t t = vt_[j];
                    const Tri& tri = tris_[t];
                    normal += cross(pos_[tri.v[1]] - pos_[tri.v[0]], pos_[tri.v[2]] - pos_[tri.v[0]]);
                    for (int k = 0; k < 3; ++k) {
                        const std::uint32_t u = tri.v[k];
                        if (u == v) continue;
                        sum += pos_[u];
                        for (std::size_t m = 0; m < corners_.size(); ++m) valueSum[m] += corners_[m][3 * t + k];
                        count += 1.0f;
                    }
                }
                if (count == 0.0f) continue;
                const Vec3 n = normalizedOrZero(normal);
                const Vec3 offset = sum / count - pos_[v];
                const Vec3 tangential = offset - n * dot(offset, n);
                // Blend factor along the neighbour average that the tangential move represents.
                const float along = lengthSq(offset) > 0.0f ? 0.5f * dot(tangential, offset) / lengthSq(offset) : 0.0f;
                next[i] = pos_[v] + tangential * 0.5f;
                for (std::size_t m = 0; m < corners_.size(); ++m) {
                    Vec3 current;
                    for (std::uint32_t j = vtStart_[v]; j < vtStart_[v + 1]; ++j) {
                        const std::uint32_t t = vt_[j];
                        for (int k = 0; k < 3; ++k)
                            if (tris_[t].v[k] == v) current = corners_[m][3 * t + k];
                    }
                    nextValues[m][i] = current + (valueSum[m] / count - current) * along;
                }
                moved[i] = 1u;
            }
            for (std::size_t i = 0; i < created_.size(); ++i) {
                const std::uint32_t v = created_[i];
                pos_[v] = next[i];
                if (!moved[i]) continue;
                for (std::size_t m = 0; m < corners_.size(); ++m)
                    for (std::uint32_t j = vtStart_[v]; j < vtStart_[v + 1]; ++j) {
                        const std::uint32_t t = vt_[j];
                        for (int k = 0; k < 3; ++k)
                            if (tris_[t].v[k] == v) corners_[m][3 * t + k] = nextValues[m][i];
                    }
            }
        }
    }

    const PolyData& in_;
    std::vector<Vec3> pos_;
    std::vector<float> mask_;
    std::vector<float> weight_;
    std::vector<std::uint8_t> locked_;
    std::vector<std::uint8_t> inRegion_;
    std::vector<Tri> tris_;
    std::vector<std::vector<Vec3>> corners_;  // Per map: 3 values per triangle.
    std::vector<EdgeRef> edges_;
    std::vector<std::uint32_t> vtStart_, vt_;
    std::vector<std::uint32_t> created_;
};

}  // namespace

bool remeshRegion(PolyData& poly, const std::vector<float>& weight, const DensityOptions& options, std::string* error) {
    if (!poly.valid(error)) return false;
    if (!(options.targetLength > 0.0f) || !std::isfinite(options.targetLength)) {
        if (error) *error = "invalid target edge length";
        return false;
    }
    Remesher remesher(poly, weight);
    if (!remesher.Collect()) {
        if (error) *error = "nothing painted";
        return false;
    }
    if (options.reduce)
        remesher.Reduce(options.targetLength);
    else
        remesher.Refine(options.targetLength, options.maxNewVertices);
    return remesher.Assemble(poly, error);
}

}  // namespace sculpt
