// SculptCore — mirror / radial symmetry as a list of object-space transforms.
#pragma once

#include <vector>

#include "sculpt/math.h"

namespace sculpt {

struct SymmetrySettings {
    bool mirrorX = false;
    bool mirrorY = false;
    bool mirrorZ = false;
    bool radial = false;
    int radialCount = 6;  // 2..32
    int radialAxis = 2;   // 0 X, 1 Y, 2 Z
};

// Every transform a dab is repeated with. The first one is always identity.
// Mirrors reflect through the object's origin planes; radial copies rotate
// about the chosen object axis. Combinations are multiplied out (max 128).
std::vector<Mat3> symmetryTransforms(const SymmetrySettings& settings);

}  // namespace sculpt
