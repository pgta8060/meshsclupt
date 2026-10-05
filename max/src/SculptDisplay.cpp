#include "SculptDisplay.h"

#include <algorithm>
#include <cmath>
#include <functional>

#include <Graphics/MaterialRequiredStreams.h>
#include <Graphics/RenderNodeHandle.h>
#include <Graphics/SimpleRenderGeometry.h>

using namespace MaxSDK::Graphics;

namespace {

constexpr int kLookupWidth = 64;    // Mask steps (u).
constexpr int kLookupRows = 64;     // Row 0: no group colour; rows 1..63: group palette (v).
const float kClay[3] = {0.62f, 0.60f, 0.58f};
const float kMaskTint[3] = {0.16f, 0.17f, 0.20f};  // Fully masked colour.

// Pleasant, well separated group colours (golden-ratio hue steps).
void GroupColor(int row, float rgb[3]) {
    const float h = std::fmod(static_cast<float>(row) * 0.618034f, 1.0f) * 6.0f;
    const float s = 0.45f, v = 0.85f;
    const int i = static_cast<int>(h);
    const float f = h - static_cast<float>(i);
    const float p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    const float table[6][3] = {{v, t, p}, {q, v, p}, {p, v, t}, {p, q, v}, {t, p, v}, {v, p, q}};
    for (int k = 0; k < 3; ++k) rgb[k] = table[i % 6][k];
}

float GroupRowV(std::int32_t group) {
    const int row = group <= 0 ? 0 : 1 + (group - 1) % (kLookupRows - 1);
    return (static_cast<float>(row) + 0.5f) / static_cast<float>(kLookupRows);
}

float MaskU(float mask) {
    const float m = std::min(std::max(mask, 0.0f), 1.0f);
    return (m * static_cast<float>(kLookupWidth - 1) + 0.5f) / static_cast<float>(kLookupWidth);
}

MaterialRequiredStreamElement Stream(MeshChannelCategory category, int index) {
    MaterialRequiredStreamElement e;
    e.SetType(VertexFieldFloat3);
    e.SetChannelCategory(category);
    e.SetStreamIndex(static_cast<BYTE>(index));
    return e;
}

void WriteVec3(VertexBufferHandle& buffer, std::size_t count, const std::function<Point3(std::size_t)>& value) {
    if (count == 0) return;
    Point3* out = reinterpret_cast<Point3*>(buffer.Lock(0, count, WriteAcess));
    if (!out) return;
    for (std::size_t i = 0; i < count; ++i) out[i] = value(i);
    buffer.Unlock();
}

}  // namespace

void SculptDisplay::Clear() {
    gpu_.clear();  // Handles release their GPU resources.
    chunks_.clear();
}

void SculptDisplay::FillLookupTexture() {
    LockedRect rect;
    if (!lookup_.WriteOnlyLockRectangle(0, rect, nullptr) || !rect.pBits) return;
    for (int row = 0; row < kLookupRows; ++row) {
        float base[3] = {kClay[0], kClay[1], kClay[2]};
        if (options_.showGroups && row > 0) GroupColor(row, base);
        unsigned char* line = rect.pBits + static_cast<std::size_t>(row) * rect.pitch;
        for (int x = 0; x < kLookupWidth; ++x) {
            const float m = options_.showMask ? static_cast<float>(x) / static_cast<float>(kLookupWidth - 1) : 0.0f;
            float rgb[3];
            for (int k = 0; k < 3; ++k) rgb[k] = base[k] + (kMaskTint[k] - base[k]) * m * 0.85f;
            // A8R8G8B8 in memory order B, G, R, A.
            line[4 * x + 0] = static_cast<unsigned char>(std::lround(rgb[2] * 255.0f));
            line[4 * x + 1] = static_cast<unsigned char>(std::lround(rgb[1] * 255.0f));
            line[4 * x + 2] = static_cast<unsigned char>(std::lround(rgb[0] * 255.0f));
            line[4 * x + 3] = 255;
        }
    }
    lookup_.WriteOnlyUnlockRectangle();
}

void SculptDisplay::SetOptions(const Options& options) {
    if (options == options_) return;
    options_ = options;
    if (IsBuilt()) FillLookupTexture();
}

