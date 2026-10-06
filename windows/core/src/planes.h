// 浮点平面上的通用运算（内部使用）：盒式近似高斯模糊、窗口最大/最小值。
#pragma once
#include <vector>

namespace comp::detail {

using Plane = std::vector<float>;

// 三次盒式模糊近似高斯（σ），边缘外延。
void blurPlane(Plane& plane, int width, int height, double sigma);
// 方形窗口内的最大值（smallest 为假）或最小值；越界处按 0 计。
Plane extremePlane(const Plane& source, int width, int height, int reach, bool smallest);

} // namespace comp::detail
