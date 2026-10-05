#include "SculptDisplay.h"

#include <Graphics/MaterialRequiredStreams.h>
#include <Graphics/RenderNodeHandle.h>
#include <Graphics/SimpleRenderGeometry.h>

using namespace MaxSDK::Graphics;

namespace {

// Positions in stream 0 and normals in stream 1: what StandardMaterialHandle
// (shaded) and the node's wireframe material (lines) both expect.
MaterialRequiredStreams PositionNormalStreams() {
    MaterialRequiredStreams streams;
    MaterialRequiredStreamElement position;
    position.SetType(VertexFieldFloat3);
    position.SetChannelCategory(MeshChannelPosition);
    position.SetStreamIndex(0);
    streams.AddStream(position);
    MaterialRequiredStreamElement normal;
    normal.SetType(VertexFieldFloat3);
    normal.SetChannelCategory(MeshChannelVertexNormal);
    normal.SetStreamIndex(1);
    streams.AddStream(normal);
    return streams;
}

void WritePositions(VertexBufferHandle& buffer, const std::vector<std::uint32_t>& vertices, const sculpt::Mesh& mesh,
                    bool normals) {
    if (vertices.empty()) return;
    Point3* out = reinterpret_cast<Point3*>(buffer.Lock(0, vertices.size(), WriteAcess));
    if (!out) return;
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        const sculpt::Vec3& v = normals ? mesh.normal(vertices[i]) : mesh.position(vertices[i]);
        out[i] = Point3(v.x, v.y, v.z);
    }
    buffer.Unlock();
}

}  // namespace

void SculptDisplay::Clear() {
    gpu_.clear();  // Handles release their GPU resources.
    chunks_.clear();
}

void SculptDisplay::Build(const sculpt::Mesh& mesh) {
    Clear();
    chunks_.build(mesh);

    clay_.Initialize();
    clay_.SetDiffuse(AColor(0.62f, 0.60f, 0.58f, 1.0f));
    clay_.SetAmbient(AColor(0.12f, 0.12f, 0.12f, 1.0f));
    clay_.SetSpecular(AColor(0.35f, 0.35f, 0.35f, 1.0f));
    clay_.SetSpecularPower(18.0f);
    clay_.SetOpacity(1.0f);

    const MaterialRequiredStreams streams = PositionNormalStreams();
    gpu_.resize(chunks_.chunks().size());
    for (std::size_t c = 0; c < gpu_.size(); ++c) {
        const sculpt::DisplayChunk& chunk = chunks_.chunks()[c];
        ChunkGpu& g = gpu_[c];
        const std::size_t vertexCount = chunk.vertices.size();

        g.positions.Initialize(sizeof(Point3), vertexCount, nullptr, BufferUsageDynamic);
        g.normals.Initialize(sizeof(Point3), vertexCount, nullptr, BufferUsageDynamic);
        g.triangles.Initialize(IndexTypeInt, chunk.triangles.size(),
                               const_cast<std::uint32_t*>(chunk.triangles.data()));

        SimpleRenderGeometry* solidGeometry = new SimpleRenderGeometry();
        solidGeometry->SetPrimitiveType(PrimitiveTriangleList);
        solidGeometry->SetPrimitiveCount(chunk.triangles.size() / 3);
        solidGeometry->SetSteamRequirement(streams);
        solidGeometry->AddVertexBuffer(g.positions);
        solidGeometry->AddVertexBuffer(g.normals);
        solidGeometry->SetIndexBuffer(g.triangles);

        g.solid.Initialize();
        g.solid.SetRenderGeometry(solidGeometry);
        g.solid.SetVisibilityGroup(RenderItemVisible_Shaded);
        g.solid.SetZBias(ZBiasPresets_Shaded);
        g.solid.SetDescriptionBits(MeshElementTypeSolidMesh);
        g.solid.SetCustomMaterial(clay_);

        if (!chunk.edges.empty()) {
            g.edges.Initialize(IndexTypeInt, chunk.edges.size(), const_cast<std::uint32_t*>(chunk.edges.data()));
            SimpleRenderGeometry* wireGeometry = new SimpleRenderGeometry();
            wireGeometry->SetPrimitiveType(PrimitiveLineList);
            wireGeometry->SetPrimitiveCount(chunk.edges.size() / 2);
            wireGeometry->SetSteamRequirement(streams);
            wireGeometry->AddVertexBuffer(g.positions);
            wireGeometry->AddVertexBuffer(g.normals);
            wireGeometry->SetIndexBuffer(g.edges);

            g.wire.Initialize();
            g.wire.SetRenderGeometry(wireGeometry);
            g.wire.SetVisibilityGroup(RenderItemVisible_Wireframe);
            g.wire.SetZBias(ZBiasPresets_Wireframe);
            g.wire.SetDescriptionBits(MeshElementTypeEdge);
        }
    }
    Upload(mesh);
}

void SculptDisplay::Upload(const sculpt::Mesh& mesh) {
    if (gpu_.empty()) return;
    for (std::uint32_t c : chunks_.takeDirty()) {
        if (c >= gpu_.size()) continue;
        const std::vector<std::uint32_t>& vertices = chunks_.chunks()[c].vertices;
        WritePositions(gpu_[c].positions, vertices, mesh, false);
        WritePositions(gpu_[c].normals, vertices, mesh, true);
    }
}

void SculptDisplay::AddRenderItems(UpdateNodeContext& nodeContext, IRenderItemContainer& container) {
    container.ClearAllRenderItems();
    const BaseMaterialHandle wireMaterial = nodeContext.GetRenderNode().GetWireframeMaterial();
    for (ChunkGpu& g : gpu_) {
        container.AddRenderItem(g.solid);
        if (g.wire.IsValid()) {
            g.wire.SetCustomMaterial(wireMaterial);
            container.AddRenderItem(g.wire);
        }
    }
}
