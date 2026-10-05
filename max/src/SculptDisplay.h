// Fast viewport display used while sculpting: the mesh is split into display
// chunks, each with its own GPU buffers, and after a dab only the chunks that
// contain changed vertices are re-uploaded (instead of rebuilding the whole
// PolyObject display, which is what makes dense meshes slow).
//
// Mask and SculptGroup colours come from a small lookup texture: every vertex
// carries (u = mask, v = group colour row) in map channel 1, so the standard
// lit material keeps its shading while showing the mask and groups.
#pragma once

#include <cstdint>
#include <vector>

#include <max.h>
#include <Graphics/GeometryRenderItemHandle.h>
#include <Graphics/IRenderItemContainer.h>
#include <Graphics/IndexBufferHandle.h>
#include <Graphics/StandardMaterialHandle.h>
#include <Graphics/TextureHandle.h>
#include <Graphics/UpdateNodeContext.h>
#include <Graphics/VertexBufferHandle.h>

#include "sculpt/display_chunks.h"
#include "sculpt/paint.h"
#include "sculpt/session.h"

class SculptDisplay {
public:
    struct Options {
        bool showMask = true;
        bool showGroups = false;
        bool paint = false;  // Show the paint texture instead of clay (needs a paint source).
        bool operator==(const Options& o) const {
            return showMask == o.showMask && showGroups == o.showGroups && paint == o.paint;
        }
    };
    // Texture painting: a UV key per session polygon corner, the UV of every
    // key and the composite image. The data must outlive the display.
    struct PaintSource {
        const std::vector<std::uint32_t>* cornerKeys = nullptr;
        const std::vector<sculpt::Vec3>* keyUVs = nullptr;
        const sculpt::Image* image = nullptr;
        bool valid() const { return cornerKeys && keyUVs && image && !image->empty(); }
    };

    // (Re)creates chunk buffers and render items for the session (visible
    // polygons only, split per SculptGroup) and uploads everything.
    void Build(const sculpt::SculptSession& session, const Options& options);
    void Clear();
    bool IsBuilt() const { return !gpu_.empty(); }

    void MarkVertices(sculpt::Span<std::uint32_t> vertices) { chunks_.markVertices(vertices); }
    void MarkAll() { chunks_.markAll(); }
    // Mask/group colour switches only change the lookup texture. Returns
    // true when the chunks must be rebuilt (paint display switched).
    bool SetOptions(const Options& options);
    void SetPaintSource(const PaintSource& source) { paintSource_ = source; }
    bool PaintShown() const { return paintShown_; }
    // Copies the paint composite into its texture.
    void UploadPaint();

    // Re-uploads positions, normals and colours of every dirty chunk.
    void Upload(const sculpt::SculptSession& session);

    // Replaces the node's render items with the chunk items.
    void AddRenderItems(MaxSDK::Graphics::UpdateNodeContext& nodeContext,
                        MaxSDK::Graphics::IRenderItemContainer& container);

private:
    struct ChunkGpu {
        MaxSDK::Graphics::VertexBufferHandle positions;
        MaxSDK::Graphics::VertexBufferHandle normals;
        MaxSDK::Graphics::VertexBufferHandle attributes;  // (mask, group row, 0)
        MaxSDK::Graphics::IndexBufferHandle triangles;
        MaxSDK::Graphics::IndexBufferHandle edges;
        MaxSDK::Graphics::GeometryRenderItemHandle solid;
        MaxSDK::Graphics::GeometryRenderItemHandle wire;
    };

    void FillLookupTexture();

    sculpt::DisplayChunks chunks_;
    std::vector<ChunkGpu> gpu_;
    MaxSDK::Graphics::StandardMaterialHandle clay_;
    MaxSDK::Graphics::TextureHandle lookup_;
    MaxSDK::Graphics::TextureHandle paintTexture_;
    Options options_;
    PaintSource paintSource_;
    bool paintShown_ = false;
    int paintWidth_ = 0, paintHeight_ = 0;
};
