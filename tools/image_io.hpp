#pragma once

#include <string>

#include "render/image.hpp"

// Writes a render::Image to a 24-bit uncompressed BMP for visual inspection (this is a
// demo/debug tool, not part of the acceptance-test pipeline in docs/phase2-rendering.md --
// those compare raw radiance values, not display pixels, per render/image.hpp's own comment).
// Applies a Reinhard tonemap (x / (1+x)) then a 1/2.2 gamma so a physically-unbounded radiance
// image degrades gracefully to 8-bit display range instead of just clipping to flat white.

namespace tools {

void writeBmp(const std::string& path, const render::Image& image, double exposure = 1.0);

} // namespace tools
