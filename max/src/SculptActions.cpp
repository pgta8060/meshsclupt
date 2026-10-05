#include "SculptActions.h"

#include <actiontable.h>

#include "SculptCommands.h"
#include "SculptMeshPlugin.h"
#include "SculptMode.h"
#include "SculptUI.h"
#include "Stencil.h"

namespace {

// Never change: stored in users' hotkey files.
constexpr ActionTableId kTableId = 0x5C3A71E2;
constexpr ActionContextId kContextId = 0x5C3A71E3;

// Not const: the 3ds Max 2024 SDK takes a non-const pointer.
ActionDescription kActions[] = {
    {ID_SCULPT_SLOT1, IDS_ACT_SLOT1, IDS_ACT_SLOT1, IDS_ACTION_CATEGORY},
    {ID_SCULPT_SLOT2, IDS_ACT_SLOT2, IDS_ACT_SLOT2, IDS_ACTION_CATEGORY},
    {ID_SCULPT_SLOT3, IDS_ACT_SLOT3, IDS_ACT_SLOT3, IDS_ACTION_CATEGORY},
    {ID_SCULPT_SLOT4, IDS_ACT_SLOT4, IDS_ACT_SLOT4, IDS_ACTION_CATEGORY},
    {ID_SCULPT_SLOT5, IDS_ACT_SLOT5, IDS_ACT_SLOT5, IDS_ACTION_CATEGORY},
    {ID_SCULPT_QUICK_MENU, IDS_ACT_QUICK_MENU, IDS_ACT_QUICK_MENU, IDS_ACTION_CATEGORY},
    {ID_SCULPT_MOVE, IDS_ACT_MOVE, IDS_ACT_MOVE, IDS_ACTION_CATEGORY},
    {ID_SCULPT_ROTATE, IDS_ACT_ROTATE, IDS_ACT_ROTATE, IDS_ACTION_CATEGORY},
    {ID_SCULPT_SCALE, IDS_ACT_SCALE, IDS_ACT_SCALE, IDS_ACTION_CATEGORY},
    {ID_SCULPT_GROUP_FROM_MASK, IDS_ACT_GROUP_FROM_MASK, IDS_ACT_GROUP_FROM_MASK, IDS_ACTION_CATEGORY},
    {ID_SCULPT_TOGGLE_MENUS, IDS_ACT_TOGGLE_MENUS, IDS_ACT_TOGGLE_MENUS, IDS_ACTION_CATEGORY},
    {ID_SCULPT_CANCEL, IDS_ACT_CANCEL, IDS_ACT_CANCEL, IDS_ACTION_CATEGORY},
    {ID_SCULPT_STENCIL, IDS_ACT_STENCIL, IDS_ACT_STENCIL, IDS_ACTION_CATEGORY},
};

class Callback : public ActionCallback {
public:
    BOOL ExecuteAction(int id) override {
        switch (id) {
            case ID_SCULPT_SLOT1:
            case ID_SCULPT_SLOT2:
            case ID_SCULPT_SLOT3:
            case ID_SCULPT_SLOT4:
            case ID_SCULPT_SLOT5:
                SculptCommands::SelectPaletteSlot(id - ID_SCULPT_SLOT1);
                return TRUE;
            case ID_SCULPT_QUICK_MENU: {
                if (SculptMode::Get().CutGestureActive()) return TRUE;  // Space pans the cut preview.
                POINT cursor;
                GetCursorPos(&cursor);
                SculptUI::ShowQuickMenu(cursor, true);
                return TRUE;
            }
            case ID_SCULPT_MOVE:
                SculptCommands::Transform(SculptCommands::TransformKind::Move);
                return TRUE;
            case ID_SCULPT_ROTATE:
                SculptCommands::Transform(SculptCommands::TransformKind::Rotate);
                return TRUE;
            case ID_SCULPT_SCALE:
                SculptCommands::Transform(SculptCommands::TransformKind::Scale);
                return TRUE;
            case ID_SCULPT_GROUP_FROM_MASK:
                SculptCommands::Run(SculptCommands::Op::GroupFromMask);
                return TRUE;
            case ID_SCULPT_TOGGLE_MENUS:
                SculptUI::Toggle();
                return TRUE;
            case ID_SCULPT_CANCEL:
                return SculptMode::Get().CancelPending() ? TRUE : FALSE;
            case ID_SCULPT_STENCIL:
                // Held S transforms the stencil (read with GetKeyState by its mouse hook);
                // the key is only taken while a stencil is loaded.
                return Stencil::Get().Loaded() ? TRUE : FALSE;
            default:
                return FALSE;
        }
    }
};

Callback callback;
bool active = false;

}  // namespace

namespace SculptActions {

int TableCount() { return 1; }

ActionTable* Table(int i) {
    if (i != 0) return nullptr;
    static ActionTable* table = nullptr;  // Owned by 3ds Max once registered.
    if (!table) {
        HACCEL defaults = LoadAccelerators(hInstance, MAKEINTRESOURCE(IDR_SCULPT_SHORTCUTS));
        table = new ActionTable(kTableId, kContextId, MSTR(GetString(IDS_ACTION_TABLE)), defaults,
                                static_cast<int>(sizeof(kActions) / sizeof(kActions[0])), kActions, hInstance);
        if (Interface* core = GetCOREInterface())
            core->GetActionManager()->RegisterActionContext(kContextId, GetString(IDS_ACTION_TABLE));
    }
    return table;
}

void Activate() {
    Interface* core = GetCOREInterface();
    if (active || !core) return;
    active = core->GetActionManager()->ActivateActionTable(&callback, kTableId) != FALSE;
}

void Deactivate() {
    Interface* core = GetCOREInterface();
    if (!active || !core) return;
    core->GetActionManager()->DeactivateActionTable(&callback, kTableId);
    active = false;
}

}  // namespace SculptActions
