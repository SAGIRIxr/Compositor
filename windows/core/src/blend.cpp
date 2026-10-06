#include "compositor/blend.h"
#include <algorithm>
#include <cmath>

namespace comp {

static const char* const kBlendNames[kBlendModeCount] = {
    "Normal",
    "Darken", "Multiply", "Color Burn", "Linear Burn",
    "Lighten", "Screen", "Color Dodge", "Linear Dodge (Add)",
    "Overlay", "Soft Light", "Hard Light", "Vivid Light", "Linear Light", "Pin Light", "Hard Mix",
    "Difference", "Exclusion", "Subtract", "Divide",
    "Hue", "Saturation", "Color", "Luminosity",
};

const char* blendModeName(BlendMode mode) {
    int index = int(mode);
    return index >= 0 && index < kBlendModeCount ? kBlendNames[index] : "Normal";
}

bool blendModeFromName(const std::string& name, BlendMode& out) {
    for (int i = 0; i < kBlendModeCount; ++i) {
        if (name == kBlendNames[i]) { out = BlendMode(i); return true; }
    }
    return false;
}

const std::vector<std::vector<BlendMode>>& blendModeGroups() {
    using B = BlendMode;
    static const std::vector<std::vector<BlendMode>> groups = {
        {B::Normal},
        {B::Darken, B::Multiply, B::ColorBurn, B::LinearBurn},
        {B::Lighten, B::Screen, B::ColorDodge, B::LinearDodge},
        {B::Overlay, B::SoftLight, B::HardLight, B::VividLight, B::LinearLight, B::PinLight, B::HardMix},
        {B::Difference, B::Exclusion, B::Subtract, B::Divide},
        {B::Hue, B::Saturation, B::Color, B::Luminosity},
    };
    return groups;
}

namespace {

inline float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

inline float colorDodge(float b, float s) {
    if (b <= 0) return 0;
    if (s >= 1) return 1;
    return std::min(1.0f, b / (1 - s));
}
inline float colorBurn(float b, float s) {
    if (b >= 1) return 1;
    if (s <= 0) return 0;
    return 1 - std::min(1.0f, (1 - b) / s);
}
inline float hardLight(float b, float s) {
    return s <= 0.5f ? b * 2 * s : b + (2 * s - 1) - b * (2 * s - 1);
}

// 可分离模式：b 为底色，s 为混合色，均为直通 0–1。
inline float separable(BlendMode mode, float b, float s) {
    switch (mode) {
    case BlendMode::Normal: return s;
    case BlendMode::Darken: return std::min(b, s);
    case BlendMode::Multiply: return b * s;
    case BlendMode::ColorBurn: return colorBurn(b, s);
    case BlendMode::LinearBurn: return std::max(0.0f, b + s - 1);
    case BlendMode::Lighten: return std::max(b, s);
    case BlendMode::Screen: return b + s - b * s;
    case BlendMode::ColorDodge: return colorDodge(b, s);
    case BlendMode::LinearDodge: return std::min(1.0f, b + s);
    case BlendMode::Overlay: return hardLight(s, b);
    case BlendMode::SoftLight:
        // Photoshop 的柔光公式。
        return s <= 0.5f ? 2 * b * s + b * b * (1 - 2 * s) : 2 * b * (1 - s) + std::sqrt(b) * (2 * s - 1);
    case BlendMode::HardLight: return hardLight(b, s);
    case BlendMode::VividLight: return s <= 0.5f ? colorBurn(b, 2 * s) : colorDodge(b, 2 * (s - 0.5f));
    case BlendMode::LinearLight: return clamp01(b + 2 * s - 1);
    case BlendMode::PinLight: return s <= 0.5f ? std::min(b, 2 * s) : std::max(b, 2 * s - 1);
    case BlendMode::HardMix: return b + s >= 1 ? 1.0f : 0.0f;
    case BlendMode::Difference: return std::abs(b - s);
    case BlendMode::Exclusion: return b + s - 2 * b * s;
    case BlendMode::Subtract: return std::max(0.0f, b - s);
    case BlendMode::Divide: return s <= 0 ? (b > 0 ? 1.0f : 0.0f) : std::min(1.0f, b / s);
    default: return s;
    }
}

inline float lum(const float c[3]) { return 0.3f * c[0] + 0.59f * c[1] + 0.11f * c[2]; }

void clipColor(float c[3]) {
    float l = lum(c);
    float n = std::min({c[0], c[1], c[2]}), x = std::max({c[0], c[1], c[2]});
    if (n < 0) for (int i = 0; i < 3; ++i) c[i] = l + (c[i] - l) * l / (l - n);
    if (x > 1) for (int i = 0; i < 3; ++i) c[i] = l + (c[i] - l) * (1 - l) / (x - l);
}

void setLum(const float c[3], float l, float out[3]) {
    float d = l - lum(c);
    for (int i = 0; i < 3; ++i) out[i] = c[i] + d;
    clipColor(out);
}

float sat(const float c[3]) { return std::max({c[0], c[1], c[2]}) - std::min({c[0], c[1], c[2]}); }

void setSat(const float c[3], float s, float out[3]) {
    int maxI = 0, minI = 0;
    for (int i = 1; i < 3; ++i) { if (c[i] > c[maxI]) maxI = i; if (c[i] < c[minI]) minI = i; }
    if (maxI == minI) { out[0] = out[1] = out[2] = 0; return; }
    int midI = 3 - maxI - minI;
    float range = c[maxI] - c[minI];
    out[midI] = (c[midI] - c[minI]) * s / range;
    out[maxI] = s;
    out[minI] = 0;
}

} // namespace

void blendPixel(BlendMode mode, const float src[4], float dst[4]) {
    float as = src[3];
    if (as <= 0) return;
    float ab = dst[3];
    if (mode == BlendMode::Normal || ab <= 0) {
        for (int c = 0; c < 4; ++c) dst[c] = src[c] + dst[c] * (1 - as);
        return;
    }
    float cs[3], cb[3];
    for (int c = 0; c < 3; ++c) {
        cs[c] = clamp01(src[c] / as);
        cb[c] = clamp01(dst[c] / ab);
    }
    float mixed[3];
    switch (mode) {
    case BlendMode::Hue: { float t[3]; setSat(cs, sat(cb), t); setLum(t, lum(cb), mixed); break; }
    case BlendMode::Saturation: { float t[3]; setSat(cb, sat(cs), t); setLum(t, lum(cb), mixed); break; }
    case BlendMode::Color: setLum(cs, lum(cb), mixed); break;
    case BlendMode::Luminosity: setLum(cb, lum(cs), mixed); break;
    default:
        for (int c = 0; c < 3; ++c) mixed[c] = separable(mode, cb[c], cs[c]);
    }
    // W3C：co = cs·(1 − ab) + cb·(1 − as) + as·ab·B(Cb, Cs)
    for (int c = 0; c < 3; ++c) dst[c] = src[c] * (1 - ab) + dst[c] * (1 - as) + as * ab * clamp01(mixed[c]);
    dst[3] = as + ab * (1 - as);
}

} // namespace comp
