#include "compositor/edit.h"
#include "compositor/renderer.h"
#include "compositor/selection.h"

extern "C" {
#include "HealPixels.h"
}
#include "compositor/uuid.h"
#include "parallel.h"
#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace comp {

namespace {

// 先序排列下，把一个新图层插到 anchor 之上：anchor 是文件夹时放进它的最上面。
void insertAbove(Document& doc, Layer layer, const std::string& anchorID) {
    int anchor = doc.indexOf(anchorID);
    if (anchor < 0) {
        layer.parentID.clear();
        doc.layers.push_back(std::move(layer));
        return;
    }
    int first, last;
    doc.subtreeRange(anchor, first, last);
    layer.parentID = doc.layers[size_t(anchor)].isGroup ? doc.layers[size_t(anchor)].id : doc.layers[size_t(anchor)].parentID;
    doc.layers.insert(doc.layers.begin() + last + 1, std::move(layer));
}

Layer makeLayer(const Document& doc, const std::string& name) {
    Layer layer;
    layer.id = makeUUID();
    layer.name = name;
    layer.transform = LayerTransform::canvas(doc.width, doc.height);
    return layer;
}

std::string nextLayerName(const Document& doc) {
    for (int n = 1;; ++n) {
        std::string name = "图层 " + std::to_string(n);
        bool used = false;
        for (const auto& l : doc.layers) if (l.name == name) { used = true; break; }
        if (!used) return name;
    }
}

// 文件夹或调整层这类没有像素的图层，蒙版覆盖它的变换矩形。
void maskSize(const Layer& layer, int& w, int& h) {
    if (layer.image) { w = layer.image->width; h = layer.image->height; return; }
    w = std::clamp(int(std::lround(layer.transform.width)), 1, kMaxSide);
    h = std::clamp(int(std::lround(layer.transform.height)), 1, kMaxSide);
}

void shiftTransform(LayerTransform& t, double dx, double dy) { t.x += dx; t.y += dy; }

// 预乘图像双线性取样，越界为透明。
void sampleImage(const Image& image, double x, double y, float out[4]) {
    int x0 = int(std::floor(x)), y0 = int(std::floor(y));
    float fx = float(x - x0), fy = float(y - y0);
    auto at = [&](int px, int py, int k) -> float {
        if (px < 0 || py < 0 || px >= image.width || py >= image.height) return 0;
        return image.at(px, py)[k];
    };
    for (int k = 0; k < 4; ++k) {
        float top = at(x0, y0, k) * (1 - fx) + at(x0 + 1, y0, k) * fx;
        float bottom = at(x0, y0 + 1, k) * (1 - fx) + at(x0 + 1, y0 + 1, k) * fx;
        out[k] = top * (1 - fy) + bottom * fy;
    }
}

} // namespace

Document newDocument(int width, int height, const uint8_t* fill) {
    Document doc;
    doc.id = makeUUID();
    doc.width = std::clamp(width, 1, kMaxSide);
    doc.height = std::clamp(height, 1, kMaxSide);
    doc.resolution = 72;
    Layer layer = makeLayer(doc, fill ? "背景" : "图层 1");
    if (fill) {
        auto image = std::make_shared<Image>(doc.width, doc.height);
        image->fill(fill[0], fill[1], fill[2], fill[3]);
        layer.image = image;
    }
    doc.activeLayerID = layer.id;
    doc.layers.push_back(std::move(layer));
    return doc;
}

Document documentFromImage(const Image& image, const std::string& name) {
    Document doc;
    doc.id = makeUUID();
    doc.width = image.width;
    doc.height = image.height;
    doc.resolution = 72;
    Layer layer = makeLayer(doc, name.empty() ? "背景" : name);
    layer.image = std::make_shared<Image>(image);
    doc.activeLayerID = layer.id;
    doc.layers.push_back(std::move(layer));
    return doc;
}

std::string addImageLayer(Document& doc, Image image, const std::string& name, bool fitCanvas) {
    Layer layer = makeLayer(doc, name.empty() ? nextLayerName(doc) : name);
    double w = image.width, h = image.height;
    if (fitCanvas && (w > doc.width || h > doc.height)) {
        double factor = std::min(doc.width / w, doc.height / h);
        w = std::max(1.0, std::round(w * factor));
        h = std::max(1.0, std::round(h * factor));
    }
    layer.transform.width = w;
    layer.transform.height = h;
    layer.transform.x = std::round((doc.width - w) / 2);
    layer.transform.y = std::round((doc.height - h) / 2);
    layer.image = std::make_shared<Image>(std::move(image));
    std::string id = layer.id;
    insertAbove(doc, std::move(layer), doc.activeLayerID);
    doc.activeLayerID = id;
    return id;
}

