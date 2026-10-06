#include "compositor/effects.h"
#include "compositor/document.h"
#include "parallel.h"
#include "planes.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace comp {

namespace {

const char* const kKeys[6] = {"stroke", "shadow", "colorOverlay", "innerShadow", "outerGlow", "innerGlow"};
const char* const kLabels[6] = {"描边", "投影", "颜色叠加", "内阴影", "外发光", "内发光"};

double num(const Json& o, const char* key, double fallback) {
    auto it = o.find(key);
    if (it == o.end() || it->is_null()) return fallback;
    if (!it->is_number()) throw std::runtime_error(std::string("图层效果参数 ") + key + " 不是数字");
    return it->get<double>();
}
bool flag(const Json& o, const char* key, bool fallback) {
    auto it = o.find(key);
    if (it == o.end() || it->is_null()) return fallback;
    if (!it->is_boolean()) throw std::runtime_error(std::string("图层效果参数 ") + key + " 不是布尔值");
    return it->get<bool>();
}

using detail::Plane;

// 单调队列求窗口内最大（或最小）值，越界处为 0，耗时与半径无关。
void extremePass(const float* in, float* out, int lines, int count, size_t lineStep, size_t elementStep, int radius, bool smallest) {
    parallelRanges(lines, [&](int begin, int end) {
        std::vector<int> queue(size_t(count) + 1);
        for (int line = begin; line < end; ++line) {
            const float* src = in + size_t(line) * lineStep;
            float* dst = out + size_t(line) * lineStep;
            int head = 0, tail = 0, next = 0;
            for (int center = 0; center < count; ++center) {
                while (next <= std::min(count - 1, center + radius)) {
                    float value = src[size_t(next) * elementStep];
                    while (tail > head) {
                        float previous = src[size_t(queue[size_t(tail - 1)]) * elementStep];
                        if (smallest ? previous < value : previous > value) break;
                        --tail;
                    }
                    queue[size_t(tail++)] = next++;
                }
                while (head < tail && queue[size_t(head)] < center - radius) ++head;
                bool outside = center < radius || center + radius >= count;
                dst[size_t(center) * elementStep] = (smallest && outside) ? 0.0f : src[size_t(queue[size_t(head)]) * elementStep];
            }
        }
    }, 4);
}

Plane extremeImpl(const Plane& source, int w, int h, int reach, bool smallest) {
    Plane pass(source.size()), result(source.size());
    extremePass(source.data(), pass.data(), h, w, size_t(w), 1, reach, smallest);
    extremePass(pass.data(), result.data(), w, h, 1, size_t(w), reach, smallest);
    return result;
}

void boxPass(const float* in, float* out, int lines, int count, size_t lineStep, size_t elementStep, int radius) {
    parallelRanges(lines, [&](int begin, int end) {
        for (int line = begin; line < end; ++line) {
            const float* src = in + size_t(line) * lineStep;
            float* dst = out + size_t(line) * lineStep;
            auto at = [&](int i) { return src[size_t(std::clamp(i, 0, count - 1)) * elementStep]; };
            double sum = 0;
            for (int i = -radius; i <= radius; ++i) sum += at(i);
            float window = float(2 * radius + 1);
            for (int i = 0; i < count; ++i) {
                dst[size_t(i) * elementStep] = float(sum / window);
                sum += at(i + radius + 1) - at(i - radius);
            }
        }
    }, 4);
}

// 三次盒式模糊近似高斯（σ），边缘外延。
void blurImpl(Plane& plane, int w, int h, double sigma) {
    if (sigma < 0.3) return;
    double ideal = std::sqrt(12 * sigma * sigma / 3 + 1);
    int wl = int(std::floor(ideal));
    if (wl % 2 == 0) --wl;
    int wu = wl + 2;
    int m = int(std::lround((12 * sigma * sigma - 3.0 * wl * wl - 12.0 * wl - 9.0) / (-4.0 * wl - 4)));
    Plane temp(plane.size());
    for (int pass = 0; pass < 3; ++pass) {
        int radius = ((pass < m ? wl : wu) - 1) / 2;
        if (radius <= 0) continue;
        boxPass(plane.data(), temp.data(), h, w, size_t(w), 1, radius);
        boxPass(temp.data(), plane.data(), w, h, 1, size_t(w), radius);
    }
}

// 平移（双线性），移出的部分为 0。
Plane shifted(const Plane& source, int w, int h, double dx, double dy) {
    Plane out(source.size(), 0);
    parallelRanges(h, [&](int begin, int end) {
        for (int y = begin; y < end; ++y) {
            for (int x = 0; x < w; ++x) {
                double sx = x - dx, sy = y - dy;
                if (sx < 0 || sy < 0 || sx > w - 1 || sy > h - 1) continue;
                int x0 = int(sx), y0 = int(sy), x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
                float fx = float(sx - x0), fy = float(sy - y0);
                float top = source[size_t(y0) * size_t(w) + size_t(x0)] * (1 - fx) + source[size_t(y0) * size_t(w) + size_t(x1)] * fx;
                float bottom = source[size_t(y1) * size_t(w) + size_t(x0)] * (1 - fx) + source[size_t(y1) * size_t(w) + size_t(x1)] * fx;
                out[size_t(y) * size_t(w) + size_t(x)] = top * (1 - fy) + bottom * fy;
            }
        }
    }, 8);
    return out;
}

// 阴影落在光源的反方向；图层像素的 y 向下。
void shadowOffset(const EffectParams& e, double& dx, double& dy) {
    double r = e.angle * kPi / 180;
    dx = -std::cos(r) * e.distance;
    dy = std::sin(r) * e.distance;
}

bool colorValid(const double c[3]) {
    for (int i = 0; i < 3; ++i) if (!std::isfinite(c[i]) || c[i] < 0 || c[i] > 1) return false;
    return true;
}

} // namespace

