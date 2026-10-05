// Temporary Modify-panel rollout for phase 1 (replaced by the floating UI in
// phase 3). All values live in SculptSettings, so MAXScript and the panel
// always agree.
#pragma once

#include "SculptMeshPlugin.h"

class SculptMeshObject;

namespace SculptPanel {

void Open(IObjParam* ip, SculptMeshObject* object);
void Close(IObjParam* ip);
// Re-reads settings, sculpt-mode state and mesh statistics into the controls.
void Refresh();

}  // namespace SculptPanel
