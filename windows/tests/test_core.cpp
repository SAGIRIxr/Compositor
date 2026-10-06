#include "test.h"
#include "compositor/adjustment.h"
#include "compositor/edit.h"
#include "compositor/effects.h"
#include "compositor/selection.h"
#include "compositor/image_io.h"
#include "compositor/project_io.h"
#include "compositor/renderer.h"
#include "compositor/uuid.h"
#include <filesystem>
#include <fstream>
#include <random>

using namespace comp;

namespace {

ImageRef solid(int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    auto image = std::make_shared<Image>(w, h);
    image->fill(r, g, b, a);
    return image;
}

Layer pixelLayer(const std::string& name, ImageRef image, double x = 0, double y = 0) {
    Layer layer;
    layer.id = makeUUID();
    layer.name = name;
    layer.image = image;
    layer.transform.x = x;
    layer.transform.y = y;
    layer.transform.width = image->width;
    layer.transform.height = image->height;
    return layer;
}

Document canvas(int w, int h) {
    Document doc;
    doc.id = makeUUID();
    doc.width = w;
    doc.height = h;
    return doc;
}

const uint8_t* px(const Image& image, int x, int y) { return image.at(x, y); }

struct TempDir {
    fs::path path;
    TempDir() {
        std::random_device random;
        path = fs::temp_directory_path() / fs::u8path("compositor-测试-" + std::to_string(random()));
        fs::create_directories(path);
    }
    ~TempDir() { std::error_code ec; fs::remove_all(path, ec); }
};

// writing-comp-files.md 里的示例清单（含曲线调整层）。
const char* kExampleManifest = R"({
  "format": "com.compositor.project",
  "version": 11,
  "colorSpace": "sRGB",
  "documentID": "0C5E7A91-3B2D-4F6A-8E1C-9D0B7A6F5E4D",
  "width": 4,
  "height": 2,
  "resolution": 72,
  "activeLayerID": "6F1D3C2A-0B7E-4E8A-9C4D-2A1B3C4D5E6F",
  "layers": [
    {
      "id": "6F1D3C2A-0B7E-4E8A-9C4D-2A1B3C4D5E6F",
      "name": "Background",
      "imageFile": "6F1D3C2A-0B7E-4E8A-9C4D-2A1B3C4D5E6F.png",
      "isVisible": true, "isGroup": false, "opacity": 1, "blendMode": "Normal",
      "transform": { "origin": [0, 0], "size": [4, 2], "rotation": 0, "flipX": false, "flipY": false, "sampling": "High quality" },
      "text": { "content": "Hi", "fontName": "Helvetica", "fontSize": 12, "red": 1, "green": 0, "blue": 0 },
      "effects": { "stroke": { "size": 3, "red": 0, "green": 0, "blue": 0, "opacity": 1, "inside": false } }
    },
    {
      "id": "A1B2C3D4-E5F6-4A7B-8C9D-0E1F2A3B4C5D",
      "name": "Warm Grade",
      "isVisible": true, "isGroup": false, "opacity": 1, "blendMode": "Normal",
      "transform": { "origin": [0, 0], "size": [4, 2], "rotation": 0, "flipX": false, "flipY": false, "sampling": "High quality" },
      "adjustment": {
        "kind": "Curves", "hue": 0, "saturation": 0, "lightness": 0, "colorize": false,
        "levels": { "channel": "RGB", "ranges": [
          { "black": 0, "gamma": 1, "white": 255, "outputBlack": 0, "outputWhite": 255 },
          { "black": 0, "gamma": 1, "white": 255, "outputBlack": 0, "outputWhite": 255 },
          { "black": 0, "gamma": 1, "white": 255, "outputBlack": 0, "outputWhite": 255 },
          { "black": 0, "gamma": 1, "white": 255, "outputBlack": 0, "outputWhite": 255 } ] },
        "curves": { "channel": "RGB", "channels": [
          [ { "x": 0, "y": 0 }, { "x": 255, "y": 255 } ],
          [ { "x": 0, "y": 0 }, { "x": 120, "y": 147 }, { "x": 255, "y": 255 } ],
          [ { "x": 0, "y": 0 }, { "x": 100, "y": 114 }, { "x": 255, "y": 255 } ],
          [ { "x": 0, "y": 0 }, { "x": 115, "y": 97 }, { "x": 255, "y": 238 } ] ] }
      }
    }
  ],
  "futureField": { "kept": true }
})";

void writeExample(const fs::path& package, const Image& background) {
    fs::create_directories(package / "images");
    std::ofstream(package / "manifest.json") << kExampleManifest;
    auto png = encodePNG(background);
    writeFileAtomic(package / "images" / "6F1D3C2A-0B7E-4E8A-9C4D-2A1B3C4D5E6F.png", png);
}

} // namespace

TEST(blend_normal_and_multiply) {
    float dst[4] = {0.5f, 0.5f, 0.5f, 1};
    float src[4] = {0.5f, 0, 0, 0.5f}; // 半透明纯红（预乘）
    blendPixel(BlendMode::Normal, src, dst);
    CHECK_NEAR(dst[0], 0.75, 1e-6);
    CHECK_NEAR(dst[1], 0.25, 1e-6);
    CHECK_NEAR(dst[3], 1, 1e-6);
    float d2[4] = {0.5f, 0.5f, 0.5f, 1};
    float s2[4] = {1, 0, 0, 1};
    blendPixel(BlendMode::Multiply, s2, d2);
    CHECK_NEAR(d2[0], 0.5, 1e-6);
    CHECK_NEAR(d2[1], 0, 1e-6);
}

TEST(blend_modes_names_round_trip) {
    for (int i = 0; i < kBlendModeCount; ++i) {
        BlendMode mode;
        CHECK(blendModeFromName(blendModeName(BlendMode(i)), mode));
        CHECK_EQ(int(mode), i);
        // 所有模式在不透明底上结果都不透明，且分量在 0–1 内。
        float d[4] = {0.3f, 0.6f, 0.9f, 1};
        float s[4] = {0.8f * 0.7f, 0.2f * 0.7f, 0.5f * 0.7f, 0.7f};
        blendPixel(BlendMode(i), s, d);
        CHECK_NEAR(d[3], 1, 1e-6);
        for (int c = 0; c < 3; ++c) CHECK(d[c] >= 0 && d[c] <= 1.0001f);
    }
    BlendMode mode;
    CHECK(!blendModeFromName("Darker Color", mode));
}

