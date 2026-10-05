#include "PaintImageIO.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include <bitmap.h>
#include <commdlg.h>

namespace PaintImageIO {

namespace {

const wchar_t kImageFilter[] =
    L"Images (*.png;*.jpg;*.jpeg;*.tif;*.tiff;*.bmp;*.tga;*.psd;*.exr;*.hdr)\0"
    L"*.png;*.jpg;*.jpeg;*.tif;*.tiff;*.bmp;*.tga;*.psd;*.exr;*.hdr\0All files (*.*)\0*.*\0";

HWND Owner() {
    Interface* core = GetCOREInterface();
    return core ? core->GetMAXHWnd() : nullptr;
}

}  // namespace

bool Load(const std::wstring& path, sculpt::Image& out, MSTR* error) {
    if (!TheManager || path.empty()) return false;
    BitmapInfo info;
    info.SetName(path.c_str());
    const BOOL wasSilent = TheManager->SetSilentMode(TRUE);
    BMMRES status = BMMRES_SUCCESS;
    Bitmap* bitmap = TheManager->Load(&info, &status);
    TheManager->SetSilentMode(wasSilent);
    if (!bitmap) {
        if (error) error->printf(_T("Cannot read the image %s"), path.c_str());
        return false;
    }
    const int w = bitmap->Width(), h = bitmap->Height();
    if (status != BMMRES_SUCCESS || w <= 0 || h <= 0 || w > 16384 || h > 16384) {
        bitmap->DeleteThis();
        if (error) error->printf(_T("Cannot read the image %s"), path.c_str());
        return false;
    }
    const bool hasAlpha = bitmap->HasAlpha() != 0;
    out.resize(w, h, 0, 0, 0, 255);
    std::vector<BMM_Color_fl> line(static_cast<std::size_t>(w));
    auto byte = [](float v) {
        // Linear HDR values are clamped; 8-bit sources come back in 0..1.
        return static_cast<std::uint8_t>(std::lround(std::min(std::max(std::isfinite(v) ? v : 0.0f, 0.0f), 1.0f) * 255.0f));
    };
    for (int y = 0; y < h; ++y) {
        if (!bitmap->GetPixels(0, y, w, line.data())) continue;
        for (int x = 0; x < w; ++x) {
            const BMM_Color_fl& c = line[static_cast<std::size_t>(x)];
            std::uint8_t* p = out.pixel(x, y);
            p[0] = byte(c.r);
            p[1] = byte(c.g);
            p[2] = byte(c.b);
            p[3] = hasAlpha ? byte(c.a) : 255;
        }
    }
    bitmap->DeleteThis();
    return true;
}

bool Save(const std::wstring& path, const sculpt::Image& image, MSTR* error) {
    if (!TheManager || path.empty() || image.empty()) return false;
    BitmapInfo info;
    info.SetName(path.c_str());
    info.SetWidth(static_cast<WORD>(image.width));
    info.SetHeight(static_cast<WORD>(image.height));
    info.SetType(BMM_TRUE_32);
    info.SetFlags(MAP_HAS_ALPHA);
    Bitmap* bitmap = TheManager->Create(&info);
    if (!bitmap) {
        if (error) *error = _T("Cannot create the image");
        return false;
    }
    std::vector<BMM_Color_fl> line(static_cast<std::size_t>(image.width));
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            const std::uint8_t* p = image.pixel(x, y);
            line[static_cast<std::size_t>(x)] = BMM_Color_fl(p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, p[3] / 255.0f);
        }
        bitmap->PutPixels(0, y, image.width, line.data());
    }
    const BOOL wasSilent = TheManager->SetSilentMode(TRUE);
    bool ok = bitmap->OpenOutput(&info) == BMMRES_SUCCESS;
    ok = ok && bitmap->Write(&info) == BMMRES_SUCCESS;
    bitmap->Close(&info);
    TheManager->SetSilentMode(wasSilent);
    bitmap->DeleteThis();
    if (!ok && error) error->printf(_T("Cannot write the image %s"), path.c_str());
    return ok;
}

std::wstring AskOpenImage(const wchar_t* title) {
    wchar_t file[MAX_PATH] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = Owner();
    ofn.lpstrFilter = kImageFilter;
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&ofn) ? std::wstring(file) : std::wstring();
}

std::wstring AskSaveImage(const wchar_t* title, const std::wstring& suggested) {
    wchar_t file[MAX_PATH] = {};
    lstrcpynW(file, suggested.c_str(), MAX_PATH);
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = Owner();
    ofn.lpstrFilter = L"PNG (*.png)\0*.png\0TIFF (*.tif)\0*.tif\0Targa (*.tga)\0*.tga\0JPEG (*.jpg)\0*.jpg\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = title;
    ofn.lpstrDefExt = L"png";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetSaveFileNameW(&ofn) ? std::wstring(file) : std::wstring();
}

}  // namespace PaintImageIO
