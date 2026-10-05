// Image files for texture painting (bmm: every format 3ds Max reads/writes),
// plus the file dialogs used by the Material / Paint rollout.
#pragma once

#include <string>

#include <max.h>

#include "sculpt/paint.h"

namespace PaintImageIO {

// Loads any image 3ds Max can read as RGBA8 (row 0 at the top).
bool Load(const std::wstring& path, sculpt::Image& out, MSTR* error = nullptr);
// Writes RGBA8 pixels; the format follows the file extension.
bool Save(const std::wstring& path, const sculpt::Image& image, MSTR* error = nullptr);

// Win32 dialogs owned by the 3ds Max window; return "" when cancelled.
std::wstring AskOpenImage(const wchar_t* title);
std::wstring AskSaveImage(const wchar_t* title, const std::wstring& suggested);

}  // namespace PaintImageIO
