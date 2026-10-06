#include "compositor/document.h"
#include <algorithm>
#include <functional>

namespace comp {

int Document::indexOf(const std::string& layerID) const {
    for (size_t i = 0; i < layers.size(); ++i) if (layers[i].id == layerID) return int(i);
    return -1;
}

const Layer* Document::find(const std::string& layerID) const {
    int index = indexOf(layerID);
    return index < 0 ? nullptr : &layers[size_t(index)];
}

Layer* Document::find(const std::string& layerID) {
    int index = indexOf(layerID);
    return index < 0 ? nullptr : &layers[size_t(index)];
}

bool Document::isEffectivelyVisible(const Layer& layer) const {
    const Layer* current = &layer;
    for (int depth = 0; current && depth <= 64; ++depth) {
        if (!current->visible) return false;
        if (current->parentID.empty()) return true;
        current = find(current->parentID);
    }
    return current == nullptr;
}

double Document::effectiveOpacity(const Layer& layer) const {
    double opacity = layer.opacity;
    const Layer* parent = layer.parentID.empty() ? nullptr : find(layer.parentID);
    for (int depth = 0; parent && depth < 64; ++depth) {
        opacity *= parent->opacity;
        parent = parent->parentID.empty() ? nullptr : find(parent->parentID);
    }
    return opacity;
}

std::vector<const Layer*> Document::visibleLayers() const {
    // 与 macOS 版 LayerOrder.resolve 相同：从顶层开始，每个文件夹在自己的位置展开内容。
    std::unordered_map<std::string, std::vector<const Layer*>> children;
    for (const auto& layer : layers) children[layer.parentID].push_back(&layer);
    std::vector<const Layer*> result;
    std::function<void(const std::string&, int, bool)> visit = [&](const std::string& parent, int depth, bool shown) {
        if (depth > 64) return;
        auto it = children.find(parent);
        if (it == children.end()) return;
        for (const Layer* node : it->second) {
            bool effective = shown && node->visible;
            if (effective && !node->isGroup) result.push_back(node);
            if (node->isGroup) visit(node->id, depth + 1, effective);
        }
    };
    visit(std::string(), 0, true);
    return result;
}

void Document::normalizeOrder() {
    std::unordered_map<std::string, std::vector<size_t>> children;
    for (size_t i = 0; i < layers.size(); ++i) children[layers[i].parentID].push_back(i);
    std::vector<Layer> ordered;
    ordered.reserve(layers.size());
    std::vector<bool> placed(layers.size(), false);
    std::function<void(const std::string&, int)> visit = [&](const std::string& parent, int depth) {
        if (depth > 64) return;
        auto it = children.find(parent);
        if (it == children.end()) return;
        for (size_t index : it->second) {
            if (placed[index]) continue;
            placed[index] = true;
            ordered.push_back(layers[index]);
            if (layers[index].isGroup) visit(layers[index].id, depth + 1);
        }
    };
    visit(std::string(), 0);
    // 父节点缺失的（校验前的坏数据）放到最后，交给校验报错。
    for (size_t i = 0; i < layers.size(); ++i) if (!placed[i]) ordered.push_back(layers[i]);
    layers = std::move(ordered);
}

int Document::depth(const Layer& layer) const {
    int depth = 0;
    const Layer* parent = layer.parentID.empty() ? nullptr : find(layer.parentID);
    while (parent && depth < 64) {
        ++depth;
        parent = parent->parentID.empty() ? nullptr : find(parent->parentID);
    }
    return depth;
}

bool Document::isDescendant(const std::string& id, const std::string& ancestorID) const {
    const Layer* layer = find(id);
    for (int depth = 0; layer && !layer->parentID.empty() && depth < 64; ++depth) {
        if (layer->parentID == ancestorID) return true;
        layer = find(layer->parentID);
    }
    return false;
}

void Document::subtreeRange(int index, int& first, int& last) const {
    // 先序排列下，文件夹的内容紧跟在它后面。
    first = last = index;
    if (index < 0 || !layers[size_t(index)].isGroup) return;
    const std::string& id = layers[size_t(index)].id;
    int i = index + 1;
    while (i < int(layers.size()) && isDescendant(layers[size_t(i)].id, id)) ++i;
    last = i - 1;
}

} // namespace comp