std::string addBlankLayer(Document& doc, const std::string& name) {
    Layer layer = makeLayer(doc, name.empty() ? nextLayerName(doc) : name);
    std::string id = layer.id;
    insertAbove(doc, std::move(layer), doc.activeLayerID);
    doc.activeLayerID = id;
    return id;
}

std::string addGroup(Document& doc, const std::string& name) {
    Layer layer = makeLayer(doc, name.empty() ? "组" : name);
    layer.isGroup = true;
    std::string id = layer.id;
    insertAbove(doc, std::move(layer), doc.activeLayerID);
    doc.activeLayerID = id;
    return id;
}

std::string addAdjustmentLayer(Document& doc, AdjustmentKind kind, uint32_t seed) {
    Layer layer = makeLayer(doc, adjustmentKindLabel(kind));
    layer.adjustment = makeAdjustment(kind, seed);
    std::string id = layer.id;
    insertAbove(doc, std::move(layer), doc.activeLayerID);
    doc.activeLayerID = id;
    return id;
}

std::string groupLayer(Document& doc, const std::string& layerID) {
    int index = doc.indexOf(layerID);
    if (index < 0) return {};
    int first, last;
    doc.subtreeRange(index, first, last);
    Layer group = makeLayer(doc, "组");
    group.isGroup = true;
    group.parentID = doc.layers[size_t(index)].parentID;
    doc.layers[size_t(index)].parentID = group.id;
    std::string id = group.id;
    doc.layers.insert(doc.layers.begin() + first, std::move(group));
    doc.activeLayerID = id;
    return id;
}

void deleteLayer(Document& doc, const std::string& layerID) {
    int index = doc.indexOf(layerID);
    if (index < 0) return;
    int first, last;
    doc.subtreeRange(index, first, last);
    std::vector<std::string> removed;
    for (int i = first; i <= last; ++i) removed.push_back(doc.layers[size_t(i)].id);
    std::string parent = doc.layers[size_t(index)].parentID;
    doc.layers.erase(doc.layers.begin() + first, doc.layers.begin() + last + 1);
    for (auto& layer : doc.layers) {
        if (std::find(removed.begin(), removed.end(), layer.maskSourceID) != removed.end()) layer.maskSourceID.clear();
    }
    if (std::find(removed.begin(), removed.end(), doc.activeLayerID) != removed.end()) {
        // 选中下方的兄弟，没有就选上方的，再没有就选上级文件夹。
        doc.activeLayerID.clear();
        if (first - 1 >= 0) doc.activeLayerID = doc.layers[size_t(first - 1)].id;
        else if (first < int(doc.layers.size())) doc.activeLayerID = doc.layers[size_t(first)].id;
        else if (!parent.empty()) doc.activeLayerID = parent;
    }
}

std::string duplicateLayer(Document& doc, const std::string& layerID) {
    int index = doc.indexOf(layerID);
    if (index < 0) return {};
    int first, last;
    doc.subtreeRange(index, first, last);
    std::unordered_map<std::string, std::string> ids;
    for (int i = first; i <= last; ++i) ids[doc.layers[size_t(i)].id] = makeUUID();
    std::vector<Layer> copies;
    for (int i = first; i <= last; ++i) {
        Layer copy = doc.layers[size_t(i)];
        copy.id = ids[copy.id];
        if (ids.count(copy.parentID)) copy.parentID = ids[copy.parentID];
        if (ids.count(copy.maskSourceID)) copy.maskSourceID = ids[copy.maskSourceID];
        copies.push_back(std::move(copy));
    }
    copies.front().name += " 拷贝";
    std::string id = copies.front().id;
    doc.layers.insert(doc.layers.begin() + last + 1, copies.begin(), copies.end());
    doc.activeLayerID = id;
    return id;
}

bool moveLayer(Document& doc, const std::string& layerID, int direction) {
    int index = doc.indexOf(layerID);
    if (index < 0 || direction == 0) return false;
    int first, last;
    doc.subtreeRange(index, first, last);
    const std::string parent = doc.layers[size_t(index)].parentID;
    if (direction > 0) {
        int next = last + 1;
        if (next >= int(doc.layers.size()) || doc.layers[size_t(next)].parentID != parent) return false;
        return moveLayerTo(doc, layerID, doc.layers[size_t(next)].id, DropPlacement::Above);
    }
    for (int i = first - 1; i >= 0; --i) {
        if (doc.layers[size_t(i)].id == parent) return false;
        if (doc.layers[size_t(i)].parentID == parent) {
            return moveLayerTo(doc, layerID, doc.layers[size_t(i)].id, DropPlacement::Below);
        }
    }
    return false;
}

