#include "AlphaLibrary.h"

#include <algorithm>
#include <cmath>
#include <cwctype>

#include <max.h>
#include <bitmap.h>

#include "SculptSettings.h"

namespace {

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

bool LessNoCase(const std::wstring& a, const std::wstring& b) {
    return _wcsicmp(a.c_str(), b.c_str()) < 0;
}

// Lists the image files (or sub-folders) of a folder, sorted.
std::vector<std::wstring> ListFolder(const std::wstring& folder, bool folders) {
    std::vector<std::wstring> out;
    if (folder.empty()) return out;
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileW((folder + L"\\*").c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return out;
    do {
        const std::wstring name = data.cFileName;
        if (name == L"." || name == L"..") continue;
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) != 0) continue;
        const bool isFolder = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (folders != isFolder) continue;
        if (!folders && !AlphaLibrary::IsImageFile(name)) continue;
        out.push_back(name);
    } while (FindNextFileW(find, &data));
    FindClose(find);
    std::sort(out.begin(), out.end(), LessNoCase);
    return out;
}

// Box-filters a gray image into dstW x dstH.
std::vector<float> Resample(const std::vector<float>& src, int w, int h, int dstW, int dstH) {
    std::vector<float> out(static_cast<std::size_t>(dstW) * dstH, 0.0f);
    for (int y = 0; y < dstH; ++y) {
        const int y0 = y * h / dstH, y1 = std::max(y0 + 1, (y + 1) * h / dstH);
        for (int x = 0; x < dstW; ++x) {
            const int x0 = x * w / dstW, x1 = std::max(x0 + 1, (x + 1) * w / dstW);
            float sum = 0.0f;
            for (int sy = y0; sy < y1; ++sy)
                for (int sx = x0; sx < x1; ++sx) sum += src[static_cast<std::size_t>(sy) * w + sx];
            out[static_cast<std::size_t>(y) * dstW + x] = sum / static_cast<float>((y1 - y0) * (x1 - x0));
        }
    }
    return out;
}

// Reads an image as grayscale (luminance x alpha), at most maxSize pixels across.
bool ReadGrayImage(const std::wstring& path, int maxSize, std::vector<float>& values, int& width, int& height) {
    if (!TheManager) return false;
    BitmapInfo info;
    info.SetName(path.c_str());
    const BOOL wasSilent = TheManager->SetSilentMode(TRUE);  // No error dialogs for broken files.
    BMMRES status = BMMRES_SUCCESS;
    Bitmap* bitmap = TheManager->Load(&info, &status);
    TheManager->SetSilentMode(wasSilent);
    if (!bitmap) return false;
    const int w = bitmap->Width(), h = bitmap->Height();
    if (status != BMMRES_SUCCESS || w <= 0 || h <= 0 || w > 32768 || h > 32768) {
        bitmap->DeleteThis();
        return false;
    }
    const bool hasAlpha = bitmap->HasAlpha() != 0;
    std::vector<BMM_Color_fl> line(static_cast<std::size_t>(w));
    std::vector<float> gray(static_cast<std::size_t>(w) * h, 0.0f);
    for (int y = 0; y < h; ++y) {
        if (!bitmap->GetPixels(0, y, w, line.data())) continue;
        for (int x = 0; x < w; ++x) {
            const BMM_Color_fl& c = line[static_cast<std::size_t>(x)];
            float lum = 0.299f * c.r + 0.587f * c.g + 0.114f * c.b;
            if (hasAlpha) lum *= std::min(std::max(c.a, 0.0f), 1.0f);
            gray[static_cast<std::size_t>(y) * w + x] = std::isfinite(lum) ? std::min(std::max(lum, 0.0f), 1.0f) : 0.0f;
        }
    }
    bitmap->DeleteThis();

    const int longest = std::max(w, h);
    if (longest > maxSize) {
        const int dw = std::max(1, w * maxSize / longest), dh = std::max(1, h * maxSize / longest);
        values = Resample(gray, w, h, dw, dh);
        width = dw;
        height = dh;
    } else {
        values = std::move(gray);
        width = w;
        height = h;
    }
    return true;
}

}  // namespace

AlphaLibrary& AlphaLibrary::Get() {
    static AlphaLibrary library;
    return library;
}

bool AlphaLibrary::IsBuiltin(const std::string& id) { return id.rfind("builtin:", 0) == 0; }

bool AlphaLibrary::IsImageFile(const std::wstring& path) {
    const std::size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    std::wstring ext = path.substr(dot + 1);
    for (wchar_t& c : ext) c = static_cast<wchar_t>(std::towlower(c));
    static const wchar_t* const kExtensions[] = {L"png", L"jpg", L"jpeg", L"tif", L"tiff", L"bmp", L"tga", L"psd", L"exr"};
    for (const wchar_t* e : kExtensions)
        if (ext == e) return true;
    return false;
}

std::wstring AlphaLibrary::DisplayName(const std::string& id) {
    if (id.empty()) return L"No Alpha";
    if (IsBuiltin(id)) return L"Alpha " + std::to_wstring(std::atoi(id.c_str() + 8) + 1);
    std::wstring name = Widen(id);
    const std::size_t slash = name.find_last_of(L"\\/");
    if (slash != std::wstring::npos) name = name.substr(slash + 1);
    const std::size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0) name = name.substr(0, dot);
    return name;
}

