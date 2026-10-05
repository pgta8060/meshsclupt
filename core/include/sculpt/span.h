// SculptCore — minimal read-only view over contiguous memory (C++17 has no std::span).
#pragma once

#include <cstddef>
#include <vector>

namespace sculpt {

template <class T>
class Span {
public:
    constexpr Span() = default;
    constexpr Span(const T* data, std::size_t size) : data_(data), size_(size) {}
    Span(const std::vector<T>& v) : data_(v.data()), size_(v.size()) {}  // NOLINT: implicit by design

    constexpr const T* begin() const { return data_; }
    constexpr const T* end() const { return data_ + size_; }
    constexpr const T* data() const { return data_; }
    constexpr std::size_t size() const { return size_; }
    constexpr bool empty() const { return size_ == 0; }
    constexpr const T& operator[](std::size_t i) const { return data_[i]; }

private:
    const T* data_ = nullptr;
    std::size_t size_ = 0;
};

}  // namespace sculpt
