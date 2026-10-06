// 图层变换：未旋转的边界（文档像素）加上绕中心顺时针的旋转与翻转，与 macOS 版完全一致。
#pragma once
#include <cmath>
#include <string>

namespace comp {

constexpr double kPi = 3.14159265358979323846;

// 2D 仿射变换：x' = a*x + c*y + tx，y' = b*x + d*y + ty。
struct Affine {
    double a = 1, b = 0, c = 0, d = 1, tx = 0, ty = 0;
    void apply(double x, double y, double& ox, double& oy) const {
        ox = a * x + c * y + tx;
        oy = b * x + d * y + ty;
    }
    Affine inverted() const;
    // 先 this 再 next。
    Affine then(const Affine& next) const;
    static Affine scale(double sx, double sy) { return {sx, 0, 0, sy, 0, 0}; }
    static Affine translate(double x, double y) { return {1, 0, 0, 1, x, y}; }
};

enum class Sampling { Nearest, Smooth, High };
const char* samplingName(Sampling sampling);
bool samplingFromName(const std::string& name, Sampling& out);

struct LayerTransform {
    double x = 0, y = 0;          // 左上角
    double width = 1, height = 1;  // 未旋转尺寸
    double rotation = 0;           // 角度，顺时针
    bool flipX = false, flipY = false;
    Sampling sampling = Sampling::High;

    double centerX() const { return x + width / 2; }
    double centerY() const { return y + height / 2; }
    double radians() const { return std::fmod(rotation, 360.0) * kPi / 180.0; }
    bool isValid() const;
    // 单位正方形（0…1，y 向下）映射到文档坐标；包含翻转。
    Affine unitToDocument() const;
    // 文档上一点是否落在变换后的矩形里。
    bool contains(double px, double py) const;
    // 变换后矩形在文档中的外接框。
    void bounds(double& minX, double& minY, double& maxX, double& maxY) const;
    // 只比较位置，不比较采样方式。
    bool samePlacement(const LayerTransform& other) const;
    // 整像素、整角度。
    LayerTransform rounded() const;

    static LayerTransform canvas(int width, int height) {
        LayerTransform t; t.width = width; t.height = height; return t;
    }
};

} // namespace comp
