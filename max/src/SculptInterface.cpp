// MAXScript access: the static "SculptMesh" interface, e.g.
//   SculptMesh.Brush = #clay
//   SculptMesh.BrushSize = 80
//   SculptMesh.SetValue "mirrorX" 1
//   SculptMesh.Run "maskInvert"
//   SculptMesh.StartSculpt()
#include <ifnpub.h>
#include <maxscript/maxscript.h>

#include "ConvertToSculpt.h"
#include "SculptCommands.h"
#include "SculptMeshObject.h"
#include "SculptMode.h"
#include "SculptSettings.h"
#include "SculptUI.h"

namespace {

std::string Utf8(const MCHAR* text) {
    if (!text) return std::string();
    const MSTR s(text);
    return std::string(s.ToUTF8().data());
}

// "strength" (active brush), "brush.clay.strength" (a given brush) or a setting key.
bool ResolveKey(const std::string& key, Prop& prop, bool& isProp, sculpt::BrushType& brush, BrushProp& brushProp) {
    if (findProp(key, prop)) {
        isProp = true;
        return true;
    }
    isProp = false;
    brush = SculptSettings::Get().Brush();
    if (findBrushProp(key, brushProp)) return true;
    if (key.rfind("brush.", 0) != 0) return false;
    const std::size_t dot = key.find('.', 6);
    if (dot == std::string::npos || !findBrushProp(key.substr(dot + 1), brushProp)) return false;
    const std::string name = key.substr(6, dot - 6);
    for (int b = 0; b < static_cast<int>(sculpt::BrushType::Count); ++b) {
        if (_stricmp(name.c_str(), sculpt::brushInfo(static_cast<sculpt::BrushType>(b)).scriptName) == 0) {
            brush = static_cast<sculpt::BrushType>(b);
            return true;
        }
    }
    return false;
}

class SculptMeshInterface : public FPStaticInterface {
public:
    DECLARE_DESCRIPTOR(SculptMeshInterface)

    enum FunctionId {
        kStartSculpt,
        kStopSculpt,
        kIsSculpting,
        kConvertToSculpt,
        kGetBrush,
        kSetBrush,
        kGetBrushSize,
        kSetBrushSize,
        kGetBrushStrength,
        kSetBrushStrength,
        kGetStrokeSpacing,
        kSetStrokeSpacing,
        kGetSubtract,
        kSetSubtract,
        kGetUseFalloff,
        kSetUseFalloff,
        kGetBackfaceCull,
        kSetBackfaceCull,
        kGetVersion,
        kGetValue,
        kSetValue,
        kRun,
        kOpenMenus,
        kCloseMenus,
        kGetMenusOpen,
        kSetMenusOpen,
        kResetSettings,
        kSaveSettings,
        kGetLevel,
        kSetLevel,
        kGetTopLevel,
    };
    enum EnumId { kBrushEnum };

    BEGIN_FUNCTION_MAP
        FN_0(kStartSculpt, TYPE_bool, StartSculpt)
        VFN_0(kStopSculpt, StopSculpt)
        FN_0(kIsSculpting, TYPE_bool, IsSculpting)
        FN_1(kConvertToSculpt, TYPE_bool, ConvertToSculpt, TYPE_INODE)
        PROP_FNS(kGetBrush, GetBrush, kSetBrush, SetBrush, TYPE_ENUM)
        PROP_FNS(kGetBrushSize, GetBrushSize, kSetBrushSize, SetBrushSize, TYPE_FLOAT)
        PROP_FNS(kGetBrushStrength, GetBrushStrength, kSetBrushStrength, SetBrushStrength, TYPE_FLOAT)
        PROP_FNS(kGetStrokeSpacing, GetStrokeSpacing, kSetStrokeSpacing, SetStrokeSpacing, TYPE_FLOAT)
        PROP_FNS(kGetSubtract, GetSubtract, kSetSubtract, SetSubtract, TYPE_bool)
        PROP_FNS(kGetUseFalloff, GetUseFalloff, kSetUseFalloff, SetUseFalloff, TYPE_bool)
        PROP_FNS(kGetBackfaceCull, GetBackfaceCull, kSetBackfaceCull, SetBackfaceCull, TYPE_bool)
        RO_PROP_FN(kGetVersion, GetVersion, TYPE_TSTR_BV)
        FN_1(kGetValue, TYPE_FLOAT, GetValue, TYPE_STRING)
        FN_2(kSetValue, TYPE_bool, SetValue, TYPE_STRING, TYPE_FLOAT)
        FN_1(kRun, TYPE_bool, Run, TYPE_STRING)
        VFN_0(kOpenMenus, OpenMenus)
        VFN_0(kCloseMenus, CloseMenus)
        PROP_FNS(kGetMenusOpen, GetMenusOpen, kSetMenusOpen, SetMenusOpen, TYPE_bool)
        VFN_0(kResetSettings, ResetSettings)
        VFN_0(kSaveSettings, SaveSettings)
        PROP_FNS(kGetLevel, GetLevel, kSetLevel, SetLevel, TYPE_INT)
        RO_PROP_FN(kGetTopLevel, GetTopLevel, TYPE_INT)
    END_FUNCTION_MAP