TEST(blend_photoshop_values) {
    // 40% 灰上的 80% 灰：颜色减淡到 100%，颜色加深到 25%（macOS 版注释里的 Photoshop 数值）。
    float d[4] = {0.4f, 0.4f, 0.4f, 1}, s[4] = {0.8f, 0.8f, 0.8f, 1};
    blendPixel(BlendMode::ColorDodge, s, d);
    CHECK_NEAR(d[0], 1, 1e-5);
    float d2[4] = {0.4f, 0.4f, 0.4f, 1};
    blendPixel(BlendMode::ColorBurn, s, d2);
    CHECK_NEAR(d2[0], 0.25, 1e-5);
}

TEST(transform_maps_unit_square) {
    LayerTransform t;
    t.x = 10; t.y = 20; t.width = 100; t.height = 50;
    double x, y;
    t.unitToDocument().apply(0, 0, x, y);
    CHECK_NEAR(x, 10, 1e-9); CHECK_NEAR(y, 20, 1e-9);
    t.flipX = true;
    t.unitToDocument().apply(0, 0, x, y);
    CHECK_NEAR(x, 110, 1e-9); CHECK_NEAR(y, 20, 1e-9);
    t.flipX = false;
    t.rotation = 90; // 顺时针：左上角转到右上方
    t.unitToDocument().apply(0, 0, x, y);
    CHECK_NEAR(x, 85, 1e-9); CHECK_NEAR(y, -5, 1e-9);
    CHECK(t.contains(60, 45));
    Affine inverse = t.unitToDocument().inverted();
    inverse.apply(85, -5, x, y);
    CHECK_NEAR(x, 0, 1e-9); CHECK_NEAR(y, 0, 1e-9);
}

TEST(uuid_format) {
    std::string id = makeUUID();
    CHECK_EQ(id.size(), size_t(36));
    CHECK_EQ(normalizeUUID(id), id);
    CHECK_EQ(normalizeUUID("6f1d3c2a-0b7e-4e8a-9c4d-2a1b3c4d5e6f"), std::string("6F1D3C2A-0B7E-4E8A-9C4D-2A1B3C4D5E6F"));
    CHECK(normalizeUUID("not-a-uuid").empty());
}

TEST(png_round_trip_keeps_pixels) {
    Image image(3, 2);
    image.fill(10, 200, 30, 255);
    image.at(1, 1)[3] = 0; image.at(1, 1)[0] = image.at(1, 1)[1] = image.at(1, 1)[2] = 0;
    auto png = encodePNG(image, 300);
    auto decoded = decodeImage(png);
    CHECK(decoded.has_value());
    CHECK_EQ(decoded->width, 3);
    CHECK(decoded->pixels == image.pixels);
    auto info = probeImage(png);
    CHECK(info && info->isPNG && !info->is16Bit);
    GrayImage mask(2, 2, 128);
    auto gray = decodeGray(encodePNG(mask));
    CHECK(gray && gray->pixels == mask.pixels);
}

TEST(manifest_example_parses_and_round_trips) {
    Json manifest = Json::parse(kExampleManifest);
    int version = 0;
    Document doc = parseManifest(manifest, &version);
    CHECK_EQ(version, 11);
    CHECK_EQ(doc.layers.size(), size_t(2));
    CHECK(doc.layers[1].adjustment.has_value());
    CHECK(doc.layers[0].extra.contains("text"));
    CHECK(doc.extra.contains("futureField"));
    AdjustmentParams params = parseAdjustment(*doc.layers[1].adjustment);
    CHECK(params.kind == AdjustmentKind::Curves);
    CHECK(params.isValid());
    CHECK_EQ(params.curves[1].size(), size_t(3));
    // 写出的清单再读回，结构不变（图片文件名由真实像素决定，这里只看元数据）。
    doc.layers[0].image = solid(4, 2, 1, 2, 3);
    Json written = makeManifest(doc);
    CHECK_EQ(written["layers"][0]["imageFile"].get<std::string>(), doc.layers[0].id + ".png");
    CHECK(written["layers"][0].contains("effects"));
    CHECK(written["futureField"]["kept"].get<bool>());
    CHECK_EQ(written["layers"][1]["adjustment"], manifest["layers"][1]["adjustment"]);
}

TEST(validation_rejects_bad_projects) {
    auto mutate = [](auto change) {
        Json manifest = Json::parse(kExampleManifest);
        change(manifest);
        return manifest;
    };
    CHECK_THROWS(parseManifest(mutate([](Json& m) { m["version"] = 12; })));
    CHECK_THROWS(parseManifest(mutate([](Json& m) { m["format"] = "other"; })));
    CHECK_THROWS(parseManifest(mutate([](Json& m) { m["layers"][0]["imageFile"] = "wrong.png"; })));
    CHECK_THROWS(parseManifest(mutate([](Json& m) { m["layers"][0]["blendMode"] = "Darker Color"; })));
    CHECK_THROWS(parseManifest(mutate([](Json& m) { m["layers"][0]["opacity"] = 1.5; })));
    CHECK_THROWS(parseManifest(mutate([](Json& m) { m["layers"][1]["id"] = m["layers"][0]["id"]; })));
    CHECK_THROWS(parseManifest(mutate([](Json& m) { m["width"] = 30001; })));
    CHECK_THROWS(parseManifest(mutate([](Json& m) { m["layers"][0]["name"] = "   "; })));
    // 调整层在版本 6 不存在。
    CHECK_THROWS(parseManifest(mutate([](Json& m) { m["version"] = 6; })));
    // 剪贴到调整层、剪贴环。
    CHECK_THROWS(parseManifest(mutate([](Json& m) { m["layers"][0]["maskSourceID"] = m["layers"][1]["id"]; })));
    CHECK_THROWS(parseManifest(mutate([](Json& m) {
        m["layers"][1].erase("adjustment");
        m["layers"][0]["maskSourceID"] = m["layers"][1]["id"];
        m["layers"][1]["maskSourceID"] = m["layers"][0]["id"];
    })));
    // 文件夹不能用混合模式；父节点必须是文件夹。
    CHECK_THROWS(parseManifest(mutate([](Json& m) { m["layers"][1].erase("adjustment"); m["layers"][1]["isGroup"] = true; m["layers"][1]["blendMode"] = "Multiply"; })));
    CHECK_THROWS(parseManifest(mutate([](Json& m) { m["layers"][1]["parentID"] = m["layers"][0]["id"]; })));
    // 蒙版文件名与版本。
    CHECK_THROWS(parseManifest(mutate([](Json& m) { m["layers"][0]["maskFile"] = "x.mask.png"; })));
    CHECK_THROWS(parseManifest(mutate([](Json& m) { m["layers"][0]["maskEnabled"] = true; })));
    // 曲线端点不在 0 / 255。
    CHECK_THROWS(parseManifest(mutate([](Json& m) { m["layers"][1]["adjustment"]["curves"]["channels"][0][0]["x"] = 3; })));
    // 正常的能过。
    parseManifest(mutate([](Json&) {}));
}

