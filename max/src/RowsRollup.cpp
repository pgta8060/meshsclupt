#include "RowsRollup.h"

#include <algorithm>

class RowsRollup::Panel : public ui::FloatingWindow {
public:
    explicit Panel(RowsRollup& owner) : owner_(owner) {}
    ui::RowList rows;

    int ContentHeight(int width) const { return rows.ContentHeight(width - ui::Px(8)) + ui::Px(6); }

protected:
    RECT Area() const {
        const RECT c = ClientRect();
        return RECT{ui::Px(4), ui::Px(3), c.right - ui::Px(4), c.bottom};
    }
    void Paint(HDC dc, const RECT& client) override {
        ui::Fill(dc, client, ui::GetTheme().panel);
        rows.Paint(dc, Area(), 0);
    }
    void MouseDown(int x, int y, bool doubleClick) override {
        if (rows.MouseDown(hwnd_, x, y, doubleClick, 0)) Changed();
    }
    void MouseMove(int x, int y, bool captured) override {
        if (rows.MouseMove(x, y, captured, 0)) Invalidate();
    }
    void MouseUp(int x, int y) override {
        if (rows.MouseUp(x, y, 0)) Changed();
    }
    void RightClick(int x, int y) override {
        if (rows.RightClick(hwnd_, x, y, 0)) Changed();
    }
    void MouseLeave() override {
        if (rows.MouseLeave()) Invalidate();
    }
    void CaptureLost() override { rows.CaptureLost(); }
    LRESULT Message(UINT msg, WPARAM wp, LPARAM lp, bool& handled) override {
        if (msg == ui::RowList::kFinishEditMessage) {
            handled = true;
            rows.FinishEdit(wp != 0);
            Changed();
            return 0;
        }
        if (msg == WM_MOUSEWHEEL) {  // Let the command panel scroll.
            handled = true;
            return SendMessage(GetParent(hwnd_), msg, wp, lp);
        }
        handled = false;
        return 0;
    }

private:
    void Changed() {
        Invalidate();
        owner_.Layout();  // Rows may have appeared or disappeared.
    }
    RowsRollup& owner_;
};

RowsRollup::RowsRollup() = default;
RowsRollup::~RowsRollup() = default;

void RowsRollup::Open(IObjParam* ip, const MCHAR* title, Builder build, bool rolledUp) {
    if (dialog_) Close();
    ip_ = ip;
    build_ = std::move(build);
    ip->AddRollupPage(hInstance, MAKEINTRESOURCE(IDD_ROWS_ROLLUP), DlgProc, title, reinterpret_cast<LPARAM>(this),
                      rolledUp ? APPENDROLL_CLOSED : 0);
}

void RowsRollup::Close() {
    if (ip_ && dialog_) ip_->DeleteRollupPage(dialog_);  // Sends WM_DESTROY.
    dialog_ = nullptr;
    panel_.reset();
    ip_ = nullptr;
}

void RowsRollup::Init(HWND dialog) {
    dialog_ = dialog;
    panel_ = std::make_unique<Panel>(*this);
    if (build_) build_(panel_->rows);
    panel_->CreateChild(dialog);
    height_ = 0;
    Layout();
}

void RowsRollup::Layout() {
    if (!dialog_ || !panel_ || !panel_->Hwnd()) return;
    RECT client;
    GetClientRect(dialog_, &client);
    const int width = std::max<int>(client.right - client.left, ui::Px(120));
    const int height = std::max(panel_->ContentHeight(width), ui::Px(20));
    SetWindowPos(panel_->Hwnd(), nullptr, 0, 0, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    if (height != height_ && ip_) {
        height_ = height;
        if (IRollupWindow* rollup = ip_->GetCommandPanelRollup()) {
            const int index = rollup->GetPanelIndex(dialog_);
            if (index >= 0) rollup->SetPageDlgHeight(index, height);
        }
    }
    panel_->Invalidate();
}

void RowsRollup::Refresh() {
    if (!dialog_) return;
    ui::RefreshTheme();
    Layout();
}

INT_PTR CALLBACK RowsRollup::DlgProc(HWND hwnd, UINT msg, WPARAM /*wParam*/, LPARAM lParam) {
    RowsRollup* self = reinterpret_cast<RowsRollup*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_INITDIALOG:
            SetWindowLongPtr(hwnd, GWLP_USERDATA, lParam);
            self = reinterpret_cast<RowsRollup*>(lParam);
            if (self) self->Init(hwnd);
            return TRUE;
        case WM_SIZE:
            if (self) self->Layout();
            return FALSE;
        case WM_DESTROY:
            if (self) {
                if (self->panel_) self->panel_->Destroy();
                self->dialog_ = nullptr;
            }
            SetWindowLongPtr(hwnd, GWLP_USERDATA, 0);
            return FALSE;
        default:
            return FALSE;
    }
}
