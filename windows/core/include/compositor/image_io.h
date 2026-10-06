// 文件读写与 PNG/JPEG 编解码。路径一律用 std::filesystem::path，Windows 上支持中文路径。
#pragma once
#include "compositor/image.h"
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace comp {

namespace fs = std::filesystem;

// 读取整个文件；超过 maxBytes 时返回 false。
bool readFile(const fs::path& path, std::vector<uint8_t>& bytes, size_t maxBytes = size_t(-1));
// 先写同目录临时文件再改名替换，避免留下写了一半的文件。
bool writeFileAtomic(const fs::path& path, const std::vector<uint8_t>& bytes);

struct ImageInfo {
    int width = 0;
    int height = 0;
    int channels = 0;
    bool is16Bit = false;
    bool isPNG = false;
};
std::optional<ImageInfo> probeImage(const std::vector<uint8_t>& bytes);

// 解码任意常见格式（PNG、JPEG、BMP、GIF 首帧、TGA、PSD 合成图）为预乘 RGBA。
std::optional<Image> decodeImage(const std::vector<uint8_t>& bytes, std::string* error = nullptr);
// 解码为灰度（蒙版）。
std::optional<GrayImage> decodeGray(const std::vector<uint8_t>& bytes, std::string* error = nullptr);

// 编码为 PNG。resolution 为每英寸像素数，写入 pHYs 块；0 表示不写。
std::vector<uint8_t> encodePNG(const Image& image, double resolution = 0);
std::vector<uint8_t> encodePNG(const GrayImage& image);
// 编码为 JPEG：透明部分先铺在 background（直通 sRGB 0–255）上。quality 1–100。
std::vector<uint8_t> encodeJPEG(const Image& image, int quality, const uint8_t background[3], double resolution = 0);

// 等比缩放到长边不超过 longSide（只缩小），用于缩略图与预览。
Image scaledToFit(const Image& image, int longSide);

} // namespace comp