namespace detail {
void blurPlane(Plane& plane, int width, int height, double sigma) { blurImpl(plane, width, height, sigma); }
Plane extremePlane(const Plane& source, int width, int height, int reach, bool smallest) {
    return extremeImpl(source, width, height, reach, smallest);
}
} // namespace detail

const std::vector<EffectKind>& allEffectKinds() {
    static const std::vector<EffectKind> kinds = {EffectKind::Stroke, EffectKind::Shadow, EffectKind::ColorOverlay,
                                                  EffectKind::InnerShadow, EffectKind::OuterGlow, EffectKind::InnerGlow};
    return kinds;
}
const char* effectKey(EffectKind kind) { return kKeys[int(kind)]; }
const char* effectLabel(EffectKind kind) { return kLabels[int(kind)]; }

bool LayerEffectsParams::anyVisible() const {
    for (const auto& e : effects) if (e.present && e.enabled) return true;
    return false;
}

bool LayerEffectsParams::isValid() const {
    for (int i = 0; i < 6; ++i) {
        const EffectParams& e = effects[i];
        if (!e.present) continue;
        if (!colorValid(e.color) || !std::isfinite(e.opacity) || e.opacity < 0 || e.opacity > 1) return false;
        EffectKind kind = EffectKind(i);
        if (kind == EffectKind::Stroke || kind == EffectKind::OuterGlow || kind == EffectKind::InnerGlow) {
            if (!std::isfinite(e.size) || e.size < 0 || e.size > 500) return false;
        }
        if (kind == EffectKind::Shadow || kind == EffectKind::InnerShadow) {
            if (!std::isfinite(e.angle) || std::abs(e.angle) > 360 || !std::isfinite(e.distance) || e.distance < 0
                || e.distance > 5000 || !std::isfinite(e.blur) || e.blur < 0 || e.blur > 500) return false;
        }
    }
    return true;
}

EffectParams defaultEffect(EffectKind kind, const double color[3]) {
    EffectParams e;
    e.present = true;
    switch (kind) {
    case EffectKind::Stroke: e.size = 4; e.opacity = 1; std::copy(color, color + 3, e.color); break;
    case EffectKind::Shadow: e.angle = 90; e.distance = 20; e.blur = 20; e.opacity = 0.5; break;
    case EffectKind::ColorOverlay: e.opacity = 1; std::copy(color, color + 3, e.color); break;
    case EffectKind::InnerShadow: e.angle = 90; e.distance = 10; e.blur = 10; e.opacity = 0.5; break;
    case EffectKind::OuterGlow: e.size = 20; e.opacity = 0.75; e.color[0] = e.color[1] = e.color[2] = 1; break;
    case EffectKind::InnerGlow: e.size = 10; e.opacity = 0.75; e.color[0] = e.color[1] = e.color[2] = 1; break;
    }
    return e;
}

