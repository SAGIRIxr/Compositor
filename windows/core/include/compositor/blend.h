// Photoshop 顺序的 24 种混合模式。名字与 macOS 版清单里的拼写完全一致。
#pragma once
#include <string>
#include <vector>

namespace comp {

enum class BlendMode {
    Normal,
    Darken, Multiply, ColorBurn, LinearBurn,
    Lighten, Screen, ColorDodge, LinearDodge,
    Overlay, SoftLight, HardLight, VividLight, LinearLight, PinLight, HardMix,
    Difference, Exclusion, Subtract, Divide,
    Hue, Saturation, Color, Luminosity,
};

constexpr int kBlendModeCount = 24;
const char* blendModeName(BlendMode mode);
bool blendModeFromName(const std::string& name, BlendMode& out);
// 菜单分组（组与组之间画分隔线）。
const std::vector<std::vector<BlendMode>>& blendModeGroups();

// 把预乘颜色 src（0–1，含 alpha）按 mode 混到预乘 dst 上，结果写回 dst。
// 遵循 W3C 合成规范的通用公式：可分离模式逐通道、Hue/Saturation/Color/Luminosity 整体计算。
void blendPixel(BlendMode mode, const float src[4], float dst[4]);

} // namespace comp
