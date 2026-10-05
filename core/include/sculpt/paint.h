// SculptCore — texture painting: images, paint layers with blend modes, the
// texel map (which surface point every texel shows), the six paint tools and
// layer adjustments.
//
// Images are RGBA8 with straight alpha, row 0 at the top of the texture
// (UV v = 1). Texel (x, y) shows UV ((x + 0.5) / W, 1 - (y + 0.5) / H).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "sculpt/brush.h"
#include "sculpt/math.h"
#include "sculpt/mesh.h"

namespace sculpt {

struct Image {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;  // 4 bytes per pixel.

    void resize(int w, int h, std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a);
    bool empty() const { return width <= 0 || height <= 0; }
    std::uint8_t* pixel(int x, int y) { return rgba.data() + 4u * (static_cast<std::size_t>(y) * width + x); }
    const std::uint8_t* pixel(int x, int y) const { return rgba.data() + 4u * (static_cast<std::size_t>(y) * width + x); }
};

// Bilinear resize (import, generated texture size changes).
Image resampleImage(const Image& source, int width, int height);

enum class PaintBlend : int { Normal = 0, Multiply = 1, Screen = 2, Overlay = 3, Add = 4, Subtract = 5 };

struct PaintLayer {
    std::string name;
    bool enabled = true;
    float opacity = 1.0f;
    PaintBlend blend = PaintBlend::Normal;
    std::shared_ptr<Image> image;  // Shared with undo snapshots of the stack.
    std::uint64_t id = 0;
};

// Base texture + layers (index 0 at the bottom) and their composite.
class PaintCanvas {
public:
    static constexpr int kTile = 32;
    static constexpr std::uint64_t kBaseId = 1;

    void reset(Image base);
    bool valid() const { return !base_ || !base_->empty(); }
    bool empty() const { return !base_ || base_->empty(); }
    int width() const { return base_ ? base_->width : 0; }
    int height() const { return base_ ? base_->height : 0; }
    int tilesX() const { return (width() + kTile - 1) / kTile; }
    int tilesY() const { return (height() + kTile - 1) / kTile; }

    Image& base() { return *base_; }
    const Image& base() const { return *base_; }
    std::vector<PaintLayer>& layers() { return layers_; }
    const std::vector<PaintLayer>& layers() const { return layers_; }
    int active() const { return active_; }
    void setActive(int index) { active_ = index >= 0 && index < static_cast<int>(layers_.size()) ? index : -1; }
    std::uint64_t newLayerId() { return nextId_++; }
    // Image painted by the tools: the active layer, else the base.
    Image* target();
    std::uint64_t targetId() const;
    Image* imageById(std::uint64_t id);

    // Composite of the enabled layers over the base (opaque).
    const Image& composite() const { return composite_; }
    void markTile(int tx, int ty);
    void markRect(int x0, int y0, int x1, int y1);  // Inclusive pixel bounds.
    void markAll();
    // Recomposites the dirty tiles; returns false if nothing was dirty.
    bool updateComposite();

private:
    std::shared_ptr<Image> base_;
    std::vector<PaintLayer> layers_;
    int active_ = -1;
    std::uint64_t nextId_ = 2;
    Image composite_;
    std::vector<std::uint8_t> dirty_;  // Per tile.
    bool anyDirty_ = false;
};

// For every texel: the mesh triangle and barycentrics of the surface point it
// shows, so positions, normals and the mask follow sculpting without a rebuild.
class TexelMap {
public:
    static constexpr std::uint32_t kNone = 0xffffffffu;
    static constexpr int kTile = PaintCanvas::kTile;

    // `triangleUVs`: three (u, v, _) per mesh triangle. Texels within about a
    // texel of a triangle edge are filled too (gutter against seam bleeding).
    bool build(const Mesh& mesh, const std::vector<Vec3>& triangleUVs, int width, int height);
    bool valid() const { return width_ > 0 && covered_ > 0; }
    int width() const { return width_; }
    int height() const { return height_; }
    bool matches(int width, int height, std::uint32_t triangles) const {
        return width == width_ && height == height_ && triangles == triangleCount_;
    }

    bool covered(std::size_t texel) const { return tri_[texel] != kNone; }
    Vec3 position(const Mesh& mesh, std::size_t texel) const;
    Vec3 normal(const Mesh& mesh, std::size_t texel) const;
    float mask(const Mesh& mesh, const std::vector<float>& vertexMask, std::size_t texel) const;