void SculptDisplay::Build(const sculpt::SculptSession& session, const Options& options) {
    Clear();
    options_ = options;
    const sculpt::Mesh& mesh = session.mesh();
    chunks_.build(mesh, sculpt::DisplayChunks::kDefaultTrianglesPerChunk,
                  session.anyHidden() ? &session.hiddenFaces() : nullptr, &session.faceGroups());

    lookup_.Initialize(kLookupWidth, kLookupRows, TargetFormatA8R8G8B8);
    FillLookupTexture();
    clay_.Initialize();
    clay_.SetDiffuse(AColor(1.0f, 1.0f, 1.0f, 1.0f));  // Colour comes from the lookup texture.
    clay_.SetAmbient(AColor(0.12f, 0.12f, 0.12f, 1.0f));
    clay_.SetSpecular(AColor(0.35f, 0.35f, 0.35f, 1.0f));
    clay_.SetSpecularPower(18.0f);
    clay_.SetOpacity(1.0f);
    clay_.SetDiffuseTexture(lookup_);

    // Match the material's stream requirements; anything we do not produce
    // (should not happen for this material) gets the attribute stream.
    MaterialRequiredStreams streams;
    std::vector<int> streamSource;  // 0 positions, 1 normals, 2 attributes
    if (const MaterialRequiredStreams* required = clay_.GetRequiredStreams()) {
        for (size_t i = 0; i < required->GetNumberOfStreams(); ++i) {
            MaterialRequiredStreamElement e = required->GetStreamElement(i);
            e.SetStreamIndex(static_cast<BYTE>(streams.GetNumberOfStreams()));
            const MeshChannelCategory category = e.GetChannelCategory();
            streamSource.push_back(category == MeshChannelPosition ? 0 : (category == MeshChannelVertexNormal ? 1 : 2));
            streams.AddStream(e);
        }
    }
    if (streams.GetNumberOfStreams() == 0) {
        streams.AddStream(Stream(MeshChannelPosition, 0));
        streams.AddStream(Stream(MeshChannelVertexNormal, 1));
        streamSource = {0, 1};
    }
    MaterialRequiredStreams wireStreams;
    wireStreams.AddStream(Stream(MeshChannelPosition, 0));
    wireStreams.AddStream(Stream(MeshChannelVertexNormal, 1));

    gpu_.resize(chunks_.chunks().size());
    for (std::size_t c = 0; c < gpu_.size(); ++c) {
        const sculpt::DisplayChunk& chunk = chunks_.chunks()[c];
        ChunkGpu& g = gpu_[c];
        const std::size_t vertexCount = chunk.vertices.size();

        g.positions.Initialize(sizeof(Point3), vertexCount, nullptr, BufferUsageDynamic);
        g.normals.Initialize(sizeof(Point3), vertexCount, nullptr, BufferUsageDynamic);
        g.attributes.Initialize(sizeof(Point3), vertexCount, nullptr, BufferUsageDynamic);
        g.triangles.Initialize(IndexTypeInt, chunk.triangles.size(), const_cast<std::uint32_t*>(chunk.triangles.data()));

        SimpleRenderGeometry* solidGeometry = new SimpleRenderGeometry();
        solidGeometry->SetPrimitiveType(PrimitiveTriangleList);
        solidGeometry->SetPrimitiveCount(chunk.triangles.size() / 3);
        solidGeometry->SetSteamRequirement(streams);
        for (int source : streamSource)
            solidGeometry->AddVertexBuffer(source == 0 ? g.positions : (source == 1 ? g.normals : g.attributes));
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
            wireGeometry->SetSteamRequirement(wireStreams);
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
    chunks_.markAll();
    Upload(session);
}

void SculptDisplay::Upload(const sculpt::SculptSession& session) {
    if (gpu_.empty()) return;
    const sculpt::Mesh& mesh = session.mesh();
    const std::vector<float>& mask = session.mask();
    for (std::uint32_t c : chunks_.takeDirty()) {
        if (c >= gpu_.size()) continue;
        const sculpt::DisplayChunk& chunk = chunks_.chunks()[c];
        const std::size_t n = chunk.vertices.size();
        WriteVec3(gpu_[c].positions, n, [&](std::size_t i) {
            const sculpt::Vec3& p = mesh.position(chunk.vertices[i]);
            return Point3(p.x, p.y, p.z);
        });
        WriteVec3(gpu_[c].normals, n, [&](std::size_t i) {
            const sculpt::Vec3& p = mesh.normal(chunk.vertices[i]);
            return Point3(p.x, p.y, p.z);
        });
        WriteVec3(gpu_[c].attributes, n, [&](std::size_t i) {
            const std::uint32_t v = chunk.vertices[i];
            return Point3(MaskU(v < mask.size() ? mask[v] : 0.0f), GroupRowV(chunk.groups[i]), 0.0f);
        });
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
