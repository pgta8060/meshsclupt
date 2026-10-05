#include "SculptSettings.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace {

using sculpt::BrushType;

constexpr int kPropCount = static_cast<int>(Prop::Count);
constexpr int kBrushCount = static_cast<int>(BrushType::Count);
constexpr int kBrushPropCount = static_cast<int>(BrushProp::Count);

// key, min, max, default, bool, int — order must match enum Prop.
const PropInfo kProps[] = {
    {"toolMode", 0, 2, 1, false, true},
    {"brush", 0, static_cast<float>(kBrushCount - 1), 0, false, true},
    {"maskTool", 0, 2, 0, false, true},
    {"maskDirect", 0, 1, 0, true, false},
    {"strokeMode", 0, 4, 0, false, true},
    {"brushSize", 1, 2000, 50, false, false},
    {"strokeSpacing", 0.02f, 4, 0.15f, false, false},
    {"useFalloff", 0, 1, 1, true, false},
    {"followPath", 0, 1, 1, true, false},
    {"lazyMouse", 0, 1, 0, true, false},
    {"lazyAmount", 0, 1, 0.25f, false, false},
    {"useAlpha", 0, 1, 1, true, false},
    {"alphaMid", 0, 1, 0, false, false},
    {"alphaFade", 0, 1, 0, false, false},
    {"clayBorder", 0, 1, 0.5f, false, false},
    {"polishHardness", 0, 1, 0.5f, false, false},
    {"accuCurve", 0, 1, 0, false, false},
    {"scrapeOriginalPlane", 0, 1, 0, true, false},
    {"scrapeOriginalNormal", 0, 1, 0, true, false},
    {"scatterDensity", 1, 32, 3, false, true},
    {"scatterRadius", 0, 4, 1, false, false},
    {"sizeJitter", 0, 1, 0.3f, false, false},
    {"amountJitter", 0, 1, 0.3f, false, false},
    {"mirrorX", 0, 1, 0, true, false},
    {"mirrorY", 0, 1, 0, true, false},
    {"mirrorZ", 0, 1, 0, true, false},
    {"radialMirror", 0, 1, 0, true, false},
    {"radialCount", 2, 32, 6, false, true},
    {"radialAxis", 0, 2, 2, false, true},
    {"cavityCoverage", 0, 1, 0.55f, false, false},
    {"aoCoverage", 0, 1, 0.55f, false, false},
    {"showMask", 0, 1, 1, true, false},
    {"showGroups", 0, 1, 0, true, false},
    {"autoGroupMode", 0, 5, 5, false, true},
    {"sculptMaterialPreview", 0, 1, 1, true, false},
    {"colorAR", 0, 1, 1, false, false},
    {"colorAG", 0, 1, 1, false, false},
    {"colorAB", 0, 1, 1, false, false},
    {"colorBR", 0, 1, 0, false, false},
    {"colorBG", 0, 1, 0, false, false},
    {"colorBB", 0, 1, 0, false, false},
    {"menusOpen", 0, 1, 1, true, false},
};
static_assert(sizeof(kProps) / sizeof(kProps[0]) == static_cast<std::size_t>(kPropCount), "PropInfo table out of sync");

const char* const kBrushPropKeys[] = {"strength", "subtract", "backfaceCull", "layerMode"};
static_assert(sizeof(kBrushPropKeys) / sizeof(kBrushPropKeys[0]) == static_cast<std::size_t>(kBrushPropCount),
              "Brush property keys out of sync");

