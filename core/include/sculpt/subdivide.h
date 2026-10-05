// SculptCore — Catmull-Clark subdivision (and its reverse) for Multires.
#pragma once

#include <string>
#include <vector>

#include "sculpt/poly.h"

namespace sculpt {

struct SubdivOptions {
    bool creaseMaterials = false;  // Edges between different material IDs stay sharp.
    bool creaseSmoothing = false;  // Edges between faces without a common smoothing group stay sharp.
    bool operator==(const SubdivOptions& o) const {
        return creaseMaterials == o.creaseMaterials && creaseSmoothing == o.creaseSmoothing;
    }
};

// One Catmull-Clark step. Open borders and non-manifold edges are sharp.
// Output layout (relied on by Multires):
//   vertices [0, V)          vertex points (same index as the parent vertex)
//            [V, V+E)        edge points (EdgeTable order of `in`)
//            [V+E, V+E+F)    face points
//   faces    the children of face f are the quads [start(f), start(f)+size(f))
//            where start = in.faceStarts(); child k = (vertex k, edge k, face, edge k-1).
// Face attributes are inherited, the mask is averaged, maps are subdivided linearly.
bool subdivide(const PolyData& in, const SubdivOptions& options, PolyData& out, std::string* error = nullptr);

// Positions only, for `positions` on `in`'s topology (`edges` built from `in`).
void subdividePositions(const PolyData& in, const EdgeTable& edges, const SubdivOptions& options,
                        const std::vector<Vec3>& positions, std::vector<Vec3>& out);

// If `in` is (topologically) a Catmull-Clark subdivision of a coarser mesh,
// rebuilds that mesh: positions are fitted so its subdivision approximates
// `in`, attributes and maps are taken from the matching corners.
//
// The optional maps relate subdivide(out) to `in`: vertex i of the
// subdivision is vertex (*vertexMap)[i] of `in`; face j is face (*faceMap)[j]
// of `in`, whose corner (k + (*faceRotation)[j]) % 4 is corner k of face j.
bool unsubdivide(const PolyData& in, PolyData& out, std::string* error = nullptr,
                 std::vector<std::uint32_t>* vertexMap = nullptr, std::vector<std::uint32_t>* faceMap = nullptr,
                 std::vector<std::uint8_t>* faceRotation = nullptr);

// Given that face f of `newer` is face faceMap[f] of `older` (corner k = corner
// (k + rotation[f]) % n) and vertex i of `newer` is vertexMap[i] of `older`,
// derives the same maps for subdivide(newer) -> subdivide(older).
// The subdivided faces correspond without rotation.
void subdivisionCorrespondence(const PolyData& newer, const PolyData& older, std::vector<std::uint32_t>& vertexMap,
                               std::vector<std::uint32_t>& faceMap, std::vector<std::uint8_t>& faceRotation);

}  // namespace sculpt