    // Starts sculpting the selected Sculpt Mesh (opens the Modify panel if needed).
    bool StartSculpt() { return SculptCommands::StartSculpting(); }
    void StopSculpt() { SculptCommands::StopSculpting(); }
    bool IsSculpting() { return SculptCommands::IsSculpting(); }

    bool ConvertToSculpt(INode* node) {
        Interface* core = GetCOREInterface();
        const bool ok = ConvertNodeToSculpt(node, core ? core->GetTime() : 0);
        if (ok && core) core->RedrawViews(core->GetTime());
        return ok;
    }

    int GetBrush() { return static_cast<int>(SculptSettings::Get().Brush()); }
    void SetBrush(int brush) {
        if (brush >= 0 && brush < static_cast<int>(sculpt::BrushType::Count))
            SculptSettings::Get().SetBrush(static_cast<sculpt::BrushType>(brush));
    }
    float GetBrushSize() { return SculptSettings::Get().Size(); }
    void SetBrushSize(float v) { SculptSettings::Get().Set(Prop::BrushSize, v); }
    float GetBrushStrength() { return SculptSettings::Get().Strength(); }
    void SetBrushStrength(float v) { SculptSettings::Get().SetStrength(v); }
    float GetStrokeSpacing() { return SculptSettings::Get().Spacing(); }
    void SetStrokeSpacing(float v) { SculptSettings::Get().Set(Prop::StrokeSpacing, v); }
    bool GetSubtract() { return SculptSettings::Get().Subtract(); }
    void SetSubtract(bool v) { SculptSettings::Get().SetSubtract(v); }
    bool GetUseFalloff() { return SculptSettings::Get().Bool(Prop::UseFalloff); }
    void SetUseFalloff(bool v) { SculptSettings::Get().SetBool(Prop::UseFalloff, v); }
    bool GetBackfaceCull() { return SculptSettings::Get().BackfaceCull(); }
    void SetBackfaceCull(bool v) {
        SculptSettings& s = SculptSettings::Get();
        s.SetBrushValue(s.Brush(), BrushProp::BackfaceCull, v ? 1.0f : 0.0f);
    }
    MSTR GetVersion() { return MSTR(SCULPTMESH_VERSION_STRING); }

    float GetValue(const MCHAR* key) {
        Prop prop = Prop::Count;
        bool isProp = false;
        sculpt::BrushType brush = sculpt::BrushType::Sculpt;
        BrushProp brushProp = BrushProp::Strength;
        if (!ResolveKey(Utf8(key), prop, isProp, brush, brushProp))
            throw RuntimeError(_T("SculptMesh.GetValue: unknown setting "), key ? key : _T(""));
        const SculptSettings& s = SculptSettings::Get();
        return isProp ? s.Value(prop) : s.BrushValue(brush, brushProp);
    }

    bool SetValue(const MCHAR* key, float value) {
        Prop prop = Prop::Count;
        bool isProp = false;
        sculpt::BrushType brush = sculpt::BrushType::Sculpt;
        BrushProp brushProp = BrushProp::Strength;
        if (!ResolveKey(Utf8(key), prop, isProp, brush, brushProp)) return false;
        SculptSettings& s = SculptSettings::Get();
        if (isProp)
            s.Set(prop, value);
        else
            s.SetBrushValue(brush, brushProp, value);
        return true;
    }

    bool Run(const MCHAR* command) { return SculptCommands::RunByName(Utf8(command)); }

