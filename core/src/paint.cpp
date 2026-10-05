#include "sculpt/paint.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "sculpt/parallel.h"

namespace sculpt {

namespace {

constexpr int kTile = PaintCanvas::kTile;

inline float ToUnit(std::uint8_t v) { return static_cast<float>(v) * (1.0f / 255.0f); }
inline std::uint8_t ToByte(float v) { return static_cast<std::uint8_t>(std::lround(clamp01(v) * 255.0f)); }

float BlendChannel(PaintBlend mode, float b, float s) {
    switch (mode) {
        case PaintBlend::Multiply: return b * s;
        case PaintBlend::Screen: return 1.0f - (1.0f - b) * (1.0f - s);
        case PaintBlend::Overlay: return b < 0.5f ? 2.0f * b * s : 1.0f - 2.0f * (1.0f - b) * (1.0f - s);
        case PaintBlend::Add: return std::min(1.0f, b + s);
        case PaintBlend::Subtract: return std::max(0.0f, b - s);
        case PaintBlend::Normal:
        default: return s;
    }
}

// Copies tile `index` of `image` into / out of a 32x32 RGBA buffer.
void ReadTile(const Image& image, std::uint32_t index, std::vector<std::uint8_t>& out) {
    const int tilesX = (image.width + kTile - 1) / kTile;
    const int x0 = static_cast<int>(index % static_cast<std::uint32_t>(tilesX)) * kTile;
    const int y0 = static_cast<int>(index / static_cast<std::uint32_t>(tilesX)) * kTile;
    out.assign(4u * kTile * kTile, 0u);
    for (int y = 0; y < kTile && y0 + y < image.height; ++y) {
        const int w = std::min(kTile, image.width - x0);
        std::copy(image.pixel(x0, y0 + y), image.pixel(x0, y0 + y) + 4 * w, out.begin() + 4 * y * kTile);
    }
}

void WriteTile(Image& image, std::uint32_t index, const std::vector<std::uint8_t>& in) {
    const int tilesX = (image.width + kTile - 1) / kTile;
    const int x0 = static_cast<int>(index % static_cast<std::uint32_t>(tilesX)) * kTile;
    const int y0 = static_cast<int>(index / static_cast<std::uint32_t>(tilesX)) * kTile;
    if (in.size() != 4u * kTile * kTile) return;
    for (int y = 0; y < kTile && y0 + y < image.height; ++y) {
        const int w = std::min(kTile, image.width - x0);
        std::copy(in.begin() + 4 * y * kTile, in.begin() + 4 * y * kTile + 4 * w, image.pixel(x0, y0 + y));
    }
}

void RgbToHsl(float r, float g, float b, float& h, float& s, float& l) {
    const float mx = std::max({r, g, b}), mn = std::min({r, g, b});
    l = 0.5f * (mx + mn);
    const float d = mx - mn;
    if (d <= 1e-6f) {
        h = s = 0.0f;
        return;
    }
    s = l > 0.5f ? d / (2.0f - mx - mn) : d / (mx + mn);
    if (mx == r)
        h = (g - b) / d + (g < b ? 6.0f : 0.0f);
    else if (mx == g)
        h = (b - r) / d + 2.0f;
    else
        h = (r - g) / d + 4.0f;
    h /= 6.0f;
}

float HueToRgb(float p, float q, float t) {
    if (t < 0.0f) t += 1.0f;
    if (t > 1.0f) t -= 1.0f;
    if (t < 1.0f / 6.0f) return p + (q - p) * 6.0f * t;
    if (t < 0.5f) return q;
    if (t < 2.0f / 3.0f) return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
    return p;
}

void HslToRgb(float h, float s, float l, float& r, float& g, float& b) {
    if (s <= 0.0f) {
        r = g = b = l;
        return;
    }
    const float q = l < 0.5f ? l * (1.0f + s) : l + s - l * s;
    const float p = 2.0f * l - q;
    r = HueToRgb(p, q, h + 1.0f / 3.0f);
    g = HueToRgb(p, q, h);
    b = HueToRgb(p, q, h - 1.0f / 3.0f);
}

template <typename Fn>
void MapRgb(const Image& source, Image& out, Fn fn) {
    out.width = source.width;
    out.height = source.height;
    out.rgba.resize(source.rgba.size());
    const std::size_t count = source.rgba.size() / 4;
    parallelFor(count, 16384, [&](std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) {
            const std::uint8_t* s = source.rgba.data() + 4 * i;
            std::uint8_t* d = out.rgba.data() + 4 * i;
            float c[3] = {ToUnit(s[0]), ToUnit(s[1]), ToUnit(s[2])};
            fn(c);
            d[0] = ToByte(c[0]);
            d[1] = ToByte(c[1]);
            d[2] = ToByte(c[2]);
            d[3] = s[3];
        }
    });
}

}  // namespace

