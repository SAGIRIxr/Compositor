// 曲线编辑：点击添加控制点，拖动调整，拖出区域删除。
#pragma once
#include "compositor/adjustment.h"
#include <QWidget>
#include <vector>

class CurveEditor : public QWidget {
    Q_OBJECT
public:
    explicit CurveEditor(QWidget* parent = nullptr);
    void setPoints(const std::vector<comp::CurvePoint>& points);
    const std::vector<comp::CurvePoint>& points() const { return points_; }
    void setCurveColor(const QColor& color) { color_ = color; update(); }
    QSize sizeHint() const override { return QSize(280, 280); }

signals:
    void pointsChanged();

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;

private:
    QRectF area() const;
    QPointF toWidget(const comp::CurvePoint& p) const;
    comp::CurvePoint toCurve(QPointF p) const;
    std::vector<comp::CurvePoint> points_;
    int dragging_ = -1;
    bool removed_ = false;
    QColor color_ = Qt::white;
};
