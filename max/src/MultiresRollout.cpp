#include "MultiresRollout.h"

#include "SculptCommands.h"
#include "SculptMeshObject.h"
#include "SculptMode.h"
#include "SculptUI.h"

namespace {

class MultiresPanel {
public:
    static MultiresPanel& Get() {
        static MultiresPanel rollout;
        return rollout;
    }

    void Open(IObjParam* ip, SculptMeshObject* object) {
        if (hwnd_) Close();
        ip_ = ip;
        object_ = object;
        hwnd_ = ip->AddRollupPage(hInstance, MAKEINTRESOURCE(IDD_MULTIRES), DlgProc, GetString(IDS_ROLLUP_MULTIRES),
                                  reinterpret_cast<LPARAM>(this));
    }

    void Close() {
        if (ip_ && hwnd_) ip_->DeleteRollupPage(hwnd_);  // Sends WM_DESTROY.
        hwnd_ = nullptr;
        ip_ = nullptr;
        object_ = nullptr;
    }

    void Refresh() {
        if (!hwnd_) return;
        if (menusButton_)
            menusButton_->SetText(GetString(SculptUI::IsWanted() ? IDS_CLOSE_MENUS : IDS_OPEN_MENUS));
        if (sculptButton_) sculptButton_->SetCheck(SculptMode::Get().IsActive() && SculptMode::Get().Target() == object_);
        if (object_) {
            int faces = 0, verts = 0;
            object_->PolygonCount(ip_ ? ip_->GetTime() : 0, faces, verts);
            MSTR info;
            info.printf(GetString(IDS_INFO_FORMAT), verts, faces);
            SetDlgItemText(hwnd_, IDC_INFO_TEXT, info.data());
        }
    }

private:
    static INT_PTR CALLBACK DlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        MultiresPanel& rollout = Get();
        switch (msg) {
            case WM_INITDIALOG:
                rollout.Init(hwnd);
                return TRUE;
            case WM_DESTROY:
                rollout.Release();
                return FALSE;
            case WM_COMMAND:
                rollout.OnCommand(LOWORD(wParam));
                return TRUE;
            case WM_LBUTTONDOWN:
            case WM_LBUTTONUP:
            case WM_MOUSEMOVE:
                if (rollout.ip_) rollout.ip_->RollupMouseMessage(hwnd, msg, wParam, lParam);
                return FALSE;
            default:
                return FALSE;
        }
    }

    void Init(HWND hwnd) {
        hwnd_ = hwnd;
        menusButton_ = GetICustButton(GetDlgItem(hwnd, IDC_MENUS_BUTTON));
        sculptButton_ = GetICustButton(GetDlgItem(hwnd, IDC_SCULPT_BUTTON));
        if (sculptButton_) {
            sculptButton_->SetType(CBT_CHECK);
            sculptButton_->SetHighlightColor(GREEN_WASH);
        }
        SetDlgItemText(hwnd, IDC_HELP_TEXT, GetString(IDS_MULTIRES_HELP));
        Refresh();
    }

    void Release() {
        if (menusButton_) ReleaseICustButton(menusButton_);
        if (sculptButton_) ReleaseICustButton(sculptButton_);
        menusButton_ = nullptr;
        sculptButton_ = nullptr;
        hwnd_ = nullptr;
    }

    void OnCommand(int id) {
        switch (id) {
            case IDC_MENUS_BUTTON:
                SculptUI::Toggle();
                break;
            case IDC_SCULPT_BUTTON:
                if (SculptMode::Get().IsActive() && SculptMode::Get().Target() == object_)
                    SculptCommands::StopSculpting();
                else
                    SculptCommands::StartSculpting();
                break;
            default:
                break;
        }
        Refresh();
    }

    IObjParam* ip_ = nullptr;
    SculptMeshObject* object_ = nullptr;
    HWND hwnd_ = nullptr;
    ICustButton* menusButton_ = nullptr;
    ICustButton* sculptButton_ = nullptr;
};

}  // namespace

namespace MultiresRollout {

void Open(IObjParam* ip, SculptMeshObject* object) {
    if (ip && object) MultiresPanel::Get().Open(ip, object);
}

void Close(IObjParam* /*ip*/) { MultiresPanel::Get().Close(); }

void Refresh() { MultiresPanel::Get().Refresh(); }

}  // namespace MultiresRollout
