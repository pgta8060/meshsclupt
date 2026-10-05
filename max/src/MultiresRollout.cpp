#include "MultiresRollout.h"

#include <algorithm>
#include <string>

#include "RowsRollup.h"
#include "SculptCommands.h"
#include "SculptMeshObject.h"
#include "SculptMode.h"
#include "SculptSettings.h"
#include "SculptUI.h"

namespace {

RowsRollup& Page() {
    static RowsRollup page;
    return page;
}

// Keeps the page current and re-applies Auto Smooth when its settings change.
class SettingsWatcher : public SculptSettings::Listener {
public:
    void Start() {
        if (active_) return;
        autosmooth_ = SculptSettings::Get().Bool(Prop::Autosmooth);
        angle_ = SculptSettings::Get().Value(Prop::AutosmoothAngle);
        SculptSettings::Get().AddListener(this);
        active_ = true;
    }
    void Stop() {
        if (active_) SculptSettings::Get().RemoveListener(this);
        active_ = false;
    }
    void OnSculptSettingsChanged() override {
        const bool autosmooth = SculptSettings::Get().Bool(Prop::Autosmooth);
        const float angle = SculptSettings::Get().Value(Prop::AutosmoothAngle);
        if (autosmooth != autosmooth_ || angle != angle_) {
            autosmooth_ = autosmooth;
            angle_ = angle;
            if (SculptMeshObject* object = SculptMeshObject::EditedObject()) object->RefreshSmoothing();
        }
        Page().Refresh();
    }

private:
    bool active_ = false;
    bool autosmooth_ = true;
    float angle_ = 45.0f;
};

SettingsWatcher& Watcher() {
    static SettingsWatcher watcher;
    return watcher;
}

SculptMeshObject* Object() { return SculptMeshObject::EditedObject(); }
int Level() { return Object() ? Object()->MultiresLevel() : 0; }
int Top() { return Object() ? Object()->MultiresTopLevel() : 0; }
SculptSettings& S() { return SculptSettings::Get(); }

void Build(ui::RowList& r) {
    r.Buttons({{L"", [] { SculptUI::Toggle(); }, [] { return SculptUI::IsWanted(); }, {},
                [] { return std::wstring(GetString(SculptUI::IsWanted() ? IDS_CLOSE_MENUS : IDS_OPEN_MENUS)); }}});
    r.Buttons({{L"Sculpt",
                [] {
                    if (SculptCommands::IsSculpting())
                        SculptCommands::StopSculpting();
                    else
                        SculptCommands::StartSculpting();
                },
                [] { return SculptCommands::IsSculpting(); }, {}, {}}});
    r.Buttons({{L"Reverse Subdivision", [] { SculptCommands::ReverseSubdivision(); }, {}, [] { return Level() == 0; }, {}}});
    r.Buttons({{L"Del Lower", [] { SculptCommands::DeleteLowerLevels(); }, {}, [] { return Level() > 0; }, {}},
               {L"Del Higher", [] { SculptCommands::DeleteHigherLevels(); }, {}, [] { return Level() < Top(); }, {}}});
    ui::RowList::SliderSpec level;
    level.label = L"Level";
    level.get = [] { return static_cast<float>(Level()); };
    level.set = [](float v) { SculptCommands::SetMultiresLevel(static_cast<int>(v + 0.5f)); };
    level.min = 0.0f;
    level.max = 1.0f;
    level.dynamicMax = [] { return static_cast<float>(std::max(Top(), 1)); };
    level.decimals = 0;
    level.applyOnRelease = true;  // Every level change rebuilds the mesh.
    level.format = [](float v) {
        return std::to_wstring(static_cast<int>(v + 0.5f)) + L" / " + std::to_wstring(Top());
    };
    r.Slider(level, [] { return Top() > 0; });
    r.Buttons({{L"Subdivide Level", [] { SculptCommands::SubdivideLevel(); }, {},
                [] { return Level() == Top() && Top() < sculpt::Multires::kMaxLevel; }, {}}});
    r.Check(L"Materials ID", [] { return S().Bool(Prop::MultiresUseMaterials); },
            [](bool on) { S().SetBool(Prop::MultiresUseMaterials, on); });
    r.Check(L"Smoothing Groups", [] { return S().Bool(Prop::MultiresUseSmoothing); },
            [](bool on) { S().SetBool(Prop::MultiresUseSmoothing, on); });
    r.Check(L"Autosmooth", [] { return S().Bool(Prop::Autosmooth); }, [](bool on) { S().SetBool(Prop::Autosmooth, on); });
    ui::RowList::SliderSpec angle;
    angle.label = L"Angle";
    angle.get = [] { return S().Value(Prop::AutosmoothAngle); };
    angle.set = [](float v) { S().Set(Prop::AutosmoothAngle, v); };
    angle.min = 0.0f;
    angle.max = 180.0f;
    angle.typeMax = 180.0f;
    angle.defaultValue = 45.0f;
    angle.decimals = 1;
    angle.applyOnRelease = true;
    r.Slider(angle, [] { return S().Bool(Prop::Autosmooth); });
    r.Note([] {
        SculptMeshObject* object = Object();
        if (!object) return std::wstring();
        int faces = 0, verts = 0;
        object->PolygonCount(0, faces, verts);
        MSTR info;
        info.printf(GetString(IDS_INFO_FORMAT), verts, faces);
        return std::wstring(info.data());
    });
}

}  // namespace

namespace MultiresRollout {

void Open(IObjParam* ip, SculptMeshObject* object) {
    if (!ip || !object) return;
    Page().Open(ip, GetString(IDS_ROLLUP_MULTIRES), Build);
    Watcher().Start();
}

void Close(IObjParam* /*ip*/) {
    Watcher().Stop();
    Page().Close();
}

void Refresh() { Page().Refresh(); }

}  // namespace MultiresRollout
