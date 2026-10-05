// The "Multires" rollout in the Modify panel. For now it holds the
// Open/Close Sculpt Mesh Menus button, a Sculpt toggle and mesh statistics;
// the subdivision levels arrive with phase 7.
#pragma once

#include "SculptMeshPlugin.h"

class SculptMeshObject;

namespace MultiresRollout {

void Open(IObjParam* ip, SculptMeshObject* object);
void Close(IObjParam* ip);
// Re-reads the menu/sculpt state and mesh statistics into the controls.
void Refresh();

}  // namespace MultiresRollout
