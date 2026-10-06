// 文档模型：与 .comp 清单一一对应。未建模的字段（文字、图层效果、形状等）原样保存在 extra 里，保存时写回。
#pragma once
#include "compositor/blend.h"
#include "compositor/image.h"
#include "compositor/json.h"
#include "compositor/transform.h"
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace comp {

// 清单格式版本，与 macOS 版 ProjectManifest.current 保持一致。
constexpr int kFormatVersion = 11;
constexpr int kMaxSide = 30000;
constexpr long long kMaxSurfacePixels = 200000000LL;
constexpr long long kDocumentPixelBudget = 800000000LL;
constexpr int kMaxLayers = 10000;

struct Layer {
    std::string id;            // 大写 UUID
    std::string name;
    bool visible = true;
    LayerTransform transform;
    ImageRef image;            // 像素；文件夹、调整层与空白图层为空
    std::string parentID;      // 所在文件夹，空表示顶层
    bool isGroup = false;
    double opacity = 1;
    BlendMode blendMode = BlendMode::Normal;
    MaskRef mask;
    bool maskEnabled = true;
    std::optional<LayerTransform> maskPlacement; // 解除链接后蒙版自己的位置
    bool maskLinked = true;
    std::string maskSourceID;  // 剪贴蒙版的基底图层
    std::optional<Json> adjustment;
    Json extra = Json::object(); // shape、effects、text 以及未来新增的字段

    bool isAdjustment() const { return adjustment.has_value(); }
    bool hasText() const { return extra.contains("text"); }
    // 像素被破坏性修改后，文字与形状元数据不再对应，丢弃它们（与 macOS 版一致）。
    void dropVectorMetadata() { extra.erase("text"); extra.erase("shape"); }
};

struct Guide {
    std::string id;
    bool horizontal = true;
    double position = 0;
};

struct Document {
    std::string id;
    int width = 0;
    int height = 0;
    std::optional<double> resolution; // 每英寸像素，缺省视为 72
    std::string activeLayerID;
    std::vector<Layer> layers;        // 从下到上
    std::vector<Guide> guides;
    Json extra = Json::object();      // 清单顶层的未知字段

    double effectiveResolution() const { return resolution.value_or(72); }
    int indexOf(const std::string& layerID) const;
    const Layer* find(const std::string& layerID) const;
    Layer* find(const std::string& layerID);
    // 图层与其所有上级文件夹都可见。
    bool isEffectivelyVisible(const Layer& layer) const;
    // 自身不透明度乘以所有上级文件夹的不透明度。
    double effectiveOpacity(const Layer& layer) const;
    // 参与合成的图层（可见、非文件夹），按绘制顺序从下到上。
    std::vector<const Layer*> visibleLayers() const;
    // 按树的先序排好 layers：每个文件夹后面紧跟它的内容，兄弟之间保持原有的上下顺序。
    // macOS 版只靠 parentID 与兄弟间的相对顺序确定层级，所以这样重排不改变任何渲染结果。
    void normalizeOrder();
    // 文件夹深度（顶层为 0）。
    int depth(const Layer& layer) const;
    // id 是否为 ancestorID 的后代。
    bool isDescendant(const std::string& id, const std::string& ancestorID) const;
    // 某图层（文件夹连同内容）在 layers 中的下标区间 [first, last]；要求已 normalizeOrder。
    void subtreeRange(int index, int& first, int& last) const;
};

} // namespace comp
