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
#include "sculpt/session.h"

class SculptDisplay {
public:
    struct Options {
        bool showMask = true;
        bool showGroups = false;
        bool operator==(const Options& o) const { return showMask == o.showMask && showGroups == o.showGroups; }
    };

    // (Re)creates chunk buffers and render items for the session (visible
    // polygons only, split per SculptGroup) and uploads everything.
    void Build(const sculpt::SculptSession& session, const Options& options);
    void Clear();
    bool IsBuilt() const { return !gpu_.empty(); }

    void MarkVertices(sculpt::Span<std::uint32_t> vertices) { chunks_.markVertices(vertices); }
    void MarkAll() { chunks_.markAll(); }
    // Mask/group colour switches only change the lookup texture.
    void SetOptions(const Options& options);

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
    Options options_;
};
