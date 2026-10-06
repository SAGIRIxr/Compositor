#include "compositor/renderer.h"
#include "compositor/adjustment.h"
#include "compositor/effects.h"
#include "compositor/image_io.h"
#include "parallel.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <list>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>

extern "C" {
#include "BrushPixels.h"
}

namespace comp {

namespace {

inline uint8_t toByte(float v) { return uint8_t(v <= 0 ? 0 : (v >= 1 ? 255 : int(v * 255 + 0.5f))); }

// 灰度蒙版双线性采样，坐标越界时取边缘值。
inline float sampleGrayClamp(const GrayImage& g, double x, double y) {
    x = std::clamp(x, 0.0, double(g.width - 1));
    y = std::clamp(y, 0.0, double(g.height - 1));
    int x0 = int(x), y0 = int(y), x1 = std::min(g.width - 1, x0 + 1), y1 = std::min(g.height - 1, y0 + 1);
    float fx = float(x - x0), fy = float(y - y0);
    const uint8_t* r0 = g.row(y0);
    const uint8_t* r1 = g.row(y1);
    float top = r0[x0] + (r0[x1] - r0[x0]) * fx, bottom = r1[x0] + (r1[x1] - r1[x0]) * fx;
    return (top + (bottom - top) * fy) / 255.0f;
}

// 一个蒙版放在某个变换上：输出像素 → 蒙版像素的映射。outsideZero 为真时矩形外为 0（文件夹蒙版）。
struct PlacedMask {
    const GrayImage* mask = nullptr;
    Affine outToMask;
    bool outsideZero = false;
    int uniform = -1;
    double gradientX = 1, gradientY = 1; // 每个输出像素对应的蒙版像素数

    float at(double ox, double oy) const {
        if (uniform >= 0 && !outsideZero) return uniform / 255.0f;
        double mx, my;
        outToMask.apply(ox, oy, mx, my);
        if (outsideZero) {
            // 矩形边缘按输出像素做一个像素宽的抗锯齿（与蒙版分辨率无关，1×1 的均匀蒙版也一样）。
            double ex = (std::min(mx + 0.5, mask->width - 0.5 - mx)) / gradientX + 0.5;
            double ey = (std::min(my + 0.5, mask->height - 0.5 - my)) / gradientY + 0.5;
            float edge = float(std::clamp(std::min(ex, ey), 0.0, 1.0));
            if (edge <= 0) return 0;
            float v = uniform >= 0 ? uniform / 255.0f : sampleGrayClamp(*mask, mx, my);
            return v * edge;
        }
        return sampleGrayClamp(*mask, mx, my);
    }
};

Affine outputToDocument(const RenderOptions& o) {
    return Affine{1 / o.scale, 0, 0, 1 / o.scale, -o.offsetX / o.scale, -o.offsetY / o.scale};
}

// 输出像素中心坐标 → 某变换下 w×h 网格的像素坐标（像素中心在 .5）。
Affine outputToGrid(const RenderOptions& o, const LayerTransform& t, double w, double h) {
    Affine unitToDoc = t.unitToDocument();
    return outputToDocument(o).then(unitToDoc.inverted()).then(Affine{w, 0, 0, h, -0.5, -0.5});
}

PlacedMask placeMask(const RenderOptions& o, const GrayImage* mask, const LayerTransform& t, bool outsideZero) {
    PlacedMask p;
    p.mask = mask;
    p.outsideZero = outsideZero;
    p.uniform = mask->uniformValue();
    // 像素中心对齐：输出像素 (X, Y) 的中心是 (X + 0.5, Y + 0.5)。
    p.outToMask = Affine::translate(0.5, 0.5).then(outputToGrid(o, t, mask->width, mask->height));
    p.gradientX = std::max(1e-9, std::hypot(p.outToMask.a, p.outToMask.c));
    p.gradientY = std::max(1e-9, std::hypot(p.outToMask.b, p.outToMask.d));
    return p;
}

} // namespace

// 图层显示出来的像素：原像素乘上蒙版（链接的蒙版拉伸覆盖图层，解除链接的按它自己的位置取样）。
Image shownPixels(const Layer& l) {
    Image out = *l.image;
    if (!l.mask || !l.maskEnabled) return out;
    const GrayImage& mask = *l.mask;
    int uniform = mask.uniformValue();
    bool placed = l.maskPlacement && !l.maskLinked && !l.maskPlacement->samePlacement(l.transform);
    Affine toMask;
    if (placed) {
        // 图层像素 → 单位方块 → 文档 → 蒙版的单位方块 → 蒙版像素。
        toMask = Affine::scale(1.0 / out.width, 1.0 / out.height).then(l.transform.unitToDocument())
            .then(l.maskPlacement->unitToDocument().inverted()).then(Affine{double(mask.width), 0, 0, double(mask.height), -0.5, -0.5});
    } else {
        toMask = Affine::scale(double(mask.width) / out.width, double(mask.height) / out.height).then(Affine::translate(-0.5, -0.5));
    }
    parallelRanges(out.height, [&](int begin, int end) {
        for (int y = begin; y < end; ++y) {
            uint8_t* row = out.row(y);
            for (int x = 0; x < out.width; ++x) {
                float m;
                if (uniform >= 0) m = uniform / 255.0f;
                else { double mx, my; toMask.apply(x + 0.5, y + 0.5, mx, my); m = sampleGrayClamp(mask, mx, my); }
                for (int k = 0; k < 4; ++k) row[x * 4 + k] = uint8_t(std::lround(row[x * 4 + k] * m));
            }
        }
    });
    return out;
}

struct Renderer::Impl {
    std::mutex mutex;
    struct Entry { std::vector<std::shared_ptr<Image>> levels; uint64_t lastUse = 0; };
    std::unordered_map<uint64_t, Entry> mips;
    uint64_t generation = 0;

