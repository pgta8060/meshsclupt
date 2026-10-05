// Texture painting on SculptMeshObject: the canvas and texel map, paint undo,
// paint layers and adjustments, and the Material / Paint rollout commands
// (texture files and the node material).
#include <algorithm>
#include <cwctype>

#include <bmmlib.h>
#include <stdmat.h>

#include "PaintImageIO.h"
#include "SculptMeshObject.h"
#include "SculptMode.h"
#include "SculptSettings.h"
#include "SculptUI.h"

namespace {

constexpr int kResolutions[] = {512, 1024, 2048, 4096};
constexpr std::uint8_t kGeneratedGrey = 200;
const MCHAR kPaintMaterialName[] = _T("SculptMesh Paint");

// Keeps the node's original material alive (and restorable) while a paint
// material is assigned. Not saved with the scene.
}  // namespace

class PaintMaterialKeeper : public ReferenceMaker {
public:
    explicit PaintMaterialKeeper(Mtl* mtl) {
        if (mtl) ReplaceReference(0, mtl);
    }
    Mtl* Get() const { return mtl_; }
    int NumRefs() override { return 1; }
    RefTargetHandle GetReference(int i) override { return i == 0 ? mtl_ : nullptr; }
    RefResult NotifyRefChanged(const Interval& /*changeInt*/, RefTargetHandle target, PartID& /*partID*/,
                               RefMessage message, BOOL /*propagate*/) override {
        if (message == REFMSG_TARGET_DELETED && target == mtl_) mtl_ = nullptr;
        return REF_SUCCEED;
    }
    void DeleteThis() override { delete this; }

protected:
    void SetReference(int i, RefTargetHandle target) override {
        if (i == 0) mtl_ = static_cast<Mtl*>(target);
    }

private:
    Mtl* mtl_ = nullptr;
};