bool moveLayerTo(Document& doc, const std::string& layerID, const std::string& targetID, DropPlacement placement) {
    if (layerID == targetID) return false;
    int index = doc.indexOf(layerID), target = doc.indexOf(targetID);
    if (index < 0 || target < 0) return false;
    if (doc.isDescendant(targetID, layerID)) return false; // 不能放进自己里面
    if (placement == DropPlacement::Inside && !doc.layers[size_t(target)].isGroup) placement = DropPlacement::Above;
    if (doc.depth(doc.layers[size_t(target)]) + (placement == DropPlacement::Inside ? 1 : 0) > 63) return false;
    int first, last;
    doc.subtreeRange(index, first, last);
    std::vector<Layer> block(doc.layers.begin() + first, doc.layers.begin() + last + 1);
    doc.layers.erase(doc.layers.begin() + first, doc.layers.begin() + last + 1);
    target = doc.indexOf(targetID);
    int tFirst, tLast;
    doc.subtreeRange(target, tFirst, tLast);
    int insertAt;
    if (placement == DropPlacement::Inside) {
        block.front().parentID = targetID;
        insertAt = tLast + 1;
    } else if (placement == DropPlacement::Above) {
        block.front().parentID = doc.layers[size_t(target)].parentID;
        insertAt = tLast + 1;
    } else {
        block.front().parentID = doc.layers[size_t(target)].parentID;
        insertAt = tFirst;
    }
    doc.layers.insert(doc.layers.begin() + insertAt, block.begin(), block.end());
    // 移出剪贴组的图层不再剪贴（来源已不在正下方时由渲染器按覆盖度裁剪，这里保持 Photoshop 的习惯：释放）。
    Layer* moved = doc.find(layerID);
    if (moved && !moved->maskSourceID.empty()) {
        const Layer* source = doc.find(moved->maskSourceID);
        if (!source || source->parentID != moved->parentID) moved->maskSourceID.clear();
    }
    return true;
}

bool mergeDown(Document& doc, const std::string& layerID) {
    int index = doc.indexOf(layerID);
    if (index < 0) return false;
    Layer& top = doc.layers[size_t(index)];
    if (top.isGroup || top.adjustment) return false;
    // 下方的兄弟像素图层。
    int below = -1;
    for (int i = index - 1; i >= 0; --i) {
        if (doc.layers[size_t(i)].id == top.parentID) break;
        if (doc.layers[size_t(i)].parentID == top.parentID) { below = i; break; }
    }
    if (below < 0) return false;
    Layer& bottom = doc.layers[size_t(below)];
    if (bottom.isGroup || bottom.adjustment) return false;
    Document temp;
    temp.id = doc.id;
    temp.width = doc.width;
    temp.height = doc.height;
    Layer b = bottom, t = top;
    b.parentID.clear(); t.parentID.clear();
    b.visible = t.visible = true;
    b.opacity = 1; b.blendMode = BlendMode::Normal; b.maskSourceID.clear();
    if (t.maskSourceID != bottom.id) t.maskSourceID.clear();
    temp.layers = {b, t};
    Image merged = flatten(temp);
    bottom.image = std::make_shared<Image>(std::move(merged));
    bottom.transform = LayerTransform::canvas(doc.width, doc.height);
    bottom.mask.reset();
    bottom.maskPlacement.reset();
    bottom.maskLinked = true;
    bottom.maskEnabled = true;
    bottom.dropVectorMetadata();
    bottom.extra.erase("effects");
    std::string bottomID = bottom.id;
    deleteLayer(doc, layerID);
    doc.activeLayerID = bottomID;
    return true;
}

void flattenDocument(Document& doc) {
    Image merged = flatten(doc);
    Layer layer = makeLayer(doc, "背景");
    layer.image = std::make_shared<Image>(std::move(merged));
    doc.layers.clear();
    doc.activeLayerID = layer.id;
    doc.layers.push_back(std::move(layer));
}

bool toggleClipping(Document& doc, const std::string& layerID) {
    Layer* layer = doc.find(layerID);
    if (!layer || layer->isGroup) return false;
    if (!layer->maskSourceID.empty()) { layer->maskSourceID.clear(); return true; }
    int index = doc.indexOf(layerID);
    for (int i = index - 1; i >= 0; --i) {
        const Layer& candidate = doc.layers[size_t(i)];
        if (candidate.id == layer->parentID) return false;
        if (candidate.parentID != layer->parentID) continue;
        // 下方兄弟本身已剪贴：加入同一个剪贴组。
        std::string base = candidate.maskSourceID.empty() ? candidate.id : candidate.maskSourceID;
        const Layer* source = doc.find(base);
        if (!source || source->isGroup || source->adjustment) return false;
        layer->maskSourceID = base;
        return true;
    }
    return false;
}

