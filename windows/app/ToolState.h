// 当前工具与画笔、颜色设置，在主窗口、画布和工具栏之间共享。
#pragma once
#include "compositor/edit.h"
#include <QColor>
#include <QObject>
#include <QPointF>
#include <optional>

enum class Tool { Move, Marquee, Lasso, Wand, Brush, Eraser, Heal, Clone, Gradient, Eyedropper, Hand, Zoom };

class ToolState : public QObject {
    Q_OBJECT
public:
    Tool tool = Tool::Brush;
    comp::BrushSettings brush;
    QColor foreground = Qt::black;
    QColor background = Qt::white;
    // 画笔落在蒙版上（选中图层有蒙版时由图层面板切换）。
    bool paintOnMask = false;
    // 选区工具
    bool marqueeEllipse = false;
    bool lassoPolygon = false;
    int wandTolerance = 32;
    bool wandContiguous = true;
    bool wandAllLayers = false;
    // 修复与仿制
    int healMode = 0;               // 0 内容识别，1 创建纹理，2 近似匹配
    bool cloneAligned = true;
    bool cloneAllLayers = false;
    std::optional<QPointF> cloneSource;  // 文档坐标
    std::optional<QPointF> cloneOffset;  // 对齐模式下第一笔定下的偏移
    // 渐变
    bool gradientRadial = false;
    bool gradientToTransparent = false;
    bool gradientReversed = false;
    double gradientOpacity = 1;

    void setTool(Tool t) { if (tool != t) { tool = t; emit changed(); } }
    void notify() { emit changed(); }

signals:
    void changed();
};
