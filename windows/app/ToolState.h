// 当前工具与画笔、颜色设置，在主窗口、画布和工具栏之间共享。
#pragma once
#include "compositor/edit.h"
#include <QColor>
#include <QObject>

enum class Tool { Move, Marquee, Lasso, Wand, Brush, Eraser, Eyedropper, Hand, Zoom };

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

    void setTool(Tool t) { if (tool != t) { tool = t; emit changed(); } }
    void notify() { emit changed(); }

signals:
    void changed();
};