bool addMask(Document& doc, const std::string& layerID, bool revealAll) {
    Layer* layer = doc.find(layerID);
    if (!layer || layer->mask) return false;
    // 均匀蒙版存成 1×1，开始绘制时再展开（与 macOS 版相同）。
    layer->mask = std::make_shared<GrayImage>(1, 1, revealAll ? 255 : 0);
    layer->maskEnabled = true;
    layer->maskLinked = true;
    layer->maskPlacement.reset();
    return true;
}

bool deleteMask(Document& doc, const std::string& layerID) {
    Layer* layer = doc.find(layerID);
    if (!layer || !layer->mask) return false;
    layer->mask.reset();
    layer->maskPlacement.reset();
    layer->maskLinked = true;
    layer->maskEnabled = true;
    return true;
}

bool applyMask(Document& doc, const std::string& layerID) {
    Layer* layer = doc.find(layerID);
    if (!layer || !layer->mask || !layer->image) return false;
    if (layer->maskPlacement && !layer->maskLinked && !layer->maskPlacement->samePlacement(layer->transform)) return false;
    auto image = std::make_shared<Image>(*layer->image);
    const GrayImage& mask = *layer->mask;
    for (int y = 0; y < image->height; ++y) {
        uint8_t* row = image->row(y);
        int my = std::min(mask.height - 1, int((y + 0.5) * mask.height / image->height));
        for (int x = 0; x < image->width; ++x) {
            int mx = std::min(mask.width - 1, int((x + 0.5) * mask.width / image->width));
            unsigned m = mask.row(my)[mx];
            for (int c = 0; c < 4; ++c) row[x * 4 + c] = uint8_t((row[x * 4 + c] * m + 127) / 255);
        }
    }
    layer->image = image;
    layer->dropVectorMetadata();
    return deleteMask(doc, layerID);
}

bool invertMask(Document& doc, const std::string& layerID) {
    Layer* layer = doc.find(layerID);
    if (!layer || !layer->mask) return false;
    auto mask = std::make_shared<GrayImage>(*layer->mask);
    for (auto& v : mask->pixels) v = uint8_t(255 - v);
    layer->mask = mask;
    return true;
}

void resizeCanvas(Document& doc, int width, int height, double anchorX, double anchorY) {
    doc.selection.reset(); // 画布变了，选区作废
    width = std::clamp(width, 1, kMaxSide);
    height = std::clamp(height, 1, kMaxSide);
    double dx = std::round((width - doc.width) * anchorX), dy = std::round((height - doc.height) * anchorY);
    for (auto& layer : doc.layers) {
        shiftTransform(layer.transform, dx, dy);
        if (layer.maskPlacement) shiftTransform(*layer.maskPlacement, dx, dy);
    }
    for (auto& guide : doc.guides) guide.position += guide.horizontal ? dy : dx;
    doc.width = width;
    doc.height = height;
}

void resizeImage(Document& doc, int width, int height) {
    doc.selection.reset(); // 画布变了，选区作废
    width = std::clamp(width, 1, kMaxSide);
    height = std::clamp(height, 1, kMaxSide);
    double sx = double(width) / doc.width, sy = double(height) / doc.height;
    auto scale = [&](LayerTransform& t) {
        t.x *= sx; t.y *= sy;
        t.width = std::max(1.0, t.width * sx);
        t.height = std::max(1.0, t.height * sy);
    };
    // 非破坏：图层保留原像素，只缩放变换。
    for (auto& layer : doc.layers) {
        scale(layer.transform);
        if (layer.maskPlacement) scale(*layer.maskPlacement);
    }
    for (auto& guide : doc.guides) guide.position *= guide.horizontal ? sy : sx;
    doc.width = width;
    doc.height = height;
}

void cropCanvas(Document& doc, int x, int y, int width, int height) {
    doc.selection.reset(); // 画布变了，选区作废
    width = std::clamp(width, 1, kMaxSide);
    height = std::clamp(height, 1, kMaxSide);
    for (auto& layer : doc.layers) {
        shiftTransform(layer.transform, -x, -y);
        if (layer.maskPlacement) shiftTransform(*layer.maskPlacement, -x, -y);
    }
    for (auto& guide : doc.guides) guide.position -= guide.horizontal ? y : x;
    doc.width = width;
    doc.height = height;
}

