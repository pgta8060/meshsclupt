// SculptCore micro-benchmark: build + raycast + dab throughput on a dense sphere.
// Usage: sculpt_bench [rings] [segments]   (default 1000 x 1000 ≈ 2M triangles)
#include <chrono>
#include <cstdio>
#include <cstdlib>

#include "mesh_gen.h"
#include "sculpt/session.h"

using namespace sculpt;
using Clock = std::chrono::steady_clock;

namespace {

double msSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

}  // namespace

int main(int argc, char** argv) {
    const auto rings = static_cast<std::uint32_t>(argc > 1 ? std::atoi(argv[1]) : 1000);
    const auto segments = static_cast<std::uint32_t>(argc > 2 ? std::atoi(argv[2]) : 1000);
    if (rings < 3 || segments < 3) {
        std::fprintf(stderr, "rings and segments must be >= 3\n");
        return 2;
    }

    auto t0 = Clock::now();
    MeshInput input = sctest::makeUvSphere(rings, segments, 10.0f);
    const double genMs = msSince(t0);

    SculptSession session;
    t0 = Clock::now();
    std::string error;
    if (!session.build(std::move(input), &error)) {
        std::fprintf(stderr, "build failed: %s\n", error.c_str());
        return 1;
    }
    const double buildMs = msSince(t0);
    const Mesh& mesh = session.mesh();
    std::printf("mesh: %u verts, %u polys, %u tris  (generate %.1f ms, session build %.1f ms, %.1f MB)\n",
                mesh.vertexCount(), mesh.faceCount(), mesh.triangleCount(), genMs, buildMs,
                static_cast<double>(session.memoryBytes()) / (1024.0 * 1024.0));

    sctest::Rng rng(1);
    const int rays = 100000;
    int hits = 0;
    t0 = Clock::now();
    for (int i = 0; i < rays; ++i) {
        const Vec3 target{rng.uniform(-8, 8), rng.uniform(-8, 8), 0.0f};
        RayHit hit;
        if (session.raycast(Ray{{target.x, target.y, 100.0f}, {0, 0, -1}}, hit)) ++hits;
    }
    const double rayMs = msSince(t0);
    std::printf("raycast: %d rays in %.1f ms (%.2f us/ray, %d hits)\n", rays, rayMs, 1000.0 * rayMs / rays, hits);

    for (int b = 0; b < static_cast<int>(BrushType::Count); ++b) {
        BrushSettings s;
        s.type = static_cast<BrushType>(b);
        const float radius = 1.0f;  // ~5% of the sphere diameter.
        const int dabs = 500;
        std::size_t moved = 0;
        session.beginStroke();
        t0 = Clock::now();
        for (int i = 0; i < dabs; ++i) {
            const float x = -7.0f + 14.0f * static_cast<float>(i) / dabs;
            RayHit hit;
            if (!session.raycast(Ray{{x, 0.0f, 100.0f}, {0, 0, -1}}, hit)) continue;
            Dab d;
            d.center = hit.position;
            d.radius = radius;
            moved += session.applyDab(s, d);
        }
        const double strokeMs = msSince(t0);
        t0 = Clock::now();
        StrokeDelta delta = session.endStroke();
        const double endMs = msSince(t0);
        session.applyDelta(delta, true);
        session.clearDirty();
        std::printf("%-8s %d dabs: %.1f ms (%.3f ms/dab, %.0f verts/dab), endStroke %.1f ms, undo %.1f KB\n",
                    brushName(s.type), dabs, strokeMs, strokeMs / dabs, static_cast<double>(moved) / dabs, endMs,
                    static_cast<double>(delta.memoryBytes()) / 1024.0);
    }
    return 0;
}
