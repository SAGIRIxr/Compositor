#include "CanvasView.h"
#include "QtBridge.h"
#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>
#include <cmath>
#include <iterator>

using namespace comp;

namespace {

const QPointF kHandles[8] = {{0, 0}, {0.5, 0}, {1, 0}, {1, 0.5}, {1, 1}, {0.5, 1}, {0, 1}, {0, 0.5}};

// 不含翻转的单位点位置（与 macOS 版 LayerTransform.point 相同），用于手柄。
QPointF unitPoint(const LayerTransform& t, QPointF unit) {
    double x = (unit.x() - 0.5) * t.width, y = (unit.y() - 0.5) * t.height, r = t.radians();
    return {t.centerX() + x * std::cos(r) - y * std::sin(r), t.centerY() + x * std::sin(r) + y * std::cos(r)};
}

QRectF boundsOf(const LayerTransform& t) {
    double a, b, c, d;
    t.bounds(a, b, c, d);
    return QRectF(QPointF(a, b), QPointF(c, d));
}

const double kZoomSteps[] = {0.01, 0.02, 0.03, 0.05, 0.0833, 0.125, 0.1667, 0.25, 0.3333, 0.5, 0.6667,
                             1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 24, 32, 64};

} // namespace

CanvasView::CanvasView(Session* session, ToolState* tools, QWidget* parent)
    : QWidget(parent), session_(session), tools_(tools) {
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMinimumSize(200, 200);
    checker_ = QPixmap(16, 16);
    checker_.fill(QColor(255, 255, 255));
    {
        QPainter p(&checker_);
        p.fillRect(0, 0, 8, 8, QColor(204, 204, 204));
        p.fillRect(8, 8, 8, 8, QColor(204, 204, 204));
    }
    connect(session_, &Session::documentChanged, this, [this](bool) { invalidate(); });
    connect(session_, &Session::pixelsChanged, this, [this](QRectF rect) { renderRegion(rect); update(); });
    connect(session_, &Session::activeLayerChanged, this, [this] { update(); });
    connect(tools_, &ToolState::changed, this, [this] { updateCursor(); update(); });
}

void CanvasView::invalidate() {
    needsRender_ = true;
    update();
}

QPointF CanvasView::toDocument(QPointF p) const { return (p - pan_) / zoom_; }
QPointF CanvasView::toWidget(QPointF p) const { return p * zoom_ + pan_; }

void CanvasView::setZoom(double zoom, QPointF anchor) {
    zoom = std::clamp(zoom, 0.01, 64.0);
    if (anchor.x() < 0) anchor = QPointF(width() / 2.0, height() / 2.0);
    QPointF documentPoint = toDocument(anchor);
    zoom_ = zoom;
    pan_ = anchor - documentPoint * zoom_;
    // 缩放后对齐到物理像素，1:1 时像素不发虚。
    double dpr = devicePixelRatioF();
    pan_ = QPointF(std::round(pan_.x() * dpr) / dpr, std::round(pan_.y() * dpr) / dpr);
    emit zoomChanged(zoom_);
    invalidate();
}

void CanvasView::zoomIn() {
    for (double step : kZoomSteps) if (step > zoom_ * 1.001) { setZoom(step); return; }
}

void CanvasView::zoomOut() {
    for (int i = int(std::size(kZoomSteps)) - 1; i >= 0; --i) if (kZoomSteps[i] < zoom_ * 0.999) { setZoom(kZoomSteps[i]); return; }
}

void CanvasView::fitToWindow() {
    const Document& doc = session_->doc();
    double margin = 24;
    double fit = std::min((width() - margin * 2) / doc.width, (height() - margin * 2) / doc.height);
    zoom_ = std::clamp(fit, 0.01, 64.0);
    pan_ = QPointF((width() - doc.width * zoom_) / 2, (height() - doc.height * zoom_) / 2);
    double dpr = devicePixelRatioF();
    pan_ = QPointF(std::round(pan_.x() * dpr) / dpr, std::round(pan_.y() * dpr) / dpr);
    fitted_ = true;
    emit zoomChanged(zoom_);
    invalidate();
}

void CanvasView::actualPixels() {
    // 100% 指一个文档像素对应一个物理像素。
    setZoom(1.0 / devicePixelRatioF());
}

void CanvasView::showEvent(QShowEvent*) {
    if (!fitted_) fitToWindow();
}