    // Tile bounds from the current vertex positions (once per stroke).
    void updateBounds(const Mesh& mesh);
    // Covered texels whose surface point may lie within `radius` of `center`.
    void gather(const Mesh& mesh, const Vec3& center, float radius, std::vector<std::uint32_t>& out) const;

private:
    int width_ = 0, height_ = 0;
    std::uint32_t triangleCount_ = 0;
    std::size_t covered_ = 0;
    std::vector<std::uint32_t> tri_;
    std::vector<float> bu_, bv_;  // Weights of the triangle's 2nd and 3rd vertex.
    std::vector<Aabb> tileBounds_;
    std::vector<std::uint8_t> tileUsed_;
};

enum class PaintTool : int { Paint = 0, Smudge = 1, Fill = 2, Blur = 3, Erase = 4, Gradient = 5 };

struct PaintSettings {
    PaintTool tool = PaintTool::Paint;
    Vec3 colorA{1, 1, 1};
    Vec3 colorB{0, 0, 0};
    float opacity = 1.0f;       // Highest coverage one stroke reaches.
    float hardness = 0.5f;      // 0 soft .. 1 hard edge.
    float blurStrength = 0.5f;
    bool useFalloff = true;
    bool backfaceCull = true;
    const Alpha* alpha = nullptr;
    float alphaMid = 0.0f;
    float alphaFade = 0.0f;
};

struct PaintDab {
    Vec3 center;
    float radius = 1.0f;
    float pressure = 1.0f;
    float amount = 1.0f;     // Scatter amount jitter (opacity).
    Vec3 viewDir{0, 0, -1};
    Vec3 up{0, 1, 0};
    Vec3 color{1, 1, 1};     // Paint colour of this dab (Color Mix / jitter resolved by the caller).
    bool erase = false;      // Alt with Paint.
    // Optional stencil: object-space point -> colour and coverage (false outside).
    const std::function<bool(const Vec3&, Vec3&, float&)>* stencil = nullptr;
    bool stencilColors = true;  // Stencil supplies the colour (else it only masks colorA).
};

// Pixels before/after a paint operation, by 32x32 tile, for one image.
struct PaintDelta {
    std::uint64_t imageId = 0;
    struct Tile {
        std::uint32_t index = 0;
        std::vector<std::uint8_t> before, after;
    };
    std::vector<Tile> tiles;
    bool empty() const { return tiles.empty(); }
    std::size_t memoryBytes() const;
};
// Writes the before/after pixels back (marks the tiles dirty).
bool applyPaintDelta(PaintCanvas& canvas, const PaintDelta& delta, bool before);
// The changed tiles between two images of the same size.
PaintDelta imageDelta(std::uint64_t imageId, const Image& before, const Image& after);
// Replaces the pixels of image `id` (same size) and returns the undo record.
PaintDelta replaceImageContent(PaintCanvas& canvas, std::uint64_t id, const Image& content);

class PaintStroke {
public:
    // Starts a stroke on the canvas' target image.
    void begin(PaintCanvas& canvas, const TexelMap& map, const Mesh& mesh);
    bool active() const { return canvas_ != nullptr; }
    // One dab, repeated for every symmetry transform. Returns texels changed.
    std::size_t dab(const PaintSettings& settings, const PaintDab& dab, const std::vector<float>& vertexMask,
                    const std::vector<Mat3>& symmetry);
    // Whole-image tools.
    std::size_t fill(const PaintSettings& settings, const std::vector<float>& vertexMask);
    std::size_t gradient(const PaintSettings& settings, const Vec3& from, const Vec3& to,
                         const std::vector<float>& vertexMask);
    PaintDelta end();
    void cancel();

private:
    void Touch(std::uint32_t tile);
    float& Coverage(std::size_t texel);
    void Blend(std::size_t texel, const Vec3& color, float coverage, bool erase);
    std::size_t DabOnce(const PaintSettings& settings, const PaintDab& dab, const std::vector<float>& vertexMask);

    PaintCanvas* canvas_ = nullptr;
    const TexelMap* map_ = nullptr;
    const Mesh* mesh_ = nullptr;
    Image* image_ = nullptr;
    std::uint64_t imageId_ = 0;
    std::unordered_map<std::uint32_t, std::vector<std::uint8_t>> before_;      // Tile -> pixels at stroke start.
    std::unordered_map<std::uint32_t, std::vector<float>> coverage_;           // Tile -> stroke coverage.
    std::vector<std::uint32_t> texels_;
    bool haveLastTexel_ = false;
    float lastX_ = 0.0f, lastY_ = 0.0f;  // Texture position of the previous dab (Smudge).
};

// --- Layer adjustments (rgb only; alpha kept) ----------------------------------------------
// Hue in degrees (-180..180), saturation and luminosity in percent (-100..100).
void adjustHsl(const Image& source, Image& out, float hue, float saturation, float luminosity);
// Brightness and contrast in percent (-100..100).
void adjustBrightnessContrast(const Image& source, Image& out, float brightness, float contrast);
// Levels: input black/white and output black/white in 0..255, gamma 0.1..10.
void adjustLevels(const Image& source, Image& out, float inBlack, float gamma, float inWhite, float outBlack,
                  float outWhite);

}  // namespace sculpt
