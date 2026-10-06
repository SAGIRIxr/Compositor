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
#include <cstdlib>

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
    connect(tools_, &ToolState::changed, this, [this] {
        if (tools_->tool != Tool::Lasso || !tools_->lassoPolygon) cancelPolygon();
        updateCursor();
        update();
    });
    antsTimer_.setInterval(180);
    connect(&antsTimer_, &QTimer::timeout, this, [this] { antsOffset_ = (antsOffset_ + 1) % 8; update(); });
}

void CanvasView::invalidate() {
    needsRender_ = true;
    update();
}

QPointF CanvasView::toDocument(QPointF p) const { return (p - pan_) / zoom_; }
QPointF CanvasView::toWidget(QPointF p) const { return p * zoom_ + pan_; }

void CanvasView::setZoom(double zoom, QPointF anchor) {
    autoFit_ = false;
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
    autoFit_ = true;
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
    if ((!fitted_ || autoFit_) && isVisible()) fitToWindow();
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
    drawSelection(painter);
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
    if (tool == Tool::Clone && tools_->cloneSource) {
        // 仿制取样点的十字：对齐模式下跟着笔走。
        QPointF sample = *tools_->cloneSource;
        if (hover_.x() >= 0 && tools_->cloneAligned && tools_->cloneOffset) sample = toDocument(hover_) + *tools_->cloneOffset;
        QPointF c = toWidget(sample);
        painter.setRenderHint(QPainter::Antialiasing);
        for (QColor color : {QColor(0, 0, 0, 180), QColor(255, 255, 255, 220)}) {
            painter.setPen(QPen(color, color.red() ? 1 : 3));
            painter.drawLine(c - QPointF(8, 0), c + QPointF(8, 0));
            painter.drawLine(c - QPointF(0, 8), c + QPointF(0, 8));
        }
    }
    if (drag_ == DragMode::Gradient) {
        painter.setRenderHint(QPainter::Antialiasing);
        QPointF a = toWidget(dragStartDocument_), b = toWidget(gradientEnd_);
        painter.setPen(QPen(QColor(0, 0, 0, 160), 3));
        painter.drawLine(a, b);
        painter.setPen(QPen(Qt::white, 1));
        painter.drawLine(a, b);
        painter.setBrush(Qt::white);
        painter.drawEllipse(a, 3, 3);
        painter.drawEllipse(b, 3, 3);
    }
    if ((tool == Tool::Brush || tool == Tool::Eraser || tool == Tool::Heal || tool == Tool::Clone) && hover_.x() >= 0) {
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
    case Tool::Eraser:
    case Tool::Heal:
    case Tool::Clone:
    case Tool::Gradient: setCursor(Qt::CrossCursor); return;
    case Tool::Eyedropper:
    case Tool::Marquee:
    case Tool::Lasso:
    case Tool::Wand: setCursor(Qt::CrossCursor); return;
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
    Tool tool = effectiveTool();
    bool pixelsOnly = tool == Tool::Heal || tool == Tool::Clone;
    bool onMask = !pixelsOnly && tools_->paintOnMask && layer->mask;
    if (!onMask && (layer->isGroup || layer->isAdjustment())) {
        emit statusMessage(QStringLiteral("文件夹和调整层没有像素可画，请先给它添加蒙版并切换到“绘制到蒙版”。"));
        return;
    }
    if (pixelsOnly && !layer->image) { emit statusMessage(QStringLiteral("这个图层还没有像素可以修复或仿制。")); return; }
    if (!doc.isEffectivelyVisible(*layer)) { emit statusMessage(QStringLiteral("图层已隐藏，无法在上面绘画。")); return; }
    StrokeOptions options;
    QString name = QStringLiteral("画笔");
    switch (tool) {
    case Tool::Eraser: options.kind = StrokeKind::Erase; name = QStringLiteral("橡皮擦"); break;
    case Tool::Heal:
        options.kind = StrokeKind::Heal;
        options.healMode = tools_->healMode;
        options.seed = uint32_t(std::rand());
        name = QStringLiteral("污点修复");
        break;
    case Tool::Clone: {
        if (!tools_->cloneSource) { emit statusMessage(QStringLiteral("先按住 Alt 点击图像，设置仿制的取样点。")); return; }
        options.kind = StrokeKind::Clone;
        QPointF offset = (tools_->cloneAligned && tools_->cloneOffset) ? *tools_->cloneOffset
            : QPointF(std::round(tools_->cloneSource->x() - d.x()), std::round(tools_->cloneSource->y() - d.y()));
        if (tools_->cloneAligned) tools_->cloneOffset = offset;
        options.cloneOffsetX = offset.x();
        options.cloneOffsetY = offset.y();
        if (tools_->cloneAllLayers) options.cloneSource = std::make_shared<Image>(flatten(doc));
        name = QStringLiteral("仿制图章");
        break;
    }
    default: break;
    }
    QColor c = tools_->foreground;
    uint8_t color[4] = {uint8_t(c.red()), uint8_t(c.green()), uint8_t(c.blue()), 255};
    if (onMask) color[0] = color[1] = color[2] = uint8_t(qGray(c.rgb()));
    session_->checkpoint(name);
    stroke_ = std::make_unique<BrushStroke>(doc, layer->id, tools_->brush, color, options, onMask);
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

void CanvasView::updateGradient(QPointF d, Qt::KeyboardModifiers modifiers) {
    QPointF start = dragStartDocument_;
    if (modifiers & Qt::ShiftModifier) {
        // 约束到 45° 的倍数。
        double angle = std::atan2(d.y() - start.y(), d.x() - start.x());
        double snapped = std::round(angle / (kPi / 4)) * (kPi / 4);
        double length = QLineF(start, d).length();
        d = start + QPointF(std::cos(snapped) * length, std::sin(snapped) * length);
    }
    gradientEnd_ = d;
    if (QLineF(start, d).length() < 0.5) return;
    Document& doc = session_->doc();
    if (!gradientStarted_) {
        session_->checkpoint(QStringLiteral("渐变"));
        gradientStarted_ = true;
    }
    QColor fg = tools_->foreground, bg = tools_->background;
    uint8_t from[4] = {uint8_t(fg.red()), uint8_t(fg.green()), uint8_t(fg.blue()), 255};
    uint8_t to[4] = {uint8_t(bg.red()), uint8_t(bg.green()), uint8_t(bg.blue()), 255};
    if (tools_->gradientToTransparent) { std::copy(from, from + 3, to); to[3] = 0; }
    if (gradientOnMask_) {
        from[0] = uint8_t(qGray(fg.rgb()));
        if (!tools_->gradientToTransparent) to[0] = uint8_t(qGray(bg.rgb()));
        else to[0] = from[0];
    }
    if (tools_->gradientReversed) for (int k = 0; k < 4; ++k) std::swap(from[k], to[k]);
    GradientSettings settings;
    settings.radial = tools_->gradientRadial;
    settings.opacity = tools_->gradientOpacity;
    drawGradient(doc, doc.activeLayerID, start.x(), start.y(), d.x(), d.y(), from, to, settings, gradientOnMask_,
                 gradientImage_, gradientMask_);
    session_->notifyChanged(false);
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
    case Tool::Clone:
        if (event->modifiers() & Qt::AltModifier) {
            tools_->cloneSource = d;
            tools_->cloneOffset.reset();
            emit statusMessage(QStringLiteral("仿制取样点：%1, %2").arg(int(d.x())).arg(int(d.y())));
            update();
            return;
        }
        beginPaint(d, event->modifiers() & Qt::ShiftModifier);
        return;
    case Tool::Brush:
    case Tool::Eraser:
    case Tool::Heal:
        beginPaint(d, event->modifiers() & Qt::ShiftModifier);
        return;
    case Tool::Gradient: {
        const Layer* layer = doc.find(doc.activeLayerID);
        if (!layer) { emit statusMessage(QStringLiteral("请先选择一个图层。")); return; }
        gradientOnMask_ = tools_->paintOnMask && layer->mask;
        if (!gradientOnMask_ && (layer->isGroup || layer->isAdjustment())) {
            emit statusMessage(QStringLiteral("渐变要画在像素图层上，或者给它添加蒙版后画在蒙版上。"));
            return;
        }
        gradientImage_ = layer->image;
        if (!gradientImage_ && !gradientOnMask_) {
            // 空白图层：每次都从同一张透明底图重画，不在上一次的预览上叠加。
            gradientImage_ = std::make_shared<Image>(std::clamp(int(std::lround(layer->transform.width)), 1, kMaxSide),
                                                     std::clamp(int(std::lround(layer->transform.height)), 1, kMaxSide));
        }
        gradientMask_ = layer->mask;
        gradientStarted_ = false;
        gradientEnd_ = d;
        drag_ = DragMode::Gradient;
        return;
    }
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
    case Tool::Marquee:
        drag_ = DragMode::Marquee;
        selectMode_ = modeFor(event->modifiers());
        pressModifiers_ = event->modifiers();
        return;
    case Tool::Lasso:
        if (tools_->lassoPolygon) {
            if (!polygonActive_) {
                polygonActive_ = true;
                selectMode_ = modeFor(event->modifiers());
                lassoPoints_ = {d};
            } else if (lassoPoints_.size() >= 3 && QLineF(toWidget(lassoPoints_.front()), p).length() <= 6) {
                closePolygon();
            } else {
                lassoPoints_.push_back(d);
            }
            update();
            return;
        }
        drag_ = DragMode::Lasso;
        selectMode_ = modeFor(event->modifiers());
        lassoPoints_ = {d};
        return;
    case Tool::Wand:
        wandAt(d, modeFor(event->modifiers()));
        return;
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
        autoFit_ = false;
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
    case DragMode::Marquee:
        update();
        break;
    case DragMode::Gradient:
        updateGradient(d, event->modifiers());
        update();
        break;
    case DragMode::Lasso:
        if (lassoPoints_.empty() || QLineF(toWidget(lassoPoints_.back()), p).length() >= 1.5) lassoPoints_.push_back(d);
        update();
        break;
    case DragMode::None:
        if (polygonActive_) update();
        if (effectiveTool() == Tool::Move) updateCursor(p);
        if (effectiveTool() == Tool::Clone && tools_->cloneSource) update();
        else if (effectiveTool() == Tool::Brush || effectiveTool() == Tool::Eraser || effectiveTool() == Tool::Heal) {
            double r = tools_->brush.size / 2 * zoom_ + 4;
            update(QRectF(oldHover.x() - r, oldHover.y() - r, 2 * r, 2 * r).toAlignedRect());
            update(QRectF(p.x() - r, p.y() - r, 2 * r, 2 * r).toAlignedRect());
        }
        break;
    }
    lastWidget_ = p;
}

void CanvasView::mouseReleaseEvent(QMouseEvent* event) {
    if (drag_ == DragMode::Marquee) {
        QRectF rect = marqueeRect(toDocument(event->position()), event->modifiers());
        drag_ = DragMode::None;
        const Document& doc = session_->doc();
        if (rect.width() < 1 || rect.height() < 1) {
            // 单击：替换模式下取消选择。
            if (selectMode_ == SelectionMode::Replace && doc.selection) {
                session_->edit(QStringLiteral("取消选择"), [](Document& d) { d.selection.reset(); }, false);
            }
        } else {
            commitSelection(rectSelection(doc.width, doc.height, rect.x(), rect.y(), rect.width(), rect.height(), tools_->marqueeEllipse),
                            selectMode_, tools_->marqueeEllipse ? QStringLiteral("椭圆选框") : QStringLiteral("矩形选框"));
        }
        update();
        return;
    }
    if (drag_ == DragMode::Lasso) {
        drag_ = DragMode::None;
        if (lassoPoints_.size() >= 3) {
            std::vector<StrokePoint> points;
            for (QPointF q : lassoPoints_) points.push_back({q.x(), q.y()});
            const Document& doc = session_->doc();
            commitSelection(polygonSelection(doc.width, doc.height, points), selectMode_, QStringLiteral("套索"));
        }
        lassoPoints_.clear();
        update();
        return;
    }
    if (drag_ == DragMode::Gradient) {
        drag_ = DragMode::None;
        gradientImage_.reset();
        gradientMask_.reset();
        update();
        return;
    }
    if (drag_ == DragMode::Paint) {
        if (stroke_) {
            QApplication::setOverrideCursor(Qt::WaitCursor);
            bool ok = stroke_->finish();
            QApplication::restoreOverrideCursor();
            if (!ok) emit statusMessage(QStringLiteral("修复失败：内存不足。"));
        }
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
    if (polygonActive_) { closePolygon(); return; }
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
        autoFit_ = false;
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
    if (polygonActive_) {
        switch (event->key()) {
        case Qt::Key_Return:
        case Qt::Key_Enter: closePolygon(); return;
        case Qt::Key_Escape: cancelPolygon(); return;
        case Qt::Key_Backspace:
            if (lassoPoints_.size() > 1) lassoPoints_.pop_back(); else cancelPolygon();
            update();
            return;
        default: break;
        }
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

SelectionMode CanvasView::modeFor(Qt::KeyboardModifiers m) {
    bool shift = m & Qt::ShiftModifier, alt = m & Qt::AltModifier;
    if (shift && alt) return SelectionMode::Intersect;
    if (shift) return SelectionMode::Add;
    if (alt) return SelectionMode::Subtract;
    return SelectionMode::Replace;
}

QRectF CanvasView::marqueeRect(QPointF current, Qt::KeyboardModifiers m) const {
    QPointF start = dragStartDocument_;
    double dx = current.x() - start.x(), dy = current.y() - start.y();
    // 按下时没用来选模式的修饰键，拖动中才起作用：Shift 正方形/正圆，Alt 从中心画。
    if ((m & Qt::ShiftModifier) && !(pressModifiers_ & Qt::ShiftModifier)) {
        double side = std::max(std::abs(dx), std::abs(dy));
        dx = dx < 0 ? -side : side;
        dy = dy < 0 ? -side : side;
    }
    QRectF rect;
    if ((m & Qt::AltModifier) && !(pressModifiers_ & Qt::AltModifier)) rect = QRectF(start - QPointF(dx, dy), start + QPointF(dx, dy));
    else rect = QRectF(start, start + QPointF(dx, dy));
    rect = rect.normalized();
    // 对齐到整像素，选区边缘清晰。
    return QRectF(QPointF(std::round(rect.left()), std::round(rect.top())), QPointF(std::round(rect.right()), std::round(rect.bottom())));
}

void CanvasView::commitSelection(const GrayImage& shape, SelectionMode mode, const QString& name) {
    const Document& doc = session_->doc();
    GrayImage combined = combineSelection(doc.selection.get(), shape, mode);
    bool empty = selectionIsEmpty(combined);
    if (empty && !doc.selection) return;
    auto result = empty ? nullptr : std::make_shared<GrayImage>(std::move(combined));
    session_->edit(name, [&](Document& d) { d.selection = result; }, false);
}

void CanvasView::wandAt(QPointF d, SelectionMode mode) {
    const Document& doc = session_->doc();
    int x = int(std::floor(d.x())), y = int(std::floor(d.y()));
    if (x < 0 || y < 0 || x >= doc.width || y >= doc.height) return;
    const Layer* layer = doc.find(doc.activeLayerID);
    bool composite = tools_->wandAllLayers || !layer || layer->isGroup || layer->isAdjustment();
    Image source = composite ? flatten(doc) : renderLayerAlone(doc, *layer);
    auto selected = wandSelection(source, x, y, tools_->wandTolerance, tools_->wandContiguous, 0);
    if (!selected) return;
    commitSelection(*selected, mode, QStringLiteral("魔棒"));
}

void CanvasView::cancelPolygon() {
    if (!polygonActive_) return;
    polygonActive_ = false;
    lassoPoints_.clear();
    update();
}

bool CanvasView::closePolygon() {
    if (!polygonActive_) return false;
    polygonActive_ = false;
    std::vector<StrokePoint> points;
    for (QPointF q : lassoPoints_) points.push_back({q.x(), q.y()});
    lassoPoints_.clear();
    if (points.size() < 3) { update(); return false; }
    const Document& doc = session_->doc();
    commitSelection(polygonSelection(doc.width, doc.height, points), selectMode_, QStringLiteral("多边形套索"));
    update();
    return true;
}

void CanvasView::drawSelection(QPainter& painter) {
    const Document& doc = session_->doc();
    QTransform view = QTransform::fromTranslate(pan_.x(), pan_.y()).scale(zoom_, zoom_);
    auto ants = [&](const QPainterPath& path) {
        // 黑底白虚线，虚线随计时器移动。
        QPainterPath mapped = view.map(path);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(Qt::black, 1));
        painter.drawPath(mapped);
        QPen dashed(Qt::white, 1, Qt::CustomDashLine);
        dashed.setDashPattern({4, 4});
        dashed.setDashOffset(antsOffset_);
        painter.setPen(dashed);
        painter.drawPath(mapped);
    };
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, false);
    if (doc.selection) {
        if (antsSource_ != doc.selection.get() || antsSerial_ != doc.selection->serial) {
            antsSource_ = doc.selection.get();
            antsSerial_ = doc.selection->serial;
            antsPath_ = QPainterPath();
            std::vector<std::vector<std::pair<int, int>>> loops;
            if (selectionOutline(*doc.selection, loops)) {
                for (const auto& loop : loops) {
                    if (loop.empty()) continue;
                    antsPath_.moveTo(loop[0].first, loop[0].second);
                    for (size_t i = 1; i < loop.size(); ++i) antsPath_.lineTo(loop[i].first, loop[i].second);
                    antsPath_.closeSubpath();
                }
            } else {
                // 轮廓太复杂：画外接框。
                int x, y, w, h;
                if (selectionBounds(*doc.selection, x, y, w, h)) antsPath_.addRect(x, y, w, h);
            }
        }
        ants(antsPath_);
        if (!antsTimer_.isActive()) antsTimer_.start();
    } else if (antsTimer_.isActive()) {
        antsTimer_.stop();
    }
    // 正在画的选框或套索。
    if (drag_ == DragMode::Marquee) {
        QRectF rect = marqueeRect(toDocument(hover_), QApplication::keyboardModifiers());
        QPainterPath path;
        if (tools_->marqueeEllipse) path.addEllipse(rect); else path.addRect(rect);
        ants(path);
    }
    if ((drag_ == DragMode::Lasso || polygonActive_) && !lassoPoints_.empty()) {
        QPainterPath path(lassoPoints_.front());
        for (size_t i = 1; i < lassoPoints_.size(); ++i) path.lineTo(lassoPoints_[i]);
        if (polygonActive_ && hover_.x() >= 0) path.lineTo(toDocument(hover_));
        ants(path);
        if (polygonActive_) {
            // 起点画个小方框，点回这里就闭合。
            QPointF first = toWidget(lassoPoints_.front());
            painter.setPen(QPen(Qt::white, 1));
            painter.drawRect(QRectF(first.x() - 4, first.y() - 4, 8, 8));
        }
    }
    painter.restore();
}
