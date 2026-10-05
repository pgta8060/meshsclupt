// MAXScript access: the static "SculptMesh" interface, e.g.
//   SculptMesh.Brush = #smooth
//   SculptMesh.BrushSize = 80
//   SculptMesh.StartSculpt()
#include <ifnpub.h>

#include "ConvertToSculpt.h"
#include "SculptMeshObject.h"
#include "SculptMode.h"
#include "SculptSettings.h"

namespace {

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
    END_FUNCTION_MAP

    // Starts sculpting the selected Sculpt Mesh (opens the Modify panel if needed).
    bool StartSculpt() {
        Interface* core = GetCOREInterface();
        if (!core || core->GetSelNodeCount() != 1) return false;
        INode* node = core->GetSelNode(0);
        Object* ref = node ? node->GetObjectRef() : nullptr;
        if (!ref || ref->FindBaseObject()->ClassID() != SCULPTMESH_CLASS_ID) return false;
        if (core->GetCommandPanelTaskMode() != TASK_MODE_MODIFY) core->SetCommandPanelTaskMode(TASK_MODE_MODIFY);
        SculptMeshObject* edited = SculptMeshObject::EditedObject();
        if (!edited || edited != ref->FindBaseObject()) return false;
        return SculptMode::Get().Start(SculptMeshObject::EditInterface(), edited);
    }
    void StopSculpt() { SculptMode::Get().Stop(); }
    bool IsSculpting() { return SculptMode::Get().IsActive(); }

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
    void SetBrushSize(float v) { SculptSettings::Get().SetSize(v); }
    float GetBrushStrength() { return SculptSettings::Get().Strength(); }
    void SetBrushStrength(float v) { SculptSettings::Get().SetStrength(v); }
    float GetStrokeSpacing() { return SculptSettings::Get().Spacing(); }
    void SetStrokeSpacing(float v) { SculptSettings::Get().SetSpacing(v); }
    bool GetSubtract() { return SculptSettings::Get().Subtract(); }
    void SetSubtract(bool v) { SculptSettings::Get().SetSubtract(v); }
    bool GetUseFalloff() { return SculptSettings::Get().UseFalloff(); }
    void SetUseFalloff(bool v) { SculptSettings::Get().SetUseFalloff(v); }
    bool GetBackfaceCull() { return SculptSettings::Get().BackfaceCull(); }
    void SetBackfaceCull(bool v) { SculptSettings::Get().SetBackfaceCull(v); }
    MSTR GetVersion() { return MSTR(SCULPTMESH_VERSION_STRING); }
};

// clang-format off
SculptMeshInterface theSculptMeshInterface(
    SCULPTMESH_FP_INTERFACE_ID, _T("SculptMesh"), 0, nullptr, FP_CORE,

    SculptMeshInterface::kStartSculpt, _T("StartSculpt"), 0, TYPE_bool, 0, 0,
    SculptMeshInterface::kStopSculpt, _T("StopSculpt"), 0, TYPE_VOID, 0, 0,
    SculptMeshInterface::kIsSculpting, _T("IsSculpting"), 0, TYPE_bool, 0, 0,
    SculptMeshInterface::kConvertToSculpt, _T("ConvertToSculpt"), 0, TYPE_bool, 0, 1,
        _T("node"), 0, TYPE_INODE,

    properties,
        SculptMeshInterface::kGetBrush, SculptMeshInterface::kSetBrush, _T("Brush"), 0, TYPE_ENUM, SculptMeshInterface::kBrushEnum,
        SculptMeshInterface::kGetBrushSize, SculptMeshInterface::kSetBrushSize, _T("BrushSize"), 0, TYPE_FLOAT,
        SculptMeshInterface::kGetBrushStrength, SculptMeshInterface::kSetBrushStrength, _T("BrushStrength"), 0, TYPE_FLOAT,
        SculptMeshInterface::kGetStrokeSpacing, SculptMeshInterface::kSetStrokeSpacing, _T("StrokeSpacing"), 0, TYPE_FLOAT,
        SculptMeshInterface::kGetSubtract, SculptMeshInterface::kSetSubtract, _T("Subtract"), 0, TYPE_bool,
        SculptMeshInterface::kGetUseFalloff, SculptMeshInterface::kSetUseFalloff, _T("UseFalloff"), 0, TYPE_bool,
        SculptMeshInterface::kGetBackfaceCull, SculptMeshInterface::kSetBackfaceCull, _T("BackfaceCull"), 0, TYPE_bool,
        SculptMeshInterface::kGetVersion, FP_NO_FUNCTION, _T("Version"), 0, TYPE_TSTR_BV,

    enums,
        SculptMeshInterface::kBrushEnum, 4,
            _T("sculpt"), static_cast<int>(sculpt::BrushType::Sculpt),
            _T("smooth"), static_cast<int>(sculpt::BrushType::Smooth),
            _T("inflate"), static_cast<int>(sculpt::BrushType::Inflate),
            _T("pinch"), static_cast<int>(sculpt::BrushType::Pinch),
    p_end);
// clang-format on

}  // namespace
