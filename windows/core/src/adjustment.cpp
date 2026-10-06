#include "compositor/adjustment.h"
#include "compositor/transform.h"
#include "parallel.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

extern "C" {
#include "AdjustPixels.h"
#include "LevelsPixels.h"
#include "NoisePixels.h"
}

namespace comp {

namespace {

struct KindInfo { AdjustmentKind kind; const char* name; const char* label; };
const KindInfo kKinds[] = {
    {AdjustmentKind::HueSaturation, "Hue/Saturation", "色相/饱和度"},
    {AdjustmentKind::Levels, "Levels", "色阶"},
    {AdjustmentKind::Curves, "Curves", "曲线"},
    {AdjustmentKind::Exposure, "Exposure", "曝光度"},
    {AdjustmentKind::GradientMap, "Gradient Map", "渐变映射"},
    {AdjustmentKind::Grain, "Grain", "颗粒"},
    {AdjustmentKind::AddNoise, "Add Noise", "添加杂色"},
    {AdjustmentKind::GaussianBlur, "Gaussian Blur", "高斯模糊"},
    {AdjustmentKind::MotionBlur, "Motion Blur", "动感模糊"},
    {AdjustmentKind::Invert, "Invert", "反相"},
    {AdjustmentKind::BlackWhite, "Black & White", "黑白"},
    {AdjustmentKind::ColorBalance, "Color Balance", "色彩平衡"},
};

const char* const kRangeNames[7] = {"Master", "Reds", "Yellows", "Greens", "Cyans", "Blues", "Magentas"};
const HueBand kDefaultBands[7] = {
    {0, 0, 360, 360}, {315, 345, 15, 45}, {15, 45, 75, 105}, {75, 105, 135, 165},
    {135, 165, 195, 225}, {195, 225, 255, 285}, {255, 285, 315, 345},
};

int rangeIndex(const std::string& name) {
    for (int i = 0; i < 7; ++i) if (name == kRangeNames[i]) return i;
    return -1;
}

double number(const Json& object, const char* key, double fallback) {
    auto it = object.find(key);
    if (it == object.end() || it->is_null()) return fallback;
    if (!it->is_number()) throw std::runtime_error(std::string("调整参数 ") + key + " 不是数字");
    return it->get<double>();
}
bool boolean(const Json& object, const char* key, bool fallback) {
    auto it = object.find(key);
    if (it == object.end() || it->is_null()) return fallback;
    if (!it->is_boolean()) throw std::runtime_error(std::string("调整参数 ") + key + " 不是布尔值");
    return it->get<bool>();
}
uint32_t seed(const Json& object, const char* key) {
    auto it = object.find(key);
    if (it == object.end() || it->is_null()) return 0;
    if (!it->is_number_integer() && !it->is_number_unsigned()) throw std::runtime_error(std::string("调整参数 ") + key + " 不是整数");
    long long v = it->get<long long>();
    if (v < 0 || v > 0xFFFFFFFFLL) throw std::runtime_error("随机种子超出范围");
    return uint32_t(v);
}

// Swift 的 JSONEncoder 把以枚举为键的字典编码成 [键, 值, 键, 值…]；也接受普通对象。
template <typename Visit>
void visitKeyed(const Json& value, Visit visit) {
    if (value.is_object()) {
        for (auto it = value.begin(); it != value.end(); ++it) visit(it.key(), it.value());
    } else if (value.is_array()) {
        if (value.size() % 2) throw std::runtime_error("色相范围列表格式错误");
        for (size_t i = 0; i + 1 < value.size(); i += 2) {
            if (!value[i].is_string()) throw std::runtime_error("色相范围名称错误");
            visit(value[i].get<std::string>(), value[i + 1]);
        }
    } else throw std::runtime_error("色相范围格式错误");
}

double forward(double from, double to) {
    double d = std::fmod(to - from, 360.0);
    return d < 0 ? d + 360 : d;
}

Json colorJson(const double c[3]) { return Json{{"red", c[0]}, {"green", c[1]}, {"blue", c[2]}}; }
void readColor(const Json& object, double out[3]) {
    out[0] = number(object, "red", out[0]);
    out[1] = number(object, "green", out[1]);
    out[2] = number(object, "blue", out[2]);
}

Json levelsJson(const std::array<LevelRange, 4>& levels, const std::string& channel) {
    Json ranges = Json::array();
    for (const auto& r : levels) {
        ranges.push_back(Json{{"black", r.black}, {"gamma", r.gamma}, {"white", r.white},
                              {"outputBlack", r.outputBlack}, {"outputWhite", r.outputWhite}});
    }
    return Json{{"channel", channel}, {"ranges", ranges}};
}

Json curvesJson(const std::array<std::vector<CurvePoint>, 4>& curves, const std::string& channel) {
    Json channels = Json::array();
    for (const auto& points : curves) {
        Json list = Json::array();
        for (const auto& p : points) list.push_back(Json{{"x", p.x}, {"y", p.y}});
        channels.push_back(list);
    }
    return Json{{"channel", channel}, {"channels", channels}};
}

std::vector<CurvePoint> identityCurve() { return {{0, 0}, {255, 255}}; }

bool curveValid(const std::vector<CurvePoint>& p) {
    if (p.size() < 2 || p.size() > 32 || p.front().x != 0 || p.back().x != 255) return false;
    for (size_t i = 0; i < p.size(); ++i) {
        if (!std::isfinite(p[i].x) || !std::isfinite(p[i].y) || p[i].x < 0 || p[i].x > 255 || p[i].y < 0 || p[i].y > 255) return false;
        if (i && !(p[i - 1].x < p[i].x)) return false;
    }
    return true;
}

// 保形三次 Hermite 插值，与 macOS 版 CurvesSettings.value 相同。
double curveValueImpl(const std::vector<CurvePoint>& p, double x) {
    size_t n = p.size();
    size_t i = 0;
    for (size_t k = 0; k < n; ++k) if (p[k].x <= x) i = k;
    i = std::min(n - 2, i);
    std::vector<double> d(n - 1);
    for (size_t k = 0; k + 1 < n; ++k) d[k] = (p[k + 1].y - p[k].y) / (p[k + 1].x - p[k].x);
    auto slope = [&](size_t j) -> double {
        if (j == 0) return d[0];
        if (j == n - 1) return d.back();
        if (d[j - 1] * d[j] <= 0) return 0;
        return 2 / (1 / d[j - 1] + 1 / d[j]);
    };
    double h = p[i + 1].x - p[i].x, t = std::clamp((x - p[i].x) / h, 0.0, 1.0);
    double y = (2 * t * t * t - 3 * t * t + 1) * p[i].y + (t * t * t - 2 * t * t + t) * h * slope(i)
             + (-2 * t * t * t + 3 * t * t) * p[i + 1].y + (t * t * t - t * t) * h * slope(i + 1);
    return std::clamp(y, 0.0, 255.0);
}

void toHSL(double r, double g, double b, double& h, double& s, double& l) {
    double hi = std::max({r, g, b}), lo = std::min({r, g, b});
    l = (hi + lo) / 2;
    double delta = hi - lo;
    if (delta <= 0) { h = 0; s = 0; return; }
    s = std::min(1.0, delta / (1 - std::abs(2 * l - 1)));
    if (hi == r) h = (g - b) / delta;
    else if (hi == g) h = (b - r) / delta + 2;
    else h = (r - g) / delta + 4;
    h *= 60;
    if (h < 0) h += 360;
}

void toRGB(double h, double s, double l, double& r, double& g, double& b) {
    if (s <= 0) { r = g = b = l; return; }
    double chroma = (1 - std::abs(2 * l - 1)) * s;
    double sector = h / 60;
    double second = chroma * (1 - std::abs(std::fmod(sector, 2.0) - 1));
    double base = l - chroma / 2;
    switch (int(sector)) {
    case 0: r = chroma; g = second; b = 0; break;
    case 1: r = second; g = chroma; b = 0; break;
    case 2: r = 0; g = chroma; b = second; break;
    case 3: r = 0; g = second; b = chroma; break;
    case 4: r = second; g = 0; b = chroma; break;
    default: r = chroma; g = 0; b = second; break;
    }
    r = std::clamp(r + base, 0.0, 1.0); g = std::clamp(g + base, 0.0, 1.0); b = std::clamp(b + base, 0.0, 1.0);
}

double adjustedSaturation(double saturation, double amount) {
    amount = std::clamp(amount / 100, -1.0, 1.0);
    if (amount <= 0) return std::max(0.0, saturation * (1 + amount));
    return amount >= 1 ? (saturation > 0 ? 1 : 0) : std::min(1.0, saturation / (1 - amount));
}

std::vector<float> hueSaturationCube(const AdjustmentParams& p, int dimension) {
    // 每度色相的总偏移（各范围按权重叠加）。
    struct Response { double shift = 0, saturation = 0, lightness = 0; };
    std::vector<Response> response(361);
    for (int degree = 0; degree <= 360; ++degree) {
        for (int r = 0; r < 7; ++r) {
            const HueRange& a = p.hueRanges[size_t(r)];
            if (a.hue == 0 && a.saturation == 0 && a.lightness == 0) continue;
            double w = r == 0 ? 1 : p.hueBands[size_t(r)].weight(degree);
            if (p.invertRange && r == p.selectedRange && r != 0) w = 1 - w;
            if (w <= 0) continue;
            response[size_t(degree)].shift += a.hue * w;
            response[size_t(degree)].saturation += a.saturation * w;
            response[size_t(degree)].lightness += a.lightness * w;
        }
    }
    const HueRange& selected = p.hueRanges[size_t(std::clamp(p.selectedRange, 0, 6))];
    std::vector<float> cube(size_t(dimension) * dimension * dimension * 4);
    size_t index = 0;
    double step = dimension - 1;
    for (int bi = 0; bi < dimension; ++bi)
        for (int gi = 0; gi < dimension; ++gi)
            for (int ri = 0; ri < dimension; ++ri) {
                double h, s, l;
                toHSL(ri / step, gi / step, bi / step, h, s, l);
                double lightnessAmount;
                if (p.colorize) {
                    h = std::fmod(selected.hue, 360.0);
                    if (h < 0) h += 360;
                    s = std::clamp(selected.saturation / 100, 0.0, 1.0);
                    lightnessAmount = selected.lightness / 100;
                } else {
                    const Response& sampled = response[size_t(std::clamp(int(std::lround(h)), 0, 360))];
                    lightnessAmount = sampled.lightness / 100;
                    h = std::fmod(h + sampled.shift, 360.0);
                    if (h < 0) h += 360;
                    s = adjustedSaturation(s, sampled.saturation);
                }
                double amount = std::clamp(lightnessAmount, -1.0, 1.0);
                l = amount >= 0 ? l + (1 - l) * amount : l * (1 + amount);
                double r, g, b;
                toRGB(h, s, std::clamp(l, 0.0, 1.0), r, g, b);
                cube[index++] = float(r); cube[index++] = float(g); cube[index++] = float(b); cube[index++] = 1;
            }
    return cube;
}

// 逐行分段并行跑一个按像素处理的内核。
template <typename Kernel>
void bands(Image& image, Kernel kernel) {
    parallelRanges(image.height, [&](int begin, int end) {
        kernel(image.row(begin), size_t(image.width), size_t(end - begin), image.stride(), begin);
    }, 8);
}

void applyTables(Image& image, const std::vector<float>& tables) {
    bands(image, [&](uint8_t* p, size_t w, size_t h, size_t, int) { levels_apply(p, w * h, tables.data()); });
}

// 三次盒式模糊近似高斯（σ），作用于预乘通道，边缘外延。
void boxBlurPass(const uint8_t* src, uint8_t* dst, int width, int height, int radius, bool horizontal) {
    int lines = horizontal ? height : width, length = horizontal ? width : height;
    size_t stepPixel = horizontal ? 4 : size_t(width) * 4, stepLine = horizontal ? size_t(width) * 4 : 4;
    parallelRanges(lines, [&](int begin, int end) {
        for (int line = begin; line < end; ++line) {
            const uint8_t* s = src + size_t(line) * stepLine;
            uint8_t* d = dst + size_t(line) * stepLine;
            unsigned sum[4] = {0, 0, 0, 0};
            auto px = [&](int i) { return s + size_t(std::clamp(i, 0, length - 1)) * stepPixel; };
            for (int i = -radius; i <= radius; ++i) for (int c = 0; c < 4; ++c) sum[c] += px(i)[c];
            unsigned window = unsigned(2 * radius + 1);
            for (int i = 0; i < length; ++i) {
                uint8_t* o = d + size_t(i) * stepPixel;
                for (int c = 0; c < 4; ++c) o[c] = uint8_t((sum[c] + window / 2) / window);
                const uint8_t* add = px(i + radius + 1);
                const uint8_t* remove = px(i - radius);
                for (int c = 0; c < 4; ++c) sum[c] += add[c] - remove[c];
            }
        }
    }, 4);
}

void gaussianBlur(Image& image, double sigma) {
    if (sigma < 0.3 || image.empty()) return;
    // 三个盒子的宽度（Kovesi 方法）。
    double ideal = std::sqrt(12 * sigma * sigma / 3 + 1);
    int wl = int(std::floor(ideal));
    if (wl % 2 == 0) --wl;
    int wu = wl + 2;
    double mIdeal = (12 * sigma * sigma - 3.0 * wl * wl - 4.0 * 3 * wl - 3.0 * 3) / (-4.0 * wl - 4);
    int m = int(std::lround(mIdeal));
    std::vector<uint8_t> temp(image.pixels.size());
    for (int pass = 0; pass < 3; ++pass) {
        int radius = ((pass < m ? wl : wu) - 1) / 2;
        if (radius <= 0) continue;
        boxBlurPass(image.pixels.data(), temp.data(), image.width, image.height, radius, true);
        boxBlurPass(temp.data(), image.pixels.data(), image.width, image.height, radius, false);
    }
}

void motionBlur(Image& image, double angleDegrees, double distance) {
    if (distance < 1 || image.empty()) return;
    Image source = image;
    double radians = angleDegrees * kPi / 180;
    // 角度逆时针为正（Photoshop），图像 y 向下。
    double dx = std::cos(radians), dy = -std::sin(radians);
    int samples = std::clamp(int(std::ceil(distance)) + 1, 2, 257);
    int w = image.width, h = image.height;
    parallelRanges(h, [&](int begin, int end) {
        for (int y = begin; y < end; ++y) {
            for (int x = 0; x < w; ++x) {
                float sum[4] = {0, 0, 0, 0};
                for (int k = 0; k < samples; ++k) {
                    double t = (double(k) / (samples - 1) - 0.5) * distance;
                    double sx = std::clamp(x + dx * t, 0.0, double(w - 1)), sy = std::clamp(y + dy * t, 0.0, double(h - 1));
                    int x0 = int(sx), y0 = int(sy), x1 = std::min(w - 1, x0 + 1), y1 = std::min(h - 1, y0 + 1);
                    float fx = float(sx - x0), fy = float(sy - y0);
                    const uint8_t *a = source.at(x0, y0), *b = source.at(x1, y0), *c = source.at(x0, y1), *d = source.at(x1, y1);
                    for (int ch = 0; ch < 4; ++ch) {
                        float top = a[ch] + (b[ch] - a[ch]) * fx, bottom = c[ch] + (d[ch] - c[ch]) * fx;
                        sum[ch] += top + (bottom - top) * fy;
                    }
                }
                uint8_t* o = image.at(x, y);
                for (int ch = 0; ch < 4; ++ch) o[ch] = uint8_t(std::lround(std::clamp(sum[ch] / samples, 0.0f, 255.0f)));
                if (o[0] > o[3]) o[0] = o[3];
                if (o[1] > o[3]) o[1] = o[3];
                if (o[2] > o[3]) o[2] = o[3];
            }
        }
    }, 4);
}

} // namespace

double curveValue(const std::vector<CurvePoint>& points, double x) {
    if (points.size() < 2) return std::clamp(x, 0.0, 255.0);
    return curveValueImpl(points, x);
}

const char* adjustmentKindName(AdjustmentKind kind) {
    for (const auto& k : kKinds) if (k.kind == kind) return k.name;
    return "Invert";
}
const char* adjustmentKindLabel(AdjustmentKind kind) {
    for (const auto& k : kKinds) if (k.kind == kind) return k.label;
    return "调整";
}
bool adjustmentKindFromName(const std::string& name, AdjustmentKind& out) {
    for (const auto& k : kKinds) if (name == k.name) { out = k.kind; return true; }
    return false;
}
const std::vector<AdjustmentKind>& allAdjustmentKinds() {
    static const std::vector<AdjustmentKind> kinds = [] {
        std::vector<AdjustmentKind> list;
        for (const auto& k : kKinds) list.push_back(k.kind);
        return list;
    }();
    return kinds;
}
bool adjustmentNeedsVersion9(AdjustmentKind kind) {
    return kind == AdjustmentKind::GaussianBlur || kind == AdjustmentKind::MotionBlur || kind == AdjustmentKind::AddNoise;
}

LevelRange LevelRange::normalized() const {
    auto clamp = [](double n, double lo, double hi, double fallback) { return std::isfinite(n) ? std::clamp(n, lo, hi) : fallback; };
    LevelRange r = *this;
    r.black = clamp(black, 0, 254, 0);
    r.white = clamp(white, r.black + 1, 255, 255);
    r.gamma = clamp(gamma, 0.1, 9.99, 1);
    r.outputBlack = clamp(outputBlack, 0, 255, 0);
    r.outputWhite = clamp(outputWhite, 0, 255, 255);
    return r;
}

double LevelRange::apply(double value) const {
    LevelRange s = normalized();
    double input = std::clamp((value * 255 - s.black) / (s.white - s.black), 0.0, 1.0);
    return (s.outputBlack + std::pow(input, 1 / s.gamma) * (s.outputWhite - s.outputBlack)) / 255;
}

double HueBand::weight(double hue) const {
    double span = forward(falloffStart, falloffEnd);
    if (span <= 0) return 1;
    double position = forward(falloffStart, hue);
    if (position > span) return 0;
    double rampIn = forward(falloffStart, rangeStart);
    double plateauEnd = forward(falloffStart, rangeEnd);
    if (position < rampIn) return rampIn > 0 ? position / rampIn : 1;
    if (position <= plateauEnd) return 1;
    double rampOut = span - plateauEnd;
    return rampOut > 0 ? (span - position) / rampOut : 1;
}

bool AdjustmentParams::isValid() const {
    auto finite = [](double v) { return std::isfinite(v); };
    for (const auto& r : hueRanges) {
        if (!finite(r.hue) || std::abs(r.hue) > 360 || !finite(r.saturation) || std::abs(r.saturation) > 100
            || !finite(r.lightness) || std::abs(r.lightness) > 100) return false;
    }
    for (const auto& b : hueBands) {
        for (double v : {b.falloffStart, b.rangeStart, b.rangeEnd, b.falloffEnd}) if (!finite(v)) return false;
    }
    for (const auto& r : levels) if (!(r == r.normalized())) return false;
    for (const auto& c : curves) if (!curveValid(c)) return false;
    if (!(exposure >= -20 && exposure <= 20 && exposureOffset >= -0.5 && exposureOffset <= 0.5
          && exposureGamma >= 0.01 && exposureGamma <= 9.99)) return false;
    for (int i = 0; i < 3; ++i) {
        if (!(shadows[i] >= 0 && shadows[i] <= 1 && highlights[i] >= 0 && highlights[i] <= 1)) return false;
    }
    if (!(grainAmount >= 0 && grainAmount <= 100 && grainSize >= 0.5 && grainSize <= 20
          && grainRoughness >= 0 && grainRoughness <= 100)) return false;
    for (double w : bw) if (!(w >= -200 && w <= 300)) return false;
    if (!(tintHue >= 0 && tintHue <= 360 && tintSaturation >= 0 && tintSaturation <= 100)) return false;
    for (double v : balance) if (!(v >= -100 && v <= 100)) return false;
    return blurRadius >= 0.1 && blurRadius <= 250 && motionAngle >= -90 && motionAngle <= 90
        && motionDistance >= 1 && motionDistance <= 2000 && noiseAmount >= 0.1 && noiseAmount <= 400;
}

AdjustmentParams parseAdjustment(const Json& o) {
    if (!o.is_object()) throw std::runtime_error("调整层不是对象");
    AdjustmentParams p;
    auto kindIt = o.find("kind");
    if (kindIt == o.end() || !kindIt->is_string() || !adjustmentKindFromName(kindIt->get<std::string>(), p.kind)) {
        throw std::runtime_error("未知的调整类型");
    }
    for (int i = 0; i < 7; ++i) p.hueBands[size_t(i)] = kDefaultBands[i];
    for (auto& c : p.curves) c = identityCurve();
    // 色相/饱和度：有 hsvSettings 用它，否则用顶层的全图数值。
    p.hueRanges[0] = {number(o, "hue", 0), number(o, "saturation", 0), number(o, "lightness", 0)};
    p.colorize = boolean(o, "colorize", false);
    auto hsv = o.find("hsvSettings");
    if (hsv != o.end() && !hsv->is_null()) {
        if (!hsv->is_object()) throw std::runtime_error("hsvSettings 格式错误");
        p.colorize = boolean(*hsv, "colorize", false);
        p.invertRange = boolean(*hsv, "invertRange", false);
        auto range = hsv->find("range");
        if (range != hsv->end() && range->is_string()) p.selectedRange = std::max(0, rangeIndex(range->get<std::string>()));
        p.hueRanges = {};
        if (auto a = hsv->find("adjustments"); a != hsv->end()) {
            visitKeyed(*a, [&](const std::string& key, const Json& value) {
                int index = rangeIndex(key);
                if (index < 0 || !value.is_object()) throw std::runtime_error("色相范围错误");
                p.hueRanges[size_t(index)] = {number(value, "hue", 0), number(value, "saturation", 0), number(value, "lightness", 0)};
            });
        }
        if (auto b = hsv->find("bands"); b != hsv->end()) {
            visitKeyed(*b, [&](const std::string& key, const Json& value) {
                int index = rangeIndex(key);
                if (index < 0 || !value.is_object()) throw std::runtime_error("色相带错误");
                HueBand& band = p.hueBands[size_t(index)];
                band.falloffStart = number(value, "falloffStart", band.falloffStart);
                band.rangeStart = number(value, "rangeStart", band.rangeStart);
                band.rangeEnd = number(value, "rangeEnd", band.rangeEnd);
                band.falloffEnd = number(value, "falloffEnd", band.falloffEnd);
            });
        }
    }
    if (auto l = o.find("levels"); l != o.end() && l->is_object()) {
        auto ranges = l->find("ranges");
        if (ranges != l->end()) {
            if (!ranges->is_array() || ranges->size() != 4) throw std::runtime_error("色阶需要 4 个通道");
            for (size_t i = 0; i < 4; ++i) {
                const Json& r = (*ranges)[i];
                LevelRange& t = p.levels[i];
                t.black = number(r, "black", 0); t.gamma = number(r, "gamma", 1); t.white = number(r, "white", 255);
                t.outputBlack = number(r, "outputBlack", 0); t.outputWhite = number(r, "outputWhite", 255);
            }
        }
    }
    if (auto c = o.find("curves"); c != o.end() && c->is_object()) {
        auto channels = c->find("channels");
        if (channels != c->end()) {
            if (!channels->is_array() || channels->size() != 4) throw std::runtime_error("曲线需要 4 个通道");
            for (size_t i = 0; i < 4; ++i) {
                const Json& list = (*channels)[i];
                if (!list.is_array()) throw std::runtime_error("曲线格式错误");
                std::vector<CurvePoint> points;
                for (const auto& point : list) points.push_back({number(point, "x", 0), number(point, "y", 0)});
                p.curves[i] = points;
            }
        }
    }
    if (auto e = o.find("exposureSettings"); e != o.end() && e->is_object()) {
        p.exposure = number(*e, "exposure", 0); p.exposureOffset = number(*e, "offset", 0); p.exposureGamma = number(*e, "gamma", 1);
    }
    if (auto g = o.find("gradientMapSettings"); g != o.end() && g->is_object()) {
        if (auto s = g->find("shadows"); s != g->end() && s->is_object()) readColor(*s, p.shadows);
        if (auto h = g->find("highlights"); h != g->end() && h->is_object()) readColor(*h, p.highlights);
        p.gradientReversed = boolean(*g, "reversed", false);
    }
    if (auto g = o.find("grainSettings"); g != o.end() && g->is_object()) {
        p.grainAmount = number(*g, "amount", 25); p.grainSize = number(*g, "size", 1.5);
        p.grainRoughness = number(*g, "roughness", 50); p.grainSeed = seed(*g, "seed");
    }
    if (auto b = o.find("blackWhiteSettings"); b != o.end() && b->is_object()) {
        const char* keys[6] = {"reds", "yellows", "greens", "cyans", "blues", "magentas"};
        for (int i = 0; i < 6; ++i) p.bw[i] = number(*b, keys[i], p.bw[i]);
        p.tint = boolean(*b, "tint", false);
        p.tintHue = number(*b, "tintHue", 40); p.tintSaturation = number(*b, "tintSaturation", 20);
    }
    if (auto b = o.find("colorBalanceSettings"); b != o.end() && b->is_object()) {
        const char* keys[9] = {"shadowCyanRed", "shadowMagentaGreen", "shadowYellowBlue",
                               "midCyanRed", "midMagentaGreen", "midYellowBlue",
                               "highlightCyanRed", "highlightMagentaGreen", "highlightYellowBlue"};
        for (int i = 0; i < 9; ++i) p.balance[i] = number(*b, keys[i], 0);
        p.preserveLuminosity = boolean(*b, "preserveLuminosity", true);
    }
    p.blurRadius = number(o, "blurRadius", 10);
    p.motionAngle = number(o, "motionAngle", 0);
    p.motionDistance = number(o, "motionDistance", 10);
    p.noiseAmount = number(o, "noiseAmount", 10);
    p.noiseGaussian = boolean(o, "noiseGaussian", false);
    p.noiseMonochromatic = boolean(o, "noiseMonochromatic", false);
    p.noiseSeed = seed(o, "noiseSeed");
    return p;
}

Json makeAdjustment(AdjustmentKind kind, uint32_t randomSeed) {
    AdjustmentParams p;
    p.kind = kind;
    for (auto& c : p.curves) c = identityCurve();
    for (int i = 0; i < 7; ++i) p.hueBands[size_t(i)] = kDefaultBands[i];
    p.grainSeed = randomSeed;
    p.noiseSeed = randomSeed;
    Json o = Json::object();
    o["kind"] = adjustmentKindName(kind);
    writeAdjustment(p, o);
    return o;
}

void writeAdjustment(const AdjustmentParams& p, Json& o) {
    o["kind"] = adjustmentKindName(p.kind);
    // macOS 版解码时这几项必须存在。
    o["hue"] = p.hueRanges[0].hue;
    o["saturation"] = p.hueRanges[0].saturation;
    o["lightness"] = p.hueRanges[0].lightness;
    o["colorize"] = p.colorize;
    std::string levelsChannel = "RGB", curvesChannel = "RGB";
    if (o.contains("levels") && o["levels"].is_object() && o["levels"].contains("channel") && o["levels"]["channel"].is_string()) levelsChannel = o["levels"]["channel"].get<std::string>();
    if (o.contains("curves") && o["curves"].is_object() && o["curves"].contains("channel") && o["curves"]["channel"].is_string()) curvesChannel = o["curves"]["channel"].get<std::string>();
    o["levels"] = levelsJson(p.levels, levelsChannel);
    o["curves"] = curvesJson(p.curves, curvesChannel);
    // 已有 hsvSettings 的（macOS 上编辑过分范围的）同步更新，保持两边一致。
    if (o.contains("hsvSettings") && o["hsvSettings"].is_object()) {
        Json& hsv = o["hsvSettings"];
        hsv["colorize"] = p.colorize;
        hsv["invertRange"] = p.invertRange;
        hsv["range"] = kRangeNames[std::clamp(p.selectedRange, 0, 6)];
        Json adjustments = Json::array();
        Json bands = Json::array();
        for (int i = 0; i < 7; ++i) {
            const HueRange& r = p.hueRanges[size_t(i)];
            if (r.hue != 0 || r.saturation != 0 || r.lightness != 0) {
                adjustments.push_back(kRangeNames[i]);
                adjustments.push_back(Json{{"hue", r.hue}, {"saturation", r.saturation}, {"lightness", r.lightness}});
            }
            const HueBand& b = p.hueBands[size_t(i)];
            bands.push_back(kRangeNames[i]);
            bands.push_back(Json{{"falloffStart", b.falloffStart}, {"rangeStart", b.rangeStart},
                                 {"rangeEnd", b.rangeEnd}, {"falloffEnd", b.falloffEnd}});
        }
        hsv["adjustments"] = adjustments;
        hsv["bands"] = bands;
    }
    switch (p.kind) {
    case AdjustmentKind::Exposure:
        o["exposureSettings"] = Json{{"exposure", p.exposure}, {"offset", p.exposureOffset}, {"gamma", p.exposureGamma}};
        break;
    case AdjustmentKind::GradientMap:
        o["gradientMapSettings"] = Json{{"shadows", colorJson(p.shadows)}, {"highlights", colorJson(p.highlights)},
                                        {"reversed", p.gradientReversed}};
        break;
    case AdjustmentKind::Grain:
        o["grainSettings"] = Json{{"amount", p.grainAmount}, {"size", p.grainSize}, {"roughness", p.grainRoughness},
                                  {"seed", p.grainSeed}};
        break;
    case AdjustmentKind::BlackWhite:
        o["blackWhiteSettings"] = Json{{"reds", p.bw[0]}, {"yellows", p.bw[1]}, {"greens", p.bw[2]}, {"cyans", p.bw[3]},
                                       {"blues", p.bw[4]}, {"magentas", p.bw[5]}, {"tint", p.tint},
                                       {"tintHue", p.tintHue}, {"tintSaturation", p.tintSaturation}};
        break;
    case AdjustmentKind::ColorBalance:
        o["colorBalanceSettings"] = Json{
            {"shadowCyanRed", p.balance[0]}, {"shadowMagentaGreen", p.balance[1]}, {"shadowYellowBlue", p.balance[2]},
            {"midCyanRed", p.balance[3]}, {"midMagentaGreen", p.balance[4]}, {"midYellowBlue", p.balance[5]},
            {"highlightCyanRed", p.balance[6]}, {"highlightMagentaGreen", p.balance[7]}, {"highlightYellowBlue", p.balance[8]},
            {"preserveLuminosity", p.preserveLuminosity}};
        break;
    case AdjustmentKind::GaussianBlur:
        o["blurRadius"] = p.blurRadius;
        break;
    case AdjustmentKind::MotionBlur:
        o["motionAngle"] = p.motionAngle;
        o["motionDistance"] = p.motionDistance;
        break;
    case AdjustmentKind::AddNoise:
        o["noiseAmount"] = p.noiseAmount;
        o["noiseGaussian"] = p.noiseGaussian;
        o["noiseMonochromatic"] = p.noiseMonochromatic;
        o["noiseSeed"] = p.noiseSeed;
        break;
    default:
        break;
    }
}

void applyAdjustment(const AdjustmentParams& p, Image& image, double originX, double originY, double unitsPerPixel) {
    if (image.empty()) return;
    switch (p.kind) {
    case AdjustmentKind::HueSaturation: {
        bool identity = !p.colorize;
        for (const auto& r : p.hueRanges) if (r.hue != 0 || r.saturation != 0 || r.lightness != 0) identity = false;
        if (identity) break;
        const int dimension = 33;
        auto cube = hueSaturationCube(p, dimension);
        bands(image, [&](uint8_t* px, size_t w, size_t h, size_t, int) { cube_apply(px, w * h, cube.data(), dimension); });
        break;
    }
    case AdjustmentKind::Levels: {
        std::vector<float> tables(768);
        for (int channel = 1; channel <= 3; ++channel)
            for (int i = 0; i < 256; ++i)
                tables[size_t((channel - 1) * 256 + i)] = float(p.levels[0].apply(p.levels[size_t(channel)].apply(i / 255.0)));
        applyTables(image, tables);
        break;
    }
    case AdjustmentKind::Curves: {
        for (const auto& c : p.curves) if (!curveValid(c)) return;
        std::vector<float> tables(768);
        for (int channel = 1; channel <= 3; ++channel)
            for (int i = 0; i < 256; ++i)
                tables[size_t((channel - 1) * 256 + i)] =
                    float(curveValueImpl(p.curves[0], curveValueImpl(p.curves[size_t(channel)], i)) / 255);
        applyTables(image, tables);
        break;
    }
    case AdjustmentKind::Exposure: {
        std::vector<float> tables(768);
        double scale = std::pow(2.0, p.exposure);
        for (int i = 0; i < 256; ++i) {
            double encoded = i / 255.0;
            double linear = encoded <= 0.04045 ? encoded / 12.92 : std::pow((encoded + 0.055) / 1.055, 2.4);
            linear = std::pow(std::max(0.0, linear * scale + p.exposureOffset), 1 / p.exposureGamma);
            double output = linear <= 0.0031308 ? linear * 12.92 : 1.055 * std::pow(linear, 1 / 2.4) - 0.055;
            float v = float(std::clamp(output, 0.0, 1.0));
            tables[size_t(i)] = tables[size_t(256 + i)] = tables[size_t(512 + i)] = v;
        }
        applyTables(image, tables);
        break;
    }
    case AdjustmentKind::GradientMap: {
        const double* dark = p.gradientReversed ? p.highlights : p.shadows;
        const double* light = p.gradientReversed ? p.shadows : p.highlights;
        uint8_t table[768];
        for (int i = 0; i < 256; ++i) {
            double t = i / 255.0;
            for (int c = 0; c < 3; ++c)
                table[i * 3 + c] = uint8_t(std::clamp(std::round((dark[c] + (light[c] - dark[c]) * t) * 255), 0.0, 255.0));
        }
        bands(image, [&](uint8_t* px, size_t w, size_t h, size_t stride, int) { adjust_gradient_map(px, w, h, stride, table); });
        break;
    }
    case AdjustmentKind::Grain:
        if (p.grainAmount <= 0) break;
        bands(image, [&](uint8_t* px, size_t w, size_t h, size_t stride, int row) {
            adjust_grain(px, w, h, stride, p.grainAmount, p.grainSize, p.grainRoughness, p.grainSeed,
                         originX, originY + row * unitsPerPixel, unitsPerPixel);
        });
        break;
    case AdjustmentKind::AddNoise: {
        int64_t ox = int64_t(std::llround(originX / unitsPerPixel)), oy = int64_t(std::llround(originY / unitsPerPixel));
        bands(image, [&](uint8_t* px, size_t w, size_t h, size_t stride, int row) {
            noise_add_at(px, w, h, stride, float(p.noiseAmount), p.noiseGaussian, p.noiseMonochromatic, p.noiseSeed, ox, oy + row);
        });
        break;
    }
    case AdjustmentKind::GaussianBlur:
        gaussianBlur(image, p.blurRadius / unitsPerPixel);
        break;
    case AdjustmentKind::MotionBlur:
        motionBlur(image, p.motionAngle, p.motionDistance / unitsPerPixel);
        break;
    case AdjustmentKind::Invert:
        parallelRanges(image.height, [&](int begin, int end) {
            for (int y = begin; y < end; ++y) {
                uint8_t* row = image.row(y);
                for (int x = 0; x < image.width; ++x) {
                    uint8_t* q = row + x * 4;
                    q[0] = uint8_t(q[3] - q[0]); q[1] = uint8_t(q[3] - q[1]); q[2] = uint8_t(q[3] - q[2]);
                }
            }
        });
        break;
    case AdjustmentKind::BlackWhite: {
        float weights[6];
        for (int i = 0; i < 6; ++i) weights[i] = float(p.bw[i] / 100);
        bands(image, [&](uint8_t* px, size_t w, size_t h, size_t stride, int) {
            adjust_black_white(px, w, h, stride, weights, p.tint ? 1 : 0, p.tintHue, p.tintSaturation / 100);
        });
        break;
    }
    case AdjustmentKind::ColorBalance: {
        bool identity = true;
        for (double v : p.balance) if (v != 0) identity = false;
        if (identity) break;
        float values[9];
        for (int i = 0; i < 9; ++i) values[i] = float(p.balance[i] / 100);
        bands(image, [&](uint8_t* px, size_t w, size_t h, size_t stride, int) {
            adjust_color_balance(px, w, h, stride, values, values + 3, values + 6, p.preserveLuminosity ? 1 : 0);
        });
        break;
    }
    }
    image.touch();
}

} // namespace comp