TEST(project_save_and_load_round_trip) {
    TempDir dir;
    fs::path package = dir.path / fs::u8path("示例.comp");
    Image background(4, 2);
    background.fill(200, 100, 50, 255);
    writeExample(package, background);
    Document doc = loadProject(package);
    CHECK(doc.layers[0].image != nullptr);
    CHECK(doc.layers[0].image->pixels == background.pixels);
    // 也能从 manifest.json 打开。
    loadProject(package / "manifest.json");
    // 加一个蒙版和一个文件夹后保存，再读回。
    addMask(doc, doc.layers[0].id, true);
    std::string group = addGroup(doc, "组");
    moveLayerTo(doc, doc.layers[0].id, group, DropPlacement::Inside);
    saveProject(doc, package);
    CHECK(fs::exists(package / "QuickLook" / "Preview.jpg"));
    Document again = loadProject(package);
    CHECK_EQ(again.layers.size(), size_t(3));
    const Layer* background2 = again.find(doc.layers[doc.indexOf(group) + 1].id);
    CHECK(background2 != nullptr);
    CHECK(background2->mask != nullptr);
    CHECK_EQ(background2->parentID, group);
    CHECK(background2->extra.contains("text"));
    CHECK(again.extra.contains("futureField"));
    // 没有残留的临时文件夹。
    int entries = 0;
    for (auto& e : fs::directory_iterator(dir.path)) { (void)e; ++entries; }
    CHECK_EQ(entries, 1);
}

TEST(save_in_place_updates_and_cleans_up) {
    TempDir dir;
    fs::path package = dir.path / "inplace.comp";
    const uint8_t white[4] = {255, 255, 255, 255};
    Document doc = newDocument(8, 8, white);
    std::string extra = addImageLayer(doc, Image(4, 4), "extra");
    SaveCache cache;
    saveProject(doc, package, true, &cache);
    CHECK(fs::exists(package / "images" / fs::u8path(extra + ".png")));
    // 文件夹里的其他文件不受影响。
    std::ofstream(package / "images" / "notes.txt") << "keep";
    // 删掉一个图层后就地保存：它的图片被清掉。
    deleteLayer(doc, extra);
    auto before = fs::last_write_time(package / "images" / fs::u8path(doc.layers[0].id + ".png"));
    saveProject(doc, package, true, &cache);
    CHECK(!fs::exists(package / "images" / fs::u8path(extra + ".png")));
    CHECK(fs::exists(package / "images" / "notes.txt"));
    // 没变的图片没有重写。
    CHECK(fs::last_write_time(package / "images" / fs::u8path(doc.layers[0].id + ".png")) == before);
    Document again = loadProject(package);
    CHECK_EQ(again.layers.size(), size_t(1));
    // 同名普通文件不能当项目覆盖。
    std::ofstream(dir.path / "file.comp") << "x";
    CHECK_THROWS(saveProject(doc, dir.path / "file.comp"));
}

TEST(load_rejects_missing_or_unsafe_images) {
    TempDir dir;
    fs::path package = dir.path / "bad.comp";
    fs::create_directories(package / "images");
    std::ofstream(package / "manifest.json") << kExampleManifest;
    CHECK_THROWS(loadProject(package)); // 图片缺失
    // 16 位或非 PNG 被拒。
    const uint8_t black[3] = {0, 0, 0};
    std::vector<uint8_t> jpeg = encodeJPEG(Image(4, 2), 80, black);
    writeFileAtomic(package / "images" / "6F1D3C2A-0B7E-4E8A-9C4D-2A1B3C4D5E6F.png", jpeg);
    CHECK_THROWS(loadProject(package));
}

TEST(render_single_layer_is_exact) {
    Document doc = canvas(4, 3);
    auto image = std::make_shared<Image>(2, 2);
    image->fill(255, 0, 0, 255);
    image->at(1, 1)[0] = 0; image->at(1, 1)[2] = 255;
    doc.layers.push_back(pixelLayer("a", image, 1, 1));
    Image out = flatten(doc);
    CHECK_EQ(px(out, 0, 0)[3], 0);
    CHECK_EQ(px(out, 1, 1)[0], 255);
    CHECK_EQ(px(out, 2, 2)[2], 255);
    CHECK_EQ(px(out, 2, 2)[0], 0);
    CHECK_EQ(px(out, 3, 2)[3], 0);
}

TEST(render_opacity_folder_and_visibility) {
    Document doc = canvas(2, 2);
    doc.layers.push_back(pixelLayer("bg", solid(2, 2, 0, 0, 0)));
    Layer group;
    group.id = makeUUID(); group.name = "组"; group.isGroup = true; group.opacity = 0.5;
    group.transform = LayerTransform::canvas(2, 2);
    doc.layers.push_back(group);
    Layer white = pixelLayer("w", solid(2, 2, 255, 255, 255));
    white.parentID = group.id;
    white.opacity = 0.5;
    doc.layers.push_back(white);
    Image out = flatten(doc);
    CHECK_NEAR(px(out, 0, 0)[0], 64, 1); // 0.5 × 0.5
    doc.layers[1].visible = false;
    out = flatten(doc);
    CHECK_EQ(px(out, 0, 0)[0], 0);
}

TEST(render_masks) {
    Document doc = canvas(2, 1);
    Layer layer = pixelLayer("a", solid(2, 1, 255, 255, 255));
    auto mask = std::make_shared<GrayImage>(2, 1);
    mask->pixels = {255, 0};
    layer.mask = mask;
    doc.layers.push_back(layer);
    Image out = flatten(doc);
    CHECK_EQ(px(out, 0, 0)[3], 255);
    CHECK_EQ(px(out, 1, 0)[3], 0);
    doc.layers[0].maskEnabled = false;
    out = flatten(doc);
    CHECK_EQ(px(out, 1, 0)[3], 255);
    // 1×1 均匀蒙版。
    doc.layers[0].maskEnabled = true;
    doc.layers[0].mask = std::make_shared<GrayImage>(1, 1, 0);
    out = flatten(doc);
    CHECK_EQ(px(out, 0, 0)[3], 0);
}

TEST(render_folder_mask_clips_contents) {
    Document doc = canvas(2, 1);
    Layer group;
    group.id = makeUUID(); group.name = "组"; group.isGroup = true;
    group.transform = LayerTransform::canvas(2, 1);
    auto mask = std::make_shared<GrayImage>(2, 1);
    mask->pixels = {0, 255};
    group.mask = mask;
    doc.layers.push_back(group);
    Layer child = pixelLayer("c", solid(2, 1, 255, 0, 0));
    child.parentID = group.id;
    doc.layers.push_back(child);
    Image out = flatten(doc);
    CHECK_EQ(px(out, 0, 0)[3], 0);
    CHECK_EQ(px(out, 1, 0)[3], 255);
}