void flipCanvas(Document& doc, bool horizontal) {
    doc.selection.reset(); // 画布变了，选区作废
    auto flip = [&](LayerTransform& t) {
        if (horizontal) { t.x = doc.width - (t.x + t.width); t.flipX = !t.flipX; }
        else { t.y = doc.height - (t.y + t.height); t.flipY = !t.flipY; }
        t.rotation = -t.rotation;
    };
    for (auto& layer : doc.layers) {
        flip(layer.transform);
        if (layer.maskPlacement) flip(*layer.maskPlacement);
    }
    for (auto& guide : doc.guides) {
        if (horizontal && !guide.horizontal) guide.position = doc.width - guide.position;
        if (!horizontal && guide.horizontal) guide.position = doc.height - guide.position;
    }
}

void flipLayer(Document& doc, const std::string& layerID, bool horizontal) {
    Layer* layer = doc.find(layerID);
    if (!layer) return;
    if (horizontal) layer->transform.flipX = !layer->transform.flipX;
    else layer->transform.flipY = !layer->transform.flipY;
}

BrushStroke::BrushStroke(Document& doc, const std::string& layerID, const BrushSettings& brush,
                         const uint8_t color[4], bool erase, bool onMask)
    : layerID_(layerID), brush_(brush), erase_(erase), onMask_(onMask) {
    std::copy(color, color + 4, color_);
    options_.kind = erase ? StrokeKind::Erase : StrokeKind::Paint;
    init(doc, layerID, onMask);
}

BrushStroke::BrushStroke(Document& doc, const std::string& layerID, const BrushSettings& brush,
                         const uint8_t color[4], const StrokeOptions& options, bool onMask)
    : layerID_(layerID), brush_(brush), erase_(options.kind == StrokeKind::Erase), onMask_(onMask) {
    options_ = options;
    std::copy(color, color + 4, color_);
    if (onMask && (options.kind == StrokeKind::Clone || options.kind == StrokeKind::Heal)) return;
    init(doc, layerID, onMask);
}

void BrushStroke::init(Document& doc, const std::string& layerID, bool onMask) {

    Layer* layer = doc.find(layerID);
    if (!layer) return;
    if (onMask) {
        if (!layer->mask) return;
        originalMask_ = layer->mask;
        int w, h;
        maskSize(*layer, w, h);
        int uniform = layer->mask->uniformValue();
        if (uniform >= 0 && (layer->mask->width != w || layer->mask->height != h)) {
            mask_ = std::make_shared<GrayImage>(w, h, uint8_t(uniform)); // 展开均匀蒙版
            originalMask_ = std::make_shared<GrayImage>(*mask_);
        } else {
            mask_ = std::make_shared<GrayImage>(*layer->mask);
        }
        width_ = mask_->width; height_ = mask_->height;
        layer->mask = mask_;
    } else {
        if (layer->isGroup || layer->adjustment) return;
        if (!layer->image) {
            int w = std::clamp(int(std::lround(layer->transform.width)), 1, kMaxSide);
            int h = std::clamp(int(std::lround(layer->transform.height)), 1, kMaxSide);
            originalImage_ = std::make_shared<Image>(w, h);
        } else {
            originalImage_ = layer->image;
        }
        image_ = std::make_shared<Image>(*originalImage_);
        width_ = image_->width; height_ = image_->height;
        layer->image = image_;
        layer->dropVectorMetadata();
    }
    // 蒙版解除链接时用它自己的位置。
    const LayerTransform& placement = (onMask && layer->maskPlacement && !layer->maskLinked) ? *layer->maskPlacement : layer->transform;
    pixelToDoc_ = Affine::scale(1.0 / width_, 1.0 / height_).then(placement.unitToDocument());
    docToPixel_ = pixelToDoc_.inverted();
    coverage_.assign(size_t(width_) * size_t(height_), 0);
    if (doc.selection) selectionGrid_ = std::make_shared<GrayImage>(selectionInLayerGrid(placement, width_, height_, *doc.selection));
    valid_ = true;
}

void BrushStroke::addPoint(double x, double y) {
    if (!valid_) return;
    if (!hasPoint_) { hasPoint_ = true; lastX_ = x; lastY_ = y; apply(x, y, x, y); return; }
    apply(lastX_, lastY_, x, y);
    lastX_ = x; lastY_ = y;
}

void BrushStroke::dirtyRect(double& x, double& y, double& w, double& h) const {
    if (dirty_[2] < dirty_[0]) { x = y = w = h = 0; return; }
    x = dirty_[0]; y = dirty_[1]; w = dirty_[2] - dirty_[0]; h = dirty_[3] - dirty_[1];
}

