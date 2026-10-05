#include "SculptCommands.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <vector>

#include <MaxDirectories.h>
#include <cmdmode.h>

#include "SculptMeshObject.h"
#include "SculptMode.h"
#include "sculpt/session.h"

namespace SculptCommands {

namespace {

struct OpEntry {
    const char* scriptName;
    const MCHAR* displayName;
};

const OpEntry kOps[] = {
    {"maskClear", _T("Clear Mask")},
    {"maskInvert", _T("Invert Mask")},
    {"maskBlur", _T("Blur Mask")},
    {"maskSharpen", _T("Sharpen Mask")},
    {"maskGrow", _T("Grow Mask")},
    {"maskShrink", _T("Shrink Mask")},
    {"maskByCavity", _T("Mask by Cavity")},
    {"maskByAO", _T("Mask by AO")},
    {"maskToggleVisible", _T("Toggle Visible Mask")},
    {"groupFromMask", _T("SculptGroup from Mask")},
    {"autoGroups", _T("Auto Groups")},
    {"showAll", _T("Show All SculptGroups")},
    {"invertVisibility", _T("Invert SculptGroup Visibility")},
};
static_assert(sizeof(kOps) / sizeof(kOps[0]) == static_cast<std::size_t>(Op::Count), "Op table out of sync");

bool EqualsNoCase(const std::string& a, const char* b) {
    std::size_t i = 0;
    for (; i < a.size() && b[i]; ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return i == a.size() && b[i] == '\0';
}

bool IsSculptMeshNode(INode* node) {
    Object* ref = node ? node->GetObjectRef() : nullptr;
    Object* base = ref ? ref->FindBaseObject() : nullptr;
    return base && base->ClassID() == SCULPTMESH_CLASS_ID;
}

MSTR SettingsPath() {
    Interface* core = GetCOREInterface();
    if (!core) return MSTR();
    MSTR dir = core->GetDir(APP_PLUGCFG_LN_DIR);
    if (dir.Length() == 0) return MSTR();
    return dir + _T("\\SculptMesh.ini");
}

sculpt::StrokeDelta RunOnSession(Op op, SculptMeshObject& object, SculptSessionBridge& bridge) {
    const SculptSettings& settings = SculptSettings::Get();
    sculpt::SculptSession& s = bridge.Session();
    switch (op) {
        case Op::MaskClear: return s.maskClear();
        case Op::MaskInvert: return s.maskInvert();
        case Op::MaskBlur: return s.maskBlur(1);
        case Op::MaskSharpen: return s.maskSharpen();
        case Op::MaskGrow: return s.maskGrow();
        case Op::MaskShrink: return s.maskShrink();
        case Op::MaskByCavity: return s.maskByCavity(settings.Value(Prop::CavityCoverage));
        case Op::MaskByAO: return s.maskByAO(settings.Value(Prop::AOCoverage));
        case Op::MaskToggleVisible: return s.maskToggleVisible();
        case Op::GroupFromMask: return s.groupFromMask();
        case Op::AutoGroups: {
            const auto mode = static_cast<sculpt::AutoGroupMode>(settings.Int(Prop::AutoGroupMode));
            const bool needsKeys = mode == sculpt::AutoGroupMode::SmoothGroups ||
                                   mode == sculpt::AutoGroupMode::UVIslands ||
                                   mode == sculpt::AutoGroupMode::MaterialIDs;
            if (!needsKeys) return s.autoGroups(mode);
            const std::vector<std::uint64_t> keys = bridge.FaceKeys(object.GetMesh(), mode);
            return s.autoGroups(mode, &keys);
        }
        case Op::ShowAll: return s.showAll();
        case Op::InvertVisibility: return s.invertVisibility();
        default: return sculpt::StrokeDelta();
    }
}

}  // namespace

SculptMeshObject* Target() { return SculptMeshObject::EditedObject(); }

void Prompt(const MCHAR* text) {
    if (Interface* core = GetCOREInterface()) core->ReplacePrompt(text ? text : _T(""));
}

// --- Working mode ----------------------------------------------------------------------

bool StartSculpting() {
    Interface* core = GetCOREInterface();
    if (!core) return false;
    SculptMeshObject* object = Target();
    if (!object) {
        // Bring a selected Sculpt Mesh into the Modify panel first.
        if (core->GetSelNodeCount() != 1 || !IsSculptMeshNode(core->GetSelNode(0))) {
            Prompt(GetString(IDS_ERR_SELECT_ONE));
            return false;
        }
        if (core->GetCommandPanelTaskMode() != TASK_MODE_MODIFY) core->SetCommandPanelTaskMode(TASK_MODE_MODIFY);
        object = Target();
        if (!object) {
            // A modifier above the Sculpt Mesh is selected in the stack.
            Prompt(GetString(IDS_ERR_SELECT_BASE));
            return false;
        }
    }
    IObjParam* ip = SculptMeshObject::EditInterface();
    if (!ip) return false;
    if (ip->GetSubObjectLevel() != 0) ip->SetSubObjectLevel(0);
    return SculptMode::Get().Start(ip, object);
}

void StopSculpting() {
    if (SculptMode::Get().IsActive()) SculptMode::Get().Stop();
    if (Interface* core = GetCOREInterface()) core->SetStdCommandMode(CID_OBJSELECT);
}

bool IsSculpting() { return SculptMode::Get().IsActive(); }

void ChooseBrush(sculpt::BrushType brush) {
    SculptSettings& settings = SculptSettings::Get();
    settings.SetBrush(brush);
    if (brush == sculpt::BrushType::FaceGroups) settings.SetBool(Prop::ShowGroups, true);  // See what you paint.
    settings.Set(Prop::ToolMode, static_cast<float>(ToolMode::Sculpt));
    settings.SetBool(Prop::MaskDirect, false);
    StartSculpting();
}

void SelectPaletteSlot(int slot) {
    const std::vector<sculpt::BrushType>& order = SculptSettings::Get().PaletteOrder();
    if (slot >= 0 && slot < static_cast<int>(order.size())) ChooseBrush(order[static_cast<std::size_t>(slot)]);
}

void SelectMaskTool(MaskTool tool) {
    SculptSettings& settings = SculptSettings::Get();
    settings.Set(Prop::MaskTool, static_cast<float>(tool));
    settings.SetBool(Prop::MaskDirect, true);
    StartSculpting();
}

void SelectSculptMode() {
    SculptSettings& settings = SculptSettings::Get();
    settings.Set(Prop::ToolMode, static_cast<float>(ToolMode::Sculpt));
    settings.SetBool(Prop::MaskDirect, false);
    StartSculpting();
}

void SetStrokeMode(StrokeMode mode) {
    if (mode == StrokeMode::ColorMix) return;  // Paint-only (texture painting arrives in phase 10).
    SculptSettings::Get().Set(Prop::StrokeMode, static_cast<float>(mode));
}

// --- Operations ----------------------------------------------------------------------------

const char* OpScriptName(Op op) {
    const int i = static_cast<int>(op);
    return (i >= 0 && i < static_cast<int>(Op::Count)) ? kOps[i].scriptName : "";
}

const MCHAR* OpDisplayName(Op op) {
    const int i = static_cast<int>(op);
    return (i >= 0 && i < static_cast<int>(Op::Count)) ? kOps[i].displayName : _T("");
}

bool Run(Op op) {
    SculptMeshObject* object = Target();
    if (!object) {
        Prompt(GetString(IDS_ERR_SELECT_ONE));
        return false;
    }
    if (SculptMode::Get().StrokeActive()) return false;
    HCURSOR previous = SetCursor(LoadCursor(nullptr, IDC_WAIT));
    const bool changed = object->RunOperation(
        [op, object](SculptSessionBridge& bridge) { return RunOnSession(op, *object, bridge); }, OpDisplayName(op));
    SetCursor(previous);
    if (Interface* core = GetCOREInterface()) core->RedrawViews(core->GetTime());
    if (!changed && op == Op::GroupFromMask) Prompt(GetString(IDS_ERR_NO_MASK));
    if (changed && (op == Op::GroupFromMask || op == Op::AutoGroups))
        SculptSettings::Get().SetBool(Prop::ShowGroups, true);  // Show the new SculptGroups.
    return changed;
}

bool RunByName(const std::string& name) {
    for (int i = 0; i < static_cast<int>(Op::Count); ++i)
        if (EqualsNoCase(name, kOps[i].scriptName)) return Run(static_cast<Op>(i));
    if (EqualsNoCase(name, "startSculpt")) return StartSculpting();
    if (EqualsNoCase(name, "select")) {
        StopSculpting();
        return true;
    }
    const struct {
        const char* name;
        TransformKind kind;
    } transforms[] = {{"move", TransformKind::Move}, {"rotate", TransformKind::Rotate}, {"scale", TransformKind::Scale}};
    for (const auto& t : transforms) {
        if (EqualsNoCase(name, t.name)) {
            Transform(t.kind);
            return true;
        }
    }
    return false;
}

// --- W / E / R ---------------------------------------------------------------------------------

bool HasUsableMask() {
    SculptMeshObject* object = Target();
    if (!object) return false;
    const std::vector<float>& mask = object->Attributes().mask;
    bool anyMasked = false, anyFree = false;
    for (float m : mask) {
        anyMasked = anyMasked || m > 0.0f;
        anyFree = anyFree || m < 1.0f;
        if (anyMasked && anyFree) return true;
    }
    return false;
}

void Transform(TransformKind kind) {
    const int cid = kind == TransformKind::Move ? CID_OBJMOVE : (kind == TransformKind::Rotate ? CID_OBJROTATE : CID_OBJUSCALE);
    IObjParam* ip = SculptMeshObject::EditInterface();
    if (SculptMode::Get().StrokeActive()) return;
    if (SculptMode::Get().IsActive()) SculptMode::Get().Stop();
    if (ip && Target()) {
        // Sub-object level 1 ("Transform") moves the unmasked region; level 0 the whole object.
        const int level = HasUsableMask() ? 1 : 0;
        if (ip->GetSubObjectLevel() != level) ip->SetSubObjectLevel(level);
    }
    if (Interface* core = GetCOREInterface()) core->SetStdCommandMode(cid);
}

// --- Settings file ---------------------------------------------------------------------------

namespace {
bool settingsLoaded = false;
}  // namespace

void LoadSettings() {
    if (settingsLoaded) return;
    const MSTR path = SettingsPath();
    if (path.Length() == 0) return;
    settingsLoaded = true;  // Also when the file does not exist yet: defaults are then saved.
    FILE* file = _wfopen(path.data(), L"rb");
    if (!file) return;
    std::string text;
    char buffer[4096];
    std::size_t n = 0;
    while ((n = std::fread(buffer, 1, sizeof(buffer), file)) > 0 && text.size() < (1u << 20)) text.append(buffer, n);
    std::fclose(file);
    SculptSettings::Get().FromText(text);
}

void SaveSettings() {
    // Never overwrite the user's file with defaults if it was never read.
    if (!settingsLoaded) return;
    const MSTR path = SettingsPath();
    if (path.Length() == 0) return;
    const std::string text = SculptSettings::Get().ToText();
    // Write a temporary file first so a crash never leaves a half-written file.
    const MSTR temp = path + _T(".tmp");
    FILE* file = _wfopen(temp.data(), L"wb");
    if (!file) return;
    const bool ok = std::fwrite(text.data(), 1, text.size(), file) == text.size();
    const bool closed = std::fclose(file) == 0;
    if (ok && closed)
        MoveFileExW(temp.data(), path.data(), MOVEFILE_REPLACE_EXISTING);
    else
        DeleteFileW(temp.data());
}

}  // namespace SculptCommands
