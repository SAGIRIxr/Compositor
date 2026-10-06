// 图层效果编辑：勾选启用，右侧调整参数，实时预览，确定后作为一步撤销提交。
#pragma once
#include "Session.h"
#include "compositor/effects.h"
#include <QColor>
#include <QDialog>
#include <functional>
#include <vector>

class QListWidget;
class QStackedWidget;
class QFormLayout;

class EffectsDialog : public QDialog {
    Q_OBJECT
public:
    // newColor：新建描边或颜色叠加时用的颜色（与 macOS 版一样取背景色）。
    EffectsDialog(Session* session, const std::string& layerID, const QColor& newColor, QWidget* parent = nullptr);

private:
    QWidget* buildPage(comp::EffectKind kind);
    void addSlider(QFormLayout* form, const QString& label, double minimum, double maximum, int decimals,
                   std::function<double()> get, std::function<void(double)> set);
    void apply();
    void refresh();

    Session* session_;
    std::string layerID_;
    comp::LayerEffectsParams params_;
    double newColor_[3];
    QListWidget* list_;
    QStackedWidget* pages_;
    std::vector<std::function<void()>> refreshers_;
    bool loading_ = false;
};