    // 第 level 级缩小图（0 为原图）。
    const Image& level(const ImageRef& image, int level) {
        if (level <= 0) return *image;
        std::lock_guard<std::mutex> lock(mutex);
        Entry& entry = mips[image->serial];
        entry.lastUse = generation;
        while (int(entry.levels.size()) < level) {
            const Image& from = entry.levels.empty() ? *image : *entry.levels.back();
            if (from.width <= 1 && from.height <= 1) break;
            entry.levels.push_back(std::make_shared<Image>(halve(from)));
        }
        if (entry.levels.empty()) return *image;
        return *entry.levels[size_t(std::min<int>(level, int(entry.levels.size())) - 1)];
    }

    // 带效果的图层图：按像素、蒙版与效果参数缓存，画布每次重绘不必重算。
    struct FxEntry { uint64_t image = 0, mask = 0; std::string key; EffectsImage fx; uint64_t lastUse = 0; };
    std::vector<FxEntry> fx;

    std::optional<EffectsImage> effects(const Layer& l) {
        auto found = l.extra.find("effects");
        if (found == l.extra.end() || !l.image) return std::nullopt;
        LayerEffectsParams params;
        try { params = parseEffects(*found); } catch (...) { return std::nullopt; }
        if (!params.anyVisible() || !params.isValid()) return std::nullopt;
        bool masked = l.mask && l.maskEnabled;
        std::string key = found->dump();
        if (masked && l.maskPlacement && !l.maskLinked) key += Json{l.maskPlacement->x, l.maskPlacement->y, l.maskPlacement->width,
            l.maskPlacement->height, l.maskPlacement->rotation, l.maskPlacement->flipX, l.maskPlacement->flipY,
            l.transform.x, l.transform.y, l.transform.width, l.transform.height, l.transform.rotation}.dump();
        uint64_t maskSerial = masked ? l.mask->serial : 0;
        {
            std::lock_guard<std::mutex> lock(mutex);
            for (auto& e : fx) {
                if (e.image == l.image->serial && e.mask == maskSerial && e.key == key) { e.lastUse = generation; return e.fx; }
            }
        }
        EffectsImage made = renderEffects(masked ? shownPixels(l) : *l.image, params);
        if (!made.image) return std::nullopt;
        std::lock_guard<std::mutex> lock(mutex);
        fx.push_back({l.image->serial, maskSerial, key, made, generation});
        size_t bytes = 0;
        for (const auto& e : fx) bytes += e.fx.image->pixels.size();
        while (fx.size() > 1 && (fx.size() > 12 || bytes > (size_t(256) << 20))) {
            bytes -= fx.front().fx.image->pixels.size();
            fx.erase(fx.begin());
        }
        return made;
    }

