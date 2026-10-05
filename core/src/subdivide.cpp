#include "sculpt/subdivide.h"

#include <algorithm>

namespace sculpt {

namespace {

std::vector<std::uint8_t> SharpEdges(const PolyData& in, const EdgeTable& edges, const SubdivOptions& options) {
    std::vector<std::uint8_t> sharp(edges.size(), 0u);
    for (std::size_t e = 0; e < edges.size(); ++e) {
        if (edges.faceUses[e] != 2) {
            sharp[e] = 1u;  // Open border or non-manifold.
            continue;
        }
        const std::uint32_t f0 = edges.faces[e][0], f1 = edges.faces[e][1];
        if (options.creaseMaterials && !in.material.empty() && in.material[f0] != in.material[f1]) sharp[e] = 1u;
        if (options.creaseSmoothing && !in.smoothing.empty() && (in.smoothing[f0] & in.smoothing[f1]) == 0u) sharp[e] = 1u;
    }
    return sharp;
}

}  // namespace

void subdividePositions(const PolyData& in, const EdgeTable& edges, const SubdivOptions& options,
                        const std::vector<Vec3>& positions, std::vector<Vec3>& out) {
    const std::size_t V = in.positions.size(), E = edges.size(), F = in.faceSizes.size();
    out.assign(V + E + F, Vec3());
    const std::vector<std::uint8_t> sharp = SharpEdges(in, edges, options);

    // Face points.
    std::size_t c = 0;
    for (std::size_t f = 0; f < F; ++f) {
        const std::uint32_t n = in.faceSizes[f];
        Vec3 sum;
        for (std::uint32_t k = 0; k < n; ++k) sum += positions[in.faceVerts[c + k]];
        out[V + E + f] = sum / static_cast<float>(n);
        c += n;
    }
    // Edge points.
    for (std::size_t e = 0; e < E; ++e) {
        const Vec3& a = positions[edges.verts[e][0]];
        const Vec3& b = positions[edges.verts[e][1]];
        if (sharp[e])
            out[V + e] = (a + b) * 0.5f;
        else
            out[V + e] = (a + b + out[V + E + edges.faces[e][0]] + out[V + E + edges.faces[e][1]]) * 0.25f;
    }
    // Vertex points.
    std::vector<std::uint32_t> valence(V, 0u), sharpCount(V, 0u), faceCount(V, 0u);
    std::vector<Vec3> edgeMidSum(V), sharpSum(V), faceSum(V);
    for (std::size_t e = 0; e < E; ++e) {
        const std::uint32_t a = edges.verts[e][0], b = edges.verts[e][1];
        if (a == b) continue;
        const Vec3 mid = (positions[a] + positions[b]) * 0.5f;
        ++valence[a];
        ++valence[b];
        edgeMidSum[a] += mid;
        edgeMidSum[b] += mid;
        if (sharp[e]) {
            ++sharpCount[a];
            ++sharpCount[b];
            sharpSum[a] += positions[b];
            sharpSum[b] += positions[a];
        }
    }
    c = 0;
    for (std::size_t f = 0; f < F; ++f) {
        const std::uint32_t n = in.faceSizes[f];
        for (std::uint32_t k = 0; k < n; ++k) {
            const std::uint32_t v = in.faceVerts[c + k];
            faceSum[v] += out[V + E + f];
            ++faceCount[v];
        }
        c += n;
    }
    for (std::size_t v = 0; v < V; ++v) {
        const Vec3& p = positions[v];
        const std::uint32_t n = valence[v];
        if (faceCount[v] == 0 || sharpCount[v] > 2 || (sharpCount[v] == 2 && n == 2)) {
            out[v] = p;  // Isolated vertex, or a corner (e.g. of a flat grid) that must stay put.
        } else if (sharpCount[v] == 2) {
            out[v] = (p * 6.0f + sharpSum[v]) * 0.125f;  // Crease / border rule.
        } else if (n < 3) {
            out[v] = p;
        } else {
            const Vec3 q = faceSum[v] / static_cast<float>(faceCount[v]);
            const Vec3 r = edgeMidSum[v] / static_cast<float>(n);
            out[v] = (q + r * 2.0f + p * static_cast<float>(n - 3)) / static_cast<float>(n);
        }
    }
}

bool subdivide(const PolyData& in, const SubdivOptions& options, PolyData& out, std::string* error) {
    if (!in.valid(error)) return false;
    EdgeTable edges;
    edges.build(in);
    const std::size_t V = in.positions.size(), E = edges.size(), F = in.faceSizes.size(), C = in.faceVerts.size();
    if (V + E + F > 0xfffffff0u || C * 4 > 0xfffffff0u) {
        if (error) *error = "mesh too large to subdivide";
        return false;
    }
    PolyData result;
    subdividePositions(in, edges, options, in.positions, result.positions);
    result.faceSizes.assign(C, 4u);
    result.faceVerts.resize(C * 4);
    const auto vE = static_cast<std::uint32_t>(V), vF = static_cast<std::uint32_t>(V + E);
    std::size_t c = 0;
    for (std::size_t f = 0; f < F; ++f) {
        const std::uint32_t n = in.faceSizes[f];
        for (std::uint32_t k = 0; k < n; ++k) {
            const std::size_t child = c + k;
            const std::uint32_t prev = static_cast<std::uint32_t>(c + (k + n - 1) % n);
            result.faceVerts[4 * child + 0] = in.faceVerts[c + k];
            result.faceVerts[4 * child + 1] = vE + edges.cornerEdge[c + k];
            result.faceVerts[4 * child + 2] = vF + static_cast<std::uint32_t>(f);
            result.faceVerts[4 * child + 3] = vE + edges.cornerEdge[prev];
        }
        c += n;
    }
    // Face attributes: children inherit.
    auto inherit = [&](const auto& src, auto& dst) {
        if (src.empty()) return;
        dst.resize(C);
        std::size_t corner = 0;
        for (std::size_t f = 0; f < F; ++f)
            for (std::uint32_t k = 0; k < in.faceSizes[f]; ++k) dst[corner++] = src[f];
    };
    inherit(in.material, result.material);
    inherit(in.smoothing, result.smoothing);
    inherit(in.groups, result.groups);
    inherit(in.hidden, result.hidden);
    // Mask: averaged like linear subdivision.
    if (!in.mask.empty()) {
        result.mask.resize(V + E + F);
        for (std::size_t v = 0; v < V; ++v) result.mask[v] = in.mask[v];
        for (std::size_t e = 0; e < E; ++e)
            result.mask[V + e] = 0.5f * (in.mask[edges.verts[e][0]] + in.mask[edges.verts[e][1]]);
        c = 0;
        for (std::size_t f = 0; f < F; ++f) {
            const std::uint32_t n = in.faceSizes[f];
            float sum = 0.0f;
            for (std::uint32_t k = 0; k < n; ++k) sum += in.mask[in.faceVerts[c + k]];
            result.mask[V + E + f] = sum / static_cast<float>(n);
            c += n;
        }
    }
    // Maps: linear subdivision per face.
    result.maps.resize(in.maps.size());
    for (std::size_t m = 0; m < in.maps.size(); ++m) {
        const std::vector<Vec3>& src = in.maps[m].values;
        std::vector<Vec3>& dst = result.maps[m].values;
        result.maps[m].channel = in.maps[m].channel;
        dst.resize(C * 4);
        c = 0;
        for (std::size_t f = 0; f < F; ++f) {
            const std::uint32_t n = in.faceSizes[f];
            Vec3 center;
            for (std::uint32_t k = 0; k < n; ++k) center += src[c + k];
            center = center / static_cast<float>(n);
            for (std::uint32_t k = 0; k < n; ++k) {
                const Vec3& u = src[c + k];
                const Vec3& next = src[c + (k + 1) % n];
                const Vec3& prev = src[c + (k + n - 1) % n];
                const std::size_t child = c + k;
                dst[4 * child + 0] = u;
                dst[4 * child + 1] = (u + next) * 0.5f;
                dst[4 * child + 2] = center;
                dst[4 * child + 3] = (prev + u) * 0.5f;
            }
            c += n;
        }
    }
    out = std::move(result);
    return true;
}

bool unsubdivide(const PolyData& in, PolyData& out, std::string* error, std::vector<std::uint32_t>* vertexMap,
                 std::vector<std::uint32_t>* faceMap, std::vector<std::uint8_t>* faceRotation) {
    auto fail = [error](const char* message) {
        if (error) *error = message;
        return false;
    };
    if (!in.valid(error)) return false;
    const std::size_t V = in.positions.size(), F = in.faceSizes.size();
    if (F == 0) return fail("empty mesh");
    for (std::uint32_t n : in.faceSizes)
        if (n != 4) return fail("not every polygon is a quad");

    // Vertex -> faces.
    std::vector<std::uint32_t> vfStart(V + 1, 0u), vf(4 * F);
    for (std::uint32_t v : in.faceVerts) ++vfStart[v + 1];
    for (std::size_t v = 0; v < V; ++v) vfStart[v + 1] += vfStart[v];
    {
        std::vector<std::uint32_t> fill(vfStart.begin(), vfStart.end() - 1);
        for (std::size_t f = 0; f < F; ++f)
            for (int k = 0; k < 4; ++k) vf[fill[in.faceVerts[4 * f + k]]++] = static_cast<std::uint32_t>(f);
    }
    EdgeTable edges;
    edges.build(in);

    // Label vertices: 1 original (O), 2 edge point (E), 3 face point (F).
    // Every quad of a subdivided mesh reads O, E, F, E around its corners.
    enum : std::uint8_t { kUnknown = 0, kO = 1, kE = 2, kF = 3 };
    std::vector<std::uint8_t> label(V, kUnknown);
    std::vector<std::uint8_t> faceDone(F, 0u);
    std::vector<std::uint32_t> touched, queue, componentFaces;

    auto labelFace = [&](std::uint32_t f, int originCorner) -> bool {
        const std::uint8_t pattern[4] = {kO, kE, kF, kE};
        for (int k = 0; k < 4; ++k) {
            const std::uint32_t v = in.faceVerts[4 * f + k];
            const std::uint8_t want = pattern[(k - originCorner + 4) % 4];
            if (label[v] == kUnknown) {
                label[v] = want;
                touched.push_back(v);
            } else if (label[v] != want) {
                return false;
            }
        }
        return true;
    };
    auto originOf = [&](std::uint32_t f) -> int {  // Corner holding the O vertex, from known labels.
        for (int k = 0; k < 4; ++k) {
            const std::uint8_t l = label[in.faceVerts[4 * f + k]];
            if (l == kO) return k;
            if (l == kF) return (k + 2) % 4;
        }
        return -1;
    };
    auto propagate = [&](std::uint32_t seed, int origin) -> bool {
        touched.clear();
        componentFaces.clear();
        queue.assign(1, seed);
        faceDone[seed] = 1u;
        componentFaces.push_back(seed);
        if (!labelFace(seed, origin)) return false;
        for (std::size_t qi = 0; qi < queue.size(); ++qi) {
            const std::uint32_t f = queue[qi];
            for (int k = 0; k < 4; ++k) {
                const std::uint32_t e = edges.cornerEdge[4 * f + k];
                for (int s = 0; s < 2; ++s) {
                    const std::uint32_t g = edges.faces[e][s];
                    if (g == EdgeTable::kNone || faceDone[g]) continue;
                    const int o = originOf(g);
                    if (o < 0 || !labelFace(g, o)) return false;
                    faceDone[g] = 1u;
                    componentFaces.push_back(g);
                    queue.push_back(g);
                }
            }
        }
        return true;
    };
    auto undoComponent = [&]() {
        for (std::uint32_t v : touched) label[v] = kUnknown;
        for (std::uint32_t f : componentFaces) faceDone[f] = 0u;
    };
    // A subdivided mesh can usually be read two ways (original and face
    // points swapped, e.g. cube <-> octahedron). Try all four corner
    // assignments of the seed quad and keep the one giving the most quads.
    auto score = [&]() {
        long s = 0;
        for (std::uint32_t v : touched) {
            if (label[v] != kF) continue;
            const std::uint32_t valence = vfStart[v + 1] - vfStart[v];
            s += valence == 4 ? 2 : (valence == 3 ? 1 : -1);
        }
        return s;
    };
    for (std::uint32_t f = 0; f < F; ++f) {
        if (faceDone[f]) continue;
        int best = -1;
        long bestScore = 0;
        for (int origin = 0; origin < 4; ++origin) {
            const bool ok = propagate(f, origin);
            const long s = ok ? score() : 0;
            undoComponent();
            if (ok && (best < 0 || s > bestScore)) {
                best = origin;
                bestScore = s;
            }
        }
        if (best < 0 || !propagate(f, best)) return fail("the mesh is not a subdivided quad mesh");
    }

    // Coarse vertices: the O vertices.
    std::vector<std::uint32_t> coarseIndex(V, EdgeTable::kNone);
    PolyData coarse;
    std::vector<Vec3> target;  // Fine positions of the O vertices.
    for (std::size_t v = 0; v < V; ++v) {
        if (label[v] != kO) continue;
        coarseIndex[v] = static_cast<std::uint32_t>(coarse.positions.size());
        coarse.positions.push_back(in.positions[v]);
        target.push_back(in.positions[v]);
        if (!in.mask.empty()) coarse.mask.push_back(in.mask[v]);
    }
    if (coarse.positions.size() < 3) return fail("too few original vertices");

    // Coarse faces: walk the quads around every face point.
    auto cornerOf = [&](std::uint32_t f, std::uint32_t v) {
        for (int k = 0; k < 4; ++k)
            if (in.faceVerts[4 * f + k] == v) return k;
        return -1;
    };
    std::vector<std::uint8_t> quadUsed(F, 0u);
    std::vector<std::uint32_t> ring;
    std::vector<std::uint32_t> facePointOf;              // Coarse face -> fine face point.
    std::vector<std::uint32_t> childQuad;                // Coarse corner -> fine quad.
    std::vector<std::uint8_t> childRotation;             // Coarse corner -> rotation of that quad.
    coarse.maps.resize(in.maps.size());
    for (std::size_t m = 0; m < in.maps.size(); ++m) coarse.maps[m].channel = in.maps[m].channel;
    std::vector<std::uint32_t> edgeOwnerA(V, EdgeTable::kNone), edgeOwnerB(V, EdgeTable::kNone);
    for (std::uint32_t fp = 0; fp < V; ++fp) {
        if (label[fp] != kF) continue;
        const std::uint32_t count = vfStart[fp + 1] - vfStart[fp];
        if (count < 3) return fail("face point with fewer than 3 quads");
        ring.clear();
        std::uint32_t q = vf[vfStart[fp]];
        for (std::uint32_t step = 0; step < count; ++step) {
            if (quadUsed[q]) return fail("quad shared by two face points");
            quadUsed[q] = 1u;
            ring.push_back(q);
            const int p = cornerOf(q, fp);
            const std::uint32_t ea = in.faceVerts[4 * q + (p + 3) % 4];
            std::uint32_t next = EdgeTable::kNone;
            for (std::uint32_t i = vfStart[fp]; i < vfStart[fp + 1]; ++i) {
                const std::uint32_t g = vf[i];
                if (g == q) continue;
                const int pg = cornerOf(g, fp);
                if (in.faceVerts[4 * g + (pg + 1) % 4] == ea) {
                    next = g;
                    break;
                }
            }
            if (next == EdgeTable::kNone) return fail("open ring around a face point");
            q = next;
        }
        if (q != ring.front()) return fail("inconsistent ring around a face point");

        const std::uint32_t first = static_cast<std::uint32_t>(coarse.faceVerts.size());
        coarse.faceSizes.push_back(count);
        facePointOf.push_back(fp);
        for (std::uint32_t quad : ring) {
            const int p = cornerOf(quad, fp);
            const std::uint32_t o = in.faceVerts[4 * quad + (p + 2) % 4];
            coarse.faceVerts.push_back(coarseIndex[o]);
            childQuad.push_back(quad);
            childRotation.push_back(static_cast<std::uint8_t>((p + 2) % 4));
        }
        // Each edge point must sit between the same two original vertices everywhere.
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::uint32_t quad = ring[i];
            const int p = cornerOf(quad, fp);
            const std::uint32_t ea = in.faceVerts[4 * quad + (p + 3) % 4];
            std::uint32_t a = coarse.faceVerts[first + i], b = coarse.faceVerts[first + (i + 1) % count];
            if (a > b) std::swap(a, b);
            if (edgeOwnerA[ea] == EdgeTable::kNone) {
                edgeOwnerA[ea] = a;
                edgeOwnerB[ea] = b;
            } else if (edgeOwnerA[ea] != a || edgeOwnerB[ea] != b) {
                return fail("edge point shared by different edges");
            }
        }
        const std::uint32_t q0 = ring.front();
        if (!in.material.empty()) coarse.material.push_back(in.material[q0]);
        if (!in.smoothing.empty()) coarse.smoothing.push_back(in.smoothing[q0]);
        if (!in.groups.empty()) coarse.groups.push_back(in.groups[q0]);
        if (!in.hidden.empty()) coarse.hidden.push_back(in.hidden[q0]);
        for (std::size_t m = 0; m < in.maps.size(); ++m) {
            for (std::uint32_t quad : ring) {
                const int p = cornerOf(quad, fp);
                coarse.maps[m].values.push_back(in.maps[m].values[4 * quad + (p + 2) % 4]);
            }
        }
    }
    for (std::size_t f = 0; f < F; ++f)
        if (!quadUsed[f]) return fail("quad without a face point");

