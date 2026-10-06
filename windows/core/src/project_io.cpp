#include "compositor/project_io.h"
#include "compositor/image_io.h"
#include "compositor/renderer.h"
#include "compositor/uuid.h"
#include "compositor/adjustment.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <random>
#include <set>
#include <functional>
#include <unordered_map>

namespace comp {

namespace {

constexpr size_t kMaxManifestBytes = 4 * 1024 * 1024;
constexpr size_t kMaxAssetBytes = size_t(512) * 1024 * 1024;

[[noreturn]] void invalid(const std::string& why) { throw ProjectError(ProjectError::Code::Invalid, "不是有效的 Compositor 项目，或元数据已损坏：" + why); }
[[noreturn]] void tooLarge(const std::string& why) { throw ProjectError(ProjectError::Code::TooLarge, "项目超出支持的画布、图层、文件大小或像素总量限制：" + why); }
[[noreturn]] void missing(const std::string& why) { throw ProjectError(ProjectError::Code::MissingImage, "项目里的图片缺失或已损坏：" + why); }

double requireNumber(const Json& o, const char* key) {
    auto it = o.find(key);
    if (it == o.end() || !it->is_number()) invalid(std::string("缺少数字字段 ") + key);
    return it->get<double>();
}
bool requireBool(const Json& o, const char* key) {
    auto it = o.find(key);
    if (it == o.end() || !it->is_boolean()) invalid(std::string("缺少布尔字段 ") + key);
    return it->get<bool>();
}
std::string requireString(const Json& o, const char* key) {
    auto it = o.find(key);
    if (it == o.end() || !it->is_string()) invalid(std::string("缺少字符串字段 ") + key);
    return it->get<std::string>();
}
std::string optionalUUID(const Json& o, const char* key) {
    auto it = o.find(key);
    if (it == o.end() || it->is_null()) return {};
    if (!it->is_string()) invalid(std::string(key) + " 不是字符串");
    std::string id = normalizeUUID(it->get<std::string>());
    if (id.empty()) invalid(std::string(key) + " 不是有效的 UUID");
    return id;
}
std::optional<std::string> optionalString(const Json& o, const char* key) {
    auto it = o.find(key);
    if (it == o.end() || it->is_null()) return std::nullopt;
    if (!it->is_string()) invalid(std::string(key) + " 不是字符串");
    return it->get<std::string>();
}
std::optional<bool> optionalBool(const Json& o, const char* key) {
    auto it = o.find(key);
    if (it == o.end() || it->is_null()) return std::nullopt;
    if (!it->is_boolean()) invalid(std::string(key) + " 不是布尔值");
    return it->get<bool>();
}

// CGPoint / CGSize 在 Swift 里编码成两个数的数组。
void readPair(const Json& o, const char* key, double& a, double& b) {
    auto it = o.find(key);
    if (it == o.end()) invalid(std::string("缺少 ") + key);
    if (it->is_array() && it->size() == 2 && (*it)[0].is_number() && (*it)[1].is_number()) {
        a = (*it)[0].get<double>(); b = (*it)[1].get<double>();
        return;
    }
    // 宽容一点：也接受 {x, y} / {width, height}。
    if (it->is_object()) {
        const char* k1 = it->contains("x") ? "x" : "width";
        const char* k2 = it->contains("y") ? "y" : "height";
        a = requireNumber(*it, k1); b = requireNumber(*it, k2);
        return;
    }
    invalid(std::string(key) + " 格式错误");
}

LayerTransform parseTransform(const Json& o) {
    if (!o.is_object()) invalid("transform 不是对象");
    LayerTransform t;
    readPair(o, "origin", t.x, t.y);
    readPair(o, "size", t.width, t.height);
    t.rotation = o.contains("rotation") ? requireNumber(o, "rotation") : 0;
    t.flipX = o.contains("flipX") ? requireBool(o, "flipX") : false;
    t.flipY = o.contains("flipY") ? requireBool(o, "flipY") : false;
    if (o.contains("sampling") && !samplingFromName(requireString(o, "sampling"), t.sampling)) invalid("未知的采样方式");
    return t;
}

Json transformJson(const LayerTransform& t) {
    return Json{{"origin", Json::array({t.x, t.y})}, {"size", Json::array({t.width, t.height})},
                {"rotation", t.rotation}, {"flipX", t.flipX}, {"flipY", t.flipY}, {"sampling", samplingName(t.sampling)}};
}

// 已知（由模型负责读写）的图层字段；其余原样保存在 extra。
const std::set<std::string> kLayerKeys = {
    "id", "name", "isVisible", "transform", "imageFile", "parentID", "isGroup", "opacity", "blendMode",
    "maskFile", "maskEnabled", "maskSourceID", "adjustment", "maskPlacement", "maskLinked",
};
const std::set<std::string> kManifestKeys = {
    "format", "version", "colorSpace", "resolution", "documentID", "width", "height", "activeLayerID", "layers", "guides",
};

struct LayerFiles {
    std::optional<std::string> imageFile;
    std::optional<std::string> maskFile;
};

// 读 PNG 的 IHDR：位深与颜色类型。
bool pngHeader(const std::vector<uint8_t>& bytes, int& depth, int& colorType) {
    if (bytes.size() < 29) return false;
    depth = bytes[24];
    colorType = bytes[25];
    return true;
}

bool isInside(const fs::path& file, const fs::path& root) {
    std::error_code ec;
    fs::path a = fs::weakly_canonical(file, ec);
    if (ec) return false;
    fs::path b = fs::weakly_canonical(root, ec);
    if (ec) return false;
    auto rootString = b.generic_u8string(), fileString = a.generic_u8string();
    if (!rootString.empty() && rootString.back() != '/') rootString += '/';
    return fileString.compare(0, rootString.size(), rootString) == 0;
}

void checkFile(const fs::path& file, const fs::path& package, size_t maximum) {
    if (!isInside(file, package)) invalid("文件路径越出项目包");
    std::error_code ec;
    auto status = fs::symlink_status(file, ec);
    if (ec || !fs::exists(status)) missing(file.filename().u8string());
    if (fs::is_symlink(status) || !fs::is_regular_file(status)) tooLarge("不是普通文件");
    if (fs::file_size(file, ec) > maximum || ec) tooLarge("文件过大");
}

void checkSize(int width, int height, long long& used) {
    if (width < 1 || height < 1 || width > kMaxSide || height > kMaxSide) tooLarge("单边超过 30000 像素");
    long long pixels = (long long)width * height;
    if (pixels > kDocumentPixelBudget - used) tooLarge("像素总量超出预算");
    used += pixels;
}

} // namespace

fs::path packagePath(const fs::path& path) {
    if (path.filename() == "manifest.json") return path.parent_path();
    return path;
}

Document parseManifest(const Json& manifest, int* versionOut) {
    if (!manifest.is_object()) invalid("清单不是 JSON 对象");
    if (manifest.value("format", std::string()) != "com.compositor.project") invalid("format 不是 com.compositor.project");
    auto versionIt = manifest.find("version");
    if (versionIt == manifest.end() || !versionIt->is_number_integer()) invalid("缺少 version");
    int version = versionIt->get<int>();
    if (version < 1 || version > kFormatVersion) {
        throw ProjectError(ProjectError::Code::Version, "这个项目使用格式版本 " + std::to_string(version) +
                           "，本程序支持 1–" + std::to_string(kFormatVersion) + "。");
    }
    if (versionOut) *versionOut = version;
    Document doc;
    if (requireString(manifest, "colorSpace") != "sRGB") invalid("只支持 sRGB");
    doc.id = normalizeUUID(requireString(manifest, "documentID"));
    if (doc.id.empty()) invalid("documentID 无效");
    auto w = manifest.find("width"), h = manifest.find("height");
    if (w == manifest.end() || h == manifest.end() || !w->is_number_integer() || !h->is_number_integer()) invalid("缺少画布尺寸");
    doc.width = w->get<int>();
    doc.height = h->get<int>();
    if (auto r = manifest.find("resolution"); r != manifest.end() && !r->is_null()) {
        if (!r->is_number()) invalid("resolution 不是数字");
        doc.resolution = r->get<double>();
    }
    doc.activeLayerID = optionalUUID(manifest, "activeLayerID");
    auto layersIt = manifest.find("layers");
    if (layersIt == manifest.end() || !layersIt->is_array()) invalid("缺少 layers");
    if (layersIt->size() > size_t(kMaxLayers)) tooLarge("图层超过 10000 个");
    for (const Json& record : *layersIt) {
        if (!record.is_object()) invalid("图层不是对象");
        Layer layer;
        layer.id = normalizeUUID(requireString(record, "id"));
        if (layer.id.empty()) invalid("图层 id 无效");
        layer.name = requireString(record, "name");
        layer.visible = requireBool(record, "isVisible");
        if (!record.contains("transform")) invalid("缺少 transform");
        layer.transform = parseTransform(record["transform"]);
        layer.parentID = optionalUUID(record, "parentID");
        layer.isGroup = optionalBool(record, "isGroup").value_or(false);
        if (auto o = record.find("opacity"); o != record.end() && !o->is_null()) {
            if (!o->is_number()) invalid("opacity 不是数字");
            layer.opacity = o->get<double>();
        }
        if (auto b = optionalString(record, "blendMode"); b && !blendModeFromName(*b, layer.blendMode)) invalid("未知的混合模式 " + *b);
        layer.maskSourceID = optionalUUID(record, "maskSourceID");
        layer.maskEnabled = optionalBool(record, "maskEnabled").value_or(true);
        layer.maskLinked = optionalBool(record, "maskLinked").value_or(true);
        if (auto p = record.find("maskPlacement"); p != record.end() && !p->is_null()) layer.maskPlacement = parseTransform(*p);
        if (auto a = record.find("adjustment"); a != record.end() && !a->is_null()) layer.adjustment = *a;
        for (auto it = record.begin(); it != record.end(); ++it) {
            if (!kLayerKeys.count(it.key())) layer.extra[it.key()] = it.value();
        }
        // 文件名先存进 extra 的临时键，由 loadProject 校验并读取；这里只保留结构。
        if (auto f = optionalString(record, "imageFile")) layer.extra["\x01imageFile"] = *f;
        if (auto f = optionalString(record, "maskFile")) layer.extra["\x01maskFile"] = *f;
        if (record.contains("maskEnabled") && !record.contains("maskFile")) layer.extra["\x01maskEnabledWithoutMask"] = true;
        doc.layers.push_back(std::move(layer));
    }
    if (auto g = manifest.find("guides"); g != manifest.end() && !g->is_null()) {
        if (!g->is_array()) invalid("guides 不是数组");
        if (version < 8 && !g->empty()) invalid("版本 1–7 不能包含参考线");
        if (g->size() > 1000) tooLarge("参考线超过 1000 条");
        std::set<std::string> ids;
        for (const Json& item : *g) {
            Guide guide;
            guide.id = normalizeUUID(requireString(item, "id"));
            std::string axis = requireString(item, "axis");
            if (axis != "horizontal" && axis != "vertical") invalid("参考线方向错误");
            guide.horizontal = axis == "horizontal";
            guide.position = requireNumber(item, "position");
            if (guide.id.empty() || !ids.insert(guide.id).second || !std::isfinite(guide.position) || std::abs(guide.position) > 1000000) {
                invalid("参考线无效");
            }
            doc.guides.push_back(guide);
        }
    }
    for (auto it = manifest.begin(); it != manifest.end(); ++it) {
        if (!kManifestKeys.count(it.key())) doc.extra[it.key()] = it.value();
    }
    validateDocument(doc, version);
    return doc;
}

void validateDocument(const Document& doc, int version) {
    if (doc.resolution && (!std::isfinite(*doc.resolution) || *doc.resolution < 1 || *doc.resolution > 9600)) invalid("分辨率超出 1–9600");
    if (doc.width < 1 || doc.width > kMaxSide || doc.height < 1 || doc.height > kMaxSide) tooLarge("画布尺寸超出范围");
    if (doc.layers.size() > size_t(kMaxLayers)) tooLarge("图层过多");
    std::unordered_map<std::string, const Layer*> byID;
    for (const auto& layer : doc.layers) {
        if (!byID.emplace(layer.id, &layer).second) invalid("图层 id 重复");
    }
    for (const auto& layer : doc.layers) {
        bool hasImage = layer.image || layer.extra.contains("\x01imageFile");
        bool hasMask = layer.mask || layer.extra.contains("\x01maskFile");
        if (layer.extra.contains("text")) {
            const Json& text = layer.extra["text"];
            if (!text.is_object() || (text.contains("colorRuns") && version < 10) || (text.contains("fontRuns") && version < 11)
                || !hasImage || layer.isGroup || layer.adjustment) invalid("文字图层无效");
        }
        if (layer.adjustment) {
            if (version < 7 || layer.isGroup || hasImage) invalid("调整层无效");
            AdjustmentParams params;
            try { params = parseAdjustment(*layer.adjustment); } catch (const std::exception& e) { invalid(e.what()); }
            if (!params.isValid()) invalid("调整层参数超出范围");
            if (adjustmentNeedsVersion9(params.kind) && version < 9) invalid("该调整类型需要版本 9");
        }
        if (hasMask && version < (layer.isGroup ? 6 : 4)) invalid("该版本不支持蒙版");
        if (layer.extra.contains("\x01maskFile") && layer.extra["\x01maskFile"].get<std::string>() != layer.id + ".mask.png") invalid("蒙版文件名必须是 <图层ID>.mask.png");
        if (layer.extra.contains("\x01maskEnabledWithoutMask")) invalid("maskEnabled 需要 maskFile");
        if (layer.maskPlacement && (!layer.maskPlacement->isValid() || !hasMask)) invalid("maskPlacement 无效");
        if (!std::isfinite(layer.opacity) || layer.opacity < 0 || layer.opacity > 1) invalid("不透明度超出 0–1");
        if (version < 3 && (layer.opacity != 1 || layer.blendMode != BlendMode::Normal)) invalid("版本 1–2 不支持不透明度与混合模式");
        if (layer.isGroup && (layer.blendMode != BlendMode::Normal || (version < 8 && layer.opacity != 1))) invalid("文件夹的混合模式必须是正常");
        if (layer.isGroup && hasImage) invalid("文件夹不能有图片");
        if (!layer.transform.isValid()) invalid("变换无效");
        std::string trimmed = layer.name;
        trimmed.erase(std::remove_if(trimmed.begin(), trimmed.end(), [](unsigned char c) { return std::isspace(c); }), trimmed.end());
        if (trimmed.empty() || layer.name.size() > 16384) invalid("图层名称为空或过长");
        if (layer.extra.contains("\x01imageFile") && layer.extra["\x01imageFile"].get<std::string>() != layer.id + ".png") invalid("图片文件名必须是 <图层ID>.png");
        // 层级：父节点必须存在且是文件夹，无环，深度不超过 64。
        std::set<std::string> seen{layer.id};
        std::string parent = layer.parentID;
        while (!parent.empty()) {
            auto it = byID.find(parent);
            if (seen.size() > 64 || !seen.insert(parent).second || it == byID.end() || !it->second->isGroup) invalid("文件夹层级无效");
            parent = it->second->parentID;
        }
        // 剪贴蒙版链：来源存在、不是文件夹或调整层，无环，链长不超过 256。
        std::set<std::string> path;
        const Layer* current = &layer;
        while (current) {
            if (path.size() >= 256 || !path.insert(current->id).second) invalid("剪贴蒙版链有环或过长");
            if (current->maskSourceID.empty()) break;
            auto it = byID.find(current->maskSourceID);
            if (current->isGroup || it == byID.end() || it->second->isGroup || it->second->adjustment) invalid("剪贴蒙版来源无效");
            current = it->second;
        }
        if (version < 5 && !layer.maskSourceID.empty()) invalid("版本 1–4 不支持剪贴蒙版");
        if (version == 1 && (!layer.parentID.empty() || layer.isGroup)) invalid("版本 1 不支持文件夹");
    }
    if (!doc.activeLayerID.empty() && !byID.count(doc.activeLayerID)) invalid("activeLayerID 不存在");
    if (version < 8 && !doc.guides.empty()) invalid("版本 1–7 不能包含参考线");
}

Document loadProject(const fs::path& input, SaveCache* cache) {
    fs::path package = packagePath(input);
    std::error_code ec;
    if (!fs::is_directory(package, ec)) invalid("不是文件夹");
    fs::path manifestPath = package / "manifest.json";
    checkFile(manifestPath, package, kMaxManifestBytes);
    std::vector<uint8_t> bytes;
    if (!readFile(manifestPath, bytes, kMaxManifestBytes)) tooLarge("清单超过 4 MiB");
    Json manifest = Json::parse(bytes.begin(), bytes.end(), nullptr, false);
    if (manifest.is_discarded()) invalid("清单不是有效的 JSON");
    int version = 0;
    Document doc = parseManifest(manifest, &version);
    if (cache) { cache->package = package; cache->written.clear(); }
    long long pixels = 0, maskPixels = 0;
    for (auto& layer : doc.layers) {
        for (bool isMask : {false, true}) {
            const char* key = isMask ? "\x01maskFile" : "\x01imageFile";
            if (!layer.extra.contains(key)) continue;
            std::string filename = layer.extra[key].get<std::string>();
            layer.extra.erase(key);
            fs::path file = package / "images" / fs::u8path(filename);
            checkFile(file, package, kMaxAssetBytes);
            std::vector<uint8_t> data;
            if (!readFile(file, data, kMaxAssetBytes)) missing(filename);
            auto info = probeImage(data);
            int depth = 0, colorType = 0;
            if (!info || !info->isPNG || info->is16Bit || !pngHeader(data, depth, colorType) || depth > 8) missing(filename + "（必须是 8 位 PNG）");
            checkSize(info->width, info->height, isMask ? maskPixels : pixels);
            if (isMask) {
                // 蒙版必须是不带透明通道的灰度图。
                if (colorType != 0) invalid(filename + " 不是灰度 PNG");
                auto mask = decodeGray(data);
                if (!mask) missing(filename);
                layer.mask = std::make_shared<GrayImage>(std::move(*mask));
                if (cache) cache->written[filename] = layer.mask->serial;
            } else {
                auto image = decodeImage(data);
                if (!image) missing(filename);
                layer.image = std::make_shared<Image>(std::move(*image));
                if (cache) cache->written[filename] = layer.image->serial;
            }
        }
    }
    doc.normalizeOrder();
    return doc;
}

Json makeManifest(const Document& doc) {
    Json manifest = Json::object();
    manifest["format"] = "com.compositor.project";
    manifest["version"] = kFormatVersion;
    manifest["colorSpace"] = "sRGB";
    if (doc.resolution) manifest["resolution"] = *doc.resolution;
    manifest["documentID"] = doc.id;
    manifest["width"] = doc.width;
    manifest["height"] = doc.height;
    if (!doc.activeLayerID.empty()) manifest["activeLayerID"] = doc.activeLayerID;
    Json layers = Json::array();
    for (const auto& layer : doc.layers) {
        Json record = Json::object();
        record["id"] = layer.id;
        record["name"] = layer.name;
        record["isVisible"] = layer.visible;
        record["transform"] = transformJson(layer.transform);
        if (layer.image) record["imageFile"] = layer.id + ".png";
        if (!layer.parentID.empty()) record["parentID"] = layer.parentID;
        record["isGroup"] = layer.isGroup;
        record["opacity"] = layer.opacity;
        record["blendMode"] = blendModeName(layer.blendMode);
        if (layer.mask) {
            record["maskFile"] = layer.id + ".mask.png";
            record["maskEnabled"] = layer.maskEnabled;
            if (layer.maskPlacement) record["maskPlacement"] = transformJson(*layer.maskPlacement);
            if (!layer.maskLinked) record["maskLinked"] = false;
        }
        if (!layer.maskSourceID.empty()) record["maskSourceID"] = layer.maskSourceID;
        if (layer.adjustment) record["adjustment"] = *layer.adjustment;
        for (auto it = layer.extra.begin(); it != layer.extra.end(); ++it) {
            if (!it.key().empty() && it.key()[0] == '\x01') continue;
            record[it.key()] = it.value();
        }
        layers.push_back(record);
    }
    manifest["layers"] = layers;
    if (!doc.guides.empty()) {
        Json guides = Json::array();
        for (const auto& g : doc.guides) {
            guides.push_back(Json{{"id", g.id}, {"axis", g.horizontal ? "horizontal" : "vertical"}, {"position", g.position}});
        }
        manifest["guides"] = guides;
    }
    for (auto it = doc.extra.begin(); it != doc.extra.end(); ++it) manifest[it.key()] = it.value();
    return manifest;
}

namespace {

bool isLayerAssetName(const std::string& name) {
    // <UUID>.png 或 <UUID>.mask.png：只清理这种名字的文件，不碰文件夹里别的东西。
    if (name.size() == 40 && name.compare(36, 4, ".png") == 0) return !normalizeUUID(name.substr(0, 36)).empty();
    if (name.size() == 45 && name.compare(36, 9, ".mask.png") == 0) return !normalizeUUID(name.substr(0, 36)).empty();
    return false;
}

struct Asset { std::string filename; uint64_t serial; std::function<std::vector<uint8_t>()> encode; };

} // namespace

void saveProject(const Document& doc, const fs::path& input, bool quickLook, SaveCache* cache) {
    validateDocument(doc, kFormatVersion);
    fs::path package = packagePath(input);
    std::error_code ec;
    // 先检查尺寸，收集要写的图片。
    std::vector<Asset> assets;
    long long pixels = 0, maskPixels = 0;
    for (const auto& layer : doc.layers) {
        if (layer.image) {
            checkSize(layer.image->width, layer.image->height, pixels);
            ImageRef image = layer.image;
            assets.push_back({layer.id + ".png", image->serial, [image] { return encodePNG(*image); }});
        }
        if (layer.mask) {
            checkSize(layer.mask->width, layer.mask->height, maskPixels);
            MaskRef mask = layer.mask;
            assets.push_back({layer.id + ".mask.png", mask->serial, [mask] { return encodePNG(*mask); }});
        }
    }
    std::string text = makeManifest(doc).dump(2);
    if (text.size() > kMaxManifestBytes) throw ProjectError(ProjectError::Code::TooLarge, "清单超过 4 MiB");
    std::vector<uint8_t> manifestBytes(text.begin(), text.end());
    std::vector<uint8_t> preview;
    if (quickLook && (long long)doc.width * doc.height <= 50000000LL) {
        // 白底 JPEG，长边 1024：macOS 访达空格预览读取它，打开项目时忽略。
        const uint8_t white[3] = {255, 255, 255};
        preview = encodeJPEG(scaledToFit(flatten(doc), 1024), 80, white);
    }
    bool sameCache = cache && !cache->package.empty() && fs::equivalent(cache->package, package, ec) && !ec;
    std::unordered_map<std::string, uint64_t> written;

    bool exists = fs::exists(package, ec);
    if (exists && !fs::is_directory(package, ec)) throw ProjectError(ProjectError::Code::IO, "同名文件已存在，不是项目文件夹");

    if (!exists) {
        // 新包：在旁边写完整，再整体改名就位。
        fs::path parent = package.parent_path();
        if (parent.empty()) parent = fs::current_path();
        std::random_device random;
        fs::path staging = parent / fs::u8path("." + package.filename().u8string() + ".saving-" + std::to_string(random() % 1000000));
        fs::remove_all(staging, ec);
        if (!fs::create_directories(staging / "images", ec) || ec) {
            throw ProjectError(ProjectError::Code::IO, "无法创建临时文件夹：" + ec.message());
        }
        auto fail = [&](ProjectError::Code code, const std::string& message) {
            std::error_code ignored;
            fs::remove_all(staging, ignored);
            throw ProjectError(code, message);
        };
        for (const Asset& asset : assets) {
            auto png = asset.encode();
            if (png.empty() || !writeFileAtomic(staging / "images" / fs::u8path(asset.filename), png)) {
                fail(ProjectError::Code::Encode, "图片无法保存：" + asset.filename);
            }
            written[asset.filename] = asset.serial;
        }
        if (!writeFileAtomic(staging / "manifest.json", manifestBytes)) fail(ProjectError::Code::IO, "无法写入清单");
        if (!preview.empty()) {
            fs::create_directories(staging / "QuickLook", ec);
            if (!ec) writeFileAtomic(staging / "QuickLook" / "Preview.jpg", preview);
        }
        fs::rename(staging, package, ec);
        if (ec) fail(ProjectError::Code::IO, "无法写入项目：" + ec.message());
    } else {
        // 已有的包：就地更新，清单最后写。
        fs::create_directories(package / "images", ec);
        for (const Asset& asset : assets) {
            fs::path file = package / "images" / fs::u8path(asset.filename);
            if (sameCache) {
                auto known = cache->written.find(asset.filename);
                if (known != cache->written.end() && known->second == asset.serial && fs::is_regular_file(file, ec)) {
                    written[asset.filename] = asset.serial;
                    continue; // 像素没变
                }
            }
            auto png = asset.encode();
            if (png.empty() || !writeFileAtomic(file, png)) {
                throw ProjectError(ProjectError::Code::Encode, "图片无法保存，原项目的清单没有被替换：" + asset.filename);
            }
            written[asset.filename] = asset.serial;
        }
        if (!writeFileAtomic(package / "manifest.json", manifestBytes)) {
            throw ProjectError(ProjectError::Code::IO, "无法写入清单（文件可能被占用）");
        }
        // 清理不再引用的图片与旧的预览。
        for (auto it = fs::directory_iterator(package / "images", ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
            std::string name = it->path().filename().u8string();
            if (isLayerAssetName(name) && !written.count(name)) {
                std::error_code ignored;
                fs::remove(it->path(), ignored);
            }
        }
        if (!preview.empty()) {
            fs::create_directories(package / "QuickLook", ec);
            writeFileAtomic(package / "QuickLook" / "Preview.jpg", preview);
        } else {
            std::error_code ignored;
            fs::remove(package / "QuickLook" / "Preview.jpg", ignored);
        }
    }
    if (cache) {
        cache->package = package;
        cache->written = std::move(written);
    }
}

} // namespace comp
