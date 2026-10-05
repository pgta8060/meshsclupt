// SculptCore — local remeshing for the Density brush.
#pragma once

#include <string>
#include <vector>

#include "sculpt/poly.h"

namespace sculpt {

struct DensityOptions {
    float targetLength = 0.1f;   // Edge length where the weight is 1 (object units).
    bool reduce = false;         // Alt: collapse short edges instead of splitting long ones.
    std::size_t maxNewVertices = 4u * 1000u * 1000u;
};

// Polygons touched by a vertex with weight > 0 become triangles, which are
// then split (or collapsed) toward targetLength / weight. Everything else
// is left exactly as it was; UVs, the mask and face attributes follow.
// Returns false (poly unchanged) if nothing was painted or the result
// would be invalid.
bool remeshRegion(PolyData& poly, const std::vector<float>& weight, const DensityOptions& options,
                  std::string* error = nullptr);

}  // namespace sculpt
