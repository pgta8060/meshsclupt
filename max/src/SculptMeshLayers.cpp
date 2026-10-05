// Sculpt layers and the Displace layer of SculptMeshObject.
#include <algorithm>
#include <map>
#include <memory>

#include "AlphaLibrary.h"
#include "PolyConvert.h"
#include "SculptMeshObject.h"
#include "SculptSettings.h"
#include "SculptUI.h"

namespace {

// Undo of a layer-stack change (structure, strength, visibility, Displace).
class SculptLayersRestore : public RestoreObj {
public:
    SculptLayersRestore(SculptMeshObject* object, sculpt::LayerStack before, sculpt::LayerStack after, bool move,
                        int levelBefore, int levelAfter, std::uint64_t idBefore, std::uint64_t idAfter, MSTR name)
        : object_(object), before_(std::move(before)), after_(std::move(after)), move_(move), levelBefore_(levelBefore),
          levelAfter_(levelAfter), idBefore_(idBefore), idAfter_(idAfter), name_(std::move(name)) {}
    void Restore(int /*isUndo*/) override {
        if (object_) object_->SetLayersInternal(before_, move_, levelBefore_, idBefore_);
    }
    void Redo() override {
        if (object_) object_->SetLayersInternal(after_, move_, levelAfter_, idAfter_);
    }
    int Size() override {
        const std::size_t bytes = sizeof(*this) + before_.memoryBytes() + after_.memoryBytes();
        return bytes > 0x7fffffff ? 0x7fffffff : static_cast<int>(bytes);
    }
    MSTR Description() override { return name_; }

private:
    SculptMeshObject* object_;
    sculpt::LayerStack before_, after_;
    bool move_;
    int levelBefore_, levelAfter_;
    std::uint64_t idBefore_, idAfter_;
    MSTR name_;
};

// Undo of the part of a stroke recorded into a layer.
class SculptLayerRecordRestore : public RestoreObj {
public:
    SculptLayerRecordRestore(SculptMeshObject* object, int layer, std::vector<std::uint32_t> vertices,
                             std::vector<sculpt::Vec3> before, std::vector<sculpt::Vec3> after)
        : object_(object), layer_(layer), vertices_(std::move(vertices)), before_(std::move(before)), after_(std::move(after)) {}
    void Restore(int /*isUndo*/) override {
        if (object_) object_->SetLayerValues(layer_, vertices_, before_);
    }
    void Redo() override {
        if (object_) object_->SetLayerValues(layer_, vertices_, after_);
    }
    int Size() override {
        return static_cast<int>(sizeof(*this) + vertices_.size() * (4 + 2 * sizeof(sculpt::Vec3)));
    }
    MSTR Description() override { return MSTR(_T("Layer")); }

private:
    SculptMeshObject* object_;
    int layer_;
    std::vector<std::uint32_t> vertices_;
    std::vector<sculpt::Vec3> before_, after_;
};

std::string NextLayerName(const sculpt::LayerStack& stack) {
    for (int n = 1;; ++n) {
        const std::string name = "Layer " + std::to_string(n);
        bool used = false;
        for (const sculpt::SculptLayer& layer : stack.layers) used = used || layer.name == name;
        if (!used) return name;
    }
}

// Displace maps are reloaded only when the path or blur changes.
std::shared_ptr<const sculpt::Alpha> DisplaceImage(const std::string& path, float blur) {
    static std::string cachedPath;
    static float cachedBlur = -1.0f;
    static std::shared_ptr<const sculpt::Alpha> source, blurred;
    if (path != cachedPath) {
        source = AlphaLibrary::LoadGrayImage(path, 4096);
        cachedPath = path;
        cachedBlur = -1.0f;
    }
    if (!source) return nullptr;
    if (blur != cachedBlur) {
        blurred = blur >= 0.5f ? std::make_shared<const sculpt::Alpha>(sculpt::blurImage(*source, blur)) : source;
        cachedBlur = blur;
    }
    return blurred;
}

}  // namespace

bool SculptMeshObject::LayersUsable() const {
    if (layers_.empty()) return true;
    return layersLevel_ == level_ && layersTopologyId_ == topologyId_ &&
           layers_.sizesMatch(static_cast<std::size_t>(std::max<int>(mm.numv, 0)));
}

void SculptMeshObject::AddLayerOffsets() {
    for (int v = 0; v < mm.numv; ++v) mm.v[v].p += ToPoint3(layers_.offset(static_cast<std::size_t>(v)));
}

