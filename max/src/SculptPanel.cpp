#include "SculptPanel.h"

#include "SculptMeshObject.h"
#include "SculptMode.h"
#include "SculptSettings.h"

namespace {

class Panel : public SculptSettings::Listener {
public:
    static Panel& Get() {
        static Panel panel;
        return panel;
    }

    void Open(IObjParam* ip, SculptMeshObject* object) {
        if (hwnd_) Close();
        ip_ = ip;
        object_ = object;
        hwnd_ = ip->AddRollupPage(hInstance, MAKEINTRESOURCE(IDD_SCULPT_PANEL), DlgProc, GetString(IDS_ROLLUP_TITLE),
                                  reinterpret_cast<LPARAM>(this));
        SculptSettings::Get().AddListener(this);
    }

    void Close() {
        SculptSettings::Get().RemoveListener(this);
        if (ip_ && hwnd_) ip_->DeleteRollupPage(hwnd_);  // Sends WM_DESTROY.
        hwnd_ = nullptr;
        ip_ = nullptr;
        object_ = nullptr;
    }

    void Refresh() {
        if (!hwnd_) return;
        updating_ = true;
        const SculptSettings& s = SculptSettings::Get();
        SendDlgItemMessage(hwnd_, IDC_BRUSH_COMBO, CB_SETCURSEL, static_cast<WPARAM>(s.Brush()), 0);
        CheckRadioButton(hwnd_, IDC_ADD_RADIO, IDC_SUB_RADIO, s.Subtract() ? IDC_SUB_RADIO : IDC_ADD_RADIO);
        CheckDlgButton(hwnd_, IDC_FALLOFF_CHECK, s.UseFalloff() ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd_, IDC_BACKFACE_CHECK, s.BackfaceCull() ? BST_CHECKED : BST_UNCHECKED);
        if (size_) size_->SetValue(s.Size(), FALSE);
        if (strength_) strength_->SetValue(s.Strength(), FALSE);
        if (spacing_) spacing_->SetValue(s.Spacing(), FALSE);
        const bool signedBrush = sculpt::isSignedBrush(s.Brush());
        EnableWindow(GetDlgItem(hwnd_, IDC_ADD_RADIO), signedBrush);
        EnableWindow(GetDlgItem(hwnd_, IDC_SUB_RADIO), signedBrush);
        if (sculptButton_)
            sculptButton_->SetCheck(SculptMode::Get().IsActive() && SculptMode::Get().Target() == object_);
        if (object_) {
            int faces = 0, verts = 0;
            object_->PolygonCount(ip_ ? ip_->GetTime() : 0, faces, verts);
            MSTR info;
            info.printf(GetString(IDS_INFO_FORMAT), verts, faces);
            SetDlgItemText(hwnd_, IDC_INFO_TEXT, info.data());
        }
        updating_ = false;
    }

    void OnSculptSettingsChanged() override { Refresh(); }

private:
    static INT_PTR CALLBACK DlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        Panel& panel = Get();
        switch (msg) {
            case WM_INITDIALOG:
                panel.Init(hwnd);
                return TRUE;
            case WM_DESTROY:
                panel.Release();
                return FALSE;
            case CC_SPINNER_CHANGE:
                panel.OnSpinner(LOWORD(wParam));
                return TRUE;
            case WM_COMMAND:
                panel.OnCommand(LOWORD(wParam), HIWORD(wParam));
                return TRUE;
            case WM_LBUTTONDOWN:
            case WM_LBUTTONUP:
            case WM_MOUSEMOVE:
                if (panel.ip_) panel.ip_->RollupMouseMessage(hwnd, msg, wParam, lParam);
                return FALSE;
            default:
                return FALSE;
        }
    }

