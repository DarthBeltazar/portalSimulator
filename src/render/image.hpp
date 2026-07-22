#pragma once

#include <vector>

#include <Eigen/Core>

// A plain RGB float framebuffer. No format conversion, no tone mapping — those are display
// concerns; Phase 2's acceptance tests (docs/phase2-rendering.md §5) compare raw
// radiance/solid-angle values against docs/PHYSICS.md §2's closed form, not display pixels.

namespace render {

class Image {
public:
    Image(int width, int height)
        : width_(width),
          height_(height),
          pixels_(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), Eigen::Vector3d::Zero()) {}

    int width() const { return width_; }
    int height() const { return height_; }

    Eigen::Vector3d& at(int x, int y) { return pixels_[static_cast<std::size_t>(y) * width_ + static_cast<std::size_t>(x)]; }
    const Eigen::Vector3d& at(int x, int y) const {
        return pixels_[static_cast<std::size_t>(y) * width_ + static_cast<std::size_t>(x)];
    }

private:
    int width_;
    int height_;
    std::vector<Eigen::Vector3d> pixels_;
};

} // namespace render