    void beginRender() {
        std::lock_guard<std::mutex> lock(mutex);
        ++generation;
        // 丢掉最近几次渲染都没用到的缩小图。
        for (auto it = mips.begin(); it != mips.end();) {
            if (generation - it->second.lastUse > 3) it = mips.erase(it); else ++it;
        }
        fx.erase(std::remove_if(fx.begin(), fx.end(), [&](const FxEntry& e) { return generation - e.lastUse > 6; }), fx.end());
    }
};

namespace {

// 一次渲染的上下文。
struct Pass {
    const Document& doc;
    RenderOptions options;
    Renderer::Impl& impl;
    std::unordered_map<std::string, const Layer*> byID;
    std::unordered_map<std::string, std::shared_ptr<GrayImage>> coverageCache;
    std::unordered_set<std::string> visiting;

    Pass(const Document& d, const RenderOptions& o, Renderer::Impl& i) : doc(d), options(o), impl(i) {
        for (const auto& layer : doc.layers) byID[layer.id] = &layer;
    }

    bool cancelled() const { return options.cancel && options.cancel->load(std::memory_order_relaxed); }

    const Layer* layer(const std::string& id) const {
        auto it = byID.find(id);
        return it == byID.end() ? nullptr : it->second;
    }

    // 所有上级文件夹里启用的蒙版。
    std::vector<PlacedMask> folderClips(const Layer& l) const {
        std::vector<PlacedMask> clips;
        const Layer* parent = layer(l.parentID);
        for (int depth = 0; parent && depth < 64; ++depth) {
            if (parent->mask && parent->maskEnabled) clips.push_back(placeMask(options, parent->mask.get(), parent->transform, true));
            parent = layer(parent->parentID);
        }
        return clips;
    }

