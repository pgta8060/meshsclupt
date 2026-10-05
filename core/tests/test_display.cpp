#include <algorithm>

#include "mesh_gen.h"
#include "sculpt/display_chunks.h"
#include "sculpt/parallel.h"
#include "sculpt/session.h"
#include "test_framework.h"

using namespace sculpt;

TEST_CASE(display_chunks_cover_mesh_once) {
    Mesh mesh;
    REQUIRE(mesh.build(sctest::makeUvSphere(60, 80, 5.0f)));
    std::vector<std::uint8_t> hidden(mesh.faceCount(), 0u);
    hidden[3] = 1u;  // One hidden polygon (a cap triangle).
    DisplayChunks chunks;
    chunks.build(mesh, 500, &hidden);
    REQUIRE(chunks.chunks().size() > 4u);

    std::size_t triangles = 0;
    std::vector<std::uint32_t> seenVertexChunks(mesh.vertexCount(), 0u);
    for (std::uint32_t c = 0; c < chunks.chunks().size(); ++c) {
        const DisplayChunk& chunk = chunks.chunks()[c];
        CHECK(chunk.triangles.size() % 3 == 0u);
        CHECK(chunk.edges.size() % 2 == 0u);
        triangles += chunk.triangles.size() / 3;
        for (std::uint32_t local : chunk.triangles) CHECK(local < chunk.vertices.size());
        for (std::uint32_t local : chunk.edges) CHECK(local < chunk.vertices.size());
        for (std::uint32_t v : chunk.vertices) {
            ++seenVertexChunks[v];
            const Span<std::uint32_t> owners = chunks.chunksOfVertex(v);
            CHECK(std::find(owners.begin(), owners.end(), c) != owners.end());
        }
    }
    CHECK_EQ(triangles, static_cast<std::size_t>(mesh.triangleCount() - 1));
    for (std::uint32_t v = 0; v < mesh.vertexCount(); ++v) CHECK_EQ(chunks.chunksOfVertex(v).size(), static_cast<std::size_t>(seenVertexChunks[v]));

    CHECK_EQ(chunks.takeDirty().size(), chunks.chunks().size());  // Everything dirty after build.
    CHECK(!chunks.anyDirty());
    const std::uint32_t v = mesh.vertexCount() / 2;
    chunks.markVertices(std::vector<std::uint32_t>{v, v});
    CHECK_EQ(chunks.takeDirty().size(), chunks.chunksOfVertex(v).size());
}

TEST_CASE(parallel_results_match_serial) {
    auto run = [](unsigned threads) {
        setParallelThreadCount(threads);
        SculptSession session;
        session.build(sctest::makeUvSphere(200, 200, 10.0f));
        for (int type = 0; type < static_cast<int>(BrushType::Count); ++type) {
            BrushSettings s;
            s.type = static_cast<BrushType>(type);
            session.beginStroke();
            for (int i = 0; i < 20; ++i) {
                Dab d;
                d.center = {-3.0f + 0.3f * static_cast<float>(i), 0.0f, 10.0f};
                d.radius = 4.0f;
                session.applyDab(s, d);
            }
            session.endStroke();
        }
        return session.mesh().positions();
    };
    const std::vector<Vec3> serial = run(1);
    const std::vector<Vec3> parallel = run(8);
    setParallelThreadCount(8);
    CHECK(serial == parallel);

    std::vector<int> hits(100000, 0);
    parallelFor(hits.size(), 1000, [&](std::size_t b, std::size_t e) {
        for (std::size_t i = b; i < e; ++i) ++hits[i];
    });
    CHECK(std::all_of(hits.begin(), hits.end(), [](int h) { return h == 1; }));
}
