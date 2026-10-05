// Keyboard shortcuts (3ds Max action table "Sculpt Mesh"): 1-5 palette
// slots, Space Quick Menu, W/E/R mask-aware transforms, Ctrl+W SculptGroup
// from mask. Active while a Sculpt Mesh is open in the Modify panel; users
// can reassign the keys in the Hotkey Editor.
#pragma once

class ActionTable;

namespace SculptActions {

int TableCount();
ActionTable* Table(int i);

void Activate();
void Deactivate();

}  // namespace SculptActions