    // 把一个图层自身（像素 × 蒙版 × 不透明度）按 mode 画进 target。
    // clips 与 coverage 进一步限制覆盖范围（文件夹蒙版、剪贴蒙版）。
    void drawOwn(const Layer& l, Image& target, BlendMode mode, const std::vector<PlacedMask>& clips,
                 const GrayImage* coverage, bool ignoreOpacity = false) {
        if (!l.image || l.image->empty()) return;
        double opacity = ignoreOpacity ? 1.0 : doc.effectiveOpacity(l);
        if (opacity <= 0) return;
        // 有图层效果时画带效果的放大图：蒙版已经乘进去了，变换按边距同比例放大。
        ImageRef pixels = l.image;
        LayerTransform t = l.transform;
        bool useMask = true;
        if (auto fx = impl.effects(l)) {
            pixels = fx->image;
            t = grownTransform(l.transform, fx->image->width, fx->image->height, fx->inset);
            useMask = false;
        }
        // 输出像素中心 → 原图像素坐标，用来估计缩小倍数。
        Affine toImage = Affine::translate(0.5, 0.5).then(outputToGrid(options, t, pixels->width, pixels->height));
        double ratio = std::sqrt(std::abs(toImage.a * toImage.d - toImage.b * toImage.c));
        int level = 0;
        if (t.sampling != Sampling::Nearest) {
            while (ratio >= 2 && level < 16) { ratio /= 2; ++level; }
        }
        const Image& source = impl.level(pixels, level);
        double factor = std::ldexp(1.0, -level);
        Affine toSource = Affine::translate(0.5, 0.5).then(
            outputToGrid(options, t, pixels->width * factor, pixels->height * factor));
        bool nearest = t.sampling == Sampling::Nearest;
        // 1:1 且无旋转时直接取像素，不做插值。
        bool exact = std::abs(toSource.a - 1) < 1e-9 && std::abs(toSource.d - 1) < 1e-9 && toSource.b == 0 && toSource.c == 0
            && std::abs(toSource.tx - std::round(toSource.tx)) < 1e-6 && std::abs(toSource.ty - std::round(toSource.ty)) < 1e-6;

        std::optional<PlacedMask> ownMask;
        if (useMask && l.mask && l.mask->width > 0 && l.maskEnabled) {
            bool placed = l.maskPlacement && !l.maskLinked && !l.maskPlacement->samePlacement(t);
            ownMask = placeMask(options, l.mask.get(), placed ? *l.maskPlacement : t, false);
            if (!placed) {
                // 链接的蒙版拉伸覆盖图层自身的矩形。
                ownMask->outToMask = Affine::translate(0.5, 0.5).then(outputToGrid(options, t, l.mask->width, l.mask->height));
            }
        }

        // 图层在输出中的外接框。
        double minX, minY, maxX, maxY;
        t.bounds(minX, minY, maxX, maxY);
        int x0 = std::max(0, int(std::floor(minX * options.scale + options.offsetX)) - 1);
        int y0 = std::max(0, int(std::floor(minY * options.scale + options.offsetY)) - 1);
        int x1 = std::min(target.width, int(std::ceil(maxX * options.scale + options.offsetX)) + 1);
        int y1 = std::min(target.height, int(std::ceil(maxY * options.scale + options.offsetY)) + 1);
        if (x0 >= x1 || y0 >= y1) return;

        const int sw = source.width, sh = source.height;
        auto fetch = [&](int x, int y, float out[4]) {
            if (x < 0 || y < 0 || x >= sw || y >= sh) { out[0] = out[1] = out[2] = out[3] = 0; return; }
            const uint8_t* p = source.at(x, y);
            out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; out[3] = p[3];
        };
        float fopacity = float(opacity);
        parallelRanges(y1 - y0, [&](int begin, int end) {
            for (int y = y0 + begin; y < y0 + end; ++y) {
                uint8_t* row = target.row(y);
                const uint8_t* coverageRow = coverage ? coverage->row(y) : nullptr;
                for (int x = x0; x < x1; ++x) {
                    double sx, sy;
                    toSource.apply(x, y, sx, sy);
                    float px[4];
                    if (exact || nearest) {
                        fetch(int(std::floor(sx + 0.5)), int(std::floor(sy + 0.5)), px);
                    } else {
                        if (sx < -1 || sy < -1 || sx > sw || sy > sh) continue;
                        int ix = int(std::floor(sx)), iy = int(std::floor(sy));
                        float fx = float(sx - ix), fy = float(sy - iy);
                        float a[4], b[4], c[4], d[4];
                        fetch(ix, iy, a); fetch(ix + 1, iy, b); fetch(ix, iy + 1, c); fetch(ix + 1, iy + 1, d);
                        for (int k = 0; k < 4; ++k) {
                            float top = a[k] + (b[k] - a[k]) * fx, bottom = c[k] + (d[k] - c[k]) * fx;
                            px[k] = top + (bottom - top) * fy;
                        }
                    }
                    if (px[3] <= 0) continue;
                    float cover = fopacity / 255.0f;
                    if (ownMask) cover *= ownMask->at(x, y);
                    for (const auto& clip : clips) { if (cover <= 0) break; cover *= clip.at(x, y); }
                    if (coverageRow) cover *= coverageRow[x] / 255.0f;
                    if (cover <= 0) continue;
                    float src[4] = {px[0] * cover, px[1] * cover, px[2] * cover, px[3] * cover};
                    uint8_t* d = row + size_t(x) * 4;
                    if (mode == BlendMode::Normal) {
                        float inv = 1 - src[3];
                        for (int k = 0; k < 4; ++k) d[k] = toByte(src[k] + d[k] / 255.0f * inv);
                    } else {
                        float dst[4] = {d[0] / 255.0f, d[1] / 255.0f, d[2] / 255.0f, d[3] / 255.0f};
                        blendPixel(mode, src, dst);
                        for (int k = 0; k < 4; ++k) d[k] = toByte(dst[k]);
                    }
                }
            }
        }, 8);
    }

    // 剪贴蒙版来源的覆盖度：来源自身（含它的蒙版与它自己的剪贴来源）画在透明底上的 alpha，与可见性无关。
    const GrayImage* coverage(const std::string& id) {
        auto cached = coverageCache.find(id);
        if (cached != coverageCache.end()) return cached->second.get();
        const Layer* source = layer(id);
        if (!source || visiting.count(id) || visiting.size() >= 256) return nullptr;
        visiting.insert(id);
        Image surface(options.width, options.height);
        draw(*source, surface, {});
        visiting.erase(id);
        auto gray = std::make_shared<GrayImage>(options.width, options.height);
        layer_extract_alpha(surface.pixels.data(), surface.stride(), gray->pixels.data(), size_t(gray->width),
                            size_t(options.width), size_t(options.height));
        coverageCache[id] = gray;
        return gray.get();
    }

    // 画一个图层：有剪贴来源时先按来源的覆盖度裁剪。
    void draw(const Layer& l, Image& target, const std::vector<PlacedMask>& clips) {
        const GrayImage* cover = nullptr;
        if (!l.maskSourceID.empty()) {
            cover = coverage(l.maskSourceID);
            if (!cover) return;
        }
        drawOwn(l, target, l.blendMode, clips, cover);
    }

