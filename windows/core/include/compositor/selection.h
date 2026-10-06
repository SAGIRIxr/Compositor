// 选区：与画布同尺寸的 8 位覆盖图（255 全选，0 未选），保存在 Document::selection，
// 跟随撤销，但不写进 .comp（与 macOS 版一样只在本次编辑中有效）。
#pragma once
#include "compositor/document.h"
#include "compositor/edit.h"
#include <optional>
#include <vector>

namespace comp {

enum class SelectionMode { Replace, Add, Subtract, Intersect };

// 形状（文档坐标，边缘抗锯齿）。
GrayImage rectSelection(int width, int height, double x, double y, double w, double h, bool ellipse);
GrayImage polygonSelection(int width, int height, const std::vector<StrokePoint>& points);
// 把新形状按模式合进现有选区（existing 为空表示没有选区）。
GrayImage combineSelection(const GrayImage* existing, const GrayImage& shape, SelectionMode mode);

bool selectionIsEmpty(const GrayImage& selection);
// 覆盖度非零像素的外接框；空选区返回 false。
bool selectionBounds(const GrayImage& selection, int& x, int& y, int& w, int& h);
GrayImage invertSelection(const GrayImage* selection, int width, int height);
void featherSelection(GrayImage& selection, double radius);
// 正数扩展、负数收缩（像素）。
void growSelection(GrayImage& selection, int pixels);

// 一个图层单独画在画布上（文档尺寸，含蒙版与效果，不含不透明度与混合模式）。
Image renderLayerAlone(const Document& doc, const Layer& layer);
// 魔棒：在 source（文档尺寸）上选取与 (x, y) 相近的颜色。没有匹配时返回空。
std::optional<GrayImage> wandSelection(const Image& source, int x, int y, int tolerance, bool contiguous, int sampleRadius);
// 以图层的不透明度作为选区。
GrayImage selectionFromLayer(const Document& doc, const Layer& layer);

// 选区映射到图层自己的像素网格（w×h），图层变换任意。
GrayImage selectionInLayerGrid(const LayerTransform& transform, int w, int h, const GrayImage& selection);

// 依赖选区的编辑。返回 false 表示这个图层做不了（文件夹、调整层等）。
bool clearSelection(Document& doc, const std::string& layerID, const GrayImage& selection);
bool fillSelection(Document& doc, const std::string& layerID, const GrayImage& selection, const uint8_t color[4]);
// 用选区作为图层蒙版（替换现有蒙版）。
bool maskFromSelection(Document& doc, const std::string& layerID, const GrayImage& selection);
// 内容识别填充：1 成功，0 周围没有可用的像素，-1 内存不足，-2 图层不能处理。
int contentAwareFill(Document& doc, const std::string& layerID, const GrayImage& selection);
// 取 docImage（文档尺寸）在选区内的部分，裁到选区外接框，选区外透明。
Image copySelection(const Image& docImage, const GrayImage& selection, int& originX, int& originY);
// 裁剪画布到选区外接框，并清除选区。
bool cropToSelection(Document& doc);

// 蚂蚁线轮廓：覆盖度 ≥ 128 的像素的边界，若干闭合环，坐标为像素边。过于复杂时返回 false。
bool selectionOutline(const GrayImage& selection, std::vector<std::vector<std::pair<int, int>>>& loops);

} // namespace comp