LayerEffectsParams parseEffects(const Json& o) {
    LayerEffectsParams p;
    if (o.is_null()) return p;
    if (!o.is_object()) throw std::runtime_error("effects 不是对象");
    const double black[3] = {0, 0, 0};
    for (EffectKind kind : allEffectKinds()) {
        auto it = o.find(effectKey(kind));
        if (it == o.end() || it->is_null()) continue;
        if (!it->is_object()) throw std::runtime_error("图层效果格式错误");
        const Json& j = *it;
        EffectParams e = defaultEffect(kind, black);
        e.enabled = flag(j, "enabled", true);
        e.size = num(j, "size", e.size);
        e.angle = num(j, "angle", e.angle);
        e.distance = num(j, "distance", e.distance);
        e.blur = num(j, "blur", e.blur);
        e.color[0] = num(j, "red", e.color[0]);
        e.color[1] = num(j, "green", e.color[1]);
        e.color[2] = num(j, "blue", e.color[2]);
        e.opacity = num(j, "opacity", e.opacity);
        e.inside = flag(j, "inside", false);
        p[kind] = e;
    }
    return p;
}

void writeEffects(const LayerEffectsParams& p, Json& o) {
    if (!o.is_object()) o = Json::object();
    for (EffectKind kind : allEffectKinds()) {
        const EffectParams& e = p[kind];
        const char* key = effectKey(kind);
        if (!e.present) { o.erase(key); continue; }
        Json j = o.contains(key) && o[key].is_object() ? o[key] : Json::object();
        if (e.enabled) j.erase("enabled"); else j["enabled"] = false;
        switch (kind) {
        case EffectKind::Stroke: j["size"] = e.size; j["inside"] = e.inside; break;
        case EffectKind::Shadow:
        case EffectKind::InnerShadow: j["angle"] = e.angle; j["distance"] = e.distance; j["blur"] = e.blur; break;
        case EffectKind::OuterGlow:
        case EffectKind::InnerGlow: j["size"] = e.size; break;
        case EffectKind::ColorOverlay: break;
        }
        j["red"] = e.color[0];
        j["green"] = e.color[1];
        j["blue"] = e.color[2];
        j["opacity"] = e.opacity;
        o[key] = j;
    }
}

int effectsMargin(const LayerEffectsParams& p) {
    double margin = 0;
    const EffectParams& stroke = p[EffectKind::Stroke];
    if (stroke.present && stroke.enabled && !stroke.inside) margin = std::max(margin, stroke.size);
    const EffectParams& shadow = p[EffectKind::Shadow];
    if (shadow.present && shadow.enabled) margin = std::max(margin, shadow.distance + shadow.blur * 3);
    const EffectParams& glow = p[EffectKind::OuterGlow];
    if (glow.present && glow.enabled) margin = std::max(margin, glow.size * 3);
    return int(std::ceil(margin)) + 2;
}

