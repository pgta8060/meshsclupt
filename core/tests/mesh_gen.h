// Procedural meshes and a portable RNG for tests and benchmarks.
#pragma once

#include <cmath>
#include <cstdint>

#include "sculpt/mesh.h"

namespace sctest {

// xorshift64* — identical sequence on every compiler/platform.
class Rng {
public:
    explicit Rng(std::uint64_t seed) : state_(seed ? seed : 0x9E3779B97F4A7C15ull) {}
    std::uint64_t next() {
        state_ ^= state_ >> 12;
        state_ ^= state_ << 25;
        state_ ^= state_ >> 27;
        return state_ * 0x2545F4914F6CDD1Dull;
    }
    float uniform() { return static_cast<float>(next() >> 40) / static_cast<float>(1ull << 24); }  // [0,1)
    float uniform(float lo, float hi) { return lo + (hi - lo) * uniform(); }

private:
    std::uint64_t state_;
};

// Flat quad grid in the XY plane (z = 0), centred on the origin, normals +Z.
// `cells` quads per side; side length `size`.
inline sculpt::MeshInput makeGrid(std::uint32_t cells, float size) {
    sculpt::MeshInput in;
    const std::uint32_t n = cells + 1;
    const float step = size / static_cast<float>(cells);
    const float half = size * 0.5f;
    for (std::uint32_t j = 0; j < n; ++j)
        for (std::uint32_t i = 0; i < n; ++i)
            in.positions.push_back({static_cast<float>(i) * step - half, static_cast<float>(j) * step - half, 0.0f});
    for (std::uint32_t j = 0; j < cells; ++j) {
        for (std::uint32_t i = 0; i < cells; ++i) {
            const std::uint32_t a = j * n + i;
            in.faceSizes.push_back(4);
            in.faceVerts.insert(in.faceVerts.end(), {a, a + 1, a + n + 1, a + n});
        }
    }
    return in;
}

inline std::uint32_t gridIndex(std::uint32_t cells, std::uint32_t i, std::uint32_t j) { return j * (cells + 1) + i; }

// Closed UV sphere: quads between latitude rings, triangle fans at the poles,
// outward winding. Vertex 0 is the north pole, the last vertex the south pole.
inline sculpt::MeshInput makeUvSphere(std::uint32_t rings, std::uint32_t segments, float radius) {
    sculpt::MeshInput in;
    const float pi = 3.14159265358979323846f;
    in.positions.push_back({0.0f, 0.0f, radius});
    for (std::uint32_t k = 1; k < rings; ++k) {
        const float theta = pi * static_cast<float>(k) / static_cast<float>(rings);
        const float z = radius * std::cos(theta);
        const float rr = radius * std::sin(theta);
        for (std::uint32_t s = 0; s < segments; ++s) {
            const float phi = 2.0f * pi * static_cast<float>(s) / static_cast<float>(segments);
            in.positions.push_back({rr * std::cos(phi), rr * std::sin(phi), z});
        }
    }
    const auto south = static_cast<std::uint32_t>(in.positions.size());
    in.positions.push_back({0.0f, 0.0f, -radius});

    auto ring = [segments](std::uint32_t k, std::uint32_t s) { return 1 + (k - 1) * segments + (s % segments); };
    for (std::uint32_t s = 0; s < segments; ++s) {
        in.faceSizes.push_back(3);
        in.faceVerts.insert(in.faceVerts.end(), {0u, ring(1, s), ring(1, s + 1)});
    }
    for (std::uint32_t k = 1; k + 1 < rings; ++k) {
        for (std::uint32_t s = 0; s < segments; ++s) {
            in.faceSizes.push_back(4);
            in.faceVerts.insert(in.faceVerts.end(), {ring(k, s), ring(k + 1, s), ring(k + 1, s + 1), ring(k, s + 1)});
        }
    }
    for (std::uint32_t s = 0; s < segments; ++s) {
        in.faceSizes.push_back(3);
        in.faceVerts.insert(in.faceVerts.end(), {south, ring(rings - 1, s + 1), ring(rings - 1, s)});
    }
    return in;
}

// Unit cube centred on the origin (side 2), outward-facing quads.
inline sculpt::MeshInput makeCube() {
    sculpt::MeshInput in;
    for (std::uint32_t i = 0; i < 8; ++i)
        in.positions.push_back({(i & 1) ? 1.0f : -1.0f, (i & 2) ? 1.0f : -1.0f, (i & 4) ? 1.0f : -1.0f});
    const std::uint32_t faces[6][4] = {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4},
                                       {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
    for (const auto& f : faces) {
        in.faceSizes.push_back(4);
        in.faceVerts.insert(in.faceVerts.end(), {f[0], f[1], f[2], f[3]});
    }
    return in;
}

}  // namespace sctest
