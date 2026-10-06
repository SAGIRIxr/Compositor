#include "compositor/transform.h"
#include <algorithm>

namespace comp {

Affine Affine::inverted() const {
    double det = a * d - b * c;
    if (det == 0) return {};
    Affine r;
    r.a = d / det; r.b = -b / det; r.c = -c / det; r.d = a / det;
    r.tx = -(r.a * tx + r.c * ty);
    r.ty = -(r.b * tx + r.d * ty);
    return r;
}

Affine Affine::then(const Affine& n) const {
    Affine r;
    r.a = n.a * a + n.c * b;
    r.b = n.b * a + n.d * b;
    r.c = n.a * c + n.c * d;
    r.d = n.b * c + n.d * d;
    r.tx = n.a * tx + n.c * ty + n.tx;
    r.ty = n.b * tx + n.d * ty + n.ty;
    return r;
}

const char* samplingName(Sampling sampling) {
    switch (sampling) {
    case Sampling::Nearest: return "Nearest";
    case Sampling::Smooth: return "Smooth";
    case Sampling::High: return "High quality";
    }
    return "High quality";
}

bool samplingFromName(const std::string& name, Sampling& out) {
    if (name == "Nearest") { out = Sampling::Nearest; return true; }
    if (name == "Smooth") { out = Sampling::Smooth; return true; }
    if (name == "High quality") { out = Sampling::High; return true; }
    return false;
}

bool LayerTransform::isValid() const {
    for (double v : {x, y, width, height, rotation}) if (!std::isfinite(v)) return false;
    return width >= 1 && width <= 300000 && height >= 1 && height <= 300000
        && std::abs(x) <= 1000000 && std::abs(y) <= 1000000;
}

Affine LayerTransform::unitToDocument() const {
    // 单位方块 → 以中心为原点的图层尺寸（含翻转）→ 旋转 → 平移到中心。
    double fx = flipX ? -1 : 1, fy = flipY ? -1 : 1;
    Affine local{width * fx, 0, 0, height * fy, -0.5 * width * fx, -0.5 * height * fy};
    double r = radians(), cs = std::cos(r), sn = std::sin(r);
    Affine rotate{cs, sn, -sn, cs, centerX(), centerY()};
    return local.then(rotate);
}

bool LayerTransform::contains(double px, double py) const {
    double dx = px - centerX(), dy = py - centerY(), r = radians();
    return std::abs(dx * std::cos(r) + dy * std::sin(r)) <= width / 2
        && std::abs(-dx * std::sin(r) + dy * std::cos(r)) <= height / 2;
}

void LayerTransform::bounds(double& minX, double& minY, double& maxX, double& maxY) const {
    Affine m = unitToDocument();
    minX = minY = 1e300; maxX = maxY = -1e300;
    for (double u : {0.0, 1.0}) for (double v : {0.0, 1.0}) {
        double px, py; m.apply(u, v, px, py);
        minX = std::min(minX, px); maxX = std::max(maxX, px);
        minY = std::min(minY, py); maxY = std::max(maxY, py);
    }
}

bool LayerTransform::samePlacement(const LayerTransform& o) const {
    return x == o.x && y == o.y && width == o.width && height == o.height && rotation == o.rotation
        && flipX == o.flipX && flipY == o.flipY;
}

LayerTransform LayerTransform::rounded() const {
    LayerTransform r = *this;
    r.x = std::round(x); r.y = std::round(y);
    r.width = std::max(1.0, std::round(width)); r.height = std::max(1.0, std::round(height));
    r.rotation = std::round(rotation);
    return r;
}

} // namespace comp
