#include "DisplaceRollout.h"

#include <algorithm>
#include <string>

#include <commdlg.h>

#include "RowsRollup.h"
#include "SculptMeshObject.h"
#include "SculptSettings.h"
#include "SculptUI.h"

namespace {

SculptSettings& S() { return SculptSettings::Get(); }

RowsRollup& Page() {
    static RowsRollup page;
    return page;
}

std::wstring Widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(std::max(n, 0)), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &out[0], n);
    return out;
}

std::string Narrow(const std::wstring& s) {
    if (s.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(std::max(n, 0)), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &out[0], n, nullptr, nullptr);
    return out;
}

std::wstring MapName() {
    const std::wstring path = Widen(S().DisplaceMap());
    if (path.empty()) return L"Map: None";
    const std::size_t slash = path.find_last_of(L"\\/");
    return L"Map: " + (slash == std::wstring::npos ? path : path.substr(slash + 1));
}

ui::RowList::SliderSpec Slider(const wchar_t* label, Prop prop, float lo, float hi, int decimals) {
    const PropInfo& info = propInfo(prop);
    ui::RowList::SliderSpec s;
    s.label = label;
    s.get = [prop] { return S().Value(prop); };
    s.set = [prop](float v) { S().Set(prop, v); };
    s.min = lo;
    s.max = hi;
    s.typeMax = info.maxValue;
    s.defaultValue = info.defaultValue;
    s.decimals = decimals;
    s.applyOnRelease = true;  // Each change may recompute the layer.
    return s;
}

// Live Update: recompute shortly after the last change.
UINT_PTR liveTimer = 0;

void CALLBACK LiveTimerProc(HWND, UINT, UINT_PTR, DWORD) {
    KillTimer(nullptr, liveTimer);
    liveTimer = 0;
    SculptMeshObject* object = SculptMeshObject::EditedObject();
    if (!object || !object->HasDisplaceLayer() || !S().Bool(Prop::DisplaceLive)) return;
    MSTR error;
    HCURSOR previous = SetCursor(LoadCursor(nullptr, IDC_WAIT));
    if (!object->ApplyDisplace(error) && error.Length() > 0) {
        if (Interface* core = GetCOREInterface()) core->ReplacePrompt(error.data());
    }
    SetCursor(previous);
    if (Interface* core = GetCOREInterface()) core->RedrawViews(core->GetTime());
}

class Watcher : public SculptSettings::Listener {
public:
    void Start() {
        if (active_) return;
        Snapshot(state_);
        S().AddListener(this);
        active_ = true;
    }
    void Stop() {
        if (active_) S().RemoveListener(this);
        active_ = false;
        if (liveTimer) KillTimer(nullptr, liveTimer);
        liveTimer = 0;
    }
    void OnSculptSettingsChanged() override {
        State now;
        Snapshot(now);
        if (now == state_) return;
        state_ = now;
        Page().Refresh();
        if (!now.live) return;
        if (liveTimer) KillTimer(nullptr, liveTimer);
        liveTimer = SetTimer(nullptr, 0, 350, &LiveTimerProc);
    }

private:
    struct State {
        std::string map;
        float values[9] = {};
        bool live = false;
        bool operator==(const State& o) const {
            return map == o.map && live == o.live && std::equal(values, values + 9, o.values);
        }
    };
    static void Snapshot(State& s) {
        s.map = S().DisplaceMap();
        const Prop props[9] = {Prop::DisplaceTriplanar, Prop::DisplaceStrength, Prop::DisplaceWaterLevel,
                               Prop::DisplaceBlur, Prop::DisplaceContrast, Prop::DisplaceTileU,
                               Prop::DisplaceTileV, Prop::DisplaceOffsetU, Prop::DisplaceOffsetV};
        for (int i = 0; i < 9; ++i) s.values[i] = S().Value(props[i]);
        s.live = S().Bool(Prop::DisplaceLive);
    }
    bool active_ = false;
    State state_;
};

Watcher& TheWatcher() {
    static Watcher watcher;
    return watcher;
}