TEST(render_clipping_stack_uses_base_alpha) {
    Document doc = canvas(3, 1);
    auto base = std::make_shared<Image>(3, 1);
    base->fill(0, 0, 255, 255);
    base->at(2, 0)[0] = base->at(2, 0)[1] = base->at(2, 0)[2] = base->at(2, 0)[3] = 0; // 最右边透明
    Layer b = pixelLayer("base", base);
    doc.layers.push_back(b);
    Layer clipped = pixelLayer("clip", solid(3, 1, 255, 0, 0));
    clipped.maskSourceID = b.id;
    doc.layers.push_back(clipped);
    Image out = flatten(doc);
    CHECK_EQ(px(out, 0, 0)[0], 255);
    CHECK_EQ(px(out, 0, 0)[2], 0);
    CHECK_EQ(px(out, 2, 0)[3], 0);
    // 基底隐藏时不在同一个堆栈里：剪贴图层按基底覆盖度裁剪。
    doc.layers[0].visible = false;
    out = flatten(doc);
    CHECK_EQ(px(out, 0, 0)[0], 255);
    CHECK_EQ(px(out, 2, 0)[3], 0);
}

TEST(render_adjustment_layers) {
    Document doc = canvas(2, 1);
    doc.layers.push_back(pixelLayer("bg", solid(2, 1, 10, 100, 200)));
    std::string id = addAdjustmentLayer(doc, AdjustmentKind::Invert, 0);
    Image out = flatten(doc);
    CHECK_EQ(px(out, 0, 0)[0], 245);
    CHECK_EQ(px(out, 0, 0)[1], 155);
    CHECK_EQ(px(out, 0, 0)[2], 55);
    // 50% 不透明度：一半一半。
    doc.find(id)->opacity = 0.5;
    out = flatten(doc);
    CHECK_NEAR(px(out, 0, 0)[0], 128, 1);
    // 蒙版只作用于一半。
    doc.find(id)->opacity = 1;
    auto mask = std::make_shared<GrayImage>(2, 1);
    mask->pixels = {255, 0};
    doc.find(id)->mask = mask;
    out = flatten(doc);
    CHECK_EQ(px(out, 0, 0)[0], 245);
    CHECK_EQ(px(out, 1, 0)[0], 10);
}

TEST(uniform_masks_on_adjustments_and_folders_cover_everything) {
    // 1×1 的均匀蒙版覆盖整个矩形，边缘只有一个输出像素的过渡（回归：曾按蒙版像素算边缘，出现 X 形渐变）。
    Document doc = canvas(40, 30);
    doc.layers.push_back(pixelLayer("bg", solid(40, 30, 255, 255, 255)));
    std::string id = addAdjustmentLayer(doc, AdjustmentKind::Invert, 0);
    addMask(doc, id, true);
    for (double scale : {1.0, 2.0, 0.5}) {
        Renderer renderer;
        RenderOptions options;
        options.width = int(40 * scale); options.height = int(30 * scale); options.scale = scale;
        Image out = renderer.render(doc, options);
        for (int y = 0; y < out.height; ++y) for (int x = 0; x < out.width; ++x) CHECK_EQ(out.at(x, y)[0], 0);
    }
    // 文件夹的均匀蒙版同理。
    Document folder = canvas(20, 20);
    std::string g = addGroup(folder, "g");
    addMask(folder, g, true);
    folder.layers.push_back(pixelLayer("c", solid(20, 20, 255, 0, 0)));
    folder.layers.back().parentID = g;
    Image out = flatten(folder);
    for (int y = 0; y < 20; ++y) for (int x = 0; x < 20; ++x) CHECK_EQ(out.at(x, y)[3], 255);
}

TEST(adjustment_kinds_render_and_round_trip) {
    for (AdjustmentKind kind : allAdjustmentKinds()) {
        Json json = makeAdjustment(kind, 42);
        AdjustmentParams params = parseAdjustment(json);
        CHECK(params.kind == kind);
        CHECK(params.isValid());
        Json again = json;
        writeAdjustment(params, again);
        CHECK_EQ(again, json);
        Image image(8, 8);
        image.fill(120, 80, 40, 255);
        applyAdjustment(params, image);
        for (size_t i = 0; i < image.pixels.size(); i += 4) {
            CHECK(image.pixels[i] <= image.pixels[i + 3]); // 仍是合法的预乘
        }
    }
    // 色阶：输入黑场 128 把暗部压黑。
    Json levels = makeAdjustment(AdjustmentKind::Levels);
    levels["levels"]["ranges"][0]["black"] = 128;
    Image image(1, 1);
    image.fill(100, 200, 255, 255);
    applyAdjustment(parseAdjustment(levels), image);
    CHECK_EQ(image.pixels[0], 0);
    CHECK_NEAR(image.pixels[1], 145, 2);
    // 色相/饱和度：饱和度 −100 变灰。
    Json hsv = makeAdjustment(AdjustmentKind::HueSaturation);
    hsv["saturation"] = -100;
    Image color(1, 1);
    color.fill(200, 50, 50, 255);
    applyAdjustment(parseAdjustment(hsv), color);
    CHECK_NEAR(color.pixels[0], color.pixels[1], 2);
    CHECK_NEAR(color.pixels[1], color.pixels[2], 2);
}

TEST(hsv_settings_swift_dictionary_form) {
    // Swift 把枚举键字典编码成数组：["Master", {…}, "Reds", {…}]。
    Json json = makeAdjustment(AdjustmentKind::HueSaturation);
    json["hsvSettings"] = Json::parse(R"({"range":"Reds","colorize":false,"invertRange":false,
        "adjustments":["Reds",{"hue":0,"saturation":-100,"lightness":0}],
        "bands":["Master",{"falloffStart":0,"rangeStart":0,"rangeEnd":360,"falloffEnd":360}]})");
    AdjustmentParams params = parseAdjustment(json);
    CHECK_EQ(params.selectedRange, 1);
    CHECK_NEAR(params.hueRanges[1].saturation, -100, 0);
    Image red(1, 1), blue(1, 1);
    red.fill(220, 30, 30, 255);
    blue.fill(30, 30, 220, 255);
    applyAdjustment(params, red);
    applyAdjustment(params, blue);
    CHECK_NEAR(red.pixels[0], red.pixels[2], 3);   // 红色被去饱和
    CHECK(blue.pixels[2] > blue.pixels[0] + 100);  // 蓝色不受影响
}

