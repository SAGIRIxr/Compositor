// 图层效果：描边、投影、颜色叠加、内阴影、外发光、内发光。
// 与 macOS 版 LayerEffectsRenderer 相同：在图层自己的像素网格里计算，四周留出边距，
// 再把放大后的结果按同比例放大的变换放回画布。参数原样保存在清单的 effects 对象里。
#pragma once
#include "compositor/image.h"
#include "compositor/json.h"
#include "compositor/transform.h"
#include <memory>
#include <string>
#include <vector>

namespace comp {

enum class EffectKind { Stroke, Shadow, ColorOverlay, InnerShadow, OuterGlow, InnerGlow };

const std::vector<EffectKind>& allEffectKinds();
const char* effectKey(EffectKind kind);    // 清单里的键名，如 "shadow"
const char* effectLabel(EffectKind kind);  // 界面显示的中文名

struct EffectParams {
    bool present = false;
    bool enabled = true;
    double size = 0;      // 描边、发光
    double angle = 90;    // 阴影：光源方向，逆时针度数，90 为正上方
    double distance = 0;
    double blur = 0;
    double color[3] = {0, 0, 0};
    double opacity = 1;
    bool inside = false;  // 描边在边缘内侧
};

struct LayerEffectsParams {
    EffectParams effects[6];
    EffectParams& operator[](EffectKind kind) { return effects[int(kind)]; }
    const EffectParams& operator[](EffectKind kind) const { return effects[int(kind)]; }
    bool anyVisible() const;
    bool isValid() const;
};

// 解析清单的 effects 对象；缺少的效果 present 为 false。格式不对时抛出 std::runtime_error。
LayerEffectsParams parseEffects(const Json& object);
// 新建一个效果时的默认值（与 macOS 版相同；描边与叠加用 color 指定的颜色）。
EffectParams defaultEffect(EffectKind kind, const double color[3]);
// 写回 JSON：保留未识别的字段，没有的效果删掉，macOS 版解码需要的字段都写全。
void writeEffects(const LayerEffectsParams& params, Json& object);

// 四周需要的边距（图层像素）。
int effectsMargin(const LayerEffectsParams& params);

struct EffectsImage {
    std::shared_ptr<Image> image; // 比图层像素大 inset×2
    int inset = 0;
};
// shown 是图层显示出来的样子（像素已乘上蒙版）。没有可见效果时 image 为空。
EffectsImage renderEffects(const Image& shown, const LayerEffectsParams& params);
// 图层变换按效果图的尺寸同比例放大，让原像素仍落在原处。
LayerTransform grownTransform(const LayerTransform& transform, int width, int height, int inset);

} // namespace comp
