// 画布：显示合成结果，处理缩放平移与各工具的鼠标操作。
#pragma once
#include "Session.h"
#include "ToolState.h"
#include "compositor/edit.h"
#include "compositor/renderer.h"
#include <QImage>
#include <QPointer>
#include <QWidget>
#include <memory>
#include <optional>

class CanvasView : public QWidget {
    Q_OBJECT
public:
    CanvasView(Session* session, ToolState* tools, QWidget* parent = nullptr);
    Session* session() const { return session_; }

    double zoom() const { return zoom_; }
    void setZoom(double zoom, QPointF anchor = QPointF(-1, -1));
    void zoomIn();
    void zoomOut();
    void fitToWindow();
    void actualPixels();
    void nudge(int dx, int dy);
    // 文档坐标的合成颜色（直通 RGBA）。
    bool sampleColor(QPointF documentPoint, QColor& color);
    QPointF toDocument(QPointF widgetPoint) const;
    QPointF toWidget(QPointF documentPoint) const;
    void setShowGuides(bool show) { showGuides_ = show; update(); }
    void setShowPixelGrid(bool show) { showPixelGrid_ = show; update(); }

signals:
    void zoomChanged(double zoom);
    void cursorMoved(QPointF documentPoint, bool inside);
    void statusMessage(const QString& text);
    void colorPicked(const QColor& color, bool background);

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void showEvent(QShowEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void keyReleaseEvent(QKeyEvent*) override;
    void leaveEvent(QEvent*) override;

private:
    enum class DragMode { None, Pan, Paint, Move, Resize, Rotate };
    void invalidate();
    void renderAll();
    void renderRegion(const QRectF& documentRect);
    void updateCursor(QPointF widgetPoint = QPointF(-1, -1));
    Tool effectiveTool() const;
    // 自由变换手柄：返回命中的手柄（0–7），旋转区返回 8，框内 9，都不是返回 -1。
    int hitTransform(QPointF widgetPoint) const;
    const comp::Layer* transformTarget() const;
    void beginPaint(QPointF documentPoint, bool shift);
    comp::LayerTransform dragged(const comp::LayerTransform& original, QPointF point, Qt::KeyboardModifiers modifiers) const;

    Session* session_;
    ToolState* tools_;
    comp::Renderer renderer_;
    double zoom_ = 1;
    QPointF pan_;                 // 文档原点在控件中的位置（逻辑像素）
    QImage composite_;            // 视口大小的合成结果（物理像素）
    bool needsRender_ = true;
    bool fitted_ = false;
    bool spaceHeld_ = false;
    bool showGuides_ = true;
    bool showPixelGrid_ = true;
    DragMode drag_ = DragMode::None;
    QPointF dragStartWidget_, dragStartDocument_, lastWidget_;
    QPointF panStart_;
    int handle_ = -1;
    comp::LayerTransform originalTransform_;
    std::vector<std::pair<std::string, comp::LayerTransform>> movedLayers_; // 移动文件夹时它的内容
    std::unique_ptr<comp::BrushStroke> stroke_;
    std::optional<QPointF> lastPaintPoint_;
    QPointF hover_ = QPointF(-1, -1);
    QPixmap checker_;
};
