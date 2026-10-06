#include "compositor/image_io.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <random>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STB_IMAGE_WRITE_IMPLEMENTATION
#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif
#include "stb/stb_image.h"
#include "stb/stb_image_write.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace comp {

bool readFile(const fs::path& path, std::vector<uint8_t>& bytes, size_t maxBytes) {
    std::error_code ec;
    auto size = fs::file_size(path, ec);
    if (ec || size > maxBytes) return false;
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    bytes.resize(size_t(size));
    if (size && !in.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(size))) return false;
    return true;
}

bool writeFileAtomic(const fs::path& path, const std::vector<uint8_t>& bytes) {
    std::random_device random;
    fs::path temporary = path;
    temporary += ".tmp" + std::to_string(random() % 1000000);
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        if (!bytes.empty()) out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        if (!out) { out.close(); std::error_code ignored; fs::remove(temporary, ignored); return false; }
    }
    std::error_code ec;
    fs::rename(temporary, path, ec);
    if (ec) {
        // Windows 上目标被占用等情况下 rename 可能失败：先删再改名。
        fs::remove(path, ec);
        fs::rename(temporary, path, ec);
        if (ec) { std::error_code ignored; fs::remove(temporary, ignored); return false; }
    }
    return true;
}

static bool isPNGSignature(const std::vector<uint8_t>& bytes) {
    static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    return bytes.size() >= 8 && std::memcmp(bytes.data(), signature, 8) == 0;
}

std::optional<ImageInfo> probeImage(const std::vector<uint8_t>& bytes) {
    ImageInfo info;
    if (bytes.empty() || bytes.size() > size_t(INT32_MAX)) return std::nullopt;
    if (!stbi_info_from_memory(bytes.data(), int(bytes.size()), &info.width, &info.height, &info.channels)) return std::nullopt;
    info.is16Bit = stbi_is_16_bit_from_memory(bytes.data(), int(bytes.size())) != 0;
    info.isPNG = isPNGSignature(bytes);
    return info;
}

std::optional<Image> decodeImage(const std::vector<uint8_t>& bytes, std::string* error) {
    int w = 0, h = 0, channels = 0;
    if (bytes.empty() || bytes.size() > size_t(INT32_MAX)) { if (error) *error = "文件为空或过大"; return std::nullopt; }
    stbi_uc* data = stbi_load_from_memory(bytes.data(), int(bytes.size()), &w, &h, &channels, 4);
    if (!data) { if (error) *error = stbi_failure_reason() ? stbi_failure_reason() : "无法解码"; return std::nullopt; }
    Image image = fromStraightRGBA(data, w, h);
    stbi_image_free(data);
    return image;
}

std::optional<GrayImage> decodeGray(const std::vector<uint8_t>& bytes, std::string* error) {
    int w = 0, h = 0, channels = 0;
    if (bytes.empty() || bytes.size() > size_t(INT32_MAX)) { if (error) *error = "文件为空或过大"; return std::nullopt; }
    stbi_uc* data = stbi_load_from_memory(bytes.data(), int(bytes.size()), &w, &h, &channels, 1);
    if (!data) { if (error) *error = stbi_failure_reason() ? stbi_failure_reason() : "无法解码"; return std::nullopt; }
    GrayImage image(w, h);
    std::memcpy(image.pixels.data(), data, size_t(w) * size_t(h));
    stbi_image_free(data);
    return image;
}

static void appendBytes(void* context, void* data, int size) {
    auto* out = static_cast<std::vector<uint8_t>*>(context);
    auto* bytes = static_cast<uint8_t*>(data);
    out->insert(out->end(), bytes, bytes + size);
}

static uint32_t crc32(const uint8_t* data, size_t length) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[n] = c;
        }
        ready = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < length; ++i) c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static void putBE32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); }

