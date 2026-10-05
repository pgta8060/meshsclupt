// Brush alphas: the four built-in shapes plus an image library on disk.
// The library root's sub-folders are categories; images directly in the
// root form their own category. Images are converted to grayscale height
// stamps (luminance x alpha) and kept small, since a brush dab never needs
// more than a few hundred samples across.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "sculpt/alpha.h"

class AlphaLibrary {
public:
    static constexpr std::size_t kMaxPerCategory = 400;
    static constexpr int kMaxAlphaSize = 256;

    static AlphaLibrary& Get();

    struct Category {
        std::string id;     // "builtin", "favorites", "." (root) or a sub-folder name (UTF-8).
        std::wstring name;  // Shown in the UI.
    };
    // Built-in, Favorites, then the library root and its sub-folders.
    std::vector<Category> Categories();
    // Image paths (UTF-8) of a category, sorted by name, at most kMaxPerCategory.
    // "builtin" returns the built-in ids ("builtin:0".."builtin:3").
    const std::vector<std::string>& Items(const std::string& categoryId);

    // The alpha for an id ("builtin:N" or an image path); nullptr if unreadable.
    std::shared_ptr<const sculpt::Alpha> Load(const std::string& id);
    // The alpha selected in the settings (cached); nullptr for "No Alpha".
    std::shared_ptr<const sculpt::Alpha> Active();

    // size*size gray thumbnail (0..255), loaded on first use and cached.
    // Returns nullptr if the image cannot be read.
    const std::vector<unsigned char>* Thumbnail(const std::string& id, int size);

    // Forget the folder listing (after the library folder changed).
    void Rescan();

    // Any image file as grayscale (luminance x alpha), at most maxSize pixels across.
    static std::shared_ptr<const sculpt::Alpha> LoadGrayImage(const std::string& path, int maxSize);

    static bool IsBuiltin(const std::string& id);
    static std::wstring DisplayName(const std::string& id);
    static bool IsImageFile(const std::wstring& path);

private:
    AlphaLibrary() = default;
    std::shared_ptr<const sculpt::Alpha> LoadImageFile(const std::string& path) const;

    std::map<std::string, std::vector<std::string>> listing_;  // categoryId -> items
    std::string listedFolder_;
    std::vector<Category> categories_;
    std::string categoriesFolder_;
    bool categoriesValid_ = false;
    std::map<std::string, std::vector<unsigned char>> thumbnails_;
    std::map<std::string, bool> unreadable_;
    std::string activeId_;
    std::shared_ptr<const sculpt::Alpha> active_;
};
