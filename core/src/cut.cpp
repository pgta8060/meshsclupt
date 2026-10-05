#include "sculpt/cut.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>
#include <unordered_map>

namespace sculpt {

namespace {

constexpr std::uint32_t kNone = 0xffffffffu;
constexpr float kTwoPi = 6.28318530718f;

std::uint64_t PairKey(std::uint32_t a, std::uint32_t b) {
    if (a > b) std::swap(a, b);
    return (static_cast<std::uint64_t>(a) << 32) | b;
}
std::uint64_t DirectedKey(std::uint32_t a, std::uint32_t b) { return (static_cast<std::uint64_t>(a) << 32) | b; }

struct Builder {
    // Output mesh being assembled.
    std::vector<Vec3> positions;
    std::vector<float> mask;
    std::vector<std::uint8_t> onCut;  // Vertex lies on the cut surface.
    std::vector<std::uint32_t> sizes, verts;
    std::vector<std::vector<Vec3>> maps;
    std::vector<std::uint32_t> parent;  // Source face (kNone: cap).
    std::vector<std::uint8_t> side;     // 0 kept / first part, 1 second part.
    std::vector<std::uint16_t> capMaterial;

    void addFace(const std::vector<std::uint32_t>& v, const std::vector<std::vector<Vec3>>& values, std::uint32_t from,
                 std::uint8_t s, std::uint16_t material = 0) {
        sizes.push_back(static_cast<std::uint32_t>(v.size()));
        verts.insert(verts.end(), v.begin(), v.end());
        for (std::size_t m = 0; m < maps.size(); ++m)
            if (m < values.size() && values[m].size() == v.size())
                maps[m].insert(maps[m].end(), values[m].begin(), values[m].end());
            else
                maps[m].insert(maps[m].end(), v.size(), Vec3());
        parent.push_back(from);
        side.push_back(s);
        capMaterial.push_back(material);
    }
};

// Triangulates a planar-ish loop (cap order) by ear clipping in its best-fit plane.
bool EarClip(const std::vector<Vec3>& loop, std::vector<std::array<std::uint32_t, 3>>& out) {
    const std::size_t n = loop.size();
    if (n < 3) return false;
    Vec3 normal;
    for (std::size_t i = 0; i < n; ++i) {
        const Vec3& a = loop[i];
        const Vec3& b = loop[(i + 1) % n];
        normal.x += (a.y - b.y) * (a.z + b.z);
        normal.y += (a.z - b.z) * (a.x + b.x);
        normal.z += (a.x - b.x) * (a.y + b.y);
    }
    normal = normalizedOrZero(normal);
    if (lengthSq(normal) == 0.0f) return false;
    const Vec3 axis = std::fabs(normal.x) < 0.9f ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
    const Vec3 u = normalizedOrZero(cross(axis, normal)), v = cross(normal, u);
    std::vector<std::array<float, 2>> p(n);
    for (std::size_t i = 0; i < n; ++i) p[i] = {dot(loop[i], u), dot(loop[i], v)};
    auto cross2 = [&](std::size_t a, std::size_t b, std::size_t c) {
        return (p[b][0] - p[a][0]) * (p[c][1] - p[a][1]) - (p[b][1] - p[a][1]) * (p[c][0] - p[a][0]);
    };
    std::vector<std::uint32_t> idx(n);
    for (std::size_t i = 0; i < n; ++i) idx[i] = static_cast<std::uint32_t>(i);
    std::size_t guard = 0;
    while (idx.size() > 3 && guard++ < n * n) {
        bool clipped = false;
        for (std::size_t i = 0; i < idx.size(); ++i) {
            const std::uint32_t a = idx[(i + idx.size() - 1) % idx.size()], b = idx[i], c = idx[(i + 1) % idx.size()];
            if (cross2(a, b, c) <= 0.0f) continue;  // Reflex (the loop is counter-clockwise in this basis).
            bool inside = false;
            for (std::uint32_t q : idx) {
                if (q == a || q == b || q == c) continue;
                if (cross2(a, b, q) >= 0.0f && cross2(b, c, q) >= 0.0f && cross2(c, a, q) >= 0.0f) {
                    inside = true;
                    break;
                }
            }
            if (inside) continue;
            out.push_back({a, b, c});
            idx.erase(idx.begin() + static_cast<std::ptrdiff_t>(i));
            clipped = true;
            break;
        }
        if (!clipped) return false;
    }
    if (idx.size() == 3) out.push_back({idx[0], idx[1], idx[2]});
    return true;
}

}  // namespace

bool cutMesh(PolyData& poly, const CutOptions& o, CutStats* stats, std::string* error) {
    auto fail = [error](const char* message) {
        if (error) *error = message;
        return false;
    };
    if (!o.field) return fail("no cut shape");
    if (!poly.valid(error)) return false;
    const std::size_t V = poly.positions.size(), F = poly.faceSizes.size();
    const std::vector<std::uint32_t> starts = poly.faceStarts();

    std::vector<float> f(V);
    float maxAbs = 0.0f;
    for (std::size_t v = 0; v < V; ++v) {
        f[v] = o.field(poly.positions[v]);
        if (!std::isfinite(f[v])) f[v] = 1.0f;
        maxAbs = std::max(maxAbs, std::fabs(f[v]));
    }
    const float eps = maxAbs * 1e-6f;
    bool anyNeg = false, anyPos = false;
    for (float& value : f) {
        if (std::fabs(value) <= eps) value = 0.0f;
        anyNeg = anyNeg || value < 0.0f;
        anyPos = anyPos || value > 0.0f;
    }
    if (!anyNeg || !anyPos) return fail("the shape does not cross the mesh");

    Builder out;
    out.positions = poly.positions;
    out.mask = poly.mask.size() == V ? poly.mask : std::vector<float>(V, 0.0f);
    out.onCut.assign(V, 0u);
    for (std::size_t v = 0; v < V; ++v) out.onCut[v] = f[v] == 0.0f ? 1u : 0u;
    out.maps.resize(poly.maps.size());

    // Intersection vertex of a segment (polygon edge or fan diagonal), shared by both sides.
    std::unordered_map<std::uint64_t, std::pair<std::uint32_t, float>> crossings;  // key -> (vertex, t from min to max)
    auto crossing = [&](std::uint32_t a, std::uint32_t b, float& tFromA) {
        const std::uint64_t key = PairKey(a, b);
        auto it = crossings.find(key);
        if (it == crossings.end()) {
            const std::uint32_t lo = std::min(a, b), hi = std::max(a, b);
            const Vec3 pa = poly.positions[lo], pb = poly.positions[hi];
            float t0 = 0.0f, t1 = 1.0f, f0 = f[lo];
            float t = f[lo] / (f[lo] - f[hi]);  // Linear guess, refined by bisection.
            for (int i = 0; i < 24; ++i) {
                const float fm = o.field(lerp(pa, pb, t));
                if (!std::isfinite(fm) || fm == 0.0f) break;
                if ((fm < 0.0f) == (f0 < 0.0f)) {
                    t0 = t;
                } else {
                    t1 = t;
                }
                t = 0.5f * (t0 + t1);
                if (t1 - t0 < 1e-6f) break;
            }
            const std::uint32_t index = static_cast<std::uint32_t>(out.positions.size());
            out.positions.push_back(lerp(pa, pb, t));
            out.mask.push_back(out.mask[lo] + (out.mask[hi] - out.mask[lo]) * t);
            out.onCut.push_back(1u);
            it = crossings.emplace(key, std::make_pair(index, t)).first;
        }
        tFromA = a < b ? it->second.second : 1.0f - it->second.second;
        return it->second.first;
    };

    CutStats result;
    std::vector<std::uint32_t> faceVerts[2];
    std::vector<std::vector<Vec3>> faceValues[2];
    for (std::size_t face = 0; face < F; ++face) {
        const std::uint32_t n = poly.faceSizes[face];
        bool hasPos = false, hasNeg = false;
        for (std::uint32_t k = 0; k < n; ++k) {
            const float value = f[poly.faceVerts[starts[face] + k]];
            hasPos = hasPos || value > 0.0f;
            hasNeg = hasNeg || value < 0.0f;
        }
        auto cornerValues = [&](std::uint32_t corner, std::vector<std::vector<Vec3>>& values) {
            for (std::size_t m = 0; m < poly.maps.size(); ++m) values[m].push_back(poly.maps[m].values[corner]);
        };
        if (!hasNeg || !hasPos) {
            const std::uint8_t s = hasNeg ? 1u : 0u;
            if (s == 1u && !o.slice) {
                ++result.removedFaces;
                continue;
            }
            std::vector<std::uint32_t> v;
            std::vector<std::vector<Vec3>> values(poly.maps.size());
            for (std::uint32_t k = 0; k < n; ++k) {
                v.push_back(poly.faceVerts[starts[face] + k]);
                cornerValues(starts[face] + k, values);
            }
            out.addFace(v, values, static_cast<std::uint32_t>(face), s);
            continue;
        }
        // Crossing polygon: clip its fan triangles.
        ++result.removedFaces;
        for (std::uint32_t k = 1; k + 1 < n; ++k) {
            const std::uint32_t corners[3] = {starts[face], starts[face] + k, starts[face] + k + 1};
            for (int s = 0; s < 2; ++s) {
                faceVerts[s].clear();
                faceValues[s].assign(poly.maps.size(), {});
            }
            for (int i = 0; i < 3; ++i) {
                const std::uint32_t ci = corners[i], cj = corners[(i + 1) % 3];
                const std::uint32_t vi = poly.faceVerts[ci], vj = poly.faceVerts[cj];
                if (f[vi] >= 0.0f) {
                    faceVerts[0].push_back(vi);
                    cornerValues(ci, faceValues[0]);
                }
                if (f[vi] <= 0.0f) {
                    faceVerts[1].push_back(vi);
                    cornerValues(ci, faceValues[1]);
                }
                if (f[vi] * f[vj] < 0.0f) {
                    float t = 0.0f;
                    const std::uint32_t x = crossing(vi, vj, t);
                    for (int s = 0; s < 2; ++s) {
                        faceVerts[s].push_back(x);
                        for (std::size_t m = 0; m < poly.maps.size(); ++m)
                            faceValues[s][m].push_back(lerp(poly.maps[m].values[ci], poly.maps[m].values[cj], t));
                    }
                }
            }
            for (int s = 0; s < 2; ++s) {
                if (faceVerts[s].size() < 3) continue;
                if (s == 1 && !o.slice) continue;
                out.addFace(faceVerts[s], faceValues[s], static_cast<std::uint32_t>(face), static_cast<std::uint8_t>(s));
            }
        }
    }

    // Slice: the second part gets its own copies of the cut vertices.
    if (o.slice) {
        std::unordered_map<std::uint32_t, std::uint32_t> copies;
        std::size_t c = 0;
        for (std::size_t face = 0; face < out.sizes.size(); ++face) {
            for (std::uint32_t k = 0; k < out.sizes[face]; ++k, ++c) {
                if (out.side[face] != 1u) continue;
                const std::uint32_t v = out.verts[c];
                if (!out.onCut[v]) continue;
                auto it = copies.find(v);
                if (it == copies.end()) {
                    it = copies.emplace(v, static_cast<std::uint32_t>(out.positions.size())).first;
                    out.positions.push_back(out.positions[v]);
                    out.mask.push_back(out.mask[v]);
                    out.onCut.push_back(1u);
                }
                out.verts[c] = it->second;
            }
        }
    }

    // Openings: boundary edges between cut vertices, per side, followed in cap order.
    const std::size_t meshFaces = out.sizes.size();
    std::vector<std::vector<std::uint32_t>> loops;
    std::vector<std::uint8_t> loopSide;
    std::vector<std::uint16_t> loopMaterial;
    {
        std::unordered_map<std::uint64_t, std::uint32_t> directed;  // edge -> face
        std::size_t c = 0;
        std::vector<std::uint32_t> faceStart(meshFaces + 1, 0u);
        for (std::size_t face = 0; face < meshFaces; ++face) {
            faceStart[face] = static_cast<std::uint32_t>(c);
            const std::uint32_t n = out.sizes[face];
            for (std::uint32_t k = 0; k < n; ++k)
                directed[DirectedKey(out.verts[c + k], out.verts[c + (k + 1) % n])] = static_cast<std::uint32_t>(face);
            c += n;
        }
        std::unordered_map<std::uint32_t, std::uint32_t> capNext;
        std::unordered_map<std::uint32_t, std::uint32_t> capFace;
        std::set<std::uint32_t> ambiguous;
        for (const auto& entry : directed) {
            const std::uint32_t a = static_cast<std::uint32_t>(entry.first >> 32), b = static_cast<std::uint32_t>(entry.first);
            if (!out.onCut[a] || !out.onCut[b] || directed.count(DirectedKey(b, a))) continue;
            if (capNext.count(b)) ambiguous.insert(b);
            capNext[b] = a;  // The cap walks the opening backwards.
            capFace[b] = entry.second;
        }
        std::set<std::uint32_t> visited;
        for (const auto& entry : capNext) {
            const std::uint32_t start = entry.first;
            if (visited.count(start)) continue;
            std::vector<std::uint32_t> loop;
            std::uint32_t v = start;
            bool closed = false;
            while (!visited.count(v)) {
                visited.insert(v);
                loop.push_back(v);
                auto next = capNext.find(v);
                if (next == capNext.end() || ambiguous.count(v)) break;
                v = next->second;
                if (v == start) {
                    closed = true;
                    break;
                }
            }
            if (!closed || loop.size() < 3) {
                ++result.openLoops;
                continue;
            }
            const std::uint32_t neighbour = capFace[loop.front()];
            loops.push_back(std::move(loop));
            loopSide.push_back(out.side[neighbour]);
            const std::uint32_t src = out.parent[neighbour];
            loopMaterial.push_back(src != kNone && !poly.material.empty() ? poly.material[src] : 0);
        }
    }

    // Holes through the mesh: join the two facing openings with a tube wall.
    std::vector<std::uint8_t> capped(loops.size(), 0u);
    std::set<std::uint64_t> usedEdges;
    for (std::size_t face = 0, c = 0; face < meshFaces; ++face) {
        for (std::uint32_t k = 0; k < out.sizes[face]; ++k)
            usedEdges.insert(DirectedKey(out.verts[c + k], out.verts[c + (k + 1) % out.sizes[face]]));
        c += out.sizes[face];
    }
    if (!o.slice && o.joinTunnels && o.project && loops.size() >= 2) {
        struct Shape {
            bool ok = false;
            float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f, cx = 0, cy = 0;
            std::vector<std::array<float, 2>> pts;
        };
        std::vector<Shape> shapes(loops.size());
        for (std::size_t l = 0; l < loops.size(); ++l) {
            Shape& s = shapes[l];
            s.ok = true;
            for (std::uint32_t v : loops[l]) {
                float x, y;
                if (!o.project(out.positions[v], x, y)) {
                    s.ok = false;
                    break;
                }
                s.pts.push_back({x, y});
                s.x0 = std::min(s.x0, x);
                s.y0 = std::min(s.y0, y);
                s.x1 = std::max(s.x1, x);
                s.y1 = std::max(s.y1, y);
                s.cx += x;
                s.cy += y;
            }
            if (s.ok) {
                s.cx /= static_cast<float>(s.pts.size());
                s.cy /= static_cast<float>(s.pts.size());
            }
        }
        auto overlap = [&](const Shape& a, const Shape& b) {
            const float ix = std::max(0.0f, std::min(a.x1, b.x1) - std::max(a.x0, b.x0));
            const float iy = std::max(0.0f, std::min(a.y1, b.y1) - std::max(a.y0, b.y0));
            const float inter = ix * iy;
            const float uni = (a.x1 - a.x0) * (a.y1 - a.y0) + (b.x1 - b.x0) * (b.y1 - b.y0) - inter;
            return uni > 0.0f ? inter / uni : 0.0f;
        };
        for (std::size_t a = 0; a < loops.size(); ++a) {
            if (capped[a] || !shapes[a].ok) continue;
            std::size_t best = loops.size();
            float bestScore = 0.6f;
            for (std::size_t b = a + 1; b < loops.size(); ++b) {
                if (capped[b] || !shapes[b].ok) continue;
                const float score = overlap(shapes[a], shapes[b]);
                if (score > bestScore) {
                    bestScore = score;
                    best = b;
                }
            }
            if (best == loops.size()) continue;
            // Angles around the shared centre; the two openings must wind in opposite directions.
            const float cx = 0.5f * (shapes[a].cx + shapes[best].cx), cy = 0.5f * (shapes[a].cy + shapes[best].cy);
            auto unwrapped = [&](const Shape& s) {
                std::vector<float> angle(s.pts.size());
                for (std::size_t i = 0; i < s.pts.size(); ++i) {
                    angle[i] = std::atan2(s.pts[i][1] - cy, s.pts[i][0] - cx);
                    if (i > 0) {
                        while (angle[i] - angle[i - 1] > 3.14159265f) angle[i] -= kTwoPi;
                        while (angle[i] - angle[i - 1] < -3.14159265f) angle[i] += kTwoPi;
                    }
                }
                return angle;
            };
            std::vector<float> angA = unwrapped(shapes[a]), angB = unwrapped(shapes[best]);
            const float windA = angA.back() - angA.front(), windB = angB.back() - angB.front();
            if (std::fabs(std::fabs(windA) - kTwoPi) > 1.0f || std::fabs(std::fabs(windB) - kTwoPi) > 1.0f ||
                (windA > 0.0f) == (windB > 0.0f))
                continue;
            // Walk A forward and B backward, both in increasing (A's) angle.
            const std::vector<std::uint32_t>& A = loops[a];
            std::vector<std::uint32_t> B(loops[best].rbegin(), loops[best].rend());
            std::vector<float> bAngle(angB.rbegin(), angB.rend());
            const float dir = windA > 0.0f ? 1.0f : -1.0f;
            for (float& x : angA) x *= dir;
            for (float& x : bAngle) x *= dir;
            // Align B's start with A's start.
            std::size_t shift = 0;
            float bestDelta = 1e30f;
            for (std::size_t j = 0; j < B.size(); ++j) {
                float d = std::fmod(bAngle[j] - angA[0], kTwoPi);
                if (d < 0.0f) d += kTwoPi;
                d = std::min(d, kTwoPi - d);
                if (d < bestDelta) {
                    bestDelta = d;
                    shift = j;
                }
            }
            std::rotate(B.begin(), B.begin() + static_cast<std::ptrdiff_t>(shift), B.end());
            std::rotate(bAngle.begin(), bAngle.begin() + static_cast<std::ptrdiff_t>(shift), bAngle.end());
            for (std::size_t j = 1; j < bAngle.size(); ++j)
                while (bAngle[j] < bAngle[j - 1]) bAngle[j] += kTwoPi;
            const float base = bAngle[0] - angA[0];
            for (float& x : bAngle) x -= base;  // Same origin as A.
            std::vector<std::array<std::uint32_t, 3>> strip;
            std::size_t i = 0, j = 0;
            const std::size_t nA = A.size(), nB = B.size();
            while (i < nA || j < nB) {
                const float nextA = i < nA ? (i + 1 < nA ? angA[i + 1] : angA[0] + kTwoPi) : 1e30f;
                const float nextB = j < nB ? (j + 1 < nB ? bAngle[j + 1] : bAngle[0] + kTwoPi) : 1e30f;
                const std::uint32_t ai = A[i % nA], ai1 = A[(i + 1) % nA], bj = B[j % nB], bj1 = B[(j + 1) % nB];
                if (nextA <= nextB) {
                    strip.push_back({ai, ai1, bj});
                    ++i;
                } else {
                    strip.push_back({bj1, bj, ai});
                    ++j;
                }
            }
            bool valid = true;
            std::set<std::uint64_t> added;
            for (const auto& t : strip)
                for (int k = 0; k < 3 && valid; ++k) {
                    const std::uint64_t e = DirectedKey(t[k], t[(k + 1) % 3]);
                    valid = t[k] != t[(k + 1) % 3] && !usedEdges.count(e) && added.insert(e).second;
                }
            if (!valid) continue;
            for (const auto& t : strip) {
                out.addFace({t[0], t[1], t[2]}, {}, kNone, 0u, loopMaterial[a]);
                for (int k = 0; k < 3; ++k) usedEdges.insert(DirectedKey(t[k], t[(k + 1) % 3]));
            }
            result.capFaces += strip.size();
            capped[a] = capped[best] = 1u;
        }
    }
    // Remaining openings: flat caps.
    for (std::size_t l = 0; l < loops.size(); ++l) {
        if (capped[l]) continue;
        std::vector<Vec3> points;
        for (std::uint32_t v : loops[l]) points.push_back(out.positions[v]);
        std::vector<std::array<std::uint32_t, 3>> tris;
        if (!EarClip(points, tris)) {
            tris.clear();
            const std::uint32_t center = static_cast<std::uint32_t>(out.positions.size());
            Vec3 sum;
            for (const Vec3& p : points) sum += p;
            out.positions.push_back(sum / static_cast<float>(points.size()));
            out.mask.push_back(0.0f);
            out.onCut.push_back(1u);
            for (std::size_t i = 0; i < loops[l].size(); ++i) {
                const std::uint32_t a = loops[l][i], b = loops[l][(i + 1) % loops[l].size()];
                out.addFace({a, b, center}, {}, kNone, loopSide[l], loopMaterial[l]);
                ++result.capFaces;
            }
            continue;
        }
        for (const auto& t : tris) {
            out.addFace({loops[l][t[0]], loops[l][t[1]], loops[l][t[2]]}, {}, kNone, loopSide[l], loopMaterial[l]);
            ++result.capFaces;
        }
    }

    // Assemble, dropping unused vertices.
    PolyData cut;
    std::vector<std::uint32_t> remap(out.positions.size(), kNone);
    for (std::uint32_t v : out.verts) remap[v] = 0u;
    for (std::size_t v = 0; v < out.positions.size(); ++v) {
        if (remap[v] == kNone) continue;
        remap[v] = static_cast<std::uint32_t>(cut.positions.size());
        cut.positions.push_back(out.positions[v]);
        cut.mask.push_back(out.mask[v]);
    }
    cut.faceSizes = out.sizes;
    cut.faceVerts.reserve(out.verts.size());
    for (std::uint32_t v : out.verts) cut.faceVerts.push_back(remap[v]);
    cut.maps.resize(poly.maps.size());
    for (std::size_t m = 0; m < poly.maps.size(); ++m) {
        cut.maps[m].channel = poly.maps[m].channel;
        cut.maps[m].values = std::move(out.maps[m]);
    }
    const std::size_t outFaces = out.sizes.size();
    for (std::size_t face = 0; face < outFaces; ++face) {
        const std::uint32_t src = out.parent[face];
        if (!poly.material.empty()) cut.material.push_back(src != kNone ? poly.material[src] : out.capMaterial[face]);
        if (!poly.smoothing.empty()) cut.smoothing.push_back(src != kNone ? poly.smoothing[src] : 0u);
        std::int32_t group = src != kNone && !poly.groups.empty() ? poly.groups[src] : o.capGroup;
        if (o.slice && out.side[face] == 1u && o.secondGroup != 0) group = o.secondGroup;
        if (src == kNone) group = (o.slice && out.side[face] == 1u && o.secondGroup != 0) ? o.secondGroup : o.capGroup;
        cut.groups.push_back(group);
        cut.hidden.push_back(src != kNone && !poly.hidden.empty() ? poly.hidden[src] : 0u);
    }
    if (poly.mask.empty()) cut.mask.clear();
    if (!cut.valid(error)) return false;
    if (stats) *stats = result;
    poly = std::move(cut);
    return true;
}

}  // namespace sculpt