void Build(ui::RowList& r) {
    r.Buttons({{L"", [] { DisplaceRollout::BrowseMap(); }, {}, {}, [] { return MapName(); }},
                {L"Clear Map", [] { S().SetDisplaceMap(std::string()); }, {}, [] { return !S().DisplaceMap().empty(); }, {}}});
    r.Choice(L"Mapping", {L"UV Channel 1", L"Triplanar"}, [] { return S().Int(Prop::DisplaceTriplanar); },
             [](int i) { S().SetBool(Prop::DisplaceTriplanar, i == 1); });
    r.Slider(Slider(L"Strength", Prop::DisplaceStrength, -10.0f, 10.0f, 3));
    r.Slider(Slider(L"Water Level", Prop::DisplaceWaterLevel, -0.5f, 0.5f, 3));
    r.Slider(Slider(L"Blur (px)", Prop::DisplaceBlur, 0.0f, 32.0f, 1));
    r.Slider(Slider(L"Contrast", Prop::DisplaceContrast, 0.0f, 4.0f, 2));
    r.Slider(Slider(L"Tile U", Prop::DisplaceTileU, 0.01f, 10.0f, 2));
    r.Slider(Slider(L"Tile V", Prop::DisplaceTileV, 0.01f, 10.0f, 2));
    r.Slider(Slider(L"Offset U", Prop::DisplaceOffsetU, -1.0f, 1.0f, 3));
    r.Slider(Slider(L"Offset V", Prop::DisplaceOffsetV, -1.0f, 1.0f, 3));
    r.Check(L"Live Update", [] { return S().Bool(Prop::DisplaceLive); }, [](bool on) { S().SetBool(Prop::DisplaceLive, on); });
    r.Buttons({{L"Apply", [] { DisplaceRollout::Apply(); }, {}, [] { return !S().DisplaceMap().empty(); }, {}},
               {L"Remove",
                [] {
                    if (SculptMeshObject* object = SculptMeshObject::EditedObject()) object->RemoveDisplace();
                    if (Interface* core = GetCOREInterface()) core->RedrawViews(core->GetTime());
                },
                {},
                [] { return SculptMeshObject::EditedObject() && SculptMeshObject::EditedObject()->HasDisplaceLayer(); },
                {}}});
    r.Note([] {
        SculptMeshObject* object = SculptMeshObject::EditedObject();
        return std::wstring(object && object->HasDisplaceLayer() ? L"Displace layer: applied" : L"Displace layer: none");
    });
}

}  // namespace

namespace DisplaceRollout {

void Open(IObjParam* ip, SculptMeshObject* object) {
    if (!ip || !object) return;
    Page().Open(ip, GetString(IDS_ROLLUP_DISPLACE), Build, true);
    TheWatcher().Start();
}

void Close(IObjParam* /*ip*/) {
    TheWatcher().Stop();
    Page().Close();
}

void Refresh() { Page().Refresh(); }

bool BrowseMap() {
    wchar_t file[MAX_PATH] = {};
    const std::wstring current = Widen(S().DisplaceMap());
    lstrcpynW(file, current.c_str(), MAX_PATH);
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    Interface* core = GetCOREInterface();
    ofn.hwndOwner = core ? core->GetMAXHWnd() : nullptr;
    ofn.lpstrFilter = L"Images (*.png;*.jpg;*.jpeg;*.tif;*.tiff;*.bmp;*.tga;*.exr;*.psd)\0"
                      L"*.png;*.jpg;*.jpeg;*.tif;*.tiff;*.bmp;*.tga;*.exr;*.psd\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    DisableAccelerators();
    const BOOL ok = GetOpenFileNameW(&ofn);
    EnableAccelerators();
    if (!ok) return false;
    S().SetDisplaceMap(Narrow(file));
    return true;
}

bool Apply() {
    SculptMeshObject* object = SculptMeshObject::EditedObject();
    if (!object) return false;
    MSTR error;
    HCURSOR previous = SetCursor(LoadCursor(nullptr, IDC_WAIT));
    const bool ok = object->ApplyDisplace(error);
    SetCursor(previous);
    Interface* core = GetCOREInterface();
    if (!ok && error.Length() > 0)
        MessageBoxW(core ? core->GetMAXHWnd() : nullptr, error.data(), L"Sculpt Mesh", MB_OK | MB_ICONINFORMATION);
    if (core) core->RedrawViews(core->GetTime());
    Refresh();
    return ok;
}

}  // namespace DisplaceRollout
