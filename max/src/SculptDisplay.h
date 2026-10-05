// Fast viewport display used while sculpting: the mesh is split into display
// chunks, each with its own GPU buffers, and after a dab only the chunks that
// contain changed vertices are re-uploaded (instead of rebuilding the whole
// PolyObject display, which is what makes dense meshes slow).
#pragma once

#include <cstdint>
#include <vector>

#include <max.h>
#include <Graphics/GeometryRenderItemHandle.h>
#include <Graphics/IRenderItemContainer.h>
#include <Graphics/IndexBufferHandle.h>
#include <Graphics/StandardMaterialHandle.h>
#include <Graphics/UpdateNodeContext.h>
#include <Graphics/VertexBufferHandle.h>

#include "sculpt/display_chunks.h"
#include "sculpt/mesh.h"

class SculptDisplay {
public:
    // Creates chunk buffers and render items for `mesh` and uploads everything.
    void Build(const sculpt::Mesh& mesh);
    void Clear();
    bool IsBuilt() const { return !gpu_.empty(); }

    void MarkVertices(sculpt::Span<std::uint32_t> vertices) { chunks_.markVertices(vertices); }
    void MarkAll() { chunks_.markAll(); }

    // Re-uploads positions and normals of every dirty chunk.
    void Upload(const sculpt::Mesh& mesh);

    // Replaces the node's render items with the chunk items.
    void AddRenderItems(MaxSDK::Graphics::UpdateNodeContext& nodeContext,
                        MaxSDK::Graphics::IRenderItemContainer& container);

private:
    struct ChunkGpu {
        MaxSDK::Graphics::VertexBufferHandle positions;
        MaxSDK::Graphics::VertexBufferHandle normals;
        MaxSDK::Graphics::IndexBufferHandle triangles;
        MaxSDK::Graphics::IndexBufferHandle edges;
        MaxSDK::Graphics::GeometryRenderItemHandle solid;
        MaxSDK::Graphics::GeometryRenderItemHandle wire;
    };

    sculpt::DisplayChunks chunks_;
    std::vector<ChunkGpu> gpu_;
    MaxSDK::Graphics::StandardMaterialHandle clay_;
};
