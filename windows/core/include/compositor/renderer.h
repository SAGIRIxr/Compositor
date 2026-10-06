// CPU 合成渲染器：与 macOS 版的导出路径语义一致（直通式文件夹、文件夹蒙版、剪贴蒙版堆栈、调整层）。
#pragma once
#include "compositor/document.h"
#include <atomic>
#include <memory>

namespace comp {

struct RenderOptions {
    int width = 0;            // 输出尺寸
    int height = 0;
    double scale = 1;         // 输出像素 = 文档像素 × scale + offset
    double offsetX = 0;
    double offsetY = 0;
    const std::atomic<bool>* cancel = nullptr;
    // 不绘制这些图层（编辑时预览用）。
    const std::string* skipLayerID = nullptr;
};

class Renderer {
public:
    Renderer();
    ~Renderer();
    // 渲染文档的一个区域。被取消时返回空图像。
    Image render(const Document& document, const RenderOptions& options);
    // 以 1:1 渲染整张画布（导出）。
    Image renderFull(const Document& document);
    // 清空缩小图缓存。
    void clearCache();

    // 只渲染一个图层自身（不含混合），用于缩略图。
    static Image renderLayerThumbnail(const Document& document, const Layer& layer, int size);

    struct Impl; // 实现细节（缩小图缓存）

private:
    std::unique_ptr<Impl> impl_;
};

// 图层显示出来的像素：原像素乘上启用的蒙版（含解除链接的蒙版），尺寸与图层像素相同。
Image shownPixels(const Layer& layer);

// 便捷函数：整张画布拍平。
Image flatten(const Document& document);

} // namespace comp