void SculptMeshObject::SetLayersInternal(const sculpt::LayerStack& stack, bool moveVertices, int level,
                                         std::uint64_t topologyId) {
    if (moveVertices && LayersUsable() && stack.sizesMatch(static_cast<std::size_t>(std::max<int>(mm.numv, 0)))) {
        sculpt::StrokeDelta delta;
        for (int v = 0; v < mm.numv; ++v) {
            const sculpt::Vec3 change = stack.offset(static_cast<std::size_t>(v)) - layers_.offset(static_cast<std::size_t>(v));
            if (change == sculpt::Vec3()) continue;
            delta.vertices.push_back(static_cast<std::uint32_t>(v));
            delta.before.push_back(ToVec3(mm.v[v].p));
            delta.after.push_back(ToVec3(mm.v[v].p) + change);
        }
        layers_ = stack;
        if (!delta.empty()) ApplyDelta(delta, false);
    } else {
        layers_ = stack;
    }
    layersLevel_ = level;
    layersTopologyId_ = topologyId;
    SculptUI::Refresh();
}

void SculptMeshObject::SetLayerValues(int layer, const std::vector<std::uint32_t>& vertices,
                                      const std::vector<sculpt::Vec3>& values) {
    if (layer < 0 || layer >= static_cast<int>(layers_.layers.size())) return;
    std::vector<sculpt::Vec3>& delta = layers_.layers[static_cast<std::size_t>(layer)].delta;
    for (std::size_t i = 0; i < vertices.size() && i < values.size(); ++i)
        if (vertices[i] < delta.size()) delta[vertices[i]] = values[i];
}

void SculptMeshObject::ChangeLayers(sculpt::LayerStack after, bool moveVertices, int undoName) {
    const sculpt::LayerStack before = layers_;
    const int levelBefore = layersLevel_;
    const std::uint64_t idBefore = layersTopologyId_;
    const int levelAfter = before.empty() ? level_ : layersLevel_;  // A first layer lives on the current level.
    const std::uint64_t idAfter = before.empty() ? topologyId_ : layersTopologyId_;
    theHold.Begin();
    SetLayersInternal(after, moveVertices, levelAfter, idAfter);
    theHold.Put(new SculptLayersRestore(this, before, layers_, moveVertices, levelBefore, levelAfter, idBefore, idAfter,
                                        MSTR(GetString(undoName))));
    theHold.Accept(GetString(undoName));
}

void SculptMeshObject::RecordIntoActiveLayer(const sculpt::StrokeDelta& maxDelta) {
    const int a = layers_.active;
    if (!LayersHere() || a < 0 || a >= static_cast<int>(layers_.layers.size()) || !theHold.Holding()) return;
    sculpt::SculptLayer& layer = layers_.layers[static_cast<std::size_t>(a)];
    if (!layer.enabled || layer.strength < 0.01f) return;  // Hidden or silenced: the stroke goes to the base.
    std::vector<std::uint32_t> vertices;
    std::vector<sculpt::Vec3> before, after;
    for (std::size_t i = 0; i < maxDelta.vertices.size(); ++i) {
        const std::uint32_t v = maxDelta.vertices[i];
        if (v >= layer.delta.size()) continue;
        vertices.push_back(v);
        before.push_back(layer.delta[v]);
        layer.delta[v] += (maxDelta.after[i] - maxDelta.before[i]) / layer.strength;
        after.push_back(layer.delta[v]);
    }
    if (!vertices.empty())
        theHold.Put(new SculptLayerRecordRestore(this, a, std::move(vertices), std::move(before), std::move(after)));
}

bool SculptMeshObject::NewLayer() {
    if (!LayersUsable()) return false;
    sculpt::LayerStack after = layers_;
    sculpt::SculptLayer layer;
    layer.name = NextLayerName(after);
    layer.delta.assign(static_cast<std::size_t>(std::max<int>(mm.numv, 0)), sculpt::Vec3());
    after.layers.push_back(std::move(layer));
    after.active = static_cast<int>(after.layers.size()) - 1;
    ChangeLayers(std::move(after), false, IDS_UNDO_NEW_LAYER);
    return true;
}

bool SculptMeshObject::DeleteLayer() {
    const int a = layers_.active;
    if (!LayersUsable() || a < 0 || a >= static_cast<int>(layers_.layers.size())) return false;
    sculpt::LayerStack after = layers_;
    after.layers.erase(after.layers.begin() + a);
    after.active = after.layers.empty() ? -1 : std::min(a, static_cast<int>(after.layers.size()) - 1);
    ChangeLayers(std::move(after), true, IDS_UNDO_DELETE_LAYER);
    return true;
}

