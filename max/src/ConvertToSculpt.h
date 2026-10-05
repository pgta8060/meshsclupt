// Converting scene nodes to Sculpt Mesh objects.
#pragma once

#include "SculptMeshPlugin.h"

// Replaces the node's object (collapsing its modifier stack; world-space
// modifiers are kept) with a Sculpt Mesh built from the evaluated result.
// Undoable. Returns false (and fills *error) if the node has no geometry
// that can become polygons. A node that already is a Sculpt Mesh succeeds.
bool ConvertNodeToSculpt(INode* node, TimeValue t, MSTR* error = nullptr);
