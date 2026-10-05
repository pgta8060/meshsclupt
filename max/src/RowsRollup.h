// A Modify-panel rollout whose content is a ui::RowList drawn by a child
// window (same look as the floating menus). The page height follows the
// visible rows.
#pragma once

#include <functional>
#include <memory>

#include "SculptMeshPlugin.h"
#include "SculptUiKit.h"

class RowsRollup {
public:
    using Builder = std::function<void(ui::RowList&)>;

    RowsRollup();
    ~RowsRollup();

    void Open(IObjParam* ip, const MCHAR* title, Builder build, bool rolledUp = false);
    void Close();
    bool IsOpen() const { return dialog_ != nullptr; }
    // Repaints and fits the page to the visible rows.
    void Refresh();

private:
    class Panel;
    static INT_PTR CALLBACK DlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void Init(HWND dialog);
    void Layout();

    IObjParam* ip_ = nullptr;
    HWND dialog_ = nullptr;
    std::unique_ptr<Panel> panel_;
    Builder build_;
    int height_ = 0;
};