namespace {

std::wstring Lower(std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

BitmapTex* AsBitmap(Texmap* tex) {
    return tex && tex->ClassID() == Class_ID(BMTEX_CLASS_ID, 0) ? static_cast<BitmapTex*>(tex) : nullptr;
}

// A bitmap anywhere in the material (and its sub-materials) using `path`.
BitmapTex* FindBitmapByPath(MtlBase* mtl, const std::wstring& path, int depth = 0) {
    if (!mtl || depth > 8) return nullptr;
    for (int i = 0; i < mtl->NumSubTexmaps(); ++i) {
        Texmap* tex = mtl->GetSubTexmap(i);
        if (BitmapTex* bitmap = AsBitmap(tex)) {
            const MCHAR* name = bitmap->GetMapName();
            if (name && Lower(name) == Lower(path)) return bitmap;
        }
        if (BitmapTex* found = FindBitmapByPath(tex, path, depth + 1)) return found;
    }
    if (mtl->SuperClassID() == MATERIAL_CLASS_ID) {
        Mtl* m = static_cast<Mtl*>(mtl);
        for (int i = 0; i < m->NumSubMtls(); ++i)
            if (BitmapTex* found = FindBitmapByPath(m->GetSubMtl(i), path, depth + 1)) return found;
    }
    return nullptr;
}

// The diffuse / base colour bitmap of a material ("Existing Diffuse Texture").
BitmapTex* FindDiffuseBitmap(Mtl* mtl) {
    if (!mtl) return nullptr;
    if (mtl->ClassID() == Class_ID(DMTL_CLASS_ID, 0))
        if (BitmapTex* bitmap = AsBitmap(mtl->GetSubTexmap(ID_DI))) return bitmap;
    BitmapTex* any = nullptr;
    for (int i = 0; i < mtl->NumSubTexmaps(); ++i) {
        BitmapTex* bitmap = AsBitmap(mtl->GetSubTexmap(i));
        if (!bitmap) continue;
        const std::wstring slot = Lower(mtl->GetSubTexmapSlotName(i, false).data());
        if (slot.find(L"diffuse") != std::wstring::npos || slot.find(L"base") != std::wstring::npos) return bitmap;
        if (!any) any = bitmap;
    }
    if (any) return any;
    for (int i = 0; i < mtl->NumSubMtls(); ++i)  // Multi/Sub-Object: the first usable one.
        if (BitmapTex* bitmap = FindDiffuseBitmap(mtl->GetSubMtl(i))) return bitmap;
    return nullptr;
}

// Undo of pixels painted into one image.
class SculptPaintRestore : public RestoreObj {
public:
    SculptPaintRestore(SculptMeshObject* object, sculpt::PaintDelta delta, MSTR name)
        : object_(object), delta_(std::move(delta)), name_(std::move(name)) {}
    void Restore(int /*isUndo*/) override {
        if (object_) object_->ApplyPaintDelta(delta_, true);
    }
    void Redo() override {
        if (object_) object_->ApplyPaintDelta(delta_, false);
    }
    int Size() override {
        const std::size_t bytes = delta_.memoryBytes();
        return bytes > 0x7fffffff ? 0x7fffffff : static_cast<int>(bytes);
    }
    MSTR Description() override { return name_; }

private:
    SculptMeshObject* object_;
    sculpt::PaintDelta delta_;
    MSTR name_;
};

// Undo of a paint layer stack change (images are shared, not copied).
class SculptPaintStackRestore : public RestoreObj {
public:
    SculptPaintStackRestore(SculptMeshObject* object, std::vector<sculpt::PaintLayer> before, int beforeActive,
                            std::vector<sculpt::PaintLayer> after, int afterActive, MSTR name)
        : object_(object), before_(std::move(before)), after_(std::move(after)), beforeActive_(beforeActive),
          afterActive_(afterActive), name_(std::move(name)) {}
    void Restore(int /*isUndo*/) override {
        if (object_) object_->SetPaintLayersInternal(before_, beforeActive_);
    }
    void Redo() override {
        if (object_) object_->SetPaintLayersInternal(after_, afterActive_);
    }
    int Size() override { return static_cast<int>(sizeof(*this) + 64 * (before_.size() + after_.size())); }
    MSTR Description() override { return name_; }

private:
    SculptMeshObject* object_;
    std::vector<sculpt::PaintLayer> before_, after_;
    int beforeActive_, afterActive_;
    MSTR name_;
};

void Redraw() {
    if (Interface* core = GetCOREInterface()) core->RedrawViews(core->GetTime());
}

}  // namespace

SculptMeshObject::PaintState::PaintState() = default;

SculptMeshObject::PaintState::~PaintState() {
    if (keeper) {
        keeper->DeleteAllRefs();
        keeper->DeleteThis();
    }
}

// --- Canvas and texel map ----------------------------------------------------------------

const sculpt::PaintCanvas* SculptMeshObject::Canvas() const { return paint_ ? &paint_->canvas : nullptr; }

INode* SculptMeshObject::FindNode() const {
    Interface* core = GetCOREInterface();
    if (!core) return nullptr;
    for (int i = 0; i < core->GetSelNodeCount(); ++i) {
        INode* node = core->GetSelNode(i);
        Object* ref = node ? node->GetObjectRef() : nullptr;
        if (ref && ref->FindBaseObject() == this) return node;
    }
    return nullptr;
}

bool SculptMeshObject::PaintDisplayWanted() const {
    return paint_ && paint_->map.valid() && paint_->mapStamp == TopologyStamp() &&
           SculptSettings::Get().Mode() == ToolMode::Paint && SculptMode::Get().IsActive() &&
           SculptMode::Get().Target() == this;
}

bool SculptMeshObject::BuildPaintMap(MSTR& error) {
    PaintState& p = *paint_;
    MNMap* uv = mm.MNum() > 1 ? mm.M(1) : nullptr;
    if (!uv || uv->GetFlag(MN_DEAD) || uv->numf != mm.numf || uv->numv <= 0) {
        error = _T("Texture painting needs UVs in map channel 1. Add them with Unwrap UVW (and collapse) first.");
        return false;
    }
    const sculpt::Mesh& mesh = bridge_->Session().mesh();
    const std::uint32_t F = mesh.faceCount();
    std::vector<std::uint32_t> cornerStart(static_cast<std::size_t>(F) + 1, 0u);
    p.cornerKeys.clear();
    for (std::uint32_t f = 0; f < F; ++f) {
        const int maxFace = bridge_->ToMaxFace(f);
        const MNMapFace& mf = uv->f[maxFace];
        const sculpt::Span<std::uint32_t> poly = mesh.faceVertices(f);
        cornerStart[f + 1] = cornerStart[f] + static_cast<std::uint32_t>(poly.size());
        for (std::size_t k = 0; k < poly.size(); ++k) {
            const int tv = static_cast<int>(k) < mf.deg ? mf.tv[k] : 0;
            p.cornerKeys.push_back(tv >= 0 && tv < uv->numv ? static_cast<std::uint32_t>(tv) : 0u);
        }
    }
    p.keyUVs.resize(static_cast<std::size_t>(uv->numv));
    for (int i = 0; i < uv->numv; ++i) p.keyUVs[static_cast<std::size_t>(i)] = ToVec3(uv->v[i]);
    std::vector<sculpt::Vec3> triangleUVs;
    triangleUVs.reserve(3u * mesh.triangleCount());
    for (std::uint32_t t = 0; t < mesh.triangleCount(); ++t) {
        const std::uint32_t f = mesh.triangleFace(t);
        const sculpt::Span<std::uint32_t> poly = mesh.faceVertices(f);
        for (std::uint32_t v : mesh.triangle(t)) {
            std::uint32_t k = 0;
            while (k + 1 < poly.size() && poly[k] != v) ++k;
            triangleUVs.push_back(p.keyUVs[p.cornerKeys[cornerStart[f] + k]]);
        }
    }
    HCURSOR previous = SetCursor(LoadCursor(nullptr, IDC_WAIT));
    const bool ok = p.map.build(mesh, triangleUVs, p.canvas.width(), p.canvas.height());
    SetCursor(previous);
    if (!ok) {
        error = _T("The UVs in map channel 1 do not cover the 0-1 texture space.");
        return false;
    }
    p.mapStamp = TopologyStamp();
    return true;
}

SculptMeshObject::PaintState* SculptMeshObject::AcquirePaint(MSTR& error) {
    if (!AcquireSession(error)) return nullptr;
    const bool created = !paint_;
    if (created) {
        auto state = std::make_unique<PaintState>();
        const SculptSettings& settings = SculptSettings::Get();
        sculpt::Image base;
        if (settings.Int(Prop::PaintSource) == 1) {  // Existing diffuse texture.
            INode* node = FindNode();
            BitmapTex* bitmap = node ? FindDiffuseBitmap(node->GetMtl()) : nullptr;
            const MCHAR* file = bitmap ? bitmap->GetMapName() : nullptr;
            if (!file || !*file || !PaintImageIO::Load(file, base, &error)) {
                if (error.Length() == 0) error = _T("The material has no diffuse bitmap to paint on. Choose Generated Texture.");
                return nullptr;
            }
            const int longest = std::max(base.width, base.height);
            if (longest > 4096) base = sculpt::resampleImage(base, base.width * 4096 / longest, base.height * 4096 / longest);
            for (std::size_t i = 3; i < base.rgba.size(); i += 4) base.rgba[i] = 255;  // The base is opaque.
            state->path = file;
            state->fromDiffuse = true;
        } else {
            const int size = kResolutions[std::min(std::max(settings.Int(Prop::PaintResolution), 0), 3)];
            base.resize(size, size, kGeneratedGrey, kGeneratedGrey, kGeneratedGrey, 255);
        }
        state->canvas.reset(std::move(base));
        paint_ = std::move(state);
    }
    if (!paint_->map.valid() || paint_->mapStamp != TopologyStamp() ||
        paint_->map.width() != paint_->canvas.width() || paint_->map.height() != paint_->canvas.height()) {
        if (!BuildPaintMap(error)) {
            if (created) paint_.reset();
            return nullptr;
        }
        RefreshDisplayOptions();
    }
    return paint_.get();
}

void SculptMeshObject::PaintChanged(bool interactive) {
    if (!paint_) return;
    paint_->canvas.updateComposite();
    // Whole-texture uploads: larger textures refresh less often while painting.
    const double pixels = static_cast<double>(paint_->canvas.width()) * paint_->canvas.height();
    const DWORD interval = static_cast<DWORD>(std::max(40.0, 40.0 * pixels / (2048.0 * 2048.0)));
    const DWORD now = GetTickCount();
    if (interactive && now - paint_->lastUpload < interval) {
        paint_->uploadPending = true;
        return;
    }
    paint_->lastUpload = now;
    paint_->uploadPending = false;
    if (display_) display_->UploadPaint();
    NotifyDependents(FOREVER, PART_DISPLAY, REFMSG_CHANGE);
}

void SculptMeshObject::PutPaintUndo(sculpt::PaintDelta delta, const MCHAR* undoName) {
    if (delta.empty() || !theHold.Holding()) return;
    theHold.Put(new SculptPaintRestore(this, std::move(delta), MSTR(undoName)));
}

void SculptMeshObject::ApplyPaintDelta(const sculpt::PaintDelta& delta, bool before) {
    if (!paint_) return;
    sculpt::applyPaintDelta(paint_->canvas, delta, before);
    PaintChanged(false);
    SculptUI::Refresh();
}

// --- Layers ------------------------------------------------------------------------------------

void SculptMeshObject::SetPaintLayersInternal(const std::vector<sculpt::PaintLayer>& layers, int active) {
    if (!paint_) return;
    paint_->canvas.layers() = layers;
    paint_->canvas.setActive(active);
    paint_->canvas.markAll();
    PaintChanged(false);
    SculptUI::Refresh();
}

void SculptMeshObject::ChangePaintLayers(const std::function<bool(std::vector<sculpt::PaintLayer>&, int&)>& edit,
                                         int undoName) {
    if (!paint_) return;
    sculpt::PaintCanvas& canvas = paint_->canvas;
    std::vector<sculpt::PaintLayer> before = canvas.layers(), after = before;
    const int beforeActive = canvas.active();
    int active = beforeActive;
    if (!edit(after, active)) return;
    SetPaintLayersInternal(after, active);
    if (!theHold.RestoreOrRedoing()) {
        theHold.Begin();
        theHold.Put(new SculptPaintStackRestore(this, std::move(before), beforeActive, after, canvas.active(),
                                                MSTR(GetString(undoName))));
        theHold.Accept(GetString(undoName));
    }
}

bool SculptMeshObject::NewPaintLayer() {
    MSTR error;
    if (!AcquirePaint(error)) {
        if (Interface* core = GetCOREInterface()) core->ReplacePrompt(error.data());
        return false;
    }
    sculpt::PaintCanvas& canvas = paint_->canvas;
    sculpt::PaintLayer layer;
    layer.image = std::make_shared<sculpt::Image>();
    layer.image->resize(canvas.width(), canvas.height(), 0, 0, 0, 0);
    layer.id = canvas.newLayerId();
    int number = static_cast<int>(canvas.layers().size()) + 1;
    layer.name = "Paint Layer " + std::to_string(number);
    ChangePaintLayers(
        [&layer](std::vector<sculpt::PaintLayer>& layers, int& active) {
            const int at = active >= 0 ? active + 1 : static_cast<int>(layers.size());
            layers.insert(layers.begin() + at, layer);
            active = at;
            return true;
        },
        IDS_UNDO_NEW_LAYER);
    return true;
}

bool SculptMeshObject::DeletePaintLayer() {
    if (!paint_ || paint_->canvas.active() < 0) return false;
    ChangePaintLayers(
        [](std::vector<sculpt::PaintLayer>& layers, int& active) {
            layers.erase(layers.begin() + active);
            active = std::min(active, static_cast<int>(layers.size()) - 1);
            return true;
        },
        IDS_UNDO_DELETE_LAYER);
    return true;
}

bool SculptMeshObject::ClearPaintLayer() {
    if (!paint_ || paint_->canvas.active() < 0) return false;
    const int w = paint_->canvas.width(), h = paint_->canvas.height();
    ChangePaintLayers(
        [w, h](std::vector<sculpt::PaintLayer>& layers, int& active) {
            auto image = std::make_shared<sculpt::Image>();
            image->resize(w, h, 0, 0, 0, 0);
            layers[static_cast<std::size_t>(active)].image = image;  // The old pixels stay in the undo record.
            return true;
        },
        IDS_UNDO_CLEAR_LAYER);
    return true;
}

bool SculptMeshObject::ClearAllPaintLayers() {
    if (!paint_ || paint_->canvas.layers().empty()) return false;
    ChangePaintLayers(
        [](std::vector<sculpt::PaintLayer>& layers, int& active) {
            layers.clear();
            active = -1;
            return true;
        },
        IDS_UNDO_CLEAR_LAYER);
    return true;
}

bool SculptMeshObject::MovePaintLayer(int direction) {
    if (!paint_) return false;
    const int from = paint_->canvas.active(), to = from + direction;
    if (from < 0 || to < 0 || to >= static_cast<int>(paint_->canvas.layers().size())) return false;
    ChangePaintLayers(
        [from, to](std::vector<sculpt::PaintLayer>& layers, int& active) {
            std::swap(layers[static_cast<std::size_t>(from)], layers[static_cast<std::size_t>(to)]);
            active = to;
            return true;
        },
        IDS_UNDO_MOVE_LAYER);
    return true;
}

bool SculptMeshObject::MergePaintLayers() {
    if (!paint_ || paint_->canvas.layers().empty()) return false;
    sculpt::PaintCanvas& canvas = paint_->canvas;
    canvas.markAll();
    canvas.updateComposite();
    const sculpt::Image merged = canvas.composite();
    std::vector<sculpt::PaintLayer> before = canvas.layers();
    const int beforeActive = canvas.active();
    sculpt::PaintDelta delta = sculpt::replaceImageContent(canvas, sculpt::PaintCanvas::kBaseId, merged);
    canvas.layers().clear();
    canvas.setActive(-1);
    canvas.markAll();
    PaintChanged(false);
    if (!theHold.RestoreOrRedoing()) {
        const MCHAR* name = GetString(IDS_UNDO_BAKE_LAYERS);
        theHold.Begin();
        theHold.Put(new SculptPaintRestore(this, std::move(delta), MSTR(name)));
        theHold.Put(new SculptPaintStackRestore(this, std::move(before), beforeActive, {}, -1, MSTR(name)));
        theHold.Accept(name);
    }
    SculptUI::Refresh();
    return true;
}

void SculptMeshObject::SelectPaintLayer(int index) {
    if (!paint_) return;
    paint_->canvas.setActive(index);
    SculptUI::Refresh();
}

void SculptMeshObject::SetPaintLayerEnabled(int index, bool on) {
    if (!paint_ || index < 0 || index >= static_cast<int>(paint_->canvas.layers().size())) return;
    ChangePaintLayers(
        [index, on](std::vector<sculpt::PaintLayer>& layers, int&) {
            if (layers[static_cast<std::size_t>(index)].enabled == on) return false;
            layers[static_cast<std::size_t>(index)].enabled = on;
            return true;
        },
        IDS_UNDO_LAYER_SETTINGS);
}

void SculptMeshObject::SetPaintLayerOpacityLive(int index, float opacity) {
    if (!paint_ || index < 0 || index >= static_cast<int>(paint_->canvas.layers().size())) return;
    if (!paint_->opacityDragging) {
        paint_->opacityDragStart = paint_->canvas.layers();
        paint_->opacityDragging = true;
    }
    paint_->canvas.layers()[static_cast<std::size_t>(index)].opacity = sculpt::clamp01(opacity);
    paint_->canvas.markAll();
    PaintChanged(true);
}

void SculptMeshObject::FinishPaintLayerOpacity() {
    if (!paint_ || !paint_->opacityDragging) return;
    paint_->opacityDragging = false;
    PaintChanged(false);
    if (!theHold.RestoreOrRedoing()) {
        const MCHAR* name = GetString(IDS_UNDO_LAYER_SETTINGS);
        theHold.Begin();
        theHold.Put(new SculptPaintStackRestore(this, std::move(paint_->opacityDragStart), paint_->canvas.active(),
                                                paint_->canvas.layers(), paint_->canvas.active(), MSTR(name)));
        theHold.Accept(name);
    }
    paint_->opacityDragStart.clear();
}

void SculptMeshObject::SetPaintLayerBlend(int index, sculpt::PaintBlend blend) {
    if (!paint_ || index < 0 || index >= static_cast<int>(paint_->canvas.layers().size())) return;
    ChangePaintLayers(
        [index, blend](std::vector<sculpt::PaintLayer>& layers, int&) {
            if (layers[static_cast<std::size_t>(index)].blend == blend) return false;
            layers[static_cast<std::size_t>(index)].blend = blend;
            return true;
        },
        IDS_UNDO_LAYER_SETTINGS);
}

void SculptMeshObject::SetPaintLayerName(int index, const std::string& name) {
    if (!paint_ || index < 0 || index >= static_cast<int>(paint_->canvas.layers().size()) || name.empty()) return;
    ChangePaintLayers(
        [index, name](std::vector<sculpt::PaintLayer>& layers, int&) {
            if (layers[static_cast<std::size_t>(index)].name == name) return false;
            layers[static_cast<std::size_t>(index)].name = name;
            return true;
        },
        IDS_UNDO_LAYER_SETTINGS);
}

bool SculptMeshObject::ImportPaintTexture(int index, MSTR& error) {
    if (!paint_ || index < 0 || index >= static_cast<int>(paint_->canvas.layers().size())) return false;
    const std::wstring path = PaintImageIO::AskOpenImage(L"Import Texture");
    if (path.empty()) return false;
    sculpt::Image image;
    if (!PaintImageIO::Load(path, image, &error)) return false;
    sculpt::PaintCanvas& canvas = paint_->canvas;
    if (image.width != canvas.width() || image.height != canvas.height())
        image = sculpt::resampleImage(image, canvas.width(), canvas.height());
    const std::uint64_t id = canvas.layers()[static_cast<std::size_t>(index)].id;
    sculpt::PaintDelta delta = sculpt::replaceImageContent(canvas, id, image);
    PaintChanged(false);
    if (!theHold.RestoreOrRedoing()) {
        const MCHAR* name = _T("Import Texture");
        theHold.Begin();
        PutPaintUndo(std::move(delta), name);
        theHold.Accept(name);
    }
    return true;
}

// --- Adjustments --------------------------------------------------------------------------------

bool SculptMeshObject::BeginPaintAdjustment() {
    if (!paint_ || paint_->canvas.active() < 0) return false;
    sculpt::PaintCanvas& canvas = paint_->canvas;
    paint_->adjustImage = canvas.targetId();
    paint_->adjustSource = std::make_shared<sculpt::Image>(*canvas.target());
    return true;
}

void SculptMeshObject::PreviewPaintAdjustment(Adjustment kind, const float values[5]) {
    if (!paint_ || !paint_->adjustSource) return;
    sculpt::Image* image = paint_->canvas.imageById(paint_->adjustImage);
    if (!image) return;
    const sculpt::Image& source = *paint_->adjustSource;
    switch (kind) {
        case Adjustment::HueSaturation:
            sculpt::adjustHsl(source, *image, values[0], values[1], values[2]);
            break;
        case Adjustment::BrightnessContrast:
            sculpt::adjustBrightnessContrast(source, *image, values[0], values[1]);
            break;
        case Adjustment::Levels:
            sculpt::adjustLevels(source, *image, values[0], values[1], values[2], values[3], values[4]);
            break;
    }
    paint_->canvas.markAll();
    PaintChanged(true);
}

void SculptMeshObject::EndPaintAdjustment(bool commit) {
    if (!paint_ || !paint_->adjustSource) return;
    sculpt::Image* image = paint_->canvas.imageById(paint_->adjustImage);
    std::shared_ptr<sculpt::Image> source = std::move(paint_->adjustSource);
    paint_->adjustSource.reset();
    if (!image) return;
    if (!commit) {
        *image = *source;
    } else if (!theHold.RestoreOrRedoing()) {
        sculpt::PaintDelta delta = sculpt::imageDelta(paint_->adjustImage, *source, *image);
        const MCHAR* name = _T("Paint Layer Adjustment");
        theHold.Begin();
        PutPaintUndo(std::move(delta), name);
        theHold.Accept(name);
    }
    paint_->canvas.markAll();
    PaintChanged(false);
}

// --- Files and material ------------------------------------------------------------------------

void SculptMeshObject::ShowTextureOnNode(const std::wstring& path) {
    INode* node = FindNode();
    Interface* core = GetCOREInterface();
    if (!node || !core) return;
    Mtl* current = node->GetMtl();
    if (BitmapTex* existing = FindBitmapByPath(current, path)) {  // Already shown: reload the file.
        existing->ReloadBitmapAndUpdate();
        core->RedrawViews(core->GetTime());
        return;
    }
    if (!paint_->keeper) paint_->keeper = new PaintMaterialKeeper(current);
    StdMat2* mtl = NewDefaultStdMat();
    mtl->SetName(kPaintMaterialName);
    BitmapTex* bitmap = NewDefaultBitmapTex();
    bitmap->SetMapName(path.c_str());
    mtl->SetSubTexmap(ID_DI, bitmap);
    mtl->EnableMap(ID_DI, TRUE);
    node->SetMtl(mtl);
    core->ActivateTexture(bitmap, mtl);
    core->RedrawViews(core->GetTime());
}

bool SculptMeshObject::SavePaintTexture(bool chooseFile, MSTR& error) {
    if (!paint_) {
        error = _T("Nothing has been painted yet.");
        return false;
    }
    paint_->canvas.updateComposite();
    std::wstring path = paint_->path;
    if (chooseFile || path.empty()) {
        path = PaintImageIO::AskSaveImage(L"Save Paint Texture", path.empty() ? std::wstring(L"SculptMeshPaint.png") : path);
        if (path.empty()) return false;
    }
    if (!PaintImageIO::Save(path, paint_->canvas.composite(), &error)) return false;
    paint_->path = path;
    ShowTextureOnNode(path);
    SculptUI::Refresh();
    return true;
}

bool SculptMeshObject::ReplacePaintTexture(MSTR& error) {
    if (!AcquirePaint(error)) return false;
    const std::wstring path = PaintImageIO::AskOpenImage(L"Replace Texture");
    if (path.empty()) return false;
    sculpt::Image image;
    if (!PaintImageIO::Load(path, image, &error)) return false;
    sculpt::PaintCanvas& canvas = paint_->canvas;
    if (image.width != canvas.width() || image.height != canvas.height())
        image = sculpt::resampleImage(image, canvas.width(), canvas.height());
    for (std::size_t i = 3; i < image.rgba.size(); i += 4) image.rgba[i] = 255;
    sculpt::PaintDelta delta = sculpt::replaceImageContent(canvas, sculpt::PaintCanvas::kBaseId, image);
    PaintChanged(false);
    if (!theHold.RestoreOrRedoing()) {
        const MCHAR* name = _T("Replace Texture");
        theHold.Begin();
        PutPaintUndo(std::move(delta), name);
        theHold.Accept(name);
    }
    return true;
}

bool SculptMeshObject::ResizePaintTexture(int size, MSTR& error) {
    if (!paint_ || paint_->canvas.width() == size) return true;
    if (paint_->fromDiffuse) return true;  // The diffuse texture keeps its own size.
    sculpt::PaintCanvas& canvas = paint_->canvas;
    sculpt::Image base = sculpt::resampleImage(canvas.base(), size, size);
    std::vector<sculpt::PaintLayer> layers = canvas.layers();
    const int active = canvas.active();
    for (sculpt::PaintLayer& layer : layers)
        if (layer.image) layer.image = std::make_shared<sculpt::Image>(sculpt::resampleImage(*layer.image, size, size));
    canvas.reset(std::move(base));
    canvas.layers() = layers;
    canvas.setActive(active);
    canvas.markAll();
    paint_->map = sculpt::TexelMap();  // Earlier paint undo records no longer fit and are ignored.
    if (!AcquirePaint(error)) return false;
    PaintChanged(false);
    return true;
}

bool SculptMeshObject::HasOriginalMaterial() const { return paint_ && paint_->keeper; }

bool SculptMeshObject::RestoreMaterial() {
    if (!paint_ || !paint_->keeper) return false;
    INode* node = FindNode();
    if (!node) return false;
    node->SetMtl(paint_->keeper->Get());
    paint_->keeper->DeleteAllRefs();
    paint_->keeper->DeleteThis();
    paint_->keeper = nullptr;
    Redraw();
    SculptUI::Refresh();
    return true;
}

MSTR SculptMeshObject::PaintStatus() const {
    MSTR text;
    if (!paint_) {
        text = _T("Paint: no texture yet (the first stroke creates it)");
        return text;
    }
    const sculpt::PaintCanvas& canvas = paint_->canvas;
    std::wstring file = paint_->path;
    const std::size_t slash = file.find_last_of(L"\\/");
    if (slash != std::wstring::npos) file = file.substr(slash + 1);
    text.printf(_T("%dx%d  \x2022  %d layer(s)  \x2022  %s"), canvas.width(), canvas.height(),
                static_cast<int>(canvas.layers().size()), file.empty() ? _T("not saved") : file.c_str());
    return text;
}