void CanvasView::resizeEvent(QResizeEvent*) {
    if (!fitted_ && isVisible()) fitToWindow();
    invalidate();
}

void CanvasView::renderAll() {
    double dpr = devicePixelRatioF();
    RenderOptions options;
    options.width = std::max(1, int(std::ceil(width() * dpr)));
    options.height = std::max(1, int(std::ceil(height() * dpr)));
    options.scale = zoom_ * dpr;
    options.offsetX = pan_.x() * dpr;
    options.offsetY = pan_.y() * dpr;
    Image image = renderer_.render(session_->doc(), options);
    composite_ = toQImage(image);
    composite_.setDevicePixelRatio(dpr);
    needsRender_ = false;
}

void CanvasView::renderRegion(const QRectF& documentRect) {
    if (needsRender_ || composite_.isNull()) return; // 下次绘制时整体渲染
    double dpr = devicePixelRatioF();
    QRectF widgetRect(toWidget(documentRect.topLeft()), toWidget(documentRect.bottomRight()));
    QRect device = QRectF(widgetRect.topLeft() * dpr, widgetRect.bottomRight() * dpr).toAlignedRect().adjusted(-2, -2, 2, 2)
        .intersected(QRect(0, 0, composite_.width(), composite_.height()));
    if (device.isEmpty()) return;
    RenderOptions options;
    options.width = device.width();
    options.height = device.height();
    options.scale = zoom_ * dpr;
    options.offsetX = pan_.x() * dpr - device.x();
    options.offsetY = pan_.y() * dpr - device.y();
    Image patch = renderer_.render(session_->doc(), options);
    QPainter painter(&composite_);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    QImage patchImage = wrapImage(patch);
    painter.drawImage(QPointF(device.x() / dpr, device.y() / dpr),
                      [&] { QImage copy = patchImage.copy(); copy.setDevicePixelRatio(dpr); return copy; }());
}