EffectsImage renderEffects(const Image& shown, const LayerEffectsParams& p) {
    EffectsImage result;
    if (!p.anyVisible() || !p.isValid() || shown.empty()) return result;
    auto visible = [&](EffectKind k) -> const EffectParams* {
        const EffectParams& e = p[k];
        return e.present && e.enabled && e.opacity > 0 ? &e : nullptr;
    };
    int inset = effectsMargin(p);
    int w = shown.width + inset * 2, h = shown.height + inset * 2;
    if ((long long)w * h > kMaxSurfacePixels) return result;
    size_t count = size_t(w) * size_t(h);
    // 放进留了边距的画布。
    Image padded(w, h);
    for (int y = 0; y < shown.height; ++y) {
        std::copy(shown.row(y), shown.row(y) + shown.stride(), padded.row(y + inset) + size_t(inset) * 4);
    }
    Plane shape(count);
    for (size_t i = 0; i < count; ++i) shape[i] = padded.pixels[i * 4 + 3] / 255.0f;

    const EffectParams* stroke = visible(EffectKind::Stroke);
    if (stroke && stroke->size <= 0) stroke = nullptr;
    Plane ring;
    if (stroke) {
        int reach = std::max(1, int(std::lround(stroke->size)));
        Plane moved = extremeImpl(shape, w, h, reach, stroke->inside);
        ring.resize(count);
        for (size_t i = 0; i < count; ++i) ring[i] = std::clamp(stroke->inside ? shape[i] - moved[i] : moved[i] - shape[i], 0.0f, 1.0f);
    }
    const EffectParams* shadow = visible(EffectKind::Shadow);
    Plane shadowPlane;
    if (shadow) {
        double dx, dy;
        shadowOffset(*shadow, dx, dy);
        shadowPlane = shifted(shape, w, h, dx, dy);
        blurImpl(shadowPlane, w, h, shadow->blur / 2);
    }
    const EffectParams* inner = visible(EffectKind::InnerShadow);
    Plane innerPlane;
    if (inner) {
        double dx, dy;
        shadowOffset(*inner, dx, dy);
        Plane moved = shifted(shape, w, h, dx, dy);
        blurImpl(moved, w, h, inner->blur / 2);
        innerPlane.resize(count);
        for (size_t i = 0; i < count; ++i) innerPlane[i] = std::clamp(shape[i] * (1 - moved[i]), 0.0f, 1.0f);
    }
    const EffectParams* glow = visible(EffectKind::OuterGlow);
    if (glow && glow->size <= 0) glow = nullptr;
    Plane glowPlane;
    if (glow) { glowPlane = shape; blurImpl(glowPlane, w, h, glow->size / 2); }
    const EffectParams* innerGlow = visible(EffectKind::InnerGlow);
    if (innerGlow && innerGlow->size <= 0) innerGlow = nullptr;
    Plane innerGlowPlane;
    if (innerGlow) {
        Plane soft = shape;
        blurImpl(soft, w, h, innerGlow->size / 2);
        innerGlowPlane.resize(count);
        for (size_t i = 0; i < count; ++i) innerGlowPlane[i] = std::clamp(shape[i] * (1 - soft[i]), 0.0f, 1.0f);
    }
    const EffectParams* overlay = visible(EffectKind::ColorOverlay);

    // 合成顺序与 macOS 版相同：投影、外发光、外描边、像素、颜色叠加、内发光、内阴影、内描边。
    auto out = std::make_shared<Image>(w, h);
    parallelRanges(h, [&](int begin, int end) {
        for (int y = begin; y < end; ++y) {
            for (int x = 0; x < w; ++x) {
                size_t i = size_t(y) * size_t(w) + size_t(x);
                float c[3] = {0, 0, 0}, a = 0;
                auto over = [&](const EffectParams& e, float coverage) {
                    coverage = std::clamp(coverage * float(e.opacity), 0.0f, 1.0f);
                    if (coverage <= 0) return;
                    for (int k = 0; k < 3; ++k) c[k] = float(e.color[k]) * coverage + c[k] * (1 - coverage);
                    a = coverage + a * (1 - coverage);
                };
                if (shadow) over(*shadow, shadowPlane[i]);
                if (glow) over(*glow, glowPlane[i] * (1 - shape[i]));
                if (stroke && !stroke->inside) over(*stroke, ring[i]);
                const uint8_t* s = &padded.pixels[i * 4];
                float sa = s[3] / 255.0f;
                for (int k = 0; k < 3; ++k) c[k] = s[k] / 255.0f + c[k] * (1 - sa);
                a = sa + a * (1 - sa);
                if (overlay) over(*overlay, shape[i]);
                if (innerGlow) over(*innerGlow, innerGlowPlane[i]);
                if (inner) over(*inner, innerPlane[i]);
                if (stroke && stroke->inside) over(*stroke, ring[i]);
                uint8_t* o = &out->pixels[i * 4];
                for (int k = 0; k < 3; ++k) o[k] = uint8_t(std::lround(std::clamp(c[k], 0.0f, 1.0f) * 255));
                o[3] = uint8_t(std::lround(std::clamp(a, 0.0f, 1.0f) * 255));
                for (int k = 0; k < 3; ++k) if (o[k] > o[3]) o[k] = o[3];
            }
        }
    }, 8);
    result.image = out;
    result.inset = inset;
    return result;
}

LayerTransform grownTransform(const LayerTransform& t, int width, int height, int inset) {
    LayerTransform grown = t;
    if (width <= inset * 2 || height <= inset * 2) return t;
    grown.width = t.width * width / double(width - inset * 2);
    grown.height = t.height * height / double(height - inset * 2);
    grown.x = t.centerX() - grown.width / 2;
    grown.y = t.centerY() - grown.height / 2;
    return grown;
}

} // namespace comp