void BrushStroke::apply(double ax, double ay, double bx, double by) {
    double radius = std::max(0.5, brush_.size / 2);
    double hard = std::clamp(brush_.hardness, 0.0, 1.0) * radius;
    // 线段外扩半径后的文档外接框 → 像素外接框。
    double minX = std::min(ax, bx) - radius - 1, maxX = std::max(ax, bx) + radius + 1;
    double minY = std::min(ay, by) - radius - 1, maxY = std::max(ay, by) + radius + 1;
    dirty_[0] = std::min(dirty_[0], minX); dirty_[1] = std::min(dirty_[1], minY);
    dirty_[2] = std::max(dirty_[2], maxX); dirty_[3] = std::max(dirty_[3], maxY);
    double pminX = 1e300, pminY = 1e300, pmaxX = -1e300, pmaxY = -1e300;
    for (double cx : {minX, maxX}) for (double cy : {minY, maxY}) {
        double px, py; docToPixel_.apply(cx, cy, px, py);
        pminX = std::min(pminX, px); pmaxX = std::max(pmaxX, px);
        pminY = std::min(pminY, py); pmaxY = std::max(pmaxY, py);
    }
    int x0 = std::max(0, int(std::floor(pminX))), x1 = std::min(width_, int(std::ceil(pmaxX)) + 1);
    int y0 = std::max(0, int(std::floor(pminY))), y1 = std::min(height_, int(std::ceil(pmaxY)) + 1);
    if (x0 >= x1 || y0 >= y1) return;
    double sx = bx - ax, sy = by - ay, length2 = sx * sx + sy * sy;
    float opacity = float(std::clamp(brush_.opacity, 0.0, 1.0));
    const Image* original = originalImage_.get();
    const GrayImage* originalMask = originalMask_.get();
    parallelRanges(y1 - y0, [&](int begin, int end) {
        for (int py = y0 + begin; py < y0 + end; ++py) {
            for (int px = x0; px < x1; ++px) {
                double dx, dy;
                pixelToDoc_.apply(px + 0.5, py + 0.5, dx, dy);
                double t = length2 > 0 ? std::clamp(((dx - ax) * sx + (dy - ay) * sy) / length2, 0.0, 1.0) : 0;
                double ex = dx - (ax + sx * t), ey = dy - (ay + sy * t);
                double d = std::sqrt(ex * ex + ey * ey);
                double c;
                if (d <= hard) c = 1;
                else if (d >= radius) c = 0;
                else { double u = (radius - d) / std::max(1e-6, radius - hard); c = u * u * (3 - 2 * u); }
                // 硬边笔刷在边缘留半个像素的抗锯齿。
                if (brush_.hardness >= 0.999) c = std::clamp(radius - d + 0.5, 0.0, 1.0);
                size_t index = size_t(py) * size_t(width_) + size_t(px);
                if (selectionGrid_) c *= selectionGrid_->pixels[index] / 255.0;
                uint8_t value = uint8_t(std::lround(c * 255));
                if (value <= coverage_[index]) continue;
                coverage_[index] = value;
                float a = value / 255.0f * opacity;
                if (options_.kind == StrokeKind::Heal) {
                    // 拖动时只显示暗色预览，松手后再修复。
                    const uint8_t* o = original->at(px, py);
                    uint8_t* out = image_->at(px, py);
                    float wash = a * 0.45f;
                    for (int k = 0; k < 3; ++k) out[k] = uint8_t(std::lround(o[k] * (1 - wash)));
                    out[3] = o[3];
                } else if (options_.kind == StrokeKind::Clone) {
                    // 从偏移处取像素（预乘、双线性），正常混合叠上去。
                    double sx = dx + options_.cloneOffsetX, sy = dy + options_.cloneOffsetY;
                    float src[4];
                    if (options_.cloneSource) {
                        sampleImage(*options_.cloneSource, sx - 0.5, sy - 0.5, src);
                    } else {
                        double lx, ly;
                        docToPixel_.apply(sx, sy, lx, ly);
                        sampleImage(*original, lx - 0.5, ly - 0.5, src);
                    }
                    const uint8_t* o = original->at(px, py);
                    uint8_t* out = image_->at(px, py);
                    float inv = 1 - src[3] / 255.0f * a;
                    for (int k = 0; k < 4; ++k) out[k] = uint8_t(std::lround(std::clamp(src[k] * a + o[k] * inv, 0.0f, 255.0f)));
                } else if (onMask_) {
                    float target = erase_ ? 0.0f : color_[0];
                    uint8_t o = originalMask->row(py)[px];
                    mask_->row(py)[px] = uint8_t(std::lround(o + (target - o) * a));
                } else {
                    const uint8_t* o = original->at(px, py);
                    uint8_t* out = image_->at(px, py);
                    if (erase_) {
                        for (int k = 0; k < 4; ++k) out[k] = uint8_t(std::lround(o[k] * (1 - a)));
                    } else {
                        float sa = a * color_[3] / 255.0f;
                        for (int k = 0; k < 3; ++k) out[k] = uint8_t(std::lround(color_[k] * sa + o[k] * (1 - sa)));
                        out[3] = uint8_t(std::lround(255 * sa + o[3] * (1 - sa)));
                    }
                }
            }
        }
    }, 4);
    if (image_) image_->touch();
    if (mask_) mask_->touch();
}