TEST(edit_layer_tree_operations) {
    Document doc = newDocument(10, 10, nullptr);
    std::string a = doc.activeLayerID;
    std::string b = addBlankLayer(doc, "b");
    std::string g = addGroup(doc, "g");
    std::string c = addBlankLayer(doc, "c"); // 进入文件夹
    CHECK_EQ(doc.find(c)->parentID, g);
    CHECK_EQ(doc.indexOf(c), doc.indexOf(g) + 1);
    // 文件夹整体下移一格，内容跟随。
    CHECK(moveLayer(doc, g, -1));
    CHECK_EQ(doc.indexOf(g), 1);
    CHECK_EQ(doc.indexOf(c), 2);
    CHECK_EQ(doc.indexOf(b), 3);
    CHECK(!moveLayer(doc, c, 1));
    // 复制文件夹会复制内容并给新 id。
    std::string g2 = duplicateLayer(doc, g);
    CHECK_EQ(doc.layers.size(), size_t(6));
    CHECK_EQ(doc.layers[size_t(doc.indexOf(g2) + 1)].parentID, g2);
    validateDocument(doc);
    // 删除文件夹连同内容。
    deleteLayer(doc, g);
    CHECK(doc.find(c) == nullptr);
    CHECK_EQ(doc.layers.size(), size_t(4));
    // 不能把文件夹拖进自己里面。
    CHECK(!moveLayerTo(doc, g2, doc.layers[size_t(doc.indexOf(g2) + 1)].id, DropPlacement::Inside));
    CHECK(moveLayerTo(doc, a, g2, DropPlacement::Inside));
    CHECK_EQ(doc.find(a)->parentID, g2);
    validateDocument(doc);
}

TEST(normalize_order_keeps_rendering) {
    // macOS 版允许内容在数组里不紧跟文件夹；规范化后渲染不变。
    Document doc = canvas(1, 1);
    Layer group;
    group.id = makeUUID(); group.name = "g"; group.isGroup = true;
    group.transform = LayerTransform::canvas(1, 1);
    Layer inside = pixelLayer("in", solid(1, 1, 255, 0, 0));
    inside.parentID = group.id;
    Layer top = pixelLayer("top", solid(1, 1, 0, 0, 255, 128));
    doc.layers = {inside, group, top};
    Image before = flatten(doc);
    doc.normalizeOrder();
    CHECK_EQ(doc.layers[0].id, group.id);
    CHECK_EQ(doc.layers[1].id, inside.id);
    Image after = flatten(doc);
    CHECK(before.pixels == after.pixels);
}

TEST(edit_clipping_and_merge) {
    Document doc = canvas(2, 1);
    doc.layers.push_back(pixelLayer("base", solid(2, 1, 0, 0, 255)));
    doc.layers.push_back(pixelLayer("top", solid(2, 1, 255, 0, 0, 128)));
    std::string top = doc.layers[1].id, base = doc.layers[0].id;
    CHECK(toggleClipping(doc, top));
    CHECK_EQ(doc.find(top)->maskSourceID, base);
    CHECK(toggleClipping(doc, top));
    CHECK(doc.find(top)->maskSourceID.empty());
    Image before = flatten(doc);
    CHECK(mergeDown(doc, top));
    CHECK_EQ(doc.layers.size(), size_t(1));
    Image after = flatten(doc);
    for (size_t i = 0; i < before.pixels.size(); ++i) CHECK_NEAR(before.pixels[i], after.pixels[i], 1);
}

TEST(brush_paints_and_erases) {
    Document doc = newDocument(20, 20, nullptr);
    std::string id = doc.activeLayerID;
    const uint8_t red[4] = {255, 0, 0, 255};
    BrushSettings brush;
    brush.size = 6;
    brush.hardness = 1;
    CHECK(paintStroke(doc, id, {{2, 10}, {18, 10}}, brush, red, false, false));
    const Image& image = *doc.find(id)->image;
    CHECK_EQ(image.at(10, 10)[0], 255);
    CHECK_EQ(image.at(10, 10)[3], 255);
    CHECK_EQ(image.at(10, 2)[3], 0);
    // 半透明画笔来回涂同一处也不会叠深。
    BrushSettings soft = brush;
    soft.opacity = 0.5;
    Document doc2 = newDocument(20, 20, nullptr);
    paintStroke(doc2, doc2.activeLayerID, {{2, 10}, {18, 10}, {2, 10}}, soft, red, false, false);
    CHECK_NEAR(doc2.find(doc2.activeLayerID)->image->at(10, 10)[3], 128, 1);
    // 橡皮擦。
    CHECK(paintStroke(doc, id, {{10, 10}}, brush, red, true, false));
    CHECK_EQ(doc.find(id)->image->at(10, 10)[3], 0);
    // 蒙版上画黑色。
    addMask(doc, id, true);
    const uint8_t black[4] = {0, 0, 0, 255};
    CHECK(paintStroke(doc, id, {{4, 10}}, brush, black, false, true));
    CHECK_EQ(doc.find(id)->mask->width, 20);
    CHECK_EQ(doc.find(id)->mask->row(10)[4], 0);
    CHECK_EQ(doc.find(id)->mask->row(0)[0], 255);
}

TEST(brush_follows_layer_transform) {
    Document doc = canvas(20, 20);
    Layer layer = pixelLayer("half", std::make_shared<Image>(40, 40));
    layer.transform.width = 20; layer.transform.height = 20; // 2 倍像素密度
    doc.layers.push_back(layer);
    const uint8_t white[4] = {255, 255, 255, 255};
    BrushSettings brush;
    brush.size = 4; brush.hardness = 1;
    paintStroke(doc, layer.id, {{5, 5}}, brush, white, false, false);
    const Image& image = *doc.find(layer.id)->image;
    CHECK_EQ(image.at(10, 10)[3], 255); // 文档 (5,5) 对应像素 (10,10)
    CHECK_EQ(image.at(5, 5)[3], 0);
}

TEST(canvas_operations) {
    const uint8_t white[4] = {255, 255, 255, 255};
    Document doc = newDocument(10, 10, white);
    resizeCanvas(doc, 20, 10, 0.5, 0.5);
    CHECK_EQ(doc.layers[0].transform.x, 5);
    cropCanvas(doc, 5, 0, 10, 10);
    CHECK_EQ(doc.layers[0].transform.x, 0);
    resizeImage(doc, 5, 5);
    CHECK_EQ(doc.layers[0].transform.width, 5);
    Image out = flatten(doc);
    CHECK_EQ(out.width, 5);
    CHECK_EQ(out.at(2, 2)[3], 255);
    flipCanvas(doc, true);
    CHECK(doc.layers[0].transform.flipX);
}

