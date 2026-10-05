#include "sculpt/layers.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "sculpt/half.h"
#include "sculpt/parallel.h"

namespace sculpt {

Vec3 LayerStack::offset(std::size_t v) const {
    Vec3 sum;
    for (std::size_t i = 0; i < layers.size(); ++i) {
        const float f = factor(i);
        if (f != 0.0f && v < layers[i].delta.size()) sum += layers[i].delta[v] * f;
    }
    return sum;
}

int LayerStack::displaceLayer() const {
    for (std::size_t i = 0; i < layers.size(); ++i)
        if (layers[i].displace) return static_cast<int>(i);
    return -1;
}

bool LayerStack::sizesMatch(std::size_t vertexCount) const {
    for (const SculptLayer& layer : layers)
        if (layer.delta.size() != vertexCount) return false;
    return true;
}

std::size_t LayerStack::memoryBytes() const {
    std::size_t bytes = sizeof(*this);
    for (const SculptLayer& layer : layers) bytes += layer.delta.size() * sizeof(Vec3) + layer.name.size();
    return bytes;
}

namespace {

template <class T>
void Put(std::vector<std::uint8_t>& out, const T& v) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
    out.insert(out.end(), p, p + sizeof(T));
}

template <class T>
bool Get(const std::uint8_t* data, std::size_t size, std::size_t& pos, T& v) {
    if (size - pos < sizeof(T)) return false;
    std::memcpy(&v, data + pos, sizeof(T));
    pos += sizeof(T);
    return true;
}

constexpr std::uint32_t kLayersMagic = 0x534c5952u;  // "SLYR"

}  // namespace

std::vector<std::uint8_t> serializeLayers(const LayerStack& stack) {
    std::vector<std::uint8_t> out;
    Put(out, kLayersMagic);
    Put(out, static_cast<std::uint32_t>(1));  // Version.
    Put(out, static_cast<std::int32_t>(stack.active));
    Put(out, static_cast<std::uint32_t>(stack.layers.size()));
    for (const SculptLayer& layer : stack.layers) {
        Put(out, static_cast<std::uint32_t>(layer.name.size()));
        out.insert(out.end(), layer.name.begin(), layer.name.end());
        Put(out, static_cast<std::uint8_t>((layer.enabled ? 1u : 0u) | (layer.displace ? 2u : 0u)));
        Put(out, layer.strength);
        float scale = 0.0f;
        for (const Vec3& d : layer.delta) scale = std::max({scale, std::fabs(d.x), std::fabs(d.y), std::fabs(d.z)});
        if (!(scale > 0.0f) || !std::isfinite(scale)) scale = 1.0f;
        Put(out, scale);
        Put(out, static_cast<std::uint32_t>(layer.delta.size()));
        for (const Vec3& d : layer.delta) {
            Put(out, floatToHalf(d.x / scale));
            Put(out, floatToHalf(d.y / scale));
            Put(out, floatToHalf(d.z / scale));
        }
    }
    return out;
}

bool deserializeLayers(const std::uint8_t* data, std::size_t size, LayerStack& out) {
    std::size_t pos = 0;
    std::uint32_t magic = 0, version = 0, count = 0;
    std::int32_t active = -1;
    if (!Get(data, size, pos, magic) || magic != kLayersMagic || !Get(data, size, pos, version) || version != 1 ||
        !Get(data, size, pos, active) || !Get(data, size, pos, count) || count > 4096)
        return false;
    LayerStack stack;
    stack.layers.resize(count);
    for (SculptLayer& layer : stack.layers) {
        std::uint32_t nameLength = 0, n = 0;
        std::uint8_t flags = 0;
        float scale = 1.0f;
        if (!Get(data, size, pos, nameLength) || nameLength > 4096 || size - pos < nameLength) return false;
        layer.name.assign(reinterpret_cast<const char*>(data + pos), nameLength);
        pos += nameLength;
        if (!Get(data, size, pos, flags) || !Get(data, size, pos, layer.strength) || !Get(data, size, pos, scale) ||
            !Get(data, size, pos, n) || !std::isfinite(layer.strength) || !std::isfinite(scale) || (size - pos) / 6 < n)
            return false;
        layer.enabled = (flags & 1u) != 0;
        layer.displace = (flags & 2u) != 0;
        layer.strength = std::min(std::max(layer.strength, 0.0f), kMaxLayerStrength);
        layer.delta.resize(n);
        for (Vec3& d : layer.delta) {
            std::uint16_t hx = 0, hy = 0, hz = 0;
            Get(data, size, pos, hx);
            Get(data, size, pos, hy);
            Get(data, size, pos, hz);
            d = Vec3{halfToFloat(hx), halfToFloat(hy), halfToFloat(hz)} * scale;
        }
    }
    stack.active = active >= 0 && active < static_cast<std::int32_t>(count) ? active : -1;
    out = std::move(stack);
    return true;
}

float layerStrengthFromSlider(float t) {
    t = clamp01(t);
    if (t <= 0.5f) return 2.0f * t;
    const float k = (t - 0.5f) * 2.0f;  // 0..1
    return 1.0f + (kMaxLayerStrength - 1.0f) * k * k;  // Progressive boost.
}