bool SculptMeshObject::ClearLayer() {
    const int a = layers_.active;
    if (!LayersUsable() || a < 0 || a >= static_cast<int>(layers_.layers.size())) return false;
    sculpt::LayerStack after = layers_;
    std::fill(after.layers[static_cast<std::size_t>(a)].delta.begin(), after.layers[static_cast<std::size_t>(a)].delta.end(),
              sculpt::Vec3());
    ChangeLayers(std::move(after), true, IDS_UNDO_CLEAR_LAYER);
    return true;
}

bool SculptMeshObject::ClearAllLayers() {
    if (!LayersUsable() || layers_.empty()) return false;
    sculpt::LayerStack after = layers_;
    for (sculpt::SculptLayer& layer : after.layers) std::fill(layer.delta.begin(), layer.delta.end(), sculpt::Vec3());
    ChangeLayers(std::move(after), true, IDS_UNDO_CLEAR_LAYER);
    return true;
}

bool SculptMeshObject::MoveLayer(int direction) {
    const int a = layers_.active;
    const int b = a + direction;
    if (!LayersUsable() || a < 0 || b < 0 || b >= static_cast<int>(layers_.layers.size())) return false;
    sculpt::LayerStack after = layers_;
    std::swap(after.layers[static_cast<std::size_t>(a)], after.layers[static_cast<std::size_t>(b)]);
    after.active = b;
    ChangeLayers(std::move(after), false, IDS_UNDO_MOVE_LAYER);
    return true;
}

bool SculptMeshObject::BakeAllLayers() {
    if (!LayersUsable() || layers_.empty()) return false;
    ChangeLayers(sculpt::LayerStack(), false, IDS_UNDO_BAKE_LAYERS);  // Positions already show every layer.
    return true;
}

void SculptMeshObject::SelectLayer(int index) {
    layers_.active = index >= 0 && index < static_cast<int>(layers_.layers.size()) ? index : -1;
    SculptUI::Refresh();
}

void SculptMeshObject::SetLayerEnabled(int index, bool on) {
    if (!LayersUsable() || index < 0 || index >= static_cast<int>(layers_.layers.size())) return;
    if (layers_.layers[static_cast<std::size_t>(index)].enabled == on) return;
    sculpt::LayerStack after = layers_;
    after.layers[static_cast<std::size_t>(index)].enabled = on;
    ChangeLayers(std::move(after), true, IDS_UNDO_LAYER_SETTINGS);
}

void SculptMeshObject::SetLayerStrengthLive(int index, float strength) {
    if (!LayersUsable() || index < 0 || index >= static_cast<int>(layers_.layers.size())) return;
    strength = std::min(std::max(strength, 0.0f), sculpt::kMaxLayerStrength);
    if (layers_.layers[static_cast<std::size_t>(index)].strength == strength) return;
    if (!strengthDragStart_) strengthDragStart_ = std::make_unique<sculpt::LayerStack>(layers_);
    sculpt::LayerStack after = layers_;
    after.layers[static_cast<std::size_t>(index)].strength = strength;
    SetLayersInternal(after, true, layersLevel_, layersTopologyId_);
}

void SculptMeshObject::FinishLayerStrength() {
    if (!strengthDragStart_) return;
    std::unique_ptr<sculpt::LayerStack> start = std::move(strengthDragStart_);
    theHold.Begin();
    theHold.Put(new SculptLayersRestore(this, *start, layers_, true, layersLevel_, layersLevel_, layersTopologyId_,
                                        layersTopologyId_, MSTR(GetString(IDS_UNDO_LAYER_SETTINGS))));
    theHold.Accept(GetString(IDS_UNDO_LAYER_SETTINGS));
}

void SculptMeshObject::SetLayerName(int index, const std::string& name) {
    if (index < 0 || index >= static_cast<int>(layers_.layers.size()) || name.empty()) return;
    if (layers_.layers[static_cast<std::size_t>(index)].name == name) return;
    sculpt::LayerStack after = layers_;
    after.layers[static_cast<std::size_t>(index)].name = name;
    ChangeLayers(std::move(after), false, IDS_UNDO_LAYER_SETTINGS);
}