    // Fit positions so the vertex points of the subdivision land on the
    // original fine positions (a few Jacobi steps).
    EdgeTable coarseEdges;
    coarseEdges.build(coarse);
    std::vector<Vec3> subdivided;
    for (int iteration = 0; iteration < 10; ++iteration) {
        subdividePositions(coarse, coarseEdges, SubdivOptions(), coarse.positions, subdivided);
        float maxError = 0.0f;
        for (std::size_t v = 0; v < coarse.positions.size(); ++v) {
            const Vec3 d = target[v] - subdivided[v];
            coarse.positions[v] += d;
            maxError = std::max(maxError, lengthSq(d));
        }
        if (maxError < 1e-14f) break;
    }
    for (const Vec3& p : coarse.positions)
        if (!isFinite(p)) return fail("position fitting diverged");

    if (vertexMap || faceMap || faceRotation) {
        // Vertex points -> O vertices, edge points -> E vertices, face points -> F vertices.
        const std::size_t Vc = coarse.positions.size(), Ec = coarseEdges.size(), Fc = coarse.faceSizes.size();
        std::vector<std::uint32_t> vmap(Vc + Ec + Fc, EdgeTable::kNone);
        for (std::size_t v = 0; v < V; ++v)
            if (coarseIndex[v] != EdgeTable::kNone) vmap[coarseIndex[v]] = static_cast<std::uint32_t>(v);
        for (std::size_t c = 0; c < childQuad.size(); ++c) {
            const std::uint32_t quad = childQuad[c];
            const std::uint32_t ea = in.faceVerts[4 * quad + (childRotation[c] + 1) % 4];
            vmap[Vc + coarseEdges.cornerEdge[c]] = ea;
        }
        for (std::size_t f = 0; f < Fc; ++f) vmap[Vc + Ec + f] = facePointOf[f];
        for (std::uint32_t v : vmap)
            if (v == EdgeTable::kNone) return fail("incomplete vertex correspondence");
        if (vertexMap) *vertexMap = std::move(vmap);
        if (faceMap) faceMap->swap(childQuad);
        if (faceRotation) faceRotation->swap(childRotation);  // Swap: avoids a GCC false null-dereference warning.
    }
    out = std::move(coarse);
    return true;
}

