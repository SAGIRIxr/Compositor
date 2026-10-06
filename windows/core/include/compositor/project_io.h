// .comp 项目包的读取、校验与保存。规则与 macOS 版 ProjectStore 相同，两边可以互相打开对方保存的项目。
#pragma once
#include "compositor/document.h"
#include <filesystem>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace comp {

namespace fs = std::filesystem;

class ProjectError : public std::runtime_error {
public:
    enum class Code { Invalid, Version, MissingImage, TooLarge, Encode, IO };
    ProjectError(Code code, const std::string& message) : std::runtime_error(message), code_(code) {}
    Code code() const { return code_; }
private:
    Code code_;
};

// 记录上次保存或读取时每个图片文件对应的像素序号，再次保存同一个包时跳过没变的图片。
struct SaveCache {
    fs::path package;
    std::unordered_map<std::string, uint64_t> written; // 文件名 → Image/GrayImage 的 serial
};

// 读取项目包。path 可以是 .comp 文件夹，也可以是其中的 manifest.json。
Document loadProject(const fs::path& path, SaveCache* cache = nullptr);
// 保存项目包，并生成 QuickLook/Preview.jpg 供 macOS 访达预览。
// 新包先写到旁边的临时文件夹再整体改名就位；已有的包就地更新：先逐个原子替换图片，
// 最后原子替换 manifest.json，再删掉不再引用的图片。这样 Windows 上即使资源管理器或
// 文件监视占着这个文件夹也能保存，正在打开它的 Compositor 也只会看到完整的新旧两种状态之一。
void saveProject(const Document& document, const fs::path& path, bool quickLook = true, SaveCache* cache = nullptr);
// 解析清单 JSON（不读图片），并校验。
Document parseManifest(const Json& manifest, int* version = nullptr);
// 把文档写成清单 JSON（版本号为当前版本）。
Json makeManifest(const Document& document);
// 校验文档结构；不合格抛出 ProjectError。
void validateDocument(const Document& document, int version = kFormatVersion);
// 给定路径对应的项目包文件夹。
fs::path packagePath(const fs::path& path);

} // namespace comp