bool SculptMeshObject::ApplyDisplace(MSTR& error, bool undoable) {
    const SculptSettings& settings = SculptSettings::Get();
    // The Displace layer lives on the highest level.
    const int top = MultiresTopLevel();
    if (!layers_.empty() && layersLevel_ != top) {
        error.printf(GetString(IDS_ERR_LAYERS_LEVEL), layersLevel_);
        return false;
    }
    if (level_ != top && !SetMultiresLevel(top, error)) return false;
    if (!LayersUsable()) {
        error.printf(GetString(IDS_ERR_LAYERS_LEVEL), layersLevel_);
        return false;
    }
    std::shared_ptr<const sculpt::Alpha> image = DisplaceImage(settings.DisplaceMap(), settings.Value(Prop::DisplaceBlur));
    if (!image) {
        error = GetString(IDS_ERR_DISPLACE_MAP);
        return false;
    }
    // Surface without the current Displace layer: positions, normals, UVs.
    sculpt::PolyData poly;
    if (!MeshToPoly(mm, attributes_, poly, &error)) return false;
    const int existing = layers_.displaceLayer();
    std::vector<int> aliveIndex;
    for (int v = 0; v < mm.numv; ++v)
        if (!mm.v[v].GetFlag(MN_DEAD)) aliveIndex.push_back(v);
    if (existing >= 0) {
        const sculpt::SculptLayer& layer = layers_.layers[static_cast<std::size_t>(existing)];
        for (std::size_t i = 0; i < aliveIndex.size(); ++i)
            poly.positions[i] -= layer.delta[static_cast<std::size_t>(aliveIndex[i])] * layers_.factor(static_cast<std::size_t>(existing));
    }
    std::vector<sculpt::Vec3> normals;
    sculpt::polyVertexNormals(poly, poly.positions, normals);
    std::vector<sculpt::Vec3> uvs;
    for (const sculpt::CornerMap& map : poly.maps) {
        if (map.channel != 1) continue;
        uvs.assign(poly.positions.size(), sculpt::Vec3());
        std::vector<std::uint8_t> seen(poly.positions.size(), 0u);
        for (std::size_t c = 0; c < poly.faceVerts.size(); ++c) {
            const std::uint32_t v = poly.faceVerts[c];
            if (!seen[v]) uvs[v] = map.values[c];
            seen[v] = 1u;
        }
    }
    sculpt::Aabb bounds;
    for (const sculpt::Vec3& p : poly.positions) bounds.expand(p);
    sculpt::DisplaceSettings ds;
    ds.triplanar = settings.Bool(Prop::DisplaceTriplanar) || uvs.empty();
    ds.strength = settings.Value(Prop::DisplaceStrength);
    ds.waterLevel = settings.Value(Prop::DisplaceWaterLevel);
    ds.contrast = settings.Value(Prop::DisplaceContrast);
    ds.tileU = settings.Value(Prop::DisplaceTileU);
    ds.tileV = settings.Value(Prop::DisplaceTileV);
    ds.offsetU = settings.Value(Prop::DisplaceOffsetU);
    ds.offsetV = settings.Value(Prop::DisplaceOffsetV);
    std::vector<sculpt::Vec3> offsets;
    sculpt::computeDisplacement(poly.positions, normals, uvs.empty() ? nullptr : &uvs, bounds, *image, ds, offsets);

    sculpt::LayerStack after = layers_;
    int index = existing;
    if (index < 0) {
        sculpt::SculptLayer layer;
        layer.name = "Displace";
        layer.displace = true;
        after.layers.push_back(std::move(layer));
        index = static_cast<int>(after.layers.size()) - 1;
    }
    sculpt::SculptLayer& target = after.layers[static_cast<std::size_t>(index)];
    target.delta.assign(static_cast<std::size_t>(std::max<int>(mm.numv, 0)), sculpt::Vec3());
    for (std::size_t i = 0; i < aliveIndex.size(); ++i) target.delta[static_cast<std::size_t>(aliveIndex[i])] = offsets[i];
    if (undoable) {
        ChangeLayers(std::move(after), true, IDS_UNDO_DISPLACE);
    } else {
        SetLayersInternal(after, true, layers_.empty() ? level_ : layersLevel_, layers_.empty() ? topologyId_ : layersTopologyId_);
    }
    return true;
}

bool SculptMeshObject::RemoveDisplace() {
    const int index = layers_.displaceLayer();
    if (index < 0 || !LayersUsable()) return false;
    sculpt::LayerStack after = layers_;
    after.layers.erase(after.layers.begin() + index);
    if (after.active == index) after.active = -1;
    if (after.active > index) --after.active;
    ChangeLayers(std::move(after), true, IDS_UNDO_DISPLACE);
    return true;
}