void subdivisionCorrespondence(const PolyData& newer, const PolyData& older, std::vector<std::uint32_t>& vertexMap,
                               std::vector<std::uint32_t>& faceMap, std::vector<std::uint8_t>& faceRotation) {
    EdgeTable newEdges, oldEdges;
    newEdges.build(newer);
    oldEdges.build(older);
    const std::size_t V = newer.positions.size(), E = newEdges.size(), F = newer.faceSizes.size();
    const std::vector<std::uint32_t> newStarts = newer.faceStarts(), oldStarts = older.faceStarts();
    std::vector<std::uint32_t> vmap(V + E + F, 0u);
    std::vector<std::uint32_t> fmap(newer.faceVerts.size(), 0u);
    for (std::size_t v = 0; v < V; ++v) vmap[v] = vertexMap[v];
    for (std::size_t f = 0; f < F; ++f) {
        const std::uint32_t n = newer.faceSizes[f];
        const std::uint32_t of = faceMap[f];
        const std::uint32_t r = faceRotation[f];
        vmap[V + E + f] = static_cast<std::uint32_t>(older.positions.size() + oldEdges.size() + of);
        for (std::uint32_t k = 0; k < n; ++k) {
            const std::uint32_t oldCorner = oldStarts[of] + (k + r) % n;
            vmap[V + newEdges.cornerEdge[newStarts[f] + k]] =
                static_cast<std::uint32_t>(older.positions.size() + oldEdges.cornerEdge[oldCorner]);
            fmap[newStarts[f] + k] = oldCorner;  // Child k of f <-> child (k + r) of the old face.
        }
    }
    vertexMap = std::move(vmap);
    faceMap = std::move(fmap);
    faceRotation.assign(faceMap.size(), 0u);
}

}  // namespace sculpt
