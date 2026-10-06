// 调整层。清单里的 adjustment 对象原样保存在 JSON 中（保证往返不丢字段），渲染时解析成参数。
#pragma once
#include "compositor/image.h"
#include "compositor/json.h"
#include <array>
#include <string>
#include <vector>

namespace comp {

enum class AdjustmentKind {
    HueSaturation, Levels, Curves, Exposure, GradientMap, Grain, AddNoise,
    GaussianBlur, MotionBlur, Invert, BlackWhite, ColorBalance,
};

const char* adjustmentKindName(AdjustmentKind kind);   // 清单里的名字，如 "Hue/Saturation"
const char* adjustmentKindLabel(AdjustmentKind kind);  // 界面显示的中文名
bool adjustmentKindFromName(const std::string& name, AdjustmentKind& out);
const std::vector<AdjustmentKind>& allAdjustmentKinds();
// 版本 9 才有的三种（需要采样邻近像素）。
bool adjustmentNeedsVersion9(AdjustmentKind kind);

// 一个通道的色阶。
struct LevelRange {
    double black = 0, gamma = 1, white = 255, outputBlack = 0, outputWhite = 255;
    LevelRange normalized() const;
    double apply(double value) const; // value 0–1
    bool operator==(const LevelRange& o) const {
        return black == o.black && gamma == o.gamma && white == o.white && outputBlack == o.outputBlack && outputWhite == o.outputWhite;
    }
};

struct CurvePoint { double x = 0, y = 0; };

struct HueRange { double hue = 0, saturation = 0, lightness = 0; };
struct HueBand { double falloffStart = 0, rangeStart = 0, rangeEnd = 360, falloffEnd = 360; double weight(double hue) const; };

// 解析后的参数，默认值与 macOS 版相同。
struct AdjustmentParams {
    AdjustmentKind kind = AdjustmentKind::Invert;
    // 色相/饱和度：七个范围（0 为全图，1–6 为红黄绿青蓝洋红）。
    bool colorize = false;
    bool invertRange = false;
    int selectedRange = 0;
    std::array<HueRange, 7> hueRanges{};
    std::array<HueBand, 7> hueBands{};
    // 色阶与曲线：RGB、红、绿、蓝。
    std::array<LevelRange, 4> levels{};
    std::array<std::vector<CurvePoint>, 4> curves{};
    // 曝光度
    double exposure = 0, exposureOffset = 0, exposureGamma = 1;
    // 渐变映射（直通 sRGB 0–1）
    double shadows[3] = {0, 0, 0}, highlights[3] = {1, 1, 1};
    bool gradientReversed = false;
    // 颗粒
    double grainAmount = 25, grainSize = 1.5, grainRoughness = 50;
    uint32_t grainSeed = 0;
    // 黑白：红、黄、绿、青、蓝、洋红
    double bw[6] = {40, 60, 40, 60, 20, 80};
    bool tint = false;
    double tintHue = 40, tintSaturation = 20;
    // 色彩平衡：阴影、中间调、高光 × 青红、洋红绿、黄蓝
    double balance[9] = {};
    bool preserveLuminosity = true;
    // 模糊与杂色
    double blurRadius = 10, motionAngle = 0, motionDistance = 10, noiseAmount = 10;
    bool noiseGaussian = false, noiseMonochromatic = false;
    uint32_t noiseSeed = 0;

    bool isValid() const;
};

// 曲线在 x（0–255）处的值：保形三次 Hermite 插值，与 macOS 版相同。
double curveValue(const std::vector<CurvePoint>& points, double x);

// 从清单 JSON 解析；出错时抛出 std::runtime_error。
AdjustmentParams parseAdjustment(const Json& object);
// 新建一个该类型的调整对象（含 macOS 版解码所需的全部必填字段）。
Json makeAdjustment(AdjustmentKind kind, uint32_t seed = 0);
// 把编辑后的参数写回 JSON（保留未识别的字段）。
void writeAdjustment(const AdjustmentParams& params, Json& object);

// 对预乘 RGBA 图像应用调整，就地修改。
// originX/originY/unitsPerPixel 把图像像素放回文档坐标，使颗粒与杂色的图案固定在文档上；
// scale 是图像像素与文档像素之比（缩小预览时模糊半径随之缩小）。
void applyAdjustment(const AdjustmentParams& params, Image& image,
                     double originX = 0, double originY = 0, double unitsPerPixel = 1);

} // namespace comp
