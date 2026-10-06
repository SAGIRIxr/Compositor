// 文档编辑操作：新建、导入、图层增删改、画笔、蒙版、画布尺寸。界面与命令行都通过这里改文档。
#pragma once
#include "compositor/adjustment.h"
#include "compositor/document.h"
#include <string>
#include <vector>

namespace comp {

Document newDocument(int width, int height, const uint8_t* fill /* 直通 RGBA，可为空 = 透明 */);
// 用一张图新建文档，画布与图同大。
Document documentFromImage(const Image& image, const std::string& name);

// 在 activeLayerID 之上（同一文件夹）插入图层，返回新图层 id。
std::string addImageLayer(Document& doc, Image image, const std::string& name, bool fitCanvas = true);
std::string addBlankLayer(Document& doc, const std::string& name);
std::string addGroup(Document& doc, const std::string& name);
std::string addAdjustmentLayer(Document& doc, AdjustmentKind kind, uint32_t seed);
// 把选中图层放进一个新文件夹。
std::string groupLayer(Document& doc, const std::string& layerID);

// 删除图层（文件夹连同内容），并解除指向它的剪贴蒙版。
void deleteLayer(Document& doc, const std::string& layerID);
std::string duplicateLayer(Document& doc, const std::string& layerID);
// 在兄弟节点之间上移（+1）或下移（-1）一格，文件夹整体移动。
bool moveLayer(Document& doc, const std::string& layerID, int direction);
// 拖放：placement 为 +1 放到 targetID 之上，-1 放到之下，0 放进该文件夹的最上面。
enum class DropPlacement { Below = -1, Inside = 0, Above = 1 };
bool moveLayerTo(Document& doc, const std::string& layerID, const std::string& targetID, DropPlacement placement);
// 向下合并：把图层画进下方像素图层。
bool mergeDown(Document& doc, const std::string& layerID);
// 拼合图像：所有可见内容合成为一个图层。
void flattenDocument(Document& doc);
// 切换剪贴蒙版：剪贴到下方兄弟图层，或释放。
bool toggleClipping(Document& doc, const std::string& layerID);

// 蒙版
bool addMask(Document& doc, const std::string& layerID, bool revealAll = true);
bool deleteMask(Document& doc, const std::string& layerID);
bool applyMask(Document& doc, const std::string& layerID);
bool invertMask(Document& doc, const std::string& layerID);

// 画布
void resizeCanvas(Document& doc, int width, int height, double anchorX, double anchorY);
void resizeImage(Document& doc, int width, int height);
void cropCanvas(Document& doc, int x, int y, int width, int height);
void flipCanvas(Document& doc, bool horizontal);
void flipLayer(Document& doc, const std::string& layerID, bool horizontal);

// 画笔
struct BrushSettings {
    double size = 30;       // 直径，文档像素
    double hardness = 0.8;  // 0–1
    double opacity = 1;     // 0–1
    double spacing = 0.15;  // 相对直径
};
struct StrokePoint { double x, y; };

// 一笔进行中的笔触：开始时记住原像素，之后每加一个点只重算新线段覆盖到的区域。
// 文档有选区时，笔触只作用在选区内（按覆盖度）。
// 同一笔内的覆盖取最大值，所以笔触自身重叠处不会越描越深（与 Photoshop 的不透明度一致）。
class BrushStroke {
public:
    BrushStroke(Document& doc, const std::string& layerID, const BrushSettings& brush,
                const uint8_t color[4], bool erase, bool onMask);
    bool isValid() const { return valid_; }
    void addPoint(double x, double y);
    // 被修改过的文档区域（文档像素，外接框）；没有修改时宽度为 0。
    void dirtyRect(double& x, double& y, double& w, double& h) const;

private:
    void apply(double x0, double y0, double x1, double y1);
    std::string layerID_;
    BrushSettings brush_;
    uint8_t color_[4];
    bool erase_ = false, onMask_ = false, valid_ = false, hasPoint_ = false;
    double lastX_ = 0, lastY_ = 0;
    std::shared_ptr<Image> image_;
    std::shared_ptr<GrayImage> mask_;
    ImageRef originalImage_;
    MaskRef originalMask_;
    std::vector<uint8_t> coverage_;
    std::shared_ptr<GrayImage> selectionGrid_; // 有选区时只在选区内落墨
    int width_ = 0, height_ = 0;
    Affine pixelToDoc_, docToPixel_;
    double dirty_[4] = {1e300, 1e300, -1e300, -1e300};
};

// 在图层像素（或蒙版）上画一笔。color 是直通 RGBA（蒙版只用灰度 = color[0]）。
// erase 为真时擦除透明度。对空白图层会先建一张画布大小的透明位图。
// 返回 false 表示该图层不能画（文件夹、调整层）。
bool paintStroke(Document& doc, const std::string& layerID, const std::vector<StrokePoint>& points,
                 const BrushSettings& brush, const uint8_t color[4], bool erase, bool onMask);

// 用颜色填充图层（整个图层范围）。
bool fillLayer(Document& doc, const std::string& layerID, const uint8_t color[4]);

// 合成结果在文档坐标 (x, y) 的颜色（直通 RGBA），用于吸管。
bool sampleComposite(const Image& composite, int x, int y, uint8_t out[4]);

} // namespace comp