TEST(rotated_and_flipped_layers_render) {
    Document doc = canvas(4, 4);
    auto image = std::make_shared<Image>(4, 4);
    image->fill(0, 0, 0, 0);
    image->at(0, 0)[0] = image->at(0, 0)[3] = 255; // 左上角一个红点
    Layer layer = pixelLayer("p", image);
    layer.transform.sampling = Sampling::Nearest;
    layer.transform.flipX = true;
    doc.layers.push_back(layer);
    Image out = flatten(doc);
    CHECK_EQ(out.at(3, 0)[3], 255);
    CHECK_EQ(out.at(0, 0)[3], 0);
    doc.layers[0].transform.flipX = false;
    doc.layers[0].transform.rotation = 90;
    out = flatten(doc);
    CHECK_EQ(out.at(3, 0)[3], 255);
    doc.layers[0].transform.rotation = 180;
    out = flatten(doc);
    CHECK_EQ(out.at(3, 3)[3], 255);
}

TEST(scaled_render_uses_mipmaps) {
    Document doc = canvas(64, 64);
    auto image = std::make_shared<Image>(64, 64);
    for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x) {
        uint8_t v = ((x + y) % 2) ? 255 : 0;
        image->at(x, y)[0] = image->at(x, y)[1] = image->at(x, y)[2] = v;
        image->at(x, y)[3] = 255;
    }
    doc.layers.push_back(pixelLayer("checker", image));
    Renderer renderer;
    RenderOptions options;
    options.width = 8; options.height = 8; options.scale = 0.125;
    Image out = renderer.render(doc, options);
    // 棋盘格缩小后应接近 50% 灰，而不是随机的黑白点。
    for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x) CHECK_NEAR(out.at(x, y)[0], 128, 12);
}

namespace {
// 20×20 画布中间一块 8×8 的白色方块，带指定效果。
Document effectsDoc(const Json& effects) {
    Document doc = canvas(20, 20);
    Layer square = pixelLayer("square", solid(8, 8, 255, 255, 255), 6, 6);
    square.extra["effects"] = effects;
    doc.layers.push_back(square);
    return doc;
}
}

TEST(effects_stroke_outside_and_inside) {
    Image out = flatten(effectsDoc(Json::parse(R"({"stroke":{"size":2,"red":1,"green":0,"blue":0,"opacity":1,"inside":false}})")));
    CHECK_EQ(out.at(10, 10)[1], 255);          // 方块本身不变
    CHECK_EQ(out.at(5, 10)[0], 255);           // 左边外侧 1 像素是红色描边
    CHECK_EQ(out.at(5, 10)[1], 0);
    CHECK_EQ(out.at(4, 10)[3], 255);           // 2 像素宽
    CHECK_EQ(out.at(3, 10)[3], 0);
    out = flatten(effectsDoc(Json::parse(R"({"stroke":{"size":2,"red":1,"green":0,"blue":0,"opacity":1,"inside":true}})")));
    CHECK_EQ(out.at(6, 10)[1], 0);             // 内侧描边压在像素上
    CHECK_EQ(out.at(10, 10)[1], 255);
    CHECK_EQ(out.at(5, 10)[3], 0);
}

TEST(effects_drop_shadow_falls_away_from_light) {
    // 光从正上方（90°）来，阴影向下 4 像素，不模糊。
    Image out = flatten(effectsDoc(Json::parse(R"({"shadow":{"angle":90,"distance":4,"blur":0,"red":0,"green":0,"blue":0,"opacity":1}})")));
    CHECK_EQ(out.at(10, 15)[3], 255);          // 方块下方是阴影
    CHECK_EQ(out.at(10, 15)[0], 0);
    CHECK_EQ(out.at(10, 4)[3], 0);             // 上方没有
    CHECK_EQ(out.at(10, 10)[0], 255);          // 方块盖在阴影上
}

TEST(effects_overlay_glow_and_disabled) {
    Image out = flatten(effectsDoc(Json::parse(R"({"colorOverlay":{"red":0,"green":0,"blue":1,"opacity":1}})")));
    CHECK_EQ(out.at(10, 10)[0], 0);
    CHECK_EQ(out.at(10, 10)[2], 255);
    CHECK_EQ(out.at(2, 2)[3], 0);
    out = flatten(effectsDoc(Json::parse(R"({"colorOverlay":{"enabled":false,"red":0,"green":0,"blue":1,"opacity":1}})")));
    CHECK_EQ(out.at(10, 10)[0], 255);          // 隐藏的效果不画
    out = flatten(effectsDoc(Json::parse(R"({"outerGlow":{"size":6,"red":1,"green":1,"blue":0,"opacity":1}})")));
    CHECK(out.at(5, 10)[3] > 0);               // 外面有光晕
    CHECK(out.at(5, 10)[3] < 255);
    CHECK_EQ(out.at(10, 10)[2], 255);          // 里面不受影响
    out = flatten(effectsDoc(Json::parse(R"({"innerShadow":{"angle":90,"distance":3,"blur":0,"red":0,"green":0,"blue":0,"opacity":1}})")));
    CHECK_EQ(out.at(10, 6)[0], 0);             // 上边缘内侧变暗
    CHECK_EQ(out.at(10, 12)[0], 255);
}

TEST(effects_follow_layer_transform_and_mask) {
    // 缩放 2 倍的图层：效果按图层像素计算，放大后描边也是 2 倍宽。
    Document doc = canvas(40, 40);
    Layer square = pixelLayer("s", solid(8, 8, 255, 255, 255), 12, 12);
    square.transform.width = square.transform.height = 16;
    square.extra["effects"] = Json::parse(R"({"stroke":{"size":2,"red":1,"green":0,"blue":0,"opacity":1,"inside":false}})");
    doc.layers.push_back(square);
    Image out = flatten(doc);
    CHECK_EQ(out.at(20, 20)[1], 255);
    CHECK_EQ(out.at(9, 20)[0], 255);
    CHECK_EQ(out.at(9, 20)[1], 0);
    CHECK_EQ(out.at(6, 20)[3], 0);             // 放大插值只柔化描边外缘的一个像素
    // 蒙版隐藏右半边：描边沿显示出来的形状走，右半边的像素也不出现。
    auto mask = std::make_shared<GrayImage>(2, 1);
    mask->pixels = {255, 0};
    doc.layers[0].mask = mask;
    out = flatten(doc);
    CHECK_EQ(out.at(14, 20)[1], 255);
    CHECK_EQ(out.at(26, 20)[1], 0);
}