// 在 IHDR 之后插入 pHYs 块，记录分辨率。
static void insertPHYs(std::vector<uint8_t>& png, double resolution) {
    if (resolution <= 0 || png.size() < 33) return;
    uint32_t perMeter = uint32_t(std::lround(resolution / 0.0254));
    uint8_t chunk[21];
    putBE32(chunk, 9);
    std::memcpy(chunk + 4, "pHYs", 4);
    putBE32(chunk + 8, perMeter);
    putBE32(chunk + 12, perMeter);
    chunk[16] = 1;
    putBE32(chunk + 17, crc32(chunk + 4, 13));
    png.insert(png.begin() + 33, chunk, chunk + 21); // 8 字节签名 + 25 字节 IHDR
}

std::vector<uint8_t> encodePNG(const Image& image, double resolution) {
    std::vector<uint8_t> out;
    if (image.empty()) return out;
    auto straight = toStraightRGBA(image);
    stbi_write_png_compression_level = 6;
    stbi_write_png_to_func(appendBytes, &out, image.width, image.height, 4, straight.data(), int(image.stride()));
    insertPHYs(out, resolution);
    return out;
}

std::vector<uint8_t> encodePNG(const GrayImage& image) {
    std::vector<uint8_t> out;
    if (image.empty()) return out;
    stbi_write_png_compression_level = 6;
    stbi_write_png_to_func(appendBytes, &out, image.width, image.height, 1, image.pixels.data(), image.width);
    return out;
}

std::vector<uint8_t> encodeJPEG(const Image& image, int quality, const uint8_t background[3], double resolution) {
    std::vector<uint8_t> out;
    if (image.empty()) return out;
    std::vector<uint8_t> rgb(size_t(image.width) * size_t(image.height) * 3);
    for (size_t i = 0, j = 0; i < image.pixels.size(); i += 4, j += 3) {
        unsigned a = image.pixels[i + 3];
        for (int c = 0; c < 3; ++c) rgb[j + c] = uint8_t(image.pixels[i + c] + (background[c] * (255 - a) + 127) / 255);
    }
    stbi_write_jpg_to_func(appendBytes, &out, image.width, image.height, 3, rgb.data(), std::clamp(quality, 1, 100));
    // stb 写的是 JFIF APP0：把像素密度改成每英寸。
    if (resolution > 0 && out.size() > 18 && out[2] == 0xFF && out[3] == 0xE0 && std::memcmp(&out[6], "JFIF", 4) == 0) {
        uint16_t dpi = uint16_t(std::clamp(std::lround(resolution), 1L, 65535L));
        out[13] = 1;
        out[14] = uint8_t(dpi >> 8); out[15] = uint8_t(dpi);
        out[16] = uint8_t(dpi >> 8); out[17] = uint8_t(dpi);
    }
    return out;
}

Image scaledToFit(const Image& source, int longSide) {
    Image image = source;
    while (std::max(image.width, image.height) >= longSide * 2) image = halve(image);
    double factor = double(longSide) / std::max(image.width, image.height);
    if (factor >= 1) return image;
    int w = std::max(1, int(std::lround(image.width * factor))), h = std::max(1, int(std::lround(image.height * factor)));
    Image out(w, h);
    for (int y = 0; y < h; ++y) {
        double sy = (y + 0.5) / factor - 0.5;
        int y0 = std::clamp(int(std::floor(sy)), 0, image.height - 1), y1 = std::min(image.height - 1, y0 + 1);
        double fy = std::clamp(sy - y0, 0.0, 1.0);
        for (int x = 0; x < w; ++x) {
            double sx = (x + 0.5) / factor - 0.5;
            int x0 = std::clamp(int(std::floor(sx)), 0, image.width - 1), x1 = std::min(image.width - 1, x0 + 1);
            double fx = std::clamp(sx - x0, 0.0, 1.0);
            for (int c = 0; c < 4; ++c) {
                double top = image.at(x0, y0)[c] * (1 - fx) + image.at(x1, y0)[c] * fx;
                double bottom = image.at(x0, y1)[c] * (1 - fx) + image.at(x1, y1)[c] * fx;
                out.at(x, y)[c] = uint8_t(std::lround(top * (1 - fy) + bottom * fy));
            }
        }
    }
    return out;
}

} // namespace comp
