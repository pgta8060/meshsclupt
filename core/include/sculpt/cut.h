// SculptCore — cutting a mesh with a screen-space shape (Cutter / Slice).
#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "sculpt/poly.h"

namespace sculpt {

struct CutOptions {
    // Signed field over object space: the surface f = 0 is the cut, f < 0 is
    // the removed side (Cutter) or the second part (Slice).
    std::function<float(const Vec3&)> field;
    // Object space -> 2D shape space (the screen); used to recognise the two
    // openings of a hole through the mesh, which are joined by a tube wall.
    std::function<bool(const Vec3&, float&, float&)> project;
    bool slice = false;          // Keep both sides as separate parts.
    bool joinTunnels = true;     // Cutter rectangle/circle: connect facing openings.
    std::int32_t capGroup = 0;   // SculptGroup of the new cap faces.
    std::int32_t secondGroup = 0;  // Slice: SculptGroup of the second part (0: keep).
};

struct CutStats {
    std::size_t removedFaces = 0;
    std::size_t capFaces = 0;
    std::size_t openLoops = 0;  // Openings that could not be closed.
};

// Returns false (poly unchanged) if the shape does not cross the mesh.
bool cutMesh(PolyData& poly, const CutOptions& options, CutStats* stats = nullptr, std::string* error = nullptr);

}  // namespace sculpt