// --- Image -------------------------------------------------------------------------------------

void Image::resize(int w, int h, std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) {
    width = std::max(w, 0);
    height = std::max(h, 0);
    rgba.resize(4u * static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    for (std::size_t i = 0; i < rgba.size(); i += 4) {
        rgba[i] = r;
        rgba[i + 1] = g;
        rgba[i + 2] = b;
        rgba[i + 3] = a;
    }
}

Image resampleImage(const Image& source, int width, int height) {
    Image out;
    out.resize(width, height, 0, 0, 0, 0);
    if (source.empty() || out.empty()) return out;
    for (int y = 0; y < height; ++y) {
        const float sy = std::min(std::max((static_cast<float>(y) + 0.5f) * source.height / height - 0.5f, 0.0f),
                                  static_cast<float>(source.height - 1));
        const int y0 = static_cast<int>(sy), y1 = std::min(y0 + 1, source.height - 1);
        const float fy = sy - static_cast<float>(y0);
        for (int x = 0; x < width; ++x) {
            const float sx = std::min(std::max((static_cast<float>(x) + 0.5f) * source.width / width - 0.5f, 0.0f),
                                      static_cast<float>(source.width - 1));
            const int x0 = static_cast<int>(sx), x1 = std::min(x0 + 1, source.width - 1);
            const float fx = sx - static_cast<float>(x0);
            for (int k = 0; k < 4; ++k) {
                const float a = source.pixel(x0, y0)[k] + (source.pixel(x1, y0)[k] - source.pixel(x0, y0)[k]) * fx;
                const float b = source.pixel(x0, y1)[k] + (source.pixel(x1, y1)[k] - source.pixel(x0, y1)[k]) * fx;
                out.pixel(x, y)[k] = static_cast<std::uint8_t>(std::lround(a + (b - a) * fy));
            }
        }
    }
    return out;
}

// --- PaintCanvas --------------------------------------------------------------------------------

void PaintCanvas::reset(Image base) {
    base_ = std::make_shared<Image>(std::move(base));
    layers_.clear();
    active_ = -1;
    composite_.resize(base_->width, base_->height, 0, 0, 0, 255);
    dirty_.assign(static_cast<std::size_t>(tilesX()) * tilesY(), 0u);
    markAll();
    updateComposite();
}

Image* PaintCanvas::target() {
    if (active_ >= 0 && active_ < static_cast<int>(layers_.size()) && layers_[active_].image)
        return layers_[active_].image.get();
    return base_.get();
}

std::uint64_t PaintCanvas::targetId() const {
    if (active_ >= 0 && active_ < static_cast<int>(layers_.size())) return layers_[active_].id;
    return kBaseId;
}

Image* PaintCanvas::imageById(std::uint64_t id) {
    if (id == kBaseId) return base_.get();
    for (PaintLayer& layer : layers_)
        if (layer.id == id) return layer.image.get();
    return nullptr;
}

void PaintCanvas::markTile(int tx, int ty) {
    if (tx < 0 || ty < 0 || tx >= tilesX() || ty >= tilesY()) return;
    dirty_[static_cast<std::size_t>(ty) * tilesX() + tx] = 1u;
    anyDirty_ = true;
}

void PaintCanvas::markRect(int x0, int y0, int x1, int y1) {
    for (int ty = std::max(0, y0 / kTile); ty <= std::min(tilesY() - 1, y1 / kTile); ++ty)
        for (int tx = std::max(0, x0 / kTile); tx <= std::min(tilesX() - 1, x1 / kTile); ++tx) markTile(tx, ty);
}

void PaintCanvas::markAll() {
    std::fill(dirty_.begin(), dirty_.end(), std::uint8_t{1});
    anyDirty_ = !dirty_.empty();
}

bool PaintCanvas::updateComposite() {
    if (!anyDirty_ || empty()) return false;
    anyDirty_ = false;
    if (composite_.width != width() || composite_.height != height()) composite_.resize(width(), height(), 0, 0, 0, 255);
    std::vector<std::uint32_t> tiles;
    for (std::size_t i = 0; i < dirty_.size(); ++i)
        if (dirty_[i]) {
            tiles.push_back(static_cast<std::uint32_t>(i));
            dirty_[i] = 0u;
        }
    const int tx = tilesX();
    parallelFor(tiles.size(), 8, [&](std::size_t begin, std::size_t end) {
        for (std::size_t t = begin; t < end; ++t) {
            const int x0 = static_cast<int>(tiles[t] % static_cast<std::uint32_t>(tx)) * kTile;
            const int y0 = static_cast<int>(tiles[t] / static_cast<std::uint32_t>(tx)) * kTile;
            for (int y = y0; y < std::min(y0 + kTile, height()); ++y) {
                for (int x = x0; x < std::min(x0 + kTile, width()); ++x) {
                    const std::uint8_t* b = base_->pixel(x, y);
                    float c[3] = {ToUnit(b[0]), ToUnit(b[1]), ToUnit(b[2])};
                    for (const PaintLayer& layer : layers_) {
                        if (!layer.enabled || !layer.image || layer.image->width != width()) continue;
                        const std::uint8_t* p = layer.image->pixel(x, y);
                        const float a = ToUnit(p[3]) * clamp01(layer.opacity);
                        if (a <= 0.0f) continue;
                        for (int k = 0; k < 3; ++k) c[k] += (BlendChannel(layer.blend, c[k], ToUnit(p[k])) - c[k]) * a;
                    }
                    std::uint8_t* out = composite_.pixel(x, y);
                    out[0] = ToByte(c[0]);
                    out[1] = ToByte(c[1]);
                    out[2] = ToByte(c[2]);
                    out[3] = 255;
                }
            }
        }
    });
    return true;
}

// --- TexelMap ------------------------------------------------------------------------------------

bool TexelMap::build(const Mesh& mesh, const std::vector<Vec3>& triangleUVs, int width, int height) {
    width_ = std::max(width, 0);
    height_ = std::max(height, 0);
    triangleCount_ = mesh.triangleCount();
    covered_ = 0;
    const std::size_t n = static_cast<std::size_t>(width_) * height_;
    tri_.assign(n, kNone);
    bu_.assign(n, 0.0f);
    bv_.assign(n, 0.0f);
    if (n == 0 || triangleUVs.size() != 3u * triangleCount_) return false;
    // Pass 0: texel centres inside a triangle. Pass 1: a one-texel gutter.
    for (int pass = 0; pass < 2; ++pass) {
        const float tolerance = pass == 0 ? 0.0f : 1.0f;  // Pixels.
        for (std::uint32_t t = 0; t < triangleCount_; ++t) {
            float px[3], py[3];
            for (int k = 0; k < 3; ++k) {
                const Vec3& uv = triangleUVs[3u * t + k];
                px[k] = uv.x * static_cast<float>(width_) - 0.5f;
                py[k] = (1.0f - uv.y) * static_cast<float>(height_) - 0.5f;
            }
            const float area = (px[1] - px[0]) * (py[2] - py[0]) - (px[2] - px[0]) * (py[1] - py[0]);
            if (!(std::fabs(area) > 1e-12f) || !std::isfinite(area)) continue;
            const int x0 = std::max(0, static_cast<int>(std::floor(std::min({px[0], px[1], px[2]}) - tolerance)));
            const int x1 = std::min(width_ - 1, static_cast<int>(std::ceil(std::max({px[0], px[1], px[2]}) + tolerance)));
            const int y0 = std::max(0, static_cast<int>(std::floor(std::min({py[0], py[1], py[2]}) - tolerance)));
            const int y1 = std::min(height_ - 1, static_cast<int>(std::ceil(std::max({py[0], py[1], py[2]}) + tolerance)));
            // Edge lengths turn barycentric tolerances into pixel distances.
            float len[3];
            for (int k = 0; k < 3; ++k) {
                const int a = (k + 1) % 3, b = (k + 2) % 3;  // Edge opposite vertex k.
                len[k] = std::sqrt((px[b] - px[a]) * (px[b] - px[a]) + (py[b] - py[a]) * (py[b] - py[a]));
            }
            for (int y = y0; y <= y1; ++y) {
                for (int x = x0; x <= x1; ++x) {
                    const std::size_t i = static_cast<std::size_t>(y) * width_ + x;
                    if (tri_[i] != kNone) continue;
                    const float fx = static_cast<float>(x), fy = static_cast<float>(y);
                    const float w1 = ((fx - px[0]) * (py[2] - py[0]) - (px[2] - px[0]) * (fy - py[0])) / area;
                    const float w2 = ((px[1] - px[0]) * (fy - py[0]) - (fx - px[0]) * (py[1] - py[0])) / area;
                    const float w0 = 1.0f - w1 - w2;
                    const float signedArea2 = std::fabs(area);
                    // Distance (pixels) outside each edge: -w_k * 2A / |edge_k|.
                    const float d0 = -w0 * signedArea2 / std::max(len[0], 1e-6f);
                    const float d1 = -w1 * signedArea2 / std::max(len[1], 1e-6f);
                    const float d2 = -w2 * signedArea2 / std::max(len[2], 1e-6f);
                    if (std::max({d0, d1, d2}) > tolerance) continue;
                    tri_[i] = t;
                    bu_[i] = w1;
                    bv_[i] = w2;
                    ++covered_;
                }
            }
        }
    }
    tileBounds_.assign(static_cast<std::size_t>((width_ + kTile - 1) / kTile) * ((height_ + kTile - 1) / kTile), Aabb());
    tileUsed_.assign(tileBounds_.size(), 0u);
    updateBounds(mesh);
    return covered_ > 0;
}

Vec3 TexelMap::position(const Mesh& mesh, std::size_t texel) const {
    const auto v = mesh.triangle(tri_[texel]);
    const float u = bu_[texel], w = bv_[texel];
    return mesh.position(v[0]) * (1.0f - u - w) + mesh.position(v[1]) * u + mesh.position(v[2]) * w;
}

Vec3 TexelMap::normal(const Mesh& mesh, std::size_t texel) const {
    const auto v = mesh.triangle(tri_[texel]);
    const float u = bu_[texel], w = bv_[texel];
    return normalizedOrZero(mesh.normal(v[0]) * (1.0f - u - w) + mesh.normal(v[1]) * u + mesh.normal(v[2]) * w);
}

float TexelMap::mask(const Mesh& mesh, const std::vector<float>& vertexMask, std::size_t texel) const {
    if (vertexMask.size() != mesh.vertexCount()) return 0.0f;
    const auto v = mesh.triangle(tri_[texel]);
    const float u = bu_[texel], w = bv_[texel];
    return clamp01(vertexMask[v[0]] * (1.0f - u - w) + vertexMask[v[1]] * u + vertexMask[v[2]] * w);
}

void TexelMap::updateBounds(const Mesh& mesh) {
    const int tilesX = (width_ + kTile - 1) / kTile;
    parallelFor(tileBounds_.size(), 16, [&](std::size_t begin, std::size_t end) {
        for (std::size_t t = begin; t < end; ++t) {
            Aabb box;
            const int x0 = static_cast<int>(t % static_cast<std::size_t>(tilesX)) * kTile;
            const int y0 = static_cast<int>(t / static_cast<std::size_t>(tilesX)) * kTile;
            for (int y = y0; y < std::min(y0 + kTile, height_); ++y)
                for (int x = x0; x < std::min(x0 + kTile, width_); ++x) {
                    const std::size_t i = static_cast<std::size_t>(y) * width_ + x;
                    if (tri_[i] != kNone) box.expand(position(mesh, i));
                }
            tileBounds_[t] = box;
            tileUsed_[t] = box.empty() ? 0u : 1u;
        }
    });
}

void TexelMap::gather(const Mesh& mesh, const Vec3& center, float radius, std::vector<std::uint32_t>& out) const {
    out.clear();
    const float r2 = radius * radius;
    const int tilesX = (width_ + kTile - 1) / kTile;
    for (std::size_t t = 0; t < tileBounds_.size(); ++t) {
        if (!tileUsed_[t] || tileBounds_[t].distanceSq(center) > r2) continue;
        const int x0 = static_cast<int>(t % static_cast<std::size_t>(tilesX)) * kTile;
        const int y0 = static_cast<int>(t / static_cast<std::size_t>(tilesX)) * kTile;
        for (int y = y0; y < std::min(y0 + kTile, height_); ++y)
            for (int x = x0; x < std::min(x0 + kTile, width_); ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * width_ + x;
                if (tri_[i] != kNone && lengthSq(position(mesh, i) - center) <= r2) out.push_back(static_cast<std::uint32_t>(i));
            }
    }
}

// --- Undo ---------------------------------------------------------------------------------------------

std::size_t PaintDelta::memoryBytes() const {
    std::size_t bytes = sizeof(*this);
    for (const Tile& t : tiles) bytes += sizeof(Tile) + t.before.size() + t.after.size();
    return bytes;
}

bool applyPaintDelta(PaintCanvas& canvas, const PaintDelta& delta, bool before) {
    Image* image = canvas.imageById(delta.imageId);
    if (!image || image->width != canvas.width() || image->height != canvas.height()) return false;
    const int tilesX = canvas.tilesX();
    for (const PaintDelta::Tile& t : delta.tiles) {
        WriteTile(*image, t.index, before ? t.before : t.after);
        canvas.markTile(static_cast<int>(t.index % static_cast<std::uint32_t>(tilesX)),
                        static_cast<int>(t.index / static_cast<std::uint32_t>(tilesX)));
    }
    return true;
}

PaintDelta imageDelta(std::uint64_t imageId, const Image& before, const Image& after) {
    PaintDelta delta;
    delta.imageId = imageId;
    if (before.width != after.width || before.height != after.height || before.empty()) return delta;
    const std::uint32_t tiles = static_cast<std::uint32_t>(((before.width + kTile - 1) / kTile) * ((before.height + kTile - 1) / kTile));
    for (std::uint32_t t = 0; t < tiles; ++t) {
        PaintDelta::Tile tile;
        tile.index = t;
        ReadTile(before, t, tile.before);
        ReadTile(after, t, tile.after);
        if (tile.before != tile.after) delta.tiles.push_back(std::move(tile));
    }
    return delta;
}

PaintDelta replaceImageContent(PaintCanvas& canvas, std::uint64_t id, const Image& content) {
    Image* image = canvas.imageById(id);
    if (!image || image->width != content.width || image->height != content.height) return PaintDelta();
    PaintDelta delta = imageDelta(id, *image, content);
    *image = content;
    canvas.markAll();
    return delta;
}

// --- PaintStroke -------------------------------------------------------------------------------------

void PaintStroke::begin(PaintCanvas& canvas, const TexelMap& map, const Mesh& mesh) {
    cancel();
    canvas_ = &canvas;
    map_ = &map;
    mesh_ = &mesh;
    image_ = canvas.target();
    imageId_ = canvas.targetId();
    haveLastTexel_ = false;
}

void PaintStroke::Touch(std::uint32_t tile) {
    auto it = before_.find(tile);
    if (it == before_.end()) {
        std::vector<std::uint8_t> pixels;
        ReadTile(*image_, tile, pixels);
        before_.emplace(tile, std::move(pixels));
        const int tilesX = canvas_->tilesX();
        canvas_->markTile(static_cast<int>(tile % static_cast<std::uint32_t>(tilesX)),
                          static_cast<int>(tile / static_cast<std::uint32_t>(tilesX)));
    }
}

float& PaintStroke::Coverage(std::size_t texel) {
    const int x = static_cast<int>(texel % static_cast<std::size_t>(image_->width));
    const int y = static_cast<int>(texel / static_cast<std::size_t>(image_->width));
    const std::uint32_t tile = static_cast<std::uint32_t>((y / kTile) * canvas_->tilesX() + x / kTile);
    std::vector<float>& c = coverage_[tile];
    if (c.empty()) c.assign(static_cast<std::size_t>(kTile) * kTile, 0.0f);
    return c[static_cast<std::size_t>(y % kTile) * kTile + x % kTile];
}

void PaintStroke::Blend(std::size_t texel, const Vec3& color, float coverage, bool erase) {
    const int x = static_cast<int>(texel % static_cast<std::size_t>(image_->width));
    const int y = static_cast<int>(texel / static_cast<std::size_t>(image_->width));
    Touch(static_cast<std::uint32_t>((y / kTile) * canvas_->tilesX() + x / kTile));
    // Coverage only grows within a stroke; apply the increment so the
    // opacity is never exceeded however often dabs overlap.
    float& s = Coverage(texel);
    if (coverage <= s) return;
    const float delta = s >= 1.0f ? 0.0f : (coverage - s) / (1.0f - s);
    s = coverage;
    std::uint8_t* p = image_->pixel(x, y);
    const float a = ToUnit(p[3]);
    if (erase) {
        p[3] = ToByte(a * (1.0f - delta));
        return;
    }
    const float outA = a + delta * (1.0f - a);
    if (outA <= 0.0f) return;
    const float c[3] = {color.x, color.y, color.z};
    for (int k = 0; k < 3; ++k) p[k] = ToByte((ToUnit(p[k]) * a * (1.0f - delta) + c[k] * delta) / outA);
    p[3] = ToByte(outA);
}

std::size_t PaintStroke::dab(const PaintSettings& settings, const PaintDab& dab, const std::vector<float>& vertexMask,
                             const std::vector<Mat3>& symmetry) {
    if (!active() || !image_ || !(dab.radius > 0.0f) || !isFinite(dab.center)) return 0;
    std::size_t changed = 0;
    for (const Mat3& m : symmetry) {
        PaintDab d = dab;
        d.center = m * dab.center;
        d.viewDir = m * dab.viewDir;
        d.up = m * dab.up;
        changed += DabOnce(settings, d, vertexMask);
    }
    return changed;
}

std::size_t PaintStroke::DabOnce(const PaintSettings& settings, const PaintDab& dab, const std::vector<float>& vertexMask) {
    const Mesh& mesh = *mesh_;
    const TexelMap& map = *map_;
    const bool alpha = settings.alpha && !settings.alpha->empty();
    map.gather(mesh, dab.center, dab.radius * (alpha ? 1.41421356f : 1.0f), texels_);
    if (texels_.empty()) return 0;

    BrushSettings stamp;  // Alpha footprint only; the radial falloff uses the hardness below.
    stamp.useFalloff = false;
    stamp.alpha = alpha ? settings.alpha : nullptr;
    stamp.alphaMid = settings.alphaMid;
    stamp.alphaFade = settings.alphaFade;
    Dab frame;
    frame.center = dab.center;
    frame.radius = dab.radius;
    frame.up = dab.up;
    frame.viewDir = dab.viewDir;
    const Vec3 frameNormal = normalizedOrZero(-dab.viewDir);
    const float hardness = std::min(clamp01(settings.hardness), 0.999f);
    const float strength = clamp01(dab.pressure) * std::max(0.0f, dab.amount);

    std::vector<float> weight(texels_.size(), 0.0f);
    for (std::size_t i = 0; i < texels_.size(); ++i) {
        const std::size_t t = texels_[i];
        const Vec3 p = map.position(mesh, t);
        if (settings.backfaceCull && dot(map.normal(mesh, t), dab.viewDir) > 0.0f) continue;
        const float r = length(p - dab.center) / dab.radius;
        float w = r <= hardness ? 1.0f : brushFalloff((r - hardness) / (1.0f - hardness), settings.useFalloff);
        if (r > 1.0f && !alpha) w = 0.0f;
        if (w > 0.0f && alpha) w *= dabWeight(stamp, frame, frameNormal, p);
        if (w > 0.0f && !vertexMask.empty()) w *= 1.0f - map.mask(mesh, vertexMask, t);
        weight[i] = w * strength;
    }

    std::size_t changed = 0;
    const int width = image_->width;
    switch (settings.tool) {
        case PaintTool::Paint:
        case PaintTool::Erase: {
            const bool erase = settings.tool == PaintTool::Erase || dab.erase;
            for (std::size_t i = 0; i < texels_.size(); ++i) {
                float w = weight[i];
                if (w <= 0.0f) continue;
                Vec3 color = dab.color;
                if (dab.stencil) {
                    Vec3 stencilColor;
                    float coverage = 0.0f;
                    if (!(*dab.stencil)(map.position(mesh, texels_[i]), stencilColor, coverage)) continue;
                    w *= clamp01(coverage);
                    if (dab.stencilColors) color = stencilColor;
                }
                Blend(texels_[i], color, std::min(w, 1.0f) * clamp01(settings.opacity), erase);
                ++changed;
            }
            break;
        }
        case PaintTool::Blur:
        case PaintTool::Smudge: {
            // Source colours are read before anything is written.
            float dx = 0.0f, dy = 0.0f;
            if (settings.tool == PaintTool::Smudge) {
                // Texture position of the dab centre: the nearest texel.
                std::size_t nearest = texels_.front();
                float best = kInfinity;
                for (std::uint32_t t : texels_) {
                    const float d = lengthSq(map.position(mesh, t) - dab.center);
                    if (d < best) {
                        best = d;
                        nearest = t;
                    }
                }
                const float cx = static_cast<float>(nearest % static_cast<std::size_t>(width));
                const float cy = static_cast<float>(nearest / static_cast<std::size_t>(width));
                if (haveLastTexel_) {
                    dx = cx - lastX_;
                    dy = cy - lastY_;
                    const float jump = std::sqrt(dx * dx + dy * dy);
                    const float limit = std::max(4.0f, static_cast<float>(width) * 0.05f);
                    if (jump > limit) dx = dy = 0.0f;  // Crossed a UV seam: no smear this dab.
                }
                lastX_ = cx;
                lastY_ = cy;
                haveLastTexel_ = true;
                if (dx == 0.0f && dy == 0.0f) break;
            }
            const float amount = settings.tool == PaintTool::Blur ? clamp01(settings.blurStrength) : clamp01(settings.opacity);
            std::vector<std::array<float, 4>> result(texels_.size());
            for (std::size_t i = 0; i < texels_.size(); ++i) {
                if (weight[i] <= 0.0f) continue;
                const int x = static_cast<int>(texels_[i] % static_cast<std::size_t>(width));
                const int y = static_cast<int>(texels_[i] / static_cast<std::size_t>(width));
                std::array<float, 4> sum{0, 0, 0, 0};
                if (settings.tool == PaintTool::Blur) {
                    float count = 0.0f;
                    for (int oy = -2; oy <= 2; ++oy)
                        for (int ox = -2; ox <= 2; ++ox) {
                            const int sx = x + ox, sy = y + oy;
                            if (sx < 0 || sy < 0 || sx >= width || sy >= image_->height) continue;
                            if (!map.covered(static_cast<std::size_t>(sy) * width + sx)) continue;
                            const std::uint8_t* p = image_->pixel(sx, sy);
                            for (int k = 0; k < 4; ++k) sum[k] += ToUnit(p[k]);
                            count += 1.0f;
                        }
                    for (float& v : sum) v /= std::max(count, 1.0f);
                } else {  // Smudge: bilinear sample behind the motion.
                    const float sx = std::min(std::max(static_cast<float>(x) - dx, 0.0f), static_cast<float>(width - 1));
                    const float sy = std::min(std::max(static_cast<float>(y) - dy, 0.0f), static_cast<float>(image_->height - 1));
                    const int x0 = static_cast<int>(sx), y0 = static_cast<int>(sy);
                    const int x1 = std::min(x0 + 1, width - 1), y1 = std::min(y0 + 1, image_->height - 1);
                    const float fx = sx - static_cast<float>(x0), fy = sy - static_cast<float>(y0);
                    for (int k = 0; k < 4; ++k) {
                        const float a = ToUnit(image_->pixel(x0, y0)[k]) * (1 - fx) + ToUnit(image_->pixel(x1, y0)[k]) * fx;
                        const float b = ToUnit(image_->pixel(x0, y1)[k]) * (1 - fx) + ToUnit(image_->pixel(x1, y1)[k]) * fx;
                        sum[k] = a * (1 - fy) + b * fy;
                    }
                }
                result[i] = sum;
            }
            for (std::size_t i = 0; i < texels_.size(); ++i) {
                const float w = std::min(weight[i], 1.0f) * amount;
                if (w <= 0.0f) continue;
                const int x = static_cast<int>(texels_[i] % static_cast<std::size_t>(width));
                const int y = static_cast<int>(texels_[i] / static_cast<std::size_t>(width));
                Touch(static_cast<std::uint32_t>((y / kTile) * canvas_->tilesX() + x / kTile));
                std::uint8_t* p = image_->pixel(x, y);
                for (int k = 0; k < 4; ++k) p[k] = ToByte(ToUnit(p[k]) + (result[i][k] - ToUnit(p[k])) * w);
                ++changed;
            }
            break;
        }
        default:
            break;
    }
    return changed;
}

std::size_t PaintStroke::fill(const PaintSettings& settings, const std::vector<float>& vertexMask) {
    if (!active() || !image_) return 0;
    std::size_t changed = 0;
    const std::size_t n = static_cast<std::size_t>(image_->width) * image_->height;
    for (std::size_t t = 0; t < n; ++t) {
        if (!map_->covered(t)) continue;
        const float w = clamp01(settings.opacity) * (vertexMask.empty() ? 1.0f : 1.0f - map_->mask(*mesh_, vertexMask, t));
        if (w <= 0.0f) continue;
        Blend(t, settings.colorA, w, false);
        ++changed;
    }
    return changed;
}

std::size_t PaintStroke::gradient(const PaintSettings& settings, const Vec3& from, const Vec3& to,
                                  const std::vector<float>& vertexMask) {
    if (!active() || !image_) return 0;
    const Vec3 axis = to - from;
    const float len2 = lengthSq(axis);
    if (!(len2 > 0.0f)) return 0;
    std::size_t changed = 0;
    const std::size_t n = static_cast<std::size_t>(image_->width) * image_->height;
    for (std::size_t t = 0; t < n; ++t) {
        if (!map_->covered(t)) continue;
        const float w = clamp01(settings.opacity) * (vertexMask.empty() ? 1.0f : 1.0f - map_->mask(*mesh_, vertexMask, t));
        if (w <= 0.0f) continue;
        const float s = clamp01(dot(map_->position(*mesh_, t) - from, axis) / len2);
        Blend(t, lerp(settings.colorA, settings.colorB, s), w, false);
        ++changed;
    }
    return changed;
}

PaintDelta PaintStroke::end() {
    PaintDelta delta;
    if (!active()) return delta;
    delta.imageId = imageId_;
    for (auto& entry : before_) {
        PaintDelta::Tile tile;
        tile.index = entry.first;
        ReadTile(*image_, entry.first, tile.after);
        if (tile.after == entry.second) continue;
        tile.before = std::move(entry.second);
        delta.tiles.push_back(std::move(tile));
    }
    std::sort(delta.tiles.begin(), delta.tiles.end(),
              [](const PaintDelta::Tile& a, const PaintDelta::Tile& b) { return a.index < b.index; });
    before_.clear();
    coverage_.clear();
    canvas_ = nullptr;
    image_ = nullptr;
    return delta;
}

void PaintStroke::cancel() {
    if (canvas_ && image_) {
        const int tilesX = canvas_->tilesX();
        for (const auto& entry : before_) {
            WriteTile(*image_, entry.first, entry.second);
            canvas_->markTile(static_cast<int>(entry.first % static_cast<std::uint32_t>(tilesX)),
                              static_cast<int>(entry.first / static_cast<std::uint32_t>(tilesX)));
        }
    }
    before_.clear();
    coverage_.clear();
    canvas_ = nullptr;
    image_ = nullptr;
}

// --- Adjustments ---------------------------------------------------------------------------------------

void adjustHsl(const Image& source, Image& out, float hue, float saturation, float luminosity) {
    const float dh = hue / 360.0f, ds = saturation / 100.0f, dl = luminosity / 100.0f;
    MapRgb(source, out, [=](float c[3]) {
        float h, s, l;
        RgbToHsl(c[0], c[1], c[2], h, s, l);
        h = h + dh - std::floor(h + dh);
        s = clamp01(ds >= 0.0f ? s + (1.0f - s) * ds : s * (1.0f + ds));
        l = clamp01(dl >= 0.0f ? l + (1.0f - l) * dl : l * (1.0f + dl));
        HslToRgb(h, s, l, c[0], c[1], c[2]);
    });
}

void adjustBrightnessContrast(const Image& source, Image& out, float brightness, float contrast) {
    const float b = brightness / 100.0f;
    const float c = std::max(-0.99f, contrast / 100.0f);
    const float factor = c >= 0.0f ? 1.0f / (1.0f - c) : 1.0f + c;
    MapRgb(source, out, [=](float v[3]) {
        for (int k = 0; k < 3; ++k) v[k] = (v[k] + b - 0.5f) * factor + 0.5f;
    });
}

void adjustLevels(const Image& source, Image& out, float inBlack, float gamma, float inWhite, float outBlack,
                  float outWhite) {
    const float ib = inBlack / 255.0f, iw = std::max(inWhite / 255.0f, ib + 1.0f / 255.0f);
    const float ob = outBlack / 255.0f, ow = outWhite / 255.0f;
    const float g = std::min(std::max(gamma, 0.1f), 10.0f);
    MapRgb(source, out, [=](float v[3]) {
        for (int k = 0; k < 3; ++k) {
            const float t = clamp01((v[k] - ib) / (iw - ib));
            v[k] = ob + (ow - ob) * std::pow(t, 1.0f / g);
        }
    });
}

}  // namespace sculpt
