// The floating Sculpt Mesh menus: left mode toolbar, bottom palette
// (Sculpting / Alphas), right-side rollouts (Brush Settings, Material /
// Paint, Mask, Mirror) and the Quick Menu. They float over the viewports
// while a Sculpt Mesh is open in the Modify panel and follow the viewport
// layout. Everything they show lives in SculptSettings.
#pragma once

#include <max.h>

namespace SculptUI {

// A Sculpt Mesh entered / left the Modify panel.
void OnEditBegin();
void OnEditEnd();

// Open/Close Sculpt Mesh Menus (remembered between sessions).
void Open();
void Close();
void Toggle();
bool IsOpen();     // Shown right now.
bool IsWanted();   // Shown whenever a Sculpt Mesh is edited.

// Repaints the menus (tool or mode changed outside the menus).
void Refresh();

// Quick Menu at a screen point. With `whileSpaceHeld` it closes when Space
// is released; otherwise when the cursor moves away, on Esc or on a click outside.
void ShowQuickMenu(POINT screen, bool whileSpaceHeld);
void HideQuickMenu();
bool QuickMenuVisible();

// Shows the Alphas page of the palette.
void ShowAlphasPage();

// Destroys all windows (scene reset / shutdown).
void Shutdown();

}  // namespace SculptUI
