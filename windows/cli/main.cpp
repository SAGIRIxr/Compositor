// compositor-cli：不开界面查看、校验、渲染和新建 .comp 项目。
#include "compositor/edit.h"
#include "compositor/image_io.h"
#include "compositor/project_io.h"
#include "compositor/renderer.h"
#include <cctype>
#include <cstdio>
#include <functional>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

using namespace comp;

namespace {

void usage() {
    std::cout <<
        "用法：\n"
        "  compositor-cli info <项目.comp>                 显示画布与图层结构\n"
        "  compositor-cli validate <项目.comp>             校验项目能否被 Compositor 打开\n"
        "  compositor-cli render <项目.comp> <输出.png|.jpg> [--scale 倍数] [--quality 1-100]\n"
        "                                                   拼合并导出图片\n"
        "  compositor-cli new <输出.comp> <宽> <高> [--fill RRGGBB]\n"
        "                                                   新建空白项目\n"
        "  compositor-cli import <图片> <输出.comp>         用一张图片新建项目\n";
}

std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

void printTree(const Document& doc) {
    std::cout << "画布：" << doc.width << " × " << doc.height << " 像素，" << doc.effectiveResolution() << " ppi\n";
    std::cout << "图层（从上到下）：\n";
    std::function<void(const std::string&, int)> walk = [&](const std::string& parent, int depth) {
        std::vector<const Layer*> children;
        for (const auto& l : doc.layers) if (l.parentID == parent) children.push_back(&l);
        for (auto it = children.rbegin(); it != children.rend(); ++it) {
            const Layer& l = **it;
            std::string kind = l.isGroup ? "文件夹" : l.adjustment ? "调整" : l.hasText() ? "文字" : l.image ? "像素" : "空白";
            std::cout << std::string(size_t(depth) * 2 + 2, ' ') << (l.visible ? "● " : "○ ") << l.name << "  [" << kind;
            if (l.image) std::cout << " " << l.image->width << "×" << l.image->height;
            std::cout << "，" << blendModeName(l.blendMode) << "，" << int(l.opacity * 100 + 0.5) << "%";
            if (l.mask) std::cout << "，蒙版";
            if (!l.maskSourceID.empty()) std::cout << "，剪贴";
            if (l.extra.contains("effects")) std::cout << "，效果";
            std::cout << "]\n";
            if (l.isGroup) walk(l.id, depth + 1);
        }
    };
    walk(std::string(), 0);
}

int run(const std::vector<std::string>& args) {
    if (args.size() < 2) { usage(); return 1; }
    const std::string& command = args[1];
    try {
        if (command == "info" && args.size() >= 3) {
            Document doc = loadProject(fs::u8path(args[2]));
            printTree(doc);
            return 0;
        }
        if (command == "validate" && args.size() >= 3) {
            loadProject(fs::u8path(args[2]));
            std::cout << "项目有效。\n";
            return 0;
        }
        if (command == "render" && args.size() >= 4) {
            double scale = 1;
            int quality = 90;
            for (size_t i = 4; i + 1 < args.size(); i += 2) {
                if (args[i] == "--scale") scale = std::atof(args[i + 1].c_str());
                else if (args[i] == "--quality") quality = std::atoi(args[i + 1].c_str());
            }
            Document doc = loadProject(fs::u8path(args[2]));
            if (!(scale > 0)) scale = 1;
            RenderOptions options;
            options.width = std::max(1, int(doc.width * scale + 0.5));
            options.height = std::max(1, int(doc.height * scale + 0.5));
            options.scale = scale;
            Renderer renderer;
            Image image = renderer.render(doc, options);
            fs::path out = fs::u8path(args[3]);
            std::string ext = lower(out.extension().u8string());
            std::vector<uint8_t> bytes;
            if (ext == ".jpg" || ext == ".jpeg") {
                const uint8_t white[3] = {255, 255, 255};
                bytes = encodeJPEG(image, quality, white, doc.effectiveResolution());
            } else {
                bytes = encodePNG(image, doc.effectiveResolution());
            }
            if (bytes.empty() || !writeFileAtomic(out, bytes)) { std::cerr << "无法写入 " << args[3] << "\n"; return 2; }
            std::cout << "已导出 " << image.width << " × " << image.height << " 到 " << args[3] << "\n";
            return 0;
        }
        if (command == "new" && args.size() >= 5) {
            int w = std::atoi(args[3].c_str()), h = std::atoi(args[4].c_str());
            if (w < 1 || h < 1 || w > kMaxSide || h > kMaxSide) { std::cerr << "尺寸须在 1–30000 之间\n"; return 1; }
            uint8_t fill[4] = {255, 255, 255, 255};
            bool hasFill = false;
            for (size_t i = 5; i + 1 < args.size(); i += 2) {
                if (args[i] == "--fill" && args[i + 1].size() >= 6) {
                    unsigned long v = std::strtoul(args[i + 1].c_str() + (args[i + 1][0] == '#' ? 1 : 0), nullptr, 16);
                    fill[0] = uint8_t(v >> 16); fill[1] = uint8_t(v >> 8); fill[2] = uint8_t(v);
                    hasFill = true;
                }
            }
            Document doc = newDocument(w, h, hasFill ? fill : nullptr);
            saveProject(doc, fs::u8path(args[2]));
            std::cout << "已新建 " << args[2] << "\n";
            return 0;
        }
        if (command == "import" && args.size() >= 4) {
            std::vector<uint8_t> bytes;
            if (!readFile(fs::u8path(args[2]), bytes)) { std::cerr << "无法读取 " << args[2] << "\n"; return 2; }
            std::string error;
            auto image = decodeImage(bytes, &error);
            if (!image) { std::cerr << "无法解码：" << error << "\n"; return 2; }
            if (image->width > kMaxSide || image->height > kMaxSide) { std::cerr << "图片太大\n"; return 2; }
            Document doc = documentFromImage(*image, fs::u8path(args[2]).stem().u8string());
            saveProject(doc, fs::u8path(args[3]));
            std::cout << "已新建 " << args[3] << "\n";
            return 0;
        }
    } catch (const std::exception& e) {
        std::cerr << "错误：" << e.what() << "\n";
        return 2;
    }
    usage();
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args;
#ifdef _WIN32
    // Windows 控制台用 UTF-8 输出，参数从宽字符读取，保证中文路径可用。
    SetConsoleOutputCP(CP_UTF8);
    int count = 0;
    LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &count);
    for (int i = 0; i < count; ++i) {
        int size = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
        std::string text(size_t(size > 0 ? size - 1 : 0), '\0');
        if (size > 1) WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, text.data(), size, nullptr, nullptr);
        args.push_back(text);
    }
    LocalFree(wide);
    (void)argc; (void)argv;
#else
    for (int i = 0; i < argc; ++i) args.emplace_back(argv[i]);
#endif
    return run(args);
}
