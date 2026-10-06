// 位图类型：图层像素（预乘 RGBA8）与蒙版（8 位灰度）。
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace comp {

// 每个新建或复制出来的位图都有独立序号，渲染缓存用它识别像素是否变化。
uint64_t nextImageSerial();

// 预乘 RGBA，每通道 8 位，行从上到下、无填充。
struct Image {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> pixels;
    uint64_t serial = nextImageSerial();

    Image() = default;
    Image(int w, int h) : width(w), height(h), pixels(size_t(w) * size_t(h) * 4, 0) {}
    Image(const Image& other) : width(other.width), height(other.height), pixels(other.pixels) {}
    Image& operator=(const Image& other) {
        width = other.width; height = other.height; pixels = other.pixels; serial = nextImageSerial();
        return *this;
    }
    Image(Image&&) noexcept = default;
    Image& operator=(Image&&) noexcept = default;

    bool empty() const { return width <= 0 || height <= 0; }
    size_t stride() const { return size_t(width) * 4; }
    uint8_t* row(int y) { return pixels.data() + size_t(y) * stride(); }
    const uint8_t* row(int y) const { return pixels.data() + size_t(y) * stride(); }
    uint8_t* at(int x, int y) { return row(y) + size_t(x) * 4; }
    const uint8_t* at(int x, int y) const { return row(y) + size_t(x) * 4; }
    void fill(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
    // 内容变了之后调用，让缓存失效。
    void touch() { serial = nextImageSerial(); }
};

// 8 位灰度蒙版：白色显示，黑色隐藏。
struct GrayImage {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> pixels;
    uint64_t serial = nextImageSerial();

    GrayImage() = default;
    GrayImage(int w, int h, uint8_t value = 0) : width(w), height(h), pixels(size_t(w) * size_t(h), value) {}
    GrayImage(const GrayImage& other) : width(other.width), height(other.height), pixels(other.pixels) {}
    GrayImage& operator=(const GrayImage& other) {
        width = other.width; height = other.height; pixels = other.pixels; serial = nextImageSerial();
        return *this;
    }
    GrayImage(GrayImage&&) noexcept = default;
    GrayImage& operator=(GrayImage&&) noexcept = default;

    bool empty() const { return width <= 0 || height <= 0; }
    uint8_t* row(int y) { return pixels.data() + size_t(y) * size_t(width); }
    const uint8_t* row(int y) const { return pixels.data() + size_t(y) * size_t(width); }
    void touch() { serial = nextImageSerial(); }
    // 所有像素相同（1×1 的均匀蒙版也算），返回那个值；否则返回 -1。
    int uniformValue() const;
};

using ImageRef = std::shared_ptr<const Image>;
using MaskRef = std::shared_ptr<const GrayImage>;

// 预乘与反预乘，单个像素。
inline void unpremultiply(const uint8_t* p, float out[4]) {
    float a = p[3] / 255.0f;
    out[3] = a;
    if (a <= 0) { out[0] = out[1] = out[2] = 0; return; }
    for (int c = 0; c < 3; ++c) { float v = p[c] / 255.0f / a; out[c] = v > 1 ? 1 : v; }
}

// 预乘图像转为直通（非预乘）RGBA8，供编码使用。
std::vector<uint8_t> toStraightRGBA(const Image& image);
// 直通 RGBA8 转为预乘图像。
Image fromStraightRGBA(const uint8_t* rgba, int width, int height);

// 按 2 的幂缩小（盒式平均，边缘向上取整），用于缩小显示时的高质量采样。
Image halve(const Image& image);
GrayImage halve(const GrayImage& image);

} // namespace comp
