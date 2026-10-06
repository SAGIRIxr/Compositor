#include "compositor/selection.h"
#include "compositor/renderer.h"
#include "parallel.h"
#include "planes.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>

extern "C" {
#include "ContentFill.h"
#include "WandPixels.h"
}

namespace comp {

namespace {

inline float sampleGray(const GrayImage& g, double x, double y) {
    // 双线性；越界为 0。
    int x0 = int(std::floor(x)), y0 = int(std::floor(y));
    float fx = float(x - x0), fy = float(y - y0);
    auto at = [&](int px, int py) -> float {
        if (px < 0 || py < 0 || px >= g.width || py >= g.height) return 0;
        return g.row(py)[px];
    };
    float top = at(x0, y0) * (1 - fx) + at(x0 + 1, y0) * fx;
    float bottom = at(x0, y0 + 1) * (1 - fx) + at(x0 + 1, y0 + 1) * fx;
    return (top * (1 - fy) + bottom * fy) / 255.0f;
}

// 图层可写的像素：空白图层先建一张透明位图。
std::shared_ptr<Image> writablePixels(Layer& layer) {
    if (layer.isGroup || layer.adjustment) return nullptr;
    if (layer.image) return std::make_shared<Image>(*layer.image);
    int w = std::clamp(int(std::lround(layer.transform.width)), 1, kMaxSide);
    int h = std::clamp(int(std::lround(layer.transform.height)), 1, kMaxSide);
    return std::make_shared<Image>(w, h);
}

} // namespace

GrayImage rectSelection(int width, int height, double x, double y, double w, double h, bool ellipse) {
    GrayImage out(width, height);
    if (w < 0) { x += w; w = -w; }
    if (h < 0) { y += h; h = -h; }
    if (w <= 0 || h <= 0) return out;
    int x0 = std::max(0, int(std::floor(x))), x1 = std::min(width, int(std::ceil(x + w)));
    int y0 = std::max(0, int(std::floor(y))), y1 = std::min(height, int(std::ceil(y + h)));
    if (!ellipse) {
        // 矩形：每个像素与矩形的重叠面积。
        for (int py = y0; py < y1; ++py) {
            double cy = std::min(py + 1.0, y + h) - std::max(double(py), y);
            for (int px = x0; px < x1; ++px) {
                double cx = std::min(px + 1.0, x + w) - std::max(double(px), x);
                out.row(py)[px] = uint8_t(std::lround(std::clamp(cx * cy, 0.0, 1.0) * 255));
            }
        }
        return out;
    }
    double cx = x + w / 2, cy = y + h / 2, rx = w / 2, ry = h / 2;
    parallelRanges(y1 - y0, [&](int begin, int end) {
        for (int py = y0 + begin; py < y0 + end; ++py) {
            for (int px = x0; px < x1; ++px) {
                // 4×4 超采样。
                int inside = 0;
                for (int sy = 0; sy < 4; ++sy) for (int sx = 0; sx < 4; ++sx) {
                    double dx = (px + (sx + 0.5) / 4 - cx) / rx, dy = (py + (sy + 0.5) / 4 - cy) / ry;
                    if (dx * dx + dy * dy <= 1) ++inside;
                }
                out.row(py)[px] = uint8_t(std::lround(inside * 255.0 / 16));
            }
        }
    });
    return out;
}

GrayImage polygonSelection(int width, int height, const std::vector<StrokePoint>& points) {
    GrayImage out(width, height);
    if (points.size() < 3) return out;
    double minY = 1e300, maxY = -1e300;
    for (const auto& p : points) { minY = std::min(minY, p.y); maxY = std::max(maxY, p.y); }
    int y0 = std::max(0, int(std::floor(minY))), y1 = std::min(height, int(std::ceil(maxY)));
    const int sub = 4;
    parallelRanges(y1 - y0, [&](int begin, int end) {
        std::vector<float> accumulate(size_t(width) + 1);
        std::vector<double> crossings;
        for (int py = y0 + begin; py < y0 + end; ++py) {
            std::fill(accumulate.begin(), accumulate.end(), 0.0f);
            for (int s = 0; s < sub; ++s) {
                double sy = py + (s + 0.5) / sub;
                crossings.clear();
                for (size_t i = 0; i < points.size(); ++i) {
                    const StrokePoint& a = points[i];
                    const StrokePoint& b = points[(i + 1) % points.size()];
                    if ((a.y <= sy && b.y > sy) || (b.y <= sy && a.y > sy)) {
                        crossings.push_back(a.x + (sy - a.y) / (b.y - a.y) * (b.x - a.x));
                    }
                }
                std::sort(crossings.begin(), crossings.end());
                // 奇偶规则填充，跨度两端按小数覆盖。
                for (size_t i = 0; i + 1 < crossings.size(); i += 2) {
                    double left = std::clamp(crossings[i], 0.0, double(width));
                    double right = std::clamp(crossings[i + 1], 0.0, double(width));
                    if (right <= left) continue;
                    int l = int(left), r = int(right);
                    if (l == r) { accumulate[size_t(l)] += float(right - left); continue; }
                    accumulate[size_t(l)] += float(l + 1 - left);
                    for (int x = l + 1; x < r; ++x) accumulate[size_t(x)] += 1;
                    if (r < width) accumulate[size_t(r)] += float(right - r);
                }
            }
            uint8_t* row = out.row(py);
            for (int x = 0; x < width; ++x) row[x] = uint8_t(std::lround(std::clamp(accumulate[size_t(x)] / sub, 0.0f, 1.0f) * 255));
        }
    }, 8);
    return out;
}

GrayImage combineSelection(const GrayImage* existing, const GrayImage& shape, SelectionMode mode) {
    if (!existing || mode == SelectionMode::Replace) {
        if (mode == SelectionMode::Subtract || mode == SelectionMode::Intersect) return GrayImage(shape.width, shape.height);
        return shape;
    }
    GrayImage out = *existing;
    for (size_t i = 0; i < out.pixels.size(); ++i) {
        unsigned a = out.pixels[i], b = shape.pixels[i];
        switch (mode) {
        case SelectionMode::Add: out.pixels[i] = uint8_t(std::max(a, b)); break;
        case SelectionMode::Subtract: out.pixels[i] = uint8_t(a * (255 - b) / 255); break;
        case SelectionMode::Intersect: out.pixels[i] = uint8_t(std::min(a, b)); break;
        case SelectionMode::Replace: break;
        }
    }
    out.touch();
    return out;
}

bool selectionIsEmpty(const GrayImage& selection) {
    for (uint8_t v : selection.pixels) if (v) return false;
    return true;
}

bool selectionBounds(const GrayImage& s, int& x, int& y, int& w, int& h) {
    int minX = s.width, minY = s.height, maxX = -1, maxY = -1;
    for (int py = 0; py < s.height; ++py) {
        const uint8_t* row = s.row(py);
        for (int px = 0; px < s.width; ++px) {
            if (!row[px]) continue;
            minX = std::min(minX, px); maxX = std::max(maxX, px);
            minY = std::min(minY, py); maxY = std::max(maxY, py);
        }
    }
    if (maxX < 0) return false;
    x = minX; y = minY; w = maxX - minX + 1; h = maxY - minY + 1;
    return true;
}

GrayImage invertSelection(const GrayImage* selection, int width, int height) {
    GrayImage out = selection ? *selection : GrayImage(width, height);
    for (auto& v : out.pixels) v = uint8_t(255 - v);
    out.touch();
    return out;
}

void featherSelection(GrayImage& selection, double radius) {
    if (radius <= 0) return;
    detail::Plane plane(selection.pixels.size());
    for (size_t i = 0; i < plane.size(); ++i) plane[i] = selection.pixels[i] / 255.0f;
    // Photoshop 的羽化半径大致相当于 σ ≈ 半径 / 2。
    detail::blurPlane(plane, selection.width, selection.height, radius / 2);
    for (size_t i = 0; i < plane.size(); ++i) selection.pixels[i] = uint8_t(std::lround(std::clamp(plane[i], 0.0f, 1.0f) * 255));
    selection.touch();
}

void growSelection(GrayImage& selection, int pixels) {
    if (pixels == 0) return;
    detail::Plane plane(selection.pixels.size());
    for (size_t i = 0; i < plane.size(); ++i) plane[i] = selection.pixels[i] / 255.0f;
    detail::Plane moved = detail::extremePlane(plane, selection.width, selection.height, std::abs(pixels), pixels < 0);
    if (pixels < 0) {
        // 收缩时画布边缘不算选区外（Photoshop 的默认）：边缘内的像素按原值。
        int r = -pixels;
        for (int y = 0; y < selection.height; ++y) for (int x = 0; x < selection.width; ++x) {
            if (x < r || y < r || x >= selection.width - r || y >= selection.height - r) {
                // 只在窗口里能看到的范围内取最小值。
                float best = 1;
                for (int dy = -r; dy <= r; ++dy) for (int dx = -r; dx <= r; ++dx) {
                    int sx = x + dx, sy = y + dy;
                    if (sx < 0 || sy < 0 || sx >= selection.width || sy >= selection.height) continue;
                    best = std::min(best, plane[size_t(sy) * size_t(selection.width) + size_t(sx)]);
                }
                moved[size_t(y) * size_t(selection.width) + size_t(x)] = best;
            }
        }
    }
    for (size_t i = 0; i < plane.size(); ++i) selection.pixels[i] = uint8_t(std::lround(std::clamp(moved[i], 0.0f, 1.0f) * 255));
    selection.touch();
}

Image renderLayerAlone(const Document& doc, const Layer& layer) {
    Document single;
    single.id = doc.id;
    single.width = doc.width;
    single.height = doc.height;
    Layer copy = layer;
    copy.parentID.clear();
    copy.visible = true;
    copy.opacity = 1;
    copy.blendMode = BlendMode::Normal;
    copy.maskSourceID.clear();
    single.layers.push_back(copy);
    return flatten(single);
}

std::optional<GrayImage> wandSelection(const Image& source, int x, int y, int tolerance, bool contiguous, int sampleRadius) {
    if (x < 0 || y < 0 || x >= source.width || y >= source.height) return std::nullopt;
    GrayImage mask(source.width, source.height);
    long count = wand_mask(source.pixels.data(), size_t(source.width), size_t(source.height), source.stride(),
                           size_t(x), size_t(y), size_t(std::max(0, sampleRadius)), std::clamp(tolerance, 0, 255),
                           contiguous ? 1 : 0, mask.pixels.data());
    if (count <= 0) return std::nullopt;
    mask.touch();
    return mask;
}

GrayImage selectionFromLayer(const Document& doc, const Layer& layer) {
    Image alone = renderLayerAlone(doc, layer);
    GrayImage out(doc.width, doc.height);
    for (size_t i = 0; i < out.pixels.size(); ++i) out.pixels[i] = alone.pixels[i * 4 + 3];
    out.touch();
    return out;
}

GrayImage selectionInLayerGrid(const LayerTransform& transform, int w, int h, const GrayImage& selection) {
    GrayImage out(w, h);
    Affine pixelToDoc = Affine::scale(1.0 / w, 1.0 / h).then(transform.unitToDocument());
    parallelRanges(h, [&](int begin, int end) {
        for (int y = begin; y < end; ++y) {
            uint8_t* row = out.row(y);
            for (int x = 0; x < w; ++x) {
                double dx, dy;
                pixelToDoc.apply(x + 0.5, y + 0.5, dx, dy);
                row[x] = uint8_t(std::lround(sampleGray(selection, dx - 0.5, dy - 0.5) * 255));
            }
        }
    });
    return out;
}

bool clearSelection(Document& doc, const std::string& layerID, const GrayImage& selection) {
    Layer* layer = doc.find(layerID);
    if (!layer || !layer->image) return layer && !layer->isGroup && !layer->adjustment; // 空白图层清除也算成功
    auto pixels = std::make_shared<Image>(*layer->image);
    GrayImage grid = selectionInLayerGrid(layer->transform, pixels->width, pixels->height, selection);
    for (size_t i = 0; i < grid.pixels.size(); ++i) {
        unsigned keep = 255 - grid.pixels[i];
        for (int k = 0; k < 4; ++k) pixels->pixels[i * 4 + size_t(k)] = uint8_t((pixels->pixels[i * 4 + size_t(k)] * keep + 127) / 255);
    }
    layer->image = pixels;
    layer->dropVectorMetadata();
    return true;
}

bool fillSelection(Document& doc, const std::string& layerID, const GrayImage& selection, const uint8_t color[4]) {
    Layer* layer = doc.find(layerID);
    if (!layer) return false;
    auto pixels = writablePixels(*layer);
    if (!pixels) return false;
    GrayImage grid = selectionInLayerGrid(layer->transform, pixels->width, pixels->height, selection);
    for (size_t i = 0; i < grid.pixels.size(); ++i) {
        float a = grid.pixels[i] / 255.0f * color[3] / 255.0f;
        if (a <= 0) continue;
        uint8_t* p = &pixels->pixels[i * 4];
        for (int k = 0; k < 3; ++k) p[k] = uint8_t(std::lround(color[k] * a + p[k] * (1 - a)));
        p[3] = uint8_t(std::lround(255 * a + p[3] * (1 - a)));
    }
    pixels->touch();
    layer->image = pixels;
    layer->dropVectorMetadata();
    return true;
}

bool maskFromSelection(Document& doc, const std::string& layerID, const GrayImage& selection) {
    Layer* layer = doc.find(layerID);
    if (!layer) return false;
    int w, h;
    if (layer->image) { w = layer->image->width; h = layer->image->height; }
    else {
        w = std::clamp(int(std::lround(layer->transform.width)), 1, kMaxSide);
        h = std::clamp(int(std::lround(layer->transform.height)), 1, kMaxSide);
    }
    auto mask = std::make_shared<GrayImage>(selectionInLayerGrid(layer->transform, w, h, selection));
    if (mask->uniformValue() >= 0) mask = std::make_shared<GrayImage>(1, 1, mask->pixels.empty() ? 0 : mask->pixels[0]);
    layer->mask = mask;
    layer->maskEnabled = true;
    layer->maskLinked = true;
    layer->maskPlacement.reset();
    return true;
}

int contentAwareFill(Document& doc, const std::string& layerID, const GrayImage& selection) {
    Layer* layer = doc.find(layerID);
    if (!layer || !layer->image || layer->isGroup || layer->adjustment) return -2;
    auto pixels = std::make_shared<Image>(*layer->image);
    GrayImage grid = selectionInLayerGrid(layer->transform, pixels->width, pixels->height, selection);
    // 部分选中的像素也算要填的。
    for (auto& v : grid.pixels) v = v >= 8 ? 255 : 0;
    int result = content_fill(pixels->pixels.data(), pixels->stride(), grid.pixels.data(), size_t(grid.width),
                              pixels->width, pixels->height);
    if (result != 1) return result;
    pixels->touch();
    layer->image = pixels;
    layer->dropVectorMetadata();
    return 1;
}

Image copySelection(const Image& docImage, const GrayImage& selection, int& originX, int& originY) {
    int x, y, w, h;
    if (!selectionBounds(selection, x, y, w, h)) return Image();
    Image out(w, h);
    for (int py = 0; py < h; ++py) {
        for (int px = 0; px < w; ++px) {
            unsigned m = selection.row(y + py)[x + px];
            const uint8_t* s = docImage.at(x + px, y + py);
            uint8_t* d = out.at(px, py);
            for (int k = 0; k < 4; ++k) d[k] = uint8_t((s[k] * m + 127) / 255);
        }
    }
    originX = x;
    originY = y;
    return out;
}

bool cropToSelection(Document& doc) {
    if (!doc.selection) return false;
    int x, y, w, h;
    if (!selectionBounds(*doc.selection, x, y, w, h)) return false;
    cropCanvas(doc, x, y, w, h);
    doc.selection.reset();
    return true;
}

bool selectionOutline(const GrayImage& selection, std::vector<std::vector<std::pair<int, int>>>& loops) {
    loops.clear();
    std::vector<uint8_t> binary(selection.pixels.size());
    for (size_t i = 0; i < binary.size(); ++i) binary[i] = selection.pixels[i] >= 128 ? 255 : 0;
    int32_t* points = nullptr;
    int32_t* counts = nullptr;
    size_t pointCount = 0, loopCount = 0;
    int status = wand_trace(binary.data(), size_t(selection.width), size_t(selection.height), &points, &pointCount, &counts, &loopCount);
    if (status != 0) { std::free(points); std::free(counts); return false; }
    size_t offset = 0;
    for (size_t l = 0; l < loopCount; ++l) {
        std::vector<std::pair<int, int>> loop;
        for (int32_t k = 0; k < counts[l]; ++k, ++offset) loop.emplace_back(points[offset * 2], points[offset * 2 + 1]);
        loops.push_back(std::move(loop));
    }
    std::free(points);
    std::free(counts);
    return true;
}

} // namespace comp