void CanvasView::paintEvent(QPaintEvent*) {
    if (needsRender_) renderAll();
    QPainter painter(this);
    painter.fillRect(rect(), QColor(40, 40, 40));
    const Document& doc = session_->doc();
    QRectF canvas(pan_, QSizeF(doc.width * zoom_, doc.height * zoom_));
    painter.save();
    painter.setClipRect(canvas);
    painter.setBrushOrigin(pan_);
    painter.fillRect(canvas, QBrush(checker_));
    painter.restore();
    painter.drawImage(QPointF(0, 0), composite_);
    painter.setPen(QPen(QColor(0, 0, 0, 120), 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(canvas.adjusted(-0.5, -0.5, 0.5, 0.5));

    // 放大到 8 倍以上显示像素网格。
    if (showPixelGrid_ && zoom_ >= 8) {
        painter.setPen(QPen(QColor(128, 128, 128, 70), 0));
        QRectF visible = QRectF(rect()).intersected(canvas);
        QPointF a = toDocument(visible.topLeft()), b = toDocument(visible.bottomRight());
        for (int x = int(std::floor(a.x())); x <= int(std::ceil(b.x())); ++x) {
            double wx = toWidget(QPointF(x, 0)).x();
            painter.drawLine(QPointF(wx, visible.top()), QPointF(wx, visible.bottom()));
        }
        for (int y = int(std::floor(a.y())); y <= int(std::ceil(b.y())); ++y) {
            double wy = toWidget(QPointF(0, y)).y();
            painter.drawLine(QPointF(visible.left(), wy), QPointF(visible.right(), wy));
        }
    }
    if (showGuides_) {
        painter.setPen(QPen(QColor(0, 200, 255), 0));
        for (const Guide& g : doc.guides) {
            if (g.horizontal) { double y = toWidget(QPointF(0, g.position)).y(); painter.drawLine(QPointF(0, y), QPointF(width(), y)); }
            else { double x = toWidget(QPointF(g.position, 0)).x(); painter.drawLine(QPointF(x, 0), QPointF(x, height())); }
        }
    }
    // 移动工具：画出选中图层的变换框与手柄。
    if (effectiveTool() == Tool::Move) {
        if (const Layer* layer = transformTarget()) {
            painter.setRenderHint(QPainter::Antialiasing);
            QPolygonF box;
            for (QPointF corner : {QPointF(0, 0), QPointF(1, 0), QPointF(1, 1), QPointF(0, 1)}) box << toWidget(unitPoint(layer->transform, corner));
            painter.setPen(QPen(QColor(30, 144, 255), 1));
            painter.drawPolygon(box);
            if (!layer->isGroup) {
                painter.setBrush(Qt::white);
                for (QPointF h : kHandles) {
                    QPointF c = toWidget(unitPoint(layer->transform, h));
                    painter.drawRect(QRectF(c.x() - 4, c.y() - 4, 8, 8));
                }
            }
        }
    }
    // 画笔光标：笔刷大小的圆。
    Tool tool = effectiveTool();
    if ((tool == Tool::Brush || tool == Tool::Eraser) && hover_.x() >= 0) {
        painter.setRenderHint(QPainter::Antialiasing);
        double r = tools_->brush.size / 2 * zoom_;
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(QColor(0, 0, 0, 160), 1.5));
        painter.drawEllipse(hover_, r, r);
        painter.setPen(QPen(QColor(255, 255, 255, 200), 0.75));
        painter.drawEllipse(hover_, r, r);
        if (r < 3) {
            painter.drawLine(hover_ - QPointF(5, 0), hover_ + QPointF(5, 0));
            painter.drawLine(hover_ - QPointF(0, 5), hover_ + QPointF(0, 5));
        }
    }
}

Tool CanvasView::effectiveTool() const {
    if (spaceHeld_) return Tool::Hand;
    return tools_->tool;
}

const Layer* CanvasView::transformTarget() const {
    const Layer* layer = session_->doc().find(session_->doc().activeLayerID);
    if (!layer || layer->isAdjustment()) return nullptr;
    return layer;
}

int CanvasView::hitTransform(QPointF p) const {
    const Layer* layer = transformTarget();
    if (!layer) return -1;
    if (!layer->isGroup) {
        for (int i = 0; i < 8; ++i) {
            QPointF c = toWidget(unitPoint(layer->transform, kHandles[i]));
            if (std::abs(c.x() - p.x()) <= 6 && std::abs(c.y() - p.y()) <= 6) return i;
        }
    }
    QPointF d = toDocument(p);
    if (layer->transform.contains(d.x(), d.y())) return 9;
    if (!layer->isGroup) {
        // 角外 24 像素以内旋转。
        for (int i : {0, 2, 4, 6}) {
            QPointF c = toWidget(unitPoint(layer->transform, kHandles[i]));
            if (QLineF(c, p).length() <= 24) return 8;
        }
    }
    return -1;
}

void CanvasView::updateCursor(QPointF p) {
    if (p.x() < 0) p = mapFromGlobal(QCursor::pos());
    switch (effectiveTool()) {
    case Tool::Hand: setCursor(drag_ == DragMode::Pan ? Qt::ClosedHandCursor : Qt::OpenHandCursor); return;
    case Tool::Brush:
    case Tool::Eraser: setCursor(Qt::CrossCursor); return;
    case Tool::Eyedropper: setCursor(Qt::CrossCursor); return;
    case Tool::Zoom: setCursor(Qt::PointingHandCursor); return;
    case Tool::Move: {
        int hit = drag_ == DragMode::None ? hitTransform(p) : handle_;
        if (drag_ == DragMode::Rotate || hit == 8) setCursor(Qt::CrossCursor);
        else if (hit >= 0 && hit < 8) setCursor(hit % 4 == 0 ? Qt::SizeFDiagCursor : hit % 4 == 2 ? Qt::SizeBDiagCursor
                                                : hit % 4 == 1 ? Qt::SizeVerCursor : Qt::SizeHorCursor);
        else setCursor(Qt::SizeAllCursor);
        return;
    }
    }
}

bool CanvasView::sampleColor(QPointF d, QColor& color) {
    const Document& doc = session_->doc();
    int x = int(std::floor(d.x())), y = int(std::floor(d.y()));
    if (x < 0 || y < 0 || x >= doc.width || y >= doc.height) return false;
    RenderOptions options;
    options.width = options.height = 1;
    options.offsetX = -x;
    options.offsetY = -y;
    Image pixel = renderer_.render(doc, options);
    uint8_t rgba[4];
    if (!sampleComposite(pixel, 0, 0, rgba)) return false;
    color = QColor(rgba[0], rgba[1], rgba[2], rgba[3]);
    return true;
}

void CanvasView::beginPaint(QPointF d, bool shift) {
    Document& doc = session_->doc();
    Layer* layer = doc.find(doc.activeLayerID);
    if (!layer) { emit statusMessage(QStringLiteral("请先选择一个图层。")); return; }
    bool onMask = tools_->paintOnMask && layer->mask;
    if (!onMask && (layer->isGroup || layer->isAdjustment())) {
        emit statusMessage(QStringLiteral("文件夹和调整层没有像素可画，请先给它添加蒙版并切换到“绘制到蒙版”。"));
        return;
    }
    if (!doc.isEffectivelyVisible(*layer)) { emit statusMessage(QStringLiteral("图层已隐藏，无法在上面绘画。")); return; }
    bool erase = effectiveTool() == Tool::Eraser;
    QColor c = tools_->foreground;
    uint8_t color[4] = {uint8_t(c.red()), uint8_t(c.green()), uint8_t(c.blue()), 255};
    if (onMask) color[0] = color[1] = color[2] = uint8_t(qGray(c.rgb()));
    session_->checkpoint(erase ? QStringLiteral("橡皮擦") : QStringLiteral("画笔"));
    stroke_ = std::make_unique<BrushStroke>(doc, layer->id, tools_->brush, color, erase, onMask);
    if (!stroke_->isValid()) { stroke_.reset(); return; }
    drag_ = DragMode::Paint;
    // Shift 点击：从上一笔的终点画直线。
    if (shift && lastPaintPoint_) stroke_->addPoint(lastPaintPoint_->x(), lastPaintPoint_->y());
    stroke_->addPoint(d.x(), d.y());
    lastPaintPoint_ = d;
    double x, y, w, h;
    stroke_->dirtyRect(x, y, w, h);
    session_->notifyPixels(QRectF(x, y, w, h));
}

void CanvasView::mousePressEvent(QMouseEvent* event) {
    setFocus();
    QPointF p = event->position();
    QPointF d = toDocument(p);
    dragStartWidget_ = lastWidget_ = p;
    dragStartDocument_ = d;
    if (event->button() == Qt::MiddleButton || (event->button() == Qt::LeftButton && effectiveTool() == Tool::Hand)) {
        drag_ = DragMode::Pan;
        panStart_ = pan_;
        updateCursor(p);
        return;
    }
    if (event->button() != Qt::LeftButton) return;
    Document& doc = session_->doc();
    switch (effectiveTool()) {
    case Tool::Zoom:
        setZoom(event->modifiers() & Qt::AltModifier ? zoom_ / 2 : zoom_ * 2, p);
        return;
    case Tool::Eyedropper: {
        QColor color;
        if (sampleColor(d, color)) emit colorPicked(color, event->modifiers() & Qt::AltModifier);
        return;
    }
    case Tool::Brush:
    case Tool::Eraser:
        beginPaint(d, event->modifiers() & Qt::ShiftModifier);
        return;
    case Tool::Move: {
        const Layer* layer = transformTarget();
        if (!layer) { emit statusMessage(QStringLiteral("请选择要移动的图层。")); return; }
        int hit = hitTransform(p);
        handle_ = hit;
        originalTransform_ = layer->transform;
        movedLayers_.clear();
        if (layer->isGroup) {
            // 文件夹：连同内容一起移动。
            int index = doc.indexOf(layer->id), first, last;
            doc.subtreeRange(index, first, last);
            for (int i = first; i <= last; ++i) movedLayers_.push_back({doc.layers[size_t(i)].id, doc.layers[size_t(i)].transform});
            drag_ = DragMode::Move;
            session_->checkpoint(QStringLiteral("移动"));
            return;
        }
        if (hit >= 0 && hit < 8) { drag_ = DragMode::Resize; session_->checkpoint(QStringLiteral("缩放")); }
        else if (hit == 8) { drag_ = DragMode::Rotate; session_->checkpoint(QStringLiteral("旋转")); }
        else { drag_ = DragMode::Move; session_->checkpoint(QStringLiteral("移动")); }
        return;
    }
    case Tool::Hand:
        return;
    }
}

LayerTransform CanvasView::dragged(const LayerTransform& original, QPointF point, Qt::KeyboardModifiers modifiers) const {
    // 移植自 macOS 版 TransformDrag.updated。
    LayerTransform result = original;
    bool shift = modifiers & Qt::ShiftModifier, option = modifiers & Qt::AltModifier;
    QPointF start = dragStartDocument_;
    if (drag_ == DragMode::Move) {
        double dx = point.x() - start.x(), dy = point.y() - start.y();
        if (shift) { if (std::abs(dx) >= std::abs(dy)) dy = 0; else dx = 0; }
        result.x += dx; result.y += dy;
    } else if (drag_ == DragMode::Rotate) {
        double cx = original.centerX(), cy = original.centerY();
        double delta = std::atan2(point.y() - cy, point.x() - cx) - std::atan2(start.y() - cy, start.x() - cx);
        result.rotation += delta * 180 / kPi;
        if (shift) result.rotation = std::round(result.rotation / 15) * 15;
    } else if (drag_ == DragMode::Resize && handle_ >= 0 && handle_ < 8) {
        QPointF handle = kHandles[handle_];
        QPointF anchorUnit = option ? QPointF(0.5, 0.5) : QPointF(1 - handle.x(), 1 - handle.y());
        QPointF anchor = unitPoint(original, anchorUnit);
        QPointF initialHandle = unitPoint(original, handle);
        double dx = initialHandle.x() + point.x() - start.x() - anchor.x();
        double dy = initialHandle.y() + point.y() - start.y() - anchor.y();
        double span = option ? 2 : 1, r = original.radians();
        double localX = (dx * std::cos(r) + dy * std::sin(r)) * span;
        double localY = (-dx * std::sin(r) + dy * std::cos(r)) * span;
        double sx = handle.x() * 2 - 1, sy = handle.y() * 2 - 1;
        double rawWidth = sx == 0 ? original.width : localX * sx;
        double rawHeight = sy == 0 ? original.height : localY * sy;
        bool mirroredX = rawWidth < 0, mirroredY = rawHeight < 0;
        double w = std::max(1.0, std::abs(rawWidth)), h = std::max(1.0, std::abs(rawHeight));
        if (shift) {
            double factor;
            if (sx == 0) factor = h / original.height;
            else if (sy == 0) factor = w / original.width;
            else factor = std::max(1 / std::min(original.width, original.height),
                (localX * sx * original.width + localY * sy * original.height)
                / (original.width * original.width + original.height * original.height));
            w = original.width * factor;
            h = original.height * factor;
        }
        result.width = w; result.height = h;
        if (mirroredX) result.flipX = !result.flipX;
        if (mirroredY) result.flipY = !result.flipY;
        double offsetX = (0.5 - anchorUnit.x()) * w * (mirroredX ? -1 : 1);
        double offsetY = (0.5 - anchorUnit.y()) * h * (mirroredY ? -1 : 1);
        double cx = anchor.x() + offsetX * std::cos(r) - offsetY * std::sin(r);
        double cy = anchor.y() + offsetX * std::sin(r) + offsetY * std::cos(r);
        result.x = cx - w / 2; result.y = cy - h / 2;
    }
    // 拖动留下整像素与整角度（与 macOS 版一致）。
    result = result.rounded();
    return result.isValid() ? result : original;
}

void CanvasView::mouseMoveEvent(QMouseEvent* event) {
    QPointF p = event->position();
    QPointF d = toDocument(p);
    const Document& doc = session_->doc();
    emit cursorMoved(d, d.x() >= 0 && d.y() >= 0 && d.x() < doc.width && d.y() < doc.height);
    QPointF oldHover = hover_;
    hover_ = p;
    switch (drag_) {
    case DragMode::Pan:
        pan_ = panStart_ + (p - dragStartWidget_);
        invalidate();
        break;
    case DragMode::Paint:
        if (stroke_) {
            stroke_->addPoint(d.x(), d.y());
            lastPaintPoint_ = d;
            double lx = std::min(d.x(), toDocument(lastWidget_).x()), ly = std::min(d.y(), toDocument(lastWidget_).y());
            double r = tools_->brush.size / 2 + 2;
            QRectF segment(QPointF(lx - r, ly - r), QPointF(std::max(d.x(), toDocument(lastWidget_).x()) + r,
                                                           std::max(d.y(), toDocument(lastWidget_).y()) + r));
            // 变换过的图层：脏区按图层像素到文档的映射放大，简单起见整笔外接框。
            const Layer* layer = doc.find(doc.activeLayerID);
            if (layer && (layer->transform.rotation != 0 || layer->transform.flipX || layer->transform.flipY)) {
                double x, y, w, h;
                stroke_->dirtyRect(x, y, w, h);
                segment = QRectF(x, y, w, h);
            }
            session_->notifyPixels(segment);
        }
        update();
        break;
    case DragMode::Move:
    case DragMode::Resize:
    case DragMode::Rotate: {
        Document& mutableDoc = session_->doc();
        if (!movedLayers_.empty()) {
            double dx = std::round(d.x() - dragStartDocument_.x()), dy = std::round(d.y() - dragStartDocument_.y());
            if (event->modifiers() & Qt::ShiftModifier) { if (std::abs(dx) >= std::abs(dy)) dy = 0; else dx = 0; }
            for (auto& [id, t] : movedLayers_) {
                if (Layer* l = mutableDoc.find(id)) { l->transform = t; l->transform.x += dx; l->transform.y += dy; }
            }
        } else if (Layer* layer = mutableDoc.find(mutableDoc.activeLayerID)) {
            LayerTransform before = layer->transform;
            layer->transform = dragged(originalTransform_, d, event->modifiers());
            // 链接的蒙版跟着图层；解除链接的蒙版保持原位。
            (void)before;
        }
        session_->notifyChanged(false);
        break;
    }
    case DragMode::None:
        if (effectiveTool() == Tool::Move) updateCursor(p);
        if (effectiveTool() == Tool::Brush || effectiveTool() == Tool::Eraser) {
            double r = tools_->brush.size / 2 * zoom_ + 4;
            update(QRectF(oldHover.x() - r, oldHover.y() - r, 2 * r, 2 * r).toAlignedRect());
            update(QRectF(p.x() - r, p.y() - r, 2 * r, 2 * r).toAlignedRect());
        }
        break;
    }
    lastWidget_ = p;
}

void CanvasView::mouseReleaseEvent(QMouseEvent*) {
    if (drag_ == DragMode::Paint) {
        stroke_.reset();
        session_->notifyChanged(false); // 刷新缩略图
    }
    if (drag_ == DragMode::Move || drag_ == DragMode::Resize || drag_ == DragMode::Rotate) session_->notifyChanged(false);
    drag_ = DragMode::None;
    movedLayers_.clear();
    handle_ = -1;
    updateCursor();
}

void CanvasView::mouseDoubleClickEvent(QMouseEvent* event) {
    if (effectiveTool() == Tool::Hand) fitToWindow();
    else if (effectiveTool() == Tool::Zoom) actualPixels();
    else QWidget::mouseDoubleClickEvent(event);
}

void CanvasView::wheelEvent(QWheelEvent* event) {
    QPointF delta = event->pixelDelta().isNull() ? QPointF(event->angleDelta()) / 120.0 * 40 : QPointF(event->pixelDelta());
    if (event->modifiers() & (Qt::ControlModifier | Qt::AltModifier)) {
        double steps = event->angleDelta().y() / 120.0;
        if (steps == 0) steps = event->angleDelta().x() / 120.0;
        setZoom(zoom_ * std::pow(1.25, steps), event->position());
    } else {
        if (event->modifiers() & Qt::ShiftModifier) delta = QPointF(delta.y(), delta.x());
        pan_ += delta;
        invalidate();
    }
    event->accept();
}

void CanvasView::nudge(int dx, int dy) {
    Document& doc = session_->doc();
    const Layer* layer = transformTarget();
    if (!layer) return;
    session_->edit(QStringLiteral("移动"), [&](Document& d) {
        int index = d.indexOf(layer->id), first, last;
        d.subtreeRange(index, first, last);
        for (int i = first; i <= last; ++i) { d.layers[size_t(i)].transform.x += dx; d.layers[size_t(i)].transform.y += dy; }
    }, false);
    (void)doc;
}

void CanvasView::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Space && !event->isAutoRepeat()) {
        spaceHeld_ = true;
        updateCursor();
        update();
        return;
    }
    int step = event->modifiers() & Qt::ShiftModifier ? 10 : 1;
    if (tools_->tool == Tool::Move && drag_ == DragMode::None) {
        switch (event->key()) {
        case Qt::Key_Left: nudge(-step, 0); return;
        case Qt::Key_Right: nudge(step, 0); return;
        case Qt::Key_Up: nudge(0, -step); return;
        case Qt::Key_Down: nudge(0, step); return;
        default: break;
        }
    }
    QWidget::keyPressEvent(event);
}

void CanvasView::keyReleaseEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Space && !event->isAutoRepeat()) {
        spaceHeld_ = false;
        updateCursor();
        update();
        return;
    }
    QWidget::keyReleaseEvent(event);
}

void CanvasView::leaveEvent(QEvent*) {
    hover_ = QPointF(-1, -1);
    update();
}
