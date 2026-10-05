// Converts between an MNMesh (plus the sculpt attributes stored on the
// object) and a core PolyData, for operations that change topology.
#pragma once

#include <max.h>
#include <mnmesh.h>

#include "SculptSessionBridge.h"
#include "sculpt/poly.h"

// MNMesh -> PolyData: alive vertices/polygons in order, material IDs,
// smoothing groups, every consistent map channel (per corner), mask,
// SculptGroups and visibility.
bool MeshToPoly(const MNMesh& mesh, const SculptAttributes& attributes, sculpt::PolyData& out, MSTR* error = nullptr);

// PolyData -> MNMesh (everything replaced; maps welded per vertex) and the
// matching attributes.
void PolyToMesh(const sculpt::PolyData& poly, MNMesh& mesh, SculptAttributes& attributes);

// Positions of the alive MNMesh vertices, in order (matches MeshToPoly).
std::vector<sculpt::Vec3> AlivePositions(const MNMesh& mesh);
