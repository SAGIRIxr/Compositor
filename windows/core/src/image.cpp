#include "compositor/image.h"
#include <algorithm>
#include <atomic>

namespace comp {

uint64_t nextImageSerial() {
    static std::atomic<uint64_t> counter{1};
    return counter.fetch_add(1, std::memory_order_relaxed);
}

void Image::fill(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    // 存储为预乘。
    uint8_t pr = uint8_t((r * a + 127) / 255), pg = uint8_t((g * a + 127) / 255), pb = uint8_t((b * a + 127) / 255);
    for (size_t i = 0; i < pixels.size(); i += 4) {
        pixels[i] = pr; pixels[i + 1] = pg; pixels[i + 2] = pb; pixels[i + 3] = a;
    }
    touch();
}

int GrayImage::uniformValue() const {
    if (pixels.empty()) return -1;
    uint8_t first = pixels[0];
    for (uint8_t v : pixels) if (v != first) return -1;
    return first;
}

std::vector<uint8_t> toStraightRGBA(const Image& image) {
    std::vector<uint8_t> out(image.pixels.size());
    for (size_t i = 0; i < image.pixels.size(); i += 4) {
        unsigned a = image.pixels[i + 3];
        out[i + 3] = uint8_t(a);
        for (int c = 0; c < 3; ++c) {
            out[i + c] = a == 0 ? 0 : uint8_t(std::min(255u, (image.pixels[i + c] * 255u + a / 2) / a));
        }
    }
    return out;
}

Image fromStraightRGBA(const uint8_t* rgba, int width, int height) {
    Image image(width, height);
    size_t count = size_t(width) * size_t(height) * 4;
    for (size_t i = 0; i < count; i += 4) {
        unsigned a = rgba[i + 3];
        image.pixels[i + 3] = uint8_t(a);
        for (int c = 0; c < 3; ++c) image.pixels[i + c] = uint8_t((rgba[i + c] * a + 127) / 255);
    }
    return image;
}

Image halve(const Image& image) {
    int w = std::max(1, (image.width + 1) / 2), h = std::max(1, (image.height + 1) / 2);
    Image out(w, h);
    for (int y = 0; y < h; ++y) {
        int y0 = std::min(image.height - 1, y * 2), y1 = std::min(image.height - 1, y * 2 + 1);
        for (int x = 0; x < w; ++x) {
            int x0 = std::min(image.width - 1, x * 2), x1 = std::min(image.width - 1, x * 2 + 1);
            // 越过右下边缘的半格当作透明，与 macOS 版的向上取整对半缩小一致。
            const uint8_t* p[4] = {image.at(x0, y0), image.at(x1, y0), image.at(x0, y1), image.at(x1, y1)};
            bool valid[4] = {true, x * 2 + 1 < image.width, y * 2 + 1 < image.height,
                             x * 2 + 1 < image.width && y * 2 + 1 < image.height};
            uint8_t* o = out.at(x, y);
            for (int c = 0; c < 4; ++c) {
                unsigned sum = 0;
                for (int k = 0; k < 4; ++k) if (valid[k]) sum += p[k][c];
                o[c] = uint8_t((sum + 2) / 4);
            }
        }
    }
    return out;
}

GrayImage halve(const GrayImage& image) {
    int w = std::max(1, (image.width + 1) / 2), h = std::max(1, (image.height + 1) / 2);
    GrayImage out(w, h);
    for (int y = 0; y < h; ++y) {
        int y0 = std::min(image.height - 1, y * 2), y1 = std::min(image.height - 1, y * 2 + 1);
        for (int x = 0; x < w; ++x) {
            int x0 = std::min(image.width - 1, x * 2), x1 = std::min(image.width - 1, x * 2 + 1);
            unsigned sum = image.row(y0)[x0] + image.row(y0)[x1] + image.row(y1)[x0] + image.row(y1)[x1];
            out.row(y)[x] = uint8_t((sum + 2) / 4);
        }
    }
    return out;
}

} // namespace comp
