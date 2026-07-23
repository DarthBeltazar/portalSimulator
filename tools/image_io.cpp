#include "image_io.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace tools {

namespace {

void putU16(std::vector<unsigned char>& buf, std::uint16_t v) {
    buf.push_back(static_cast<unsigned char>(v & 0xFF));
    buf.push_back(static_cast<unsigned char>((v >> 8) & 0xFF));
}

void putU32(std::vector<unsigned char>& buf, std::uint32_t v) {
    buf.push_back(static_cast<unsigned char>(v & 0xFF));
    buf.push_back(static_cast<unsigned char>((v >> 8) & 0xFF));
    buf.push_back(static_cast<unsigned char>((v >> 16) & 0xFF));
    buf.push_back(static_cast<unsigned char>((v >> 24) & 0xFF));
}

void putI32(std::vector<unsigned char>& buf, std::int32_t v) { putU32(buf, static_cast<std::uint32_t>(v)); }

unsigned char toByte(double radiance) {
    double tonemapped = radiance / (1.0 + radiance); // Reinhard
    double gammaCorrected = std::pow(std::clamp(tonemapped, 0.0, 1.0), 1.0 / 2.2);
    return static_cast<unsigned char>(std::lround(gammaCorrected * 255.0));
}

} // namespace

void writeBmp(const std::string& path, const render::Image& image, double exposure) {
    const int width = image.width();
    const int height = image.height();
    const int rowSize = width * 3;
    const int paddedRowSize = (rowSize + 3) & ~3;
    const std::uint32_t pixelDataSize = static_cast<std::uint32_t>(paddedRowSize) * static_cast<std::uint32_t>(height);
    const std::uint32_t fileSize = 14 + 40 + pixelDataSize;

    std::vector<unsigned char> buf;
    buf.reserve(fileSize);

    // BITMAPFILEHEADER
    buf.push_back('B');
    buf.push_back('M');
    putU32(buf, fileSize);
    putU16(buf, 0);
    putU16(buf, 0);
    putU32(buf, 54); // pixel data offset

    // BITMAPINFOHEADER
    putU32(buf, 40);
    putI32(buf, width);
    putI32(buf, height); // positive: bottom-up row order
    putU16(buf, 1);      // planes
    putU16(buf, 24);     // bits per pixel
    putU32(buf, 0);       // BI_RGB, no compression
    putU32(buf, pixelDataSize);
    putI32(buf, 2835); // ~72 DPI
    putI32(buf, 2835);
    putU32(buf, 0);
    putU32(buf, 0);

    std::vector<unsigned char> row(static_cast<std::size_t>(paddedRowSize), 0);
    for (int y = height - 1; y >= 0; --y) { // bottom-up
        for (int x = 0; x < width; ++x) {
            const Eigen::Vector3d& c = image.at(x, y) * exposure;
            // BMP pixel order is BGR.
            row[static_cast<std::size_t>(x) * 3 + 0] = toByte(c.z());
            row[static_cast<std::size_t>(x) * 3 + 1] = toByte(c.y());
            row[static_cast<std::size_t>(x) * 3 + 2] = toByte(c.x());
        }
        buf.insert(buf.end(), row.begin(), row.end());
    }

    std::ofstream out(path, std::ios::binary);
    if (!out) {
        throw std::runtime_error("failed to open " + path + " for writing");
    }
    out.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
}

} // namespace tools