void AlphaLibrary::Rescan() {
    listing_.clear();
    listedFolder_.clear();
    unreadable_.clear();
    categoriesValid_ = false;
}

std::vector<AlphaLibrary::Category> AlphaLibrary::Categories() {
    // Cached: the palette asks on every repaint.
    const std::string& folder = SculptSettings::Get().AlphaLibraryFolder();
    if (categoriesValid_ && folder == categoriesFolder_) return categories_;
    categoriesFolder_ = folder;
    categoriesValid_ = true;
    std::vector<Category>& out = categories_;
    out = {{"builtin", L"Built-in"}, {"favorites", L"Favorites"}};
    const std::wstring root = Widen(folder);
    if (root.empty()) return out;
    if (!ListFolder(root, false).empty()) {
        std::wstring rootName = root;
        while (!rootName.empty() && (rootName.back() == L'\\' || rootName.back() == L'/')) rootName.pop_back();
        const std::size_t slash = rootName.find_last_of(L"\\/");
        out.push_back({".", slash == std::wstring::npos ? rootName : rootName.substr(slash + 1)});
    }
    for (const std::wstring& folder : ListFolder(root, true)) out.push_back({Narrow(folder), folder});
    return out;
}

const std::vector<std::string>& AlphaLibrary::Items(const std::string& categoryId) {
    const std::string& folder = SculptSettings::Get().AlphaLibraryFolder();
    if (folder != listedFolder_) {
        listing_.clear();
        listedFolder_ = folder;
    }
    if (categoryId == "favorites") {
        // Favorites change often and are cheap: never cached.
        std::vector<std::string>& favorites = listing_["favorites"];
        favorites = SculptSettings::Get().AlphaFavorites();
        if (favorites.size() > kMaxPerCategory) favorites.resize(kMaxPerCategory);
        return favorites;
    }
    auto it = listing_.find(categoryId);
    if (it != listing_.end()) return it->second;

    std::vector<std::string> items;
    if (categoryId == "builtin") {
        for (int i = 0; i < sculpt::Alpha::kBuiltinCount; ++i) items.push_back("builtin:" + std::to_string(i));
    } else if (!folder.empty()) {
        std::wstring dir = Widen(folder);
        if (categoryId != ".") dir += L"\\" + Widen(categoryId);
        for (const std::wstring& file : ListFolder(dir, false)) {
            if (items.size() >= kMaxPerCategory) break;
            items.push_back(Narrow(dir + L"\\" + file));
        }
    }
    return listing_.emplace(categoryId, std::move(items)).first->second;
}

std::shared_ptr<const sculpt::Alpha> AlphaLibrary::LoadImageFile(const std::string& path) const {
    std::vector<float> values;
    int w = 0, h = 0;
    if (!ReadGrayImage(Widen(path), kMaxAlphaSize, values, w, h)) return nullptr;
    return std::make_shared<const sculpt::Alpha>(w, h, std::move(values));
}

std::shared_ptr<const sculpt::Alpha> AlphaLibrary::Load(const std::string& id) {
    if (id.empty()) return nullptr;
    if (IsBuiltin(id)) {
        const int index = std::atoi(id.c_str() + 8);
        if (index < 0 || index >= sculpt::Alpha::kBuiltinCount) return nullptr;
        return std::make_shared<const sculpt::Alpha>(sculpt::Alpha::builtin(index));
    }
    return LoadImageFile(id);
}

std::shared_ptr<const sculpt::Alpha> AlphaLibrary::Active() {
    const std::string& id = SculptSettings::Get().AlphaId();
    if (id != activeId_) {
        activeId_ = id;
        active_ = Load(id);
    }
    return active_;
}

const std::vector<unsigned char>* AlphaLibrary::Thumbnail(const std::string& id, int size) {
    if (id.empty() || size <= 0) return nullptr;
    const std::string key = id + "#" + std::to_string(size);
    auto it = thumbnails_.find(key);
    if (it != thumbnails_.end()) return &it->second;
    if (unreadable_.count(key)) return nullptr;

    std::vector<float> values;
    int w = 0, h = 0;
    if (IsBuiltin(id)) {
        const int index = std::atoi(id.c_str() + 8);
        if (index < 0 || index >= sculpt::Alpha::kBuiltinCount) return nullptr;
        const sculpt::Alpha alpha = sculpt::Alpha::builtin(index, size);
        values = alpha.values();
        w = alpha.width();
        h = alpha.height();
    } else if (!ReadGrayImage(Widen(id), size, values, w, h)) {
        unreadable_[key] = true;
        return nullptr;
    }
    // Centre the image in a square thumbnail.
    std::vector<unsigned char> thumb(static_cast<std::size_t>(size) * size, 0);
    const int ox = (size - w) / 2, oy = (size - h) / 2;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const int tx = x + ox, ty = y + oy;
            if (tx < 0 || ty < 0 || tx >= size || ty >= size) continue;
            thumb[static_cast<std::size_t>(ty) * size + tx] =
                static_cast<unsigned char>(std::lround(values[static_cast<std::size_t>(y) * w + x] * 255.0f));
        }
    if (thumbnails_.size() > 4096) thumbnails_.clear();  // Bound memory for huge libraries.
    return &thumbnails_.emplace(key, std::move(thumb)).first->second;
}
