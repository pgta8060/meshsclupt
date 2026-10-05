// The Profile rollout's curve editor: a RowList custom row editing the
// profile curve stored in the settings (sculpt::ProfileCurve text).
//   LMB empty graph: add a point.      LMB drag point / tangent: move it.
//   Alt + LMB point: delete it.         RMB point: Bezier, Bezier Corner, Smooth, Linear.
//   RMB empty graph: Reset View / Reset Profile.
//   Double-click empty graph: reset the view.
#pragma once

#include "SculptUiKit.h"
#include "sculpt/deform.h"

class ProfileEditor {
public:
    ui::RowList::CustomSpec Spec();

private:
    enum class Part { None, Point, In, Out };
    struct Graph {
        RECT area;
        float yMax;
    };
    Graph GraphOf(const RECT& row) const;
    static POINT ToScreen(const Graph& g, float x, float y);
    static void FromScreen(const Graph& g, int sx, int sy, float& x, float& y);
    static sculpt::ProfileCurve Load();
    static void Store(const sculpt::ProfileCurve& curve);
    bool Hit(const Graph& g, const sculpt::ProfileCurve& curve, int x, int y, int& index, Part& part) const;

    void Paint(HDC dc, const RECT& row) const;
    bool MouseDown(HWND host, const RECT& row, int x, int y, bool doubleClick);
    bool MouseDrag(const RECT& row, int x, int y);
    bool RightClick(HWND host, const RECT& row, int x, int y);

    float yMax_ = 1.5f;  // Visible value range is 0..yMax_.
    int dragIndex_ = -1;
    Part dragPart_ = Part::None;
    sculpt::ProfileCurve dragCurve_;
};