    void OpenMenus() { SculptUI::Open(); }
    void CloseMenus() { SculptUI::Close(); }
    bool GetMenusOpen() { return SculptUI::IsWanted(); }
    void SetMenusOpen(bool open) { open ? SculptUI::Open() : SculptUI::Close(); }
    void ResetSettings() { SculptSettings::Get().ResetToDefaults(); }
    int GetLevel() { return SculptMeshObject::EditedObject() ? SculptMeshObject::EditedObject()->MultiresLevel() : 0; }
    void SetLevel(int level) { SculptCommands::SetMultiresLevel(level); }
    int GetTopLevel() {
        return SculptMeshObject::EditedObject() ? SculptMeshObject::EditedObject()->MultiresTopLevel() : 0;
    }
    void SaveSettings() { SculptCommands::SaveSettings(); }
};

// clang-format off
SculptMeshInterface theSculptMeshInterface(
    SCULPTMESH_FP_INTERFACE_ID, _T("SculptMesh"), 0, nullptr, FP_CORE,

    SculptMeshInterface::kStartSculpt, _T("StartSculpt"), 0, TYPE_bool, 0, 0,
    SculptMeshInterface::kStopSculpt, _T("StopSculpt"), 0, TYPE_VOID, 0, 0,
    SculptMeshInterface::kIsSculpting, _T("IsSculpting"), 0, TYPE_bool, 0, 0,
    SculptMeshInterface::kConvertToSculpt, _T("ConvertToSculpt"), 0, TYPE_bool, 0, 1,
        _T("node"), 0, TYPE_INODE,
    SculptMeshInterface::kGetValue, _T("GetValue"), 0, TYPE_FLOAT, 0, 1,
        _T("key"), 0, TYPE_STRING,
    SculptMeshInterface::kSetValue, _T("SetValue"), 0, TYPE_bool, 0, 2,
        _T("key"), 0, TYPE_STRING,
        _T("value"), 0, TYPE_FLOAT,
    SculptMeshInterface::kRun, _T("Run"), 0, TYPE_bool, 0, 1,
        _T("command"), 0, TYPE_STRING,
    SculptMeshInterface::kOpenMenus, _T("OpenMenus"), 0, TYPE_VOID, 0, 0,
    SculptMeshInterface::kCloseMenus, _T("CloseMenus"), 0, TYPE_VOID, 0, 0,
    SculptMeshInterface::kResetSettings, _T("ResetSettings"), 0, TYPE_VOID, 0, 0,
    SculptMeshInterface::kSaveSettings, _T("SaveSettings"), 0, TYPE_VOID, 0, 0,

    properties,
        SculptMeshInterface::kGetBrush, SculptMeshInterface::kSetBrush, _T("Brush"), 0, TYPE_ENUM, SculptMeshInterface::kBrushEnum,
        SculptMeshInterface::kGetBrushSize, SculptMeshInterface::kSetBrushSize, _T("BrushSize"), 0, TYPE_FLOAT,
        SculptMeshInterface::kGetBrushStrength, SculptMeshInterface::kSetBrushStrength, _T("BrushStrength"), 0, TYPE_FLOAT,
        SculptMeshInterface::kGetStrokeSpacing, SculptMeshInterface::kSetStrokeSpacing, _T("StrokeSpacing"), 0, TYPE_FLOAT,
        SculptMeshInterface::kGetSubtract, SculptMeshInterface::kSetSubtract, _T("Subtract"), 0, TYPE_bool,
        SculptMeshInterface::kGetUseFalloff, SculptMeshInterface::kSetUseFalloff, _T("UseFalloff"), 0, TYPE_bool,
        SculptMeshInterface::kGetBackfaceCull, SculptMeshInterface::kSetBackfaceCull, _T("BackfaceCull"), 0, TYPE_bool,
        SculptMeshInterface::kGetVersion, FP_NO_FUNCTION, _T("Version"), 0, TYPE_TSTR_BV,
        SculptMeshInterface::kGetMenusOpen, SculptMeshInterface::kSetMenusOpen, _T("MenusOpen"), 0, TYPE_bool,
        SculptMeshInterface::kGetLevel, SculptMeshInterface::kSetLevel, _T("MultiresLevel"), 0, TYPE_INT,
        SculptMeshInterface::kGetTopLevel, FP_NO_FUNCTION, _T("MultiresTopLevel"), 0, TYPE_INT,

    enums,
        SculptMeshInterface::kBrushEnum, 25,
            _T("sculpt"), static_cast<int>(sculpt::BrushType::Sculpt),
            _T("smooth"), static_cast<int>(sculpt::BrushType::Smooth),
            _T("inflate"), static_cast<int>(sculpt::BrushType::Inflate),
            _T("pinch"), static_cast<int>(sculpt::BrushType::Pinch),
            _T("clay"), static_cast<int>(sculpt::BrushType::Clay),
            _T("clayBuildup"), static_cast<int>(sculpt::BrushType::ClayBuildup),
            _T("carve"), static_cast<int>(sculpt::BrushType::Carve),
            _T("knife"), static_cast<int>(sculpt::BrushType::Knife),
            _T("contrast"), static_cast<int>(sculpt::BrushType::Contrast),
            _T("scrape"), static_cast<int>(sculpt::BrushType::Scrape),
            _T("polish"), static_cast<int>(sculpt::BrushType::Polish),
            _T("move"), static_cast<int>(sculpt::BrushType::Move),
            _T("snakeHook"), static_cast<int>(sculpt::BrushType::SnakeHook),
            _T("faceGroups"), static_cast<int>(sculpt::BrushType::FaceGroups),
            _T("smoothGroupBorder"), static_cast<int>(sculpt::BrushType::SmoothGroupBorder),
            _T("density"), static_cast<int>(sculpt::BrushType::Density),
            _T("revert"), static_cast<int>(sculpt::BrushType::Revert),
            _T("clip"), static_cast<int>(sculpt::BrushType::Clip),
            _T("cutter"), static_cast<int>(sculpt::BrushType::Cutter),
            _T("slice"), static_cast<int>(sculpt::BrushType::Slice),
            _T("cloth"), static_cast<int>(sculpt::BrushType::Cloth),
            _T("pose"), static_cast<int>(sculpt::BrushType::Pose),
            _T("curveTube"), static_cast<int>(sculpt::BrushType::CurveTube),
            _T("displace"), static_cast<int>(sculpt::BrushType::Displace),
            _T("maskPaint"), static_cast<int>(sculpt::BrushType::MaskPaint),
    p_end);
// clang-format on

static_assert(static_cast<int>(sculpt::BrushType::Count) == 25, "Update the Brush enum in the SculptMesh interface.");

}  // namespace