    void Init(HWND hwnd) {
        hwnd_ = hwnd;
        sculptButton_ = GetICustButton(GetDlgItem(hwnd, IDC_SCULPT_BUTTON));
        if (sculptButton_) {
            sculptButton_->SetType(CBT_CHECK);
            sculptButton_->SetHighlightColor(GREEN_WASH);
        }

        const int brushNames[] = {IDS_BRUSH_SCULPT, IDS_BRUSH_SMOOTH, IDS_BRUSH_INFLATE, IDS_BRUSH_PINCH};
        static_assert(sizeof(brushNames) / sizeof(brushNames[0]) == static_cast<int>(sculpt::BrushType::Count),
                      "Every brush needs a name in the panel.");
        for (int id : brushNames)
            SendDlgItemMessage(hwnd, IDC_BRUSH_COMBO, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(GetString(id)));

        size_ = SetupSpinner(IDC_SIZE_SPIN, IDC_SIZE_EDIT, EDITTYPE_FLOAT, SculptSettings::kMinSize,
                             SculptSettings::kMaxSize, 1.0f);
        strength_ = SetupSpinner(IDC_STRENGTH_SPIN, IDC_STRENGTH_EDIT, EDITTYPE_FLOAT, 0.0f, 1.0f, 0.01f);
        spacing_ = SetupSpinner(IDC_SPACING_SPIN, IDC_SPACING_EDIT, EDITTYPE_FLOAT, SculptSettings::kMinSpacing,
                                SculptSettings::kMaxSpacing, 0.01f);
        SetDlgItemText(hwnd, IDC_HELP_TEXT,
                       _T("LMB drag: sculpt\nShift: smooth   Alt: invert\nRMB / Esc: cancel stroke"));
        Refresh();
    }

    ISpinnerControl* SetupSpinner(int spinId, int editId, EditSpinnerType type, float lo, float hi, float scale) {
        ISpinnerControl* spin = GetISpinner(GetDlgItem(hwnd_, spinId));
        if (!spin) return nullptr;
        spin->LinkToEdit(GetDlgItem(hwnd_, editId), type);
        spin->SetLimits(lo, hi, FALSE);
        spin->SetScale(scale);
        return spin;
    }

    void Release() {
        if (sculptButton_) ReleaseICustButton(sculptButton_);
        if (size_) ReleaseISpinner(size_);
        if (strength_) ReleaseISpinner(strength_);
        if (spacing_) ReleaseISpinner(spacing_);
        sculptButton_ = nullptr;
        size_ = strength_ = spacing_ = nullptr;
        hwnd_ = nullptr;
    }

    void OnSpinner(int id) {
        if (updating_) return;
        SculptSettings& s = SculptSettings::Get();
        switch (id) {
            case IDC_SIZE_SPIN:
                if (size_) s.SetSize(size_->GetFVal());
                break;
            case IDC_STRENGTH_SPIN:
                if (strength_) s.SetStrength(strength_->GetFVal());
                break;
            case IDC_SPACING_SPIN:
                if (spacing_) s.SetSpacing(spacing_->GetFVal());
                break;
            default:
                break;
        }
    }

    void OnCommand(int id, int code) {
        if (updating_) return;
        SculptSettings& s = SculptSettings::Get();
        switch (id) {
            case IDC_SCULPT_BUTTON:
                if (ip_ && object_) SculptMode::Get().Toggle(ip_, object_);
                Refresh();
                break;
            case IDC_BRUSH_COMBO:
                if (code == CBN_SELCHANGE) {
                    const LRESULT sel = SendDlgItemMessage(hwnd_, IDC_BRUSH_COMBO, CB_GETCURSEL, 0, 0);
                    if (sel >= 0 && sel < static_cast<LRESULT>(sculpt::BrushType::Count))
                        s.SetBrush(static_cast<sculpt::BrushType>(sel));
                }
                break;
            case IDC_ADD_RADIO:
            case IDC_SUB_RADIO:
                s.SetSubtract(IsDlgButtonChecked(hwnd_, IDC_SUB_RADIO) == BST_CHECKED);
                break;
            case IDC_FALLOFF_CHECK:
                s.SetUseFalloff(IsDlgButtonChecked(hwnd_, IDC_FALLOFF_CHECK) == BST_CHECKED);
                break;
            case IDC_BACKFACE_CHECK:
                s.SetBackfaceCull(IsDlgButtonChecked(hwnd_, IDC_BACKFACE_CHECK) == BST_CHECKED);
                break;
            default:
                break;
        }
    }

    IObjParam* ip_ = nullptr;
    SculptMeshObject* object_ = nullptr;
    HWND hwnd_ = nullptr;
    ICustButton* sculptButton_ = nullptr;
    ISpinnerControl* size_ = nullptr;
    ISpinnerControl* strength_ = nullptr;
    ISpinnerControl* spacing_ = nullptr;
    bool updating_ = false;
};

}  // namespace

namespace SculptPanel {

void Open(IObjParam* ip, SculptMeshObject* object) {
    if (ip && object) Panel::Get().Open(ip, object);
}

void Close(IObjParam* /*ip*/) { Panel::Get().Close(); }

void Refresh() { Panel::Get().Refresh(); }

}  // namespace SculptPanel