TEST(effects_json_round_trip) {
    const double gray[3] = {0.5, 0.5, 0.5};
    LayerEffectsParams params;
    for (EffectKind kind : allEffectKinds()) params[kind] = defaultEffect(kind, gray);
    params[EffectKind::Shadow].enabled = false;
    Json json = Json::parse(R"({"futureEffect":{"x":1}})");
    writeEffects(params, json);
    CHECK(json.contains("futureEffect"));
    CHECK_EQ(json["shadow"]["enabled"].get<bool>(), false);
    CHECK(!json["stroke"].contains("enabled"));
    LayerEffectsParams again = parseEffects(json);
    CHECK(again.isValid());
    CHECK(!again[EffectKind::Shadow].enabled);
    CHECK_NEAR(again[EffectKind::Stroke].color[0], 0.5, 1e-9);
    CHECK_NEAR(again[EffectKind::OuterGlow].size, 20, 1e-9);
    params[EffectKind::Stroke].present = false;
    writeEffects(params, json);
    CHECK(!json.contains("stroke"));
}

TEST(selection_shapes_and_modes) {
    GrayImage rect = rectSelection(10, 10, 2, 2, 4, 3, false);
    CHECK_EQ(rect.row(3)[3], 255);
    CHECK_EQ(rect.row(1)[3], 0);
    CHECK_EQ(rect.row(5)[3], 0);
    int x, y, w, h;
    CHECK(selectionBounds(rect, x, y, w, h));
    CHECK_EQ(x, 2); CHECK_EQ(y, 2); CHECK_EQ(w, 4); CHECK_EQ(h, 3);
    // 半像素边缘抗锯齿。
    GrayImage half = rectSelection(4, 4, 0.5, 0, 2, 4, false);
    CHECK_NEAR(half.row(0)[0], 128, 1);
    CHECK_EQ(half.row(0)[1], 255);
    GrayImage ellipse = rectSelection(20, 20, 0, 0, 20, 20, true);
    CHECK_EQ(ellipse.row(10)[10], 255);
    CHECK_EQ(ellipse.row(0)[0], 0);
    GrayImage other = rectSelection(10, 10, 4, 0, 4, 10, false);
    GrayImage added = combineSelection(&rect, other, SelectionMode::Add);
    CHECK_EQ(added.row(8)[5], 255);
    GrayImage subtracted = combineSelection(&rect, other, SelectionMode::Subtract);
    CHECK_EQ(subtracted.row(3)[2], 255);
    CHECK_EQ(subtracted.row(3)[5], 0);
    GrayImage intersected = combineSelection(&rect, other, SelectionMode::Intersect);
    CHECK_EQ(intersected.row(3)[5], 255);
    CHECK_EQ(intersected.row(3)[2], 0);
    GrayImage inverted = invertSelection(&rect, 10, 10);
    CHECK_EQ(inverted.row(0)[0], 255);
    CHECK_EQ(inverted.row(3)[3], 0);
}

TEST(selection_polygon_feather_grow) {
    // 三角形：左上、右上、左下。
    GrayImage tri = polygonSelection(10, 10, {{0, 0}, {10, 0}, {0, 10}});
    CHECK_EQ(tri.row(1)[1], 255);
    CHECK_EQ(tri.row(8)[8], 0);
    CHECK(tri.row(4)[5] > 0 && tri.row(4)[5] < 255); // 斜边上的像素部分覆盖
    GrayImage box = rectSelection(20, 20, 5, 5, 10, 10, false);
    GrayImage grown = box;
    growSelection(grown, 2);
    CHECK_EQ(grown.row(10)[3], 255);
    CHECK_EQ(grown.row(10)[2], 0);
    GrayImage shrunk = box;
    growSelection(shrunk, -2);
    CHECK_EQ(shrunk.row(10)[6], 0);
    CHECK_EQ(shrunk.row(10)[7], 255);
    GrayImage soft = box;
    featherSelection(soft, 4);
    CHECK(soft.row(10)[5] > 30 && soft.row(10)[5] < 225);
    CHECK_EQ(soft.row(10)[10], 255);
    std::vector<std::vector<std::pair<int, int>>> loops;
    CHECK(selectionOutline(box, loops));
    CHECK_EQ(loops.size(), size_t(1));
    CHECK_EQ(loops[0].size(), size_t(4));
}

TEST(selection_edits_on_transformed_layers) {
    // 图层 2 倍像素密度：清除选区要按文档坐标对上图层像素。
    Document doc = canvas(10, 10);
    Layer layer = pixelLayer("l", solid(20, 20, 255, 0, 0));
    layer.transform.width = layer.transform.height = 10;
    doc.layers.push_back(layer);
    GrayImage sel = rectSelection(10, 10, 0, 0, 5, 10, false);
    CHECK(clearSelection(doc, layer.id, sel));
    const Image& pixels = *doc.layers[0].image;
    CHECK_EQ(pixels.at(5, 10)[3], 0);     // 左半边（文档 0–5 → 像素 0–10）被清掉
    CHECK_EQ(pixels.at(15, 10)[3], 255);
    const uint8_t blue[4] = {0, 0, 255, 255};
    CHECK(fillSelection(doc, layer.id, sel, blue));
    CHECK_EQ(doc.layers[0].image->at(5, 10)[2], 255);
    CHECK(maskFromSelection(doc, layer.id, sel));
    Image out = flatten(doc);
    CHECK_EQ(out.at(2, 5)[2], 255);
    CHECK_EQ(out.at(8, 5)[3], 0);           // 右半边被蒙版隐藏
    // 文件夹不能填充。
    std::string g = addGroup(doc, "g");
    CHECK(!fillSelection(doc, g, sel, blue));
}

TEST(selection_wand_copy_crop_and_brush) {
    Document doc = canvas(10, 10);
    auto image = std::make_shared<Image>(10, 10);
    image->fill(255, 255, 255, 255);
    for (int y = 2; y < 6; ++y) for (int x = 2; x < 6; ++x) { uint8_t* p = image->at(x, y); p[0] = 0; p[1] = 0; p[2] = 0; }
    doc.layers.push_back(pixelLayer("bg", image));
    auto wand = wandSelection(*doc.layers[0].image, 3, 3, 10, true, 0);
    CHECK(wand.has_value());
    int x, y, w, h;
    CHECK(selectionBounds(*wand, x, y, w, h));
    CHECK_EQ(x, 2); CHECK_EQ(w, 4);
    CHECK(!wandSelection(*image, 20, 3, 10, true, 0).has_value());
    int ox = 0, oy = 0;
    Image copied = copySelection(flatten(doc), *wand, ox, oy);
    CHECK_EQ(copied.width, 4);
    CHECK_EQ(ox, 2);
    CHECK_EQ(copied.at(0, 0)[0], 0);
    // 画笔只在选区内落墨。
    doc.selection = std::make_shared<GrayImage>(*wand);
    BrushSettings brush;
    brush.size = 20; brush.hardness = 1;
    const uint8_t red[4] = {255, 0, 0, 255};
    CHECK(paintStroke(doc, doc.layers[0].id, {{5, 5}}, brush, red, false, false));
    CHECK_EQ(doc.layers[0].image->at(3, 3)[0], 255);
    CHECK_EQ(doc.layers[0].image->at(8, 8)[1], 255); // 选区外仍是白色
    CHECK(cropToSelection(doc));
    CHECK_EQ(doc.width, 4);
    CHECK(!doc.selection);
}

