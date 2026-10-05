// The "Displace" rollout in the Modify panel: an image applied as
// displacement into its own Displace sculpt layer on the highest level.
#pragma once

#include "SculptMeshPlugin.h"

class SculptMeshObject;

namespace DisplaceRollout {

void Open(IObjParam* ip, SculptMeshObject* object);
void Close(IObjParam* ip);
void Refresh();
// Chooses a map file (dialog); returns false if cancelled.
bool BrowseMap();
// Applies the current settings (errors are shown to the user).
bool Apply();

}  // namespace DisplaceRollout