bool EqualsNoCase(const std::string& a, const char* b) {
    std::size_t i = 0;
    for (; i < a.size() && b[i]; ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return i == a.size() && b[i] == '\0';
}

float Sanitize(const PropInfo& info, float value, float fallback) {
    if (!std::isfinite(value)) return fallback;
    value = std::min(std::max(value, info.minValue), info.maxValue);
    if (info.isBool) return value != 0.0f ? 1.0f : 0.0f;
    if (info.isInt) return std::round(value);
    return value;
}

bool InPalette(BrushType b) {
    return b != BrushType::MaskPaint && static_cast<int>(b) >= 0 && b < BrushType::Count && sculpt::brushInfo(b).available;
}

std::string Trim(const std::string& s) {
    const auto begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return std::string();
    const auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

bool FindBrushByScriptName(const std::string& name, BrushType& out) {
    for (int b = 0; b < kBrushCount; ++b) {
        if (EqualsNoCase(name, sculpt::brushInfo(static_cast<BrushType>(b)).scriptName)) {
            out = static_cast<BrushType>(b);
            return true;
        }
    }
    return false;
}

}  // namespace

const PropInfo& propInfo(Prop prop) {
    const int i = static_cast<int>(prop);
    return kProps[(i >= 0 && i < kPropCount) ? i : 0];
}

const char* brushPropKey(BrushProp prop) {
    const int i = static_cast<int>(prop);
    return kBrushPropKeys[(i >= 0 && i < kBrushPropCount) ? i : 0];
}

bool findProp(const std::string& key, Prop& out) {
    for (int i = 0; i < kPropCount; ++i) {
        if (EqualsNoCase(key, kProps[i].key)) {
            out = static_cast<Prop>(i);
            return true;
        }
    }
    return false;
}

bool findBrushProp(const std::string& key, BrushProp& out) {
    for (int i = 0; i < kBrushPropCount; ++i) {
        if (EqualsNoCase(key, kBrushPropKeys[i])) {
            out = static_cast<BrushProp>(i);
            return true;
        }
    }
    return false;
}

std::vector<BrushType> DefaultPaletteOrder() {
    // Documentation order; brushes of later phases appear once available.
    const BrushType order[] = {BrushType::Sculpt,    BrushType::Clay,       BrushType::ClayBuildup,
                               BrushType::Carve,     BrushType::Knife,      BrushType::Contrast,
                               BrushType::Scrape,    BrushType::Density,    BrushType::Revert,
                               BrushType::FaceGroups, BrushType::Clip,      BrushType::Cutter,
                               BrushType::Slice,     BrushType::SnakeHook,  BrushType::Cloth,
                               BrushType::Pose,      BrushType::CurveTube,  BrushType::Move,
                               BrushType::Polish,    BrushType::Pinch,      BrushType::Inflate,
                               BrushType::Displace,  BrushType::Smooth,     BrushType::SmoothGroupBorder};
    std::vector<BrushType> out;
    for (BrushType b : order)
        if (InPalette(b)) out.push_back(b);
    return out;
}

SculptSettings& SculptSettings::Get() {
    static SculptSettings settings;
    return settings;
}

SculptSettings::SculptSettings() { ResetToDefaults(); }

void SculptSettings::ResetToDefaults() {
    for (int i = 0; i < kPropCount; ++i) values_[i] = kProps[i].defaultValue;
    for (int b = 0; b < kBrushCount; ++b) {
        const sculpt::BrushInfo& info = sculpt::brushInfo(static_cast<BrushType>(b));
        brush_[b][static_cast<int>(BrushProp::Strength)] = info.defaultStrength;
        brush_[b][static_cast<int>(BrushProp::Subtract)] = 0.0f;
        brush_[b][static_cast<int>(BrushProp::BackfaceCull)] = info.defaultBackfaceCull ? 1.0f : 0.0f;
        brush_[b][static_cast<int>(BrushProp::LayerMode)] = 0.0f;
    }
    palette_ = DefaultPaletteOrder();
    alphaId_.clear();
    alphaFolder_.clear();
    alphaCategory_ = "builtin";
    alphaFavorites_.clear();
    Changed();
}

int SculptSettings::Int(Prop prop) const { return static_cast<int>(std::lround(Value(prop))); }

void SculptSettings::Set(Prop prop, float value) {
    const int i = static_cast<int>(prop);
    if (i < 0 || i >= kPropCount) return;
    if (prop == Prop::Brush && !InPalette(static_cast<BrushType>(static_cast<int>(std::lround(value))))) return;
    const float v = Sanitize(kProps[i], value, values_[i]);
    if (v == values_[i]) return;
    values_[i] = v;
    Changed();
}

void SculptSettings::SetBrush(BrushType brush) { Set(Prop::Brush, static_cast<float>(static_cast<int>(brush))); }

float SculptSettings::BrushValue(BrushType brush, BrushProp prop) const {
    const int b = static_cast<int>(brush), p = static_cast<int>(prop);
    if (b < 0 || b >= kBrushCount || p < 0 || p >= kBrushPropCount) return 0.0f;
    return brush_[b][p];
}

void SculptSettings::SetBrushValue(BrushType brush, BrushProp prop, float value) {
    const int b = static_cast<int>(brush), p = static_cast<int>(prop);
    if (b < 0 || b >= kBrushCount || p < 0 || p >= kBrushPropCount || !std::isfinite(value)) return;
    const bool isBool = prop != BrushProp::Strength;
    const float v = isBool ? (value != 0.0f ? 1.0f : 0.0f) : std::min(std::max(value, 0.0f), 1.0f);
    if (v == brush_[b][p]) return;
    brush_[b][p] = v;
    Changed();
}

sculpt::BrushSettings SculptSettings::MakeBrush(BrushType type) const {
    sculpt::BrushSettings s;
    s.type = type;
    s.strength = BrushValue(type, BrushProp::Strength);
    s.subtract = BrushValue(type, BrushProp::Subtract) != 0.0f;
    s.backfaceCull = BrushValue(type, BrushProp::BackfaceCull) != 0.0f;
    s.layerMode = sculpt::brushInfo(type).supportsLayerMode && BrushValue(type, BrushProp::LayerMode) != 0.0f;
    s.useFalloff = type == BrushType::FaceGroups ? false : Bool(Prop::UseFalloff);
    s.clayBorder = Value(Prop::ClayBorder);
    s.polishHardness = Value(Prop::PolishHardness);
    s.accuCurve = Value(Prop::AccuCurve);
    s.scrapeOriginalPlane = Bool(Prop::ScrapeOriginalPlane);
    s.scrapeOriginalNormal = Bool(Prop::ScrapeOriginalNormal);
    s.alphaMid = Value(Prop::AlphaMid);
    s.alphaFade = Value(Prop::AlphaFade);
    return s;
}

void SculptSettings::SetPaletteOrder(std::vector<BrushType> order) {
    std::vector<BrushType> clean;
    for (BrushType b : order)
        if (InPalette(b) && std::find(clean.begin(), clean.end(), b) == clean.end()) clean.push_back(b);
    for (BrushType b : DefaultPaletteOrder())  // Keep every available brush reachable.
        if (std::find(clean.begin(), clean.end(), b) == clean.end()) clean.push_back(b);
    if (clean == palette_) return;
    palette_ = std::move(clean);
    Changed();
}

void SculptSettings::MovePaletteItem(int from, int to) {
    const int n = static_cast<int>(palette_.size());
    if (from < 0 || from >= n || to < 0 || to >= n || from == to) return;
    std::vector<BrushType> order = palette_;
    const BrushType item = order[static_cast<std::size_t>(from)];
    order.erase(order.begin() + from);
    order.insert(order.begin() + to, item);
    SetPaletteOrder(std::move(order));
}

void SculptSettings::SetAlphaId(const std::string& id) {
    if (id == alphaId_) return;
    alphaId_ = id;
    Changed();
}

void SculptSettings::SetAlphaLibraryFolder(const std::string& folder) {
    if (folder == alphaFolder_) return;
    alphaFolder_ = folder;
    Changed();
}

void SculptSettings::SetAlphaCategory(const std::string& category) {
    const std::string value = category.empty() ? std::string("builtin") : category;
    if (value == alphaCategory_) return;
    alphaCategory_ = value;
    Changed();
}

bool SculptSettings::IsAlphaFavorite(const std::string& path) const {
    return std::find(alphaFavorites_.begin(), alphaFavorites_.end(), path) != alphaFavorites_.end();
}

void SculptSettings::ToggleAlphaFavorite(const std::string& path) {
    auto it = std::find(alphaFavorites_.begin(), alphaFavorites_.end(), path);
    if (it == alphaFavorites_.end())
        alphaFavorites_.push_back(path);
    else
        alphaFavorites_.erase(it);
    Changed();
}

std::string SculptSettings::ToText() const {
    std::ostringstream os;
    os << "# Sculpt Mesh settings\n";
    for (int i = 0; i < kPropCount; ++i) os << kProps[i].key << '=' << values_[i] << '\n';
    for (int b = 0; b < kBrushCount; ++b)
        for (int p = 0; p < kBrushPropCount; ++p)
            os << "brush." << sculpt::brushInfo(static_cast<BrushType>(b)).scriptName << '.' << kBrushPropKeys[p] << '='
               << brush_[b][p] << '\n';
    os << "palette=";
    for (std::size_t i = 0; i < palette_.size(); ++i) os << (i ? "," : "") << sculpt::brushInfo(palette_[i]).scriptName;
    os << '\n';
    os << "alpha=" << alphaId_ << '\n';
    os << "alphaFolder=" << alphaFolder_ << '\n';
    os << "alphaCategory=" << alphaCategory_ << '\n';
    for (const std::string& fav : alphaFavorites_) os << "alphaFavorite=" << fav << '\n';
    return os.str();
}

void SculptSettings::FromText(const std::string& text) {
    const bool wasNotifying = notifying_;
    notifying_ = true;  // One notification at the end, not one per line.
    std::istringstream is(text);
    std::string line;
    std::vector<std::string> favorites;
    bool sawFavorites = false;
    while (std::getline(is, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == '#') continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = Trim(line.substr(0, eq));
        const std::string value = Trim(line.substr(eq + 1));

        Prop prop;
        if (findProp(key, prop)) {
            char* end = nullptr;
            const float v = std::strtof(value.c_str(), &end);
            if (end != value.c_str()) Set(prop, v);
            continue;
        }
        if (key.rfind("brush.", 0) == 0) {
            const auto dot = key.find('.', 6);
            if (dot == std::string::npos) continue;
            BrushType brush;
            BrushProp bp;
            if (!FindBrushByScriptName(key.substr(6, dot - 6), brush) || !findBrushProp(key.substr(dot + 1), bp)) continue;
            char* end = nullptr;
            const float v = std::strtof(value.c_str(), &end);
            if (end != value.c_str()) SetBrushValue(brush, bp, v);
            continue;
        }
        if (key == "palette") {
            std::vector<BrushType> order;
            std::istringstream items(value);
            std::string item;
            while (std::getline(items, item, ',')) {
                BrushType b;
                if (FindBrushByScriptName(Trim(item), b)) order.push_back(b);
            }
            SetPaletteOrder(order);
        } else if (key == "alpha") {
            SetAlphaId(value);
        } else if (key == "alphaFolder") {
            SetAlphaLibraryFolder(value);
        } else if (key == "alphaCategory") {
            SetAlphaCategory(value);
        } else if (key == "alphaFavorite") {
            sawFavorites = true;
            if (!value.empty() && std::find(favorites.begin(), favorites.end(), value) == favorites.end())
                favorites.push_back(value);
        }
    }
    if (sawFavorites) alphaFavorites_ = favorites;
    notifying_ = wasNotifying;
    Changed();
}

void SculptSettings::AddListener(Listener* listener) {
    if (listener && std::find(listeners_.begin(), listeners_.end(), listener) == listeners_.end())
        listeners_.push_back(listener);
}

void SculptSettings::RemoveListener(Listener* listener) {
    listeners_.erase(std::remove(listeners_.begin(), listeners_.end(), listener), listeners_.end());
}

void SculptSettings::Changed() {
    if (notifying_) return;  // A listener writing settings back must not recurse.
    notifying_ = true;
    const std::vector<Listener*> snapshot = listeners_;  // Listeners may unregister.
    for (Listener* l : snapshot)
        if (std::find(listeners_.begin(), listeners_.end(), l) != listeners_.end()) l->OnSculptSettingsChanged();
    notifying_ = false;
}