    // 调整层：对 target 的现有内容做调整，再按混合模式、不透明度与蒙版混回去。
    void adjust(const Layer& l, Image& target, const std::vector<PlacedMask>& folderMasks) {
        AdjustmentParams params;
        try { params = parseAdjustment(*l.adjustment); } catch (...) { return; }
        Image adjusted = target;
        applyAdjustment(params, adjusted, -options.offsetX / options.scale, -options.offsetY / options.scale, 1 / options.scale);
        if (l.blendMode != BlendMode::Normal) {
            // 颜色在完全不透明下混合，再还原原来的 alpha，避免半透明边缘被叠厚。
            size_t w = size_t(target.width), h = size_t(target.height);
            Image base = target;
            GrayImage alpha(target.width, target.height);
            layer_extract_alpha(base.pixels.data(), base.stride(), alpha.pixels.data(), w, w, h);
            layer_unpremultiply_opaque(base.pixels.data(), base.stride(), w, h);
            layer_unpremultiply_opaque(adjusted.pixels.data(), adjusted.stride(), w, h);
            parallelRanges(target.height, [&](int begin, int end) {
                for (int y = begin; y < end; ++y) {
                    uint8_t* b = base.row(y);
                    const uint8_t* s = adjusted.row(y);
                    for (int x = 0; x < target.width; ++x) {
                        float src[4] = {s[x * 4] / 255.0f, s[x * 4 + 1] / 255.0f, s[x * 4 + 2] / 255.0f, 1};
                        float dst[4] = {b[x * 4] / 255.0f, b[x * 4 + 1] / 255.0f, b[x * 4 + 2] / 255.0f, 1};
                        blendPixel(l.blendMode, src, dst);
                        for (int k = 0; k < 3; ++k) b[x * 4 + k] = toByte(dst[k]);
                    }
                }
            });
            layer_restore_alpha(base.pixels.data(), base.stride(), alpha.pixels.data(), w, w, h);
            adjusted = std::move(base);
        }
        float opacity = float(doc.effectiveOpacity(l));
        std::optional<PlacedMask> own;
        if (l.mask && l.maskEnabled) own = placeMask(options, l.mask.get(), l.transform, true);
        parallelRanges(target.height, [&](int begin, int end) {
            for (int y = begin; y < end; ++y) {
                uint8_t* d = target.row(y);
                const uint8_t* s = adjusted.row(y);
                for (int x = 0; x < target.width; ++x) {
                    float t = opacity;
                    if (own) t *= own->at(x, y);
                    for (const auto& clip : folderMasks) { if (t <= 0) break; t *= clip.at(x, y); }
                    if (t <= 0) continue;
                    for (int k = 0; k < 4; ++k) {
                        float v = d[x * 4 + k] + (s[x * 4 + k] - d[x * 4 + k]) * t;
                        d[x * 4 + k] = uint8_t(std::lround(v));
                    }
                }
            }
        }, 8);
    }

    // 剪贴组的合成结果按基底的混合模式画到 target 上。
    void composite(const Image& group, Image& target, BlendMode mode, const std::vector<PlacedMask>& clips) {
        parallelRanges(target.height, [&](int begin, int end) {
            for (int y = begin; y < end; ++y) {
                uint8_t* d = target.row(y);
                const uint8_t* s = group.row(y);
                for (int x = 0; x < target.width; ++x) {
                    if (s[x * 4 + 3] == 0) continue;
                    float cover = 1;
                    for (const auto& clip : clips) { if (cover <= 0) break; cover *= clip.at(x, y); }
                    if (cover <= 0) continue;
                    float src[4], dst[4];
                    for (int k = 0; k < 4; ++k) { src[k] = s[x * 4 + k] / 255.0f * cover; dst[k] = d[x * 4 + k] / 255.0f; }
                    blendPixel(mode, src, dst);
                    for (int k = 0; k < 4; ++k) d[x * 4 + k] = toByte(dst[k]);
                }
            }
        }, 8);
    }