bool BrushStroke::finish() {
    if (!valid_ || options_.kind != StrokeKind::Heal || !image_) return true;
    // 涂过的范围，再向外留出修复内核搜索补丁的空间（约三个斑点宽）。
    long edges[4] = {0, 0, 0, 0};
    heal_coverage_bounds(coverage_.data(), size_t(width_), size_t(height_), size_t(width_), edges);
    if (edges[2] <= edges[0] || edges[3] <= edges[1]) { std::copy(originalImage_->pixels.begin(), originalImage_->pixels.end(), image_->pixels.begin()); image_->touch(); return true; }
    double reach = (std::max(edges[2] - edges[0], edges[3] - edges[1]) + 32) * 3.2;
    int x0 = std::max(0, int(edges[0] - reach)), y0 = std::max(0, int(edges[1] - reach));
    int x1 = std::min(width_, int(edges[2] + reach)), y1 = std::min(height_, int(edges[3] + reach));
    int w = x1 - x0, h = y1 - y0;
    std::vector<uint8_t> rgba(size_t(w) * size_t(h) * 4), gray(size_t(w) * size_t(h));
    for (int y = 0; y < h; ++y) {
        std::copy(originalImage_->row(y0 + y) + size_t(x0) * 4, originalImage_->row(y0 + y) + size_t(x1) * 4, rgba.begin() + std::ptrdiff_t(size_t(y) * size_t(w) * 4));
        std::copy(coverage_.begin() + std::ptrdiff_t(size_t(y0 + y) * size_t(width_) + size_t(x0)),
                  coverage_.begin() + std::ptrdiff_t(size_t(y0 + y) * size_t(width_) + size_t(x1)), gray.begin() + std::ptrdiff_t(size_t(y) * size_t(w)));
    }
    // 先把整张恢复成原像素（去掉预览），再写回修复好的区域。
    std::copy(originalImage_->pixels.begin(), originalImage_->pixels.end(), image_->pixels.begin());
    int status = spot_heal(rgba.data(), gray.data(), size_t(w), size_t(h), size_t(w) * 4, float(std::clamp(brush_.opacity, 0.0, 1.0)),
                           std::clamp(options_.healMode, 0, 2), options_.seed);
    if (status != 0) { image_->touch(); return false; }
    for (int y = 0; y < h; ++y) {
        std::copy(rgba.begin() + std::ptrdiff_t(size_t(y) * size_t(w) * 4), rgba.begin() + std::ptrdiff_t(size_t(y + 1) * size_t(w) * 4),
                  image_->row(y0 + y) + size_t(x0) * 4);
    }
    image_->touch();
    return true;
}

bool paintStroke(Document& doc, const std::string& layerID, const std::vector<StrokePoint>& points,
                 const BrushSettings& brush, const uint8_t color[4], bool erase, bool onMask) {
    BrushStroke stroke(doc, layerID, brush, color, erase, onMask);
    if (!stroke.isValid()) return false;
    for (const auto& p : points) stroke.addPoint(p.x, p.y);
    return stroke.finish();
}

