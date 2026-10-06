#include "CurveEditor.h"
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <cmath>

using comp::CurvePoint;

CurveEditor::CurveEditor(QWidget* parent) : QWidget(parent) {
    setMinimumSize(220, 220);
    setMouseTracking(true);
    points_ = {{0, 0}, {255, 255}};
}

void CurveEditor::setPoints(const std::vector<CurvePoint>& points) {
    points_ = points.size() >= 2 ? points : std::vector<CurvePoint>{{0, 0}, {255, 255}};
    update();
}

QRectF CurveEditor::area() const {
    double side = std::min(width(), height()) - 16;
    return QRectF((width() - side) / 2, (height() - side) / 2, side, side);
}

QPointF CurveEditor::toWidget(const CurvePoint& p) const {
    QRectF a = area();
    return {a.left() + p.x / 255 * a.width(), a.bottom() - p.y / 255 * a.height()};
}

CurvePoint CurveEditor::toCurve(QPointF p) const {
    QRectF a = area();
    return {std::clamp((p.x() - a.left()) / a.width() * 255, 0.0, 255.0),
            std::clamp((a.bottom() - p.y()) / a.height() * 255, 0.0, 255.0)};
}

void CurveEditor::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    QRectF a = area();
    p.fillRect(a, QColor(30, 30, 30));
    p.setPen(QPen(QColor(80, 80, 80), 0));
    for (int i = 1; i < 4; ++i) {
        double x = a.left() + a.width() * i / 4, y = a.top() + a.height() * i / 4;
        p.drawLine(QPointF(x, a.top()), QPointF(x, a.bottom()));
        p.drawLine(QPointF(a.left(), y), QPointF(a.right(), y));
    }
    p.drawLine(a.bottomLeft(), a.topRight());
    QPainterPath path;
    for (int x = 0; x <= 255; ++x) {
        QPointF point = toWidget({double(x), comp::curveValue(points_, x)});
        if (x == 0) path.moveTo(point); else path.lineTo(point);
    }
    p.setPen(QPen(color_, 2));
    p.drawPath(path);
    p.setBrush(color_);
    p.setPen(QPen(Qt::black, 1));
    for (size_t i = 0; i < points_.size(); ++i) {
        p.drawEllipse(toWidget(points_[i]), int(i) == dragging_ ? 5.0 : 4.0, int(i) == dragging_ ? 5.0 : 4.0);
    }
    p.setPen(QColor(160, 160, 160));
    p.setBrush(Qt::NoBrush);
    p.drawRect(a);
}

void CurveEditor::mousePressEvent(QMouseEvent* event) {
    QPointF pos = event->position();
    dragging_ = -1;
    for (size_t i = 0; i < points_.size(); ++i) {
        if (QLineF(toWidget(points_[i]), pos).length() <= 8) { dragging_ = int(i); break; }
    }
    if (dragging_ < 0 && points_.size() < 32) {
        CurvePoint c = toCurve(pos);
        if (c.x <= 0 || c.x >= 255) return;
        auto it = std::lower_bound(points_.begin(), points_.end(), c.x, [](const CurvePoint& p, double x) { return p.x < x; });
        if (it != points_.end() && std::abs(it->x - c.x) < 1) return;
        dragging_ = int(it - points_.begin());
        points_.insert(it, c);
        emit pointsChanged();
    }
    removed_ = false;
    update();
}

void CurveEditor::mouseMoveEvent(QMouseEvent* event) {
    if (dragging_ < 0 || !(event->buttons() & Qt::LeftButton)) return;
    QPointF pos = event->position();
    QRectF a = area();
    bool endpoint = dragging_ == 0 || dragging_ == int(points_.size()) - 1;
    // 中间点拖出区域外就删掉。
    if (!endpoint && !a.adjusted(-24, -24, 24, 24).contains(pos)) {
        if (!removed_) {
            points_.erase(points_.begin() + dragging_);
            removed_ = true;
            dragging_ = -1;
            emit pointsChanged();
            update();
        }
        return;
    }
    CurvePoint c = toCurve(pos);
    if (dragging_ == 0) c.x = 0;
    else if (dragging_ == int(points_.size()) - 1) c.x = 255;
    else c.x = std::clamp(c.x, points_[size_t(dragging_ - 1)].x + 1, points_[size_t(dragging_ + 1)].x - 1);
    c.x = std::round(c.x);
    c.y = std::round(c.y);
    points_[size_t(dragging_)] = c;
    emit pointsChanged();
    update();
}

void CurveEditor::mouseReleaseEvent(QMouseEvent*) {
    dragging_ = -1;
    update();
}
