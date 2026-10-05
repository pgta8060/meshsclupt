// Every user-level command of the plugin in one place, so the floating UI,
// keyboard shortcuts, the Modify panel and MAXScript all behave the same.
#pragma once

#include <string>

#include "SculptMeshPlugin.h"
#include "SculptSettings.h"

class SculptMeshObject;

namespace SculptCommands {

// The Sculpt Mesh shown in the Modify panel (nullptr if none).
SculptMeshObject* Target();

// --- Working mode --------------------------------------------------------------------
// Starts sculpting: brings the selected Sculpt Mesh into the Modify panel if
// needed and returns to the object level. False if nothing can be sculpted.
bool StartSculpting();
// Back to object selection (the toolbar's Select tool).
void StopSculpting();
bool IsSculpting();

// Tool choices. Each one also starts sculpting.
void ChooseBrush(sculpt::BrushType brush);
void SelectPaletteSlot(int slot);  // 0..4: keys 1-5 follow the palette order.
void SelectMaskTool(MaskTool tool);
void SelectSculptMode();           // Sculpt/Paint flyout: Sculpt.
void SetStrokeMode(StrokeMode mode);

// --- Whole-mesh operations (one undo step each) -----------------------------------
enum class Op : int {
    MaskClear = 0,
    MaskInvert,
    MaskBlur,
    MaskSharpen,
    MaskGrow,
    MaskShrink,
    MaskByCavity,
    MaskByAO,
    MaskToggleVisible,
    GroupFromMask,
    AutoGroups,
    ShowAll,
    InvertVisibility,
    Count
};
const char* OpScriptName(Op op);
const MCHAR* OpDisplayName(Op op);
bool Run(Op op);
// Runs an operation or tool command by its script name, e.g. "maskInvert".
bool RunByName(const std::string& name);

// --- Multires and Surface Snapshot (errors are shown to the user) -------------------
bool SetMultiresLevel(int level);
bool SubdivideLevel();
bool DeleteLowerLevels();
bool DeleteHigherLevels();
bool ReverseSubdivision();
void CaptureSurface();
void ClearSurface();

// --- W / E / R -------------------------------------------------------------------------
// With a usable mask the unmasked region is transformed (sub-object level
// "Transform"); otherwise the normal Move/Rotate/Scale command is used.
enum class TransformKind { Move, Rotate, Scale };
void Transform(TransformKind kind);
// True when the mask protects part, but not all, of the mesh.
bool HasUsableMask();

// --- Settings file (plugcfg/SculptMesh.ini) -------------------------------------------
void LoadSettings();  // Once per session; later calls do nothing.
void SaveSettings();

// Status line text, e.g. after an operation that did nothing.
void Prompt(const MCHAR* text);

}  // namespace SculptCommands