bool drawGradient(Document& doc, const std::string& layerID, double ax, double ay, double bx, double by,
                  const uint8_t from[4], const uint8_t to[4], const GradientSettings& settings, bool onMask,
                  ImageRef originalImage, MaskRef originalMask) {
    Layer* layer = doc.find(layerID);
    if (!layer) return false;
    double vx = bx - ax, vy = by - ay, length2 = vx * vx + vy * vy;
    if (length2 < 0.25) return false;
    double length = std::sqrt(length2);
    float opacity = float(std::clamp(settings.opacity, 0.0, 1.0));
    auto t = [&](double dx, double dy) {
        double v = settings.radial ? std::hypot(dx - ax, dy - ay) / length : ((dx - ax) * vx + (dy - ay) * vy) / length2;
        return float(std::clamp(v, 0.0, 1.0));
    };
    if (onMask) {
        if (!layer->mask) return false;
        int w, h;
        maskSize(*layer, w, h);
        MaskRef base = originalMask ? originalMask : layer->mask;
        GrayImage start = base->uniformValue() >= 0 && (base->width != w || base->height != h) ? GrayImage(w, h, uint8_t(base->uniformValue())) : *base;
        w = start.width; h = start.height;
        const LayerTransform& placement = (layer->maskPlacement && !layer->maskLinked) ? *layer->maskPlacement : layer->transform;
        Affine toDoc = Affine::scale(1.0 / w, 1.0 / h).then(placement.unitToDocument());
        std::shared_ptr<GrayImage> selection;
        if (doc.selection) selection = std::make_shared<GrayImage>(selectionInLayerGrid(placement, w, h, *doc.selection));
        auto out = std::make_shared<GrayImage>(start);
        parallelRanges(h, [&](int begin, int end) {
            for (int y = begin; y < end; ++y) for (int x = 0; x < w; ++x) {
                double dx, dy;
                toDoc.apply(x + 0.5, y + 0.5, dx, dy);
                float k = t(dx, dy);
                float value = from[0] + (to[0] - from[0]) * k;
                float alpha = (from[3] + (to[3] - from[3]) * k) / 255.0f * opacity;
                if (selection) alpha *= selection->row(y)[x] / 255.0f;
                uint8_t& o = out->row(y)[x];
                o = uint8_t(std::lround(o + (value - o) * alpha));
            }
        });
        out->touch();
        layer->mask = out;
        return true;
    }
    if (layer->isGroup || layer->adjustment) return false;
    ImageRef base = originalImage ? originalImage : layer->image;
    auto out = base ? std::make_shared<Image>(*base) : std::make_shared<Image>(
        std::clamp(int(std::lround(layer->transform.width)), 1, kMaxSide), std::clamp(int(std::lround(layer->transform.height)), 1, kMaxSide));
    int w = out->width, h = out->height;
    Affine toDoc = Affine::scale(1.0 / w, 1.0 / h).then(layer->transform.unitToDocument());
    std::shared_ptr<GrayImage> selection;
    if (doc.selection) selection = std::make_shared<GrayImage>(selectionInLayerGrid(layer->transform, w, h, *doc.selection));
    parallelRanges(h, [&](int begin, int end) {
        for (int y = begin; y < end; ++y) for (int x = 0; x < w; ++x) {
            double dx, dy;
            toDoc.apply(x + 0.5, y + 0.5, dx, dy);
            float k = t(dx, dy);
            float a = (from[3] + (to[3] - from[3]) * k) / 255.0f * opacity;
            if (selection) a *= selection->row(y)[x] / 255.0f;
            if (a <= 0) continue;
            uint8_t* p = out->at(x, y);
            for (int c = 0; c < 3; ++c) {
                float color = from[c] + (to[c] - from[c]) * k;
                p[c] = uint8_t(std::lround(std::clamp(color * a + p[c] * (1 - a), 0.0f, 255.0f)));
            }
            p[3] = uint8_t(std::lround(std::clamp(255 * a + p[3] * (1 - a), 0.0f, 255.0f)));
        }
    });
    out->touch();
    layer->image = out;
    layer->dropVectorMetadata();
    return true;
}

bool fillLayer(Document& doc, const std::string& layerID, const uint8_t color[4]) {
    Layer* layer = doc.find(layerID);
    if (!layer || layer->isGroup || layer->adjustment) return false;
    int w = layer->image ? layer->image->width : std::clamp(int(std::lround(layer->transform.width)), 1, kMaxSide);
    int h = layer->image ? layer->image->height : std::clamp(int(std::lround(layer->transform.height)), 1, kMaxSide);
    auto image = std::make_shared<Image>(w, h);
    image->fill(color[0], color[1], color[2], color[3]);
    layer->image = image;
    layer->dropVectorMetadata();
    return true;
}

bool sampleComposite(const Image& composite, int x, int y, uint8_t out[4]) {
    if (x < 0 || y < 0 || x >= composite.width || y >= composite.height) return false;
    const uint8_t* p = composite.at(x, y);
    unsigned a = p[3];
    out[3] = uint8_t(a);
    for (int c = 0; c < 3; ++c) out[c] = a ? uint8_t(std::min(255u, (p[c] * 255u + a / 2) / a)) : 0;
    return true;
}

} // namespace comp