float layerSliderFromStrength(float strength) {
    strength = std::min(std::max(strength, 0.0f), kMaxLayerStrength);
    if (strength <= 1.0f) return 0.5f * strength;
    return 0.5f + 0.5f * std::sqrt((strength - 1.0f) / (kMaxLayerStrength - 1.0f));
}

float sampleWrapped(const Alpha& image, float u, float v) {
    const int w = image.width(), h = image.height();
    if (w <= 0 || h <= 0 || !isFinite(u) || !isFinite(v)) return 0.5f;
    const float x = (u - std::floor(u)) * static_cast<float>(w) - 0.5f;
    const float y = (v - std::floor(v)) * static_cast<float>(h) - 0.5f;
    const float fx = std::floor(x), fy = std::floor(y);
    const float tx = x - fx, ty = y - fy;
    auto at = [&](int xi, int yi) {
        xi = ((xi % w) + w) % w;
        yi = ((yi % h) + h) % h;
        return image.values()[static_cast<std::size_t>(yi) * static_cast<std::size_t>(w) + static_cast<std::size_t>(xi)];
    };
    const int x0 = static_cast<int>(fx), y0 = static_cast<int>(fy);
    const float top = at(x0, y0) * (1.0f - tx) + at(x0 + 1, y0) * tx;
    const float bottom = at(x0, y0 + 1) * (1.0f - tx) + at(x0 + 1, y0 + 1) * tx;
    return top * (1.0f - ty) + bottom * ty;
}

Alpha blurImage(const Alpha& image, float radiusPx) {
    const int w = image.width(), h = image.height();
    if (w <= 0 || h <= 0 || !(radiusPx >= 0.5f)) return image;
    const int r = std::max(1, static_cast<int>(std::lround(radiusPx / 1.732f)));  // Three passes ~ Gaussian sigma.
    std::vector<float> a = image.values(), b(a.size());
    auto passH = [&](const std::vector<float>& src, std::vector<float>& dst) {
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                float sum = 0.0f;
                for (int k = -r; k <= r; ++k) {
                    const int xi = ((x + k) % w + w) % w;  // Wraps like the sampling.
                    sum += src[static_cast<std::size_t>(y) * w + xi];
                }
                dst[static_cast<std::size_t>(y) * w + x] = sum / static_cast<float>(2 * r + 1);
            }
    };
    auto passV = [&](const std::vector<float>& src, std::vector<float>& dst) {
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                float sum = 0.0f;
                for (int k = -r; k <= r; ++k) {
                    const int yi = ((y + k) % h + h) % h;
                    sum += src[static_cast<std::size_t>(yi) * w + x];
                }
                dst[static_cast<std::size_t>(y) * w + x] = sum / static_cast<float>(2 * r + 1);
            }
    };
    for (int pass = 0; pass < 3; ++pass) {
        passH(a, b);
        passV(b, a);
    }
    return Alpha(w, h, std::move(a));
}

void computeDisplacement(const std::vector<Vec3>& positions, const std::vector<Vec3>& normals,
                         const std::vector<Vec3>* uvs, const Aabb& bounds, const Alpha& image,
                         const DisplaceSettings& s, std::vector<Vec3>& offsets) {
    offsets.assign(positions.size(), Vec3());
    if (image.empty() || normals.size() != positions.size()) return;
    const bool useUv = !s.triplanar && uvs && uvs->size() == positions.size();
    const Vec3 extent = bounds.empty() ? Vec3{1, 1, 1} : bounds.extent();
    const float size = std::max({extent.x, extent.y, extent.z, 1e-6f});
    const Vec3 origin = bounds.empty() ? Vec3() : bounds.lo;
    auto sample = [&](float u, float v) {
        // Map v points up, image rows go down.
        return sampleWrapped(image, u * s.tileU + s.offsetU, 1.0f - (v * s.tileV + s.offsetV));
    };
    auto value = [&](float h) {
        h = (h - 0.5f) * s.contrast + 0.5f;
        return h - (0.5f + s.waterLevel);
    };
    parallelFor(positions.size(), 4096, [&](std::size_t begin, std::size_t end) {
        for (std::size_t v = begin; v < end; ++v) {
            const Vec3& n = normals[v];
            if (lengthSq(n) == 0.0f) continue;
            float h;
            if (useUv) {
                h = sample((*uvs)[v].x, (*uvs)[v].y);
            } else {
                const Vec3 p = (positions[v] - origin) / size;
                float wx = n.x * n.x, wy = n.y * n.y, wz = n.z * n.z;
                wx *= wx;
                wy *= wy;
                wz *= wz;
                const float total = wx + wy + wz;
                h = (sample(p.y, p.z) * wx + sample(p.x, p.z) * wy + sample(p.x, p.y) * wz) / (total > 0.0f ? total : 1.0f);
            }
            offsets[v] = n * (value(h) * s.strength);
        }
    });
}

}  // namespace sculpt
