#include "sculpt/symmetry.h"

#include <algorithm>

namespace sculpt {

std::vector<Mat3> symmetryTransforms(const SymmetrySettings& settings) {
    std::vector<Mat3> mirrors{Mat3::identity()};
    const bool flags[3] = {settings.mirrorX, settings.mirrorY, settings.mirrorZ};
    for (int axis = 0; axis < 3; ++axis) {
        if (!flags[axis]) continue;
        Vec3 d{1, 1, 1};
        if (axis == 0) d.x = -1.0f;
        if (axis == 1) d.y = -1.0f;
        if (axis == 2) d.z = -1.0f;
        const Mat3 reflect = Mat3::diagonal(d);
        const std::size_t n = mirrors.size();
        for (std::size_t i = 0; i < n; ++i) mirrors.push_back(reflect * mirrors[i]);
    }

    std::vector<Mat3> out;
    const int count = settings.radial ? std::min(std::max(settings.radialCount, 2), 32) : 1;
    const int axis = std::min(std::max(settings.radialAxis, 0), 2);
    for (int k = 0; k < count; ++k) {
        const Mat3 rot = Mat3::rotation(axis, 6.28318530718f * static_cast<float>(k) / static_cast<float>(count));
        for (const Mat3& m : mirrors) {
            const Mat3 t = rot * m;
            const bool duplicate =
                std::any_of(out.begin(), out.end(), [&](const Mat3& existing) { return existing.nearlyEquals(t); });
            if (!duplicate) out.push_back(t);
            if (out.size() >= 128) return out;
        }
    }
    return out;
}

}  // namespace sculpt