TEST(content_aware_fill_uses_surroundings) {
    // 灰色画面中间有个黑点，选中黑点做内容识别填充，结果应接近周围的灰色。
    Document doc = canvas(32, 32);
    auto image = std::make_shared<Image>(32, 32);
    image->fill(128, 128, 128, 255);
    for (int y = 14; y < 18; ++y) for (int x = 14; x < 18; ++x) { uint8_t* p = image->at(x, y); p[0] = p[1] = p[2] = 0; }
    doc.layers.push_back(pixelLayer("bg", image));
    GrayImage sel = rectSelection(32, 32, 13, 13, 6, 6, false);
    CHECK_EQ(contentAwareFill(doc, doc.layers[0].id, sel), 1);
    CHECK_NEAR(doc.layers[0].image->at(15, 15)[0], 128, 4);
}

TEST(clone_stamp_copies_from_offset) {
    // 左半红右半蓝，从右边（+10 像素）取样画在左边。
    Document doc = canvas(20, 10);
    auto image = std::make_shared<Image>(20, 10);
    for (int y = 0; y < 10; ++y) for (int x = 0; x < 20; ++x) {
        uint8_t* p = image->at(x, y);
        p[0] = x < 10 ? 255 : 0; p[1] = 0; p[2] = x < 10 ? 0 : 255; p[3] = 255;
    }
    doc.layers.push_back(pixelLayer("bg", image));
    BrushSettings brush;
    brush.size = 4; brush.hardness = 1;
    StrokeOptions options;
    options.kind = StrokeKind::Clone;
    options.cloneOffsetX = 10;
    const uint8_t none[4] = {0, 0, 0, 255};
    BrushStroke stroke(doc, doc.layers[0].id, brush, none, options, false);
    CHECK(stroke.isValid());
    stroke.addPoint(5, 5);
    CHECK(stroke.finish());
    CHECK_EQ(doc.layers[0].image->at(5, 5)[2], 255);
    CHECK_EQ(doc.layers[0].image->at(5, 5)[0], 0);
    CHECK_EQ(doc.layers[0].image->at(1, 5)[0], 255); // 笔刷外不变
    // 对所有图层取样：从合成图取。
    Document doc2 = doc;
    options.cloneSource = std::make_shared<Image>(flatten(doc2));
    options.cloneOffsetX = -10;
    BrushStroke back(doc2, doc2.layers[0].id, brush, none, options, false);
    back.addPoint(18, 5);                              // 从 x=8 取样（第一笔没碰到的红色）
    back.finish();
    CHECK_EQ(doc2.layers[0].image->at(18, 5)[0], 255);
    // 仿制不能画在蒙版上。
    addMask(doc, doc.layers[0].id, true);
    BrushStroke onMask(doc, doc.layers[0].id, brush, none, options, true);
    CHECK(!onMask.isValid());
}

TEST(spot_heal_removes_blemish) {
    Document doc = canvas(64, 64);
    auto image = std::make_shared<Image>(64, 64);
    image->fill(120, 140, 160, 255);
    for (int y = 30; y < 34; ++y) for (int x = 30; x < 34; ++x) { uint8_t* p = image->at(x, y); p[0] = p[1] = p[2] = 0; }
    doc.layers.push_back(pixelLayer("bg", image));
    BrushSettings brush;
    brush.size = 10; brush.hardness = 0.5;
    StrokeOptions options;
    options.kind = StrokeKind::Heal;
    options.seed = 7;
    const uint8_t none[4] = {0, 0, 0, 255};
    BrushStroke stroke(doc, doc.layers[0].id, brush, none, options, false);
    stroke.addPoint(31.5, 31.5);
    CHECK(doc.layers[0].image->at(20, 31)[0] == 120);   // 预览只动笔刷范围
    CHECK(stroke.finish());
    const uint8_t* healed = doc.layers[0].image->at(31, 31);
    CHECK_NEAR(healed[0], 120, 12);
    CHECK_NEAR(healed[2], 160, 12);
}

TEST(gradient_linear_radial_selection_and_mask) {
    Document doc = newDocument(11, 3, nullptr);
    std::string id = doc.activeLayerID;
    const uint8_t black[4] = {0, 0, 0, 255}, white[4] = {255, 255, 255, 255};
    GradientSettings settings;
    CHECK(drawGradient(doc, id, 0.5, 1, 10.5, 1, black, white, settings, false));
    const Image& g = *doc.find(id)->image;
    CHECK_EQ(g.at(0, 1)[0], 0);
    CHECK_NEAR(g.at(5, 1)[0], 128, 1);
    CHECK_EQ(g.at(10, 1)[0], 255);
    CHECK_EQ(g.at(5, 1)[3], 255);
    // 径向：中心是起始色。
    settings.radial = true;
    CHECK(drawGradient(doc, id, 5.5, 1.5, 10.5, 1.5, white, black, settings, false));
    CHECK_EQ(doc.find(id)->image->at(5, 1)[0], 255);
    CHECK(doc.find(id)->image->at(9, 1)[0] < 80);
    // 选区限制：只有左边几列变。
    settings.radial = false;
    doc.selection = std::make_shared<GrayImage>(rectSelection(11, 3, 0, 0, 3, 3, false));
    const uint8_t red[4] = {255, 0, 0, 255};
    CHECK(drawGradient(doc, id, 0, 0, 11, 0, red, red, settings, false));
    CHECK_EQ(doc.find(id)->image->at(1, 1)[1], 0);
    CHECK(doc.find(id)->image->at(8, 1)[1] > 0);
    doc.selection.reset();
    // 蒙版上：从黑到白。
    addMask(doc, id, true);
    CHECK(drawGradient(doc, id, 0.5, 1, 10.5, 1, black, white, settings, true));
    CHECK_EQ(doc.find(id)->mask->width, 11);
    CHECK_EQ(doc.find(id)->mask->row(1)[0], 0);
    CHECK_EQ(doc.find(id)->mask->row(1)[10], 255);
    // 太短的线不画。
    CHECK(!drawGradient(doc, id, 1, 1, 1.2, 1, black, white, settings, false));
}