    Image run() {
        Image canvas(options.width, options.height);
        auto visible = doc.visibleLayers();
        if (options.skipLayerID) {
            visible.erase(std::remove_if(visible.begin(), visible.end(),
                [&](const Layer* l) { return l->id == *options.skipLayerID; }), visible.end());
        }
        // 剪贴组：基底后面紧跟的、剪贴到它且在同一文件夹里的图层共享基底的 alpha。
        std::unordered_map<std::string, std::vector<const Layer*>> stacks;
        std::unordered_set<std::string> stacked;
        for (size_t i = 0; i < visible.size(); ++i) {
            const Layer* base = visible[i];
            if (!base->maskSourceID.empty() || base->adjustment) continue;
            std::vector<const Layer*> children;
            for (size_t j = i + 1; j < visible.size(); ++j) {
                if (visible[j]->maskSourceID != base->id || visible[j]->parentID != base->parentID) break;
                children.push_back(visible[j]);
            }
            if (children.empty()) continue;
            for (const Layer* child : children) stacked.insert(child->id);
            stacks[base->id] = std::move(children);
        }
        for (const Layer* l : visible) {
            if (cancelled()) return Image();
            if (stacked.count(l->id)) continue;
            auto clips = folderClips(*l);
            if (l->adjustment) {
                if (l->maskSourceID.empty()) adjust(*l, canvas, clips);
                continue;
            }
            auto stack = stacks.find(l->id);
            if (stack == stacks.end()) { draw(*l, canvas, clips); continue; }
            Image group(options.width, options.height);
            drawOwn(*l, group, BlendMode::Normal, {}, nullptr);
            size_t w = size_t(group.width), h = size_t(group.height);
            GrayImage alpha(group.width, group.height);
            layer_extract_alpha(group.pixels.data(), group.stride(), alpha.pixels.data(), w, w, h);
            layer_unpremultiply_opaque(group.pixels.data(), group.stride(), w, h);
            for (const Layer* child : stack->second) {
                if (child->adjustment) adjust(*child, group, {});
                else drawOwn(*child, group, child->blendMode, {}, nullptr);
            }
            layer_restore_alpha(group.pixels.data(), group.stride(), alpha.pixels.data(), w, w, h);
            composite(group, canvas, l->blendMode, clips);
        }
        return canvas;
    }
};

// 渲染需要的额外边距（输出像素），让模糊类调整在可视区边缘也取到外面的像素。
int samplingMargin(const Document& doc, double scale) {
    double margin = 0;
    for (const Layer* l : doc.visibleLayers()) {
        if (!l->adjustment) continue;
        try {
            AdjustmentParams p = parseAdjustment(*l->adjustment);
            if (p.kind == AdjustmentKind::GaussianBlur) margin = std::max(margin, p.blurRadius * 3 + 2);
            if (p.kind == AdjustmentKind::MotionBlur) margin = std::max(margin, p.motionDistance / 2 + 2);
        } catch (...) {}
    }
    return int(std::ceil(margin * scale));
}

} // namespace

Renderer::Renderer() : impl_(std::make_unique<Impl>()) {}
Renderer::~Renderer() = default;

void Renderer::clearCache() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->mips.clear();
}

Image Renderer::render(const Document& document, const RenderOptions& options) {
    if (options.width <= 0 || options.height <= 0 || !(options.scale > 0)) return Image();
    impl_->beginRender();
    int margin = samplingMargin(document, options.scale);
    if (margin <= 0) {
        Pass pass(document, options, *impl_);
        return pass.run();
    }
    margin = std::min(margin, 4096);
    RenderOptions grown = options;
    grown.width += margin * 2;
    grown.height += margin * 2;
    grown.offsetX += margin;
    grown.offsetY += margin;
    Pass pass(document, grown, *impl_);
    Image big = pass.run();
    if (big.empty()) return big;
    Image out(options.width, options.height);
    for (int y = 0; y < options.height; ++y) {
        std::memcpy(out.row(y), big.row(y + margin) + size_t(margin) * 4, out.stride());
    }
    return out;
}

Image Renderer::renderFull(const Document& document) {
    RenderOptions options;
    options.width = document.width;
    options.height = document.height;
    return render(document, options);
}

Image Renderer::renderLayerThumbnail(const Document& document, const Layer& layer, int size) {
    (void)document;
    if (layer.image && !layer.image->empty()) return scaledToFit(*layer.image, size);
    return Image();
}

Image flatten(const Document& document) {
    Renderer renderer;
    return renderer.renderFull(document);
}

} // namespace comp
