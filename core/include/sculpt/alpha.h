// SculptCore — grayscale brush alphas (height stamps).
#pragma once

#include <vector>

namespace sculpt {

class Alpha {
public:
    static constexpr int kBuiltinCount = 4;

    Alpha() = default;
    // `values` holds width*height samples in [0,1], row-major, top row first.
    Alpha(int width, int height, std::vector<float> values);

    // The four built-in alphas: 0 soft round, 1 small dot, 2 square, 3 round.
    static Alpha builtin(int index, int size = 128);

    bool empty() const { return values_.empty(); }
    int width() const { return width_; }
    int height() const { return height_; }
    const std::vector<float>& values() const { return values_; }

    // Bilinear sample at u,v in [0,1] (u to the right, v downwards); 0 outside.
    float sample(float u, float v) const;

private:
    int width_ = 0;
    int height_ = 0;
    std::vector<float> values_;
};

}  // namespace sculpt
