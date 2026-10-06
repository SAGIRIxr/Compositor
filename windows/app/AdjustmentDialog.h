// 调整图层编辑器：每种调整一套控件，改动实时预览，确定后作为一步撤销提交。
#pragma once
#include "Session.h"
#include "compositor/adjustment.h"
#include <QDialog>
#include <functional>
#include <vector>

class QFormLayout;

class AdjustmentDialog : public QDialog {
    Q_OBJECT
public:
    AdjustmentDialog(Session* session, const std::string& layerID, QWidget* parent = nullptr);

private:
    void build(QFormLayout* form);
    void apply();
    void refresh();
    void addSlider(QFormLayout* form, const QString& label, double minimum, double maximum, int decimals,
                   std::function<double()> get, std::function<void(double)> set);

    Session* session_;
    std::string layerID_;
    comp::AdjustmentParams params_;
    std::vector<std::function<void()>> refreshers_;
    int levelsChannel_ = 0;
    int curvesChannel_ = 0;
    int balanceTone_ = 1;
    bool loading_ = false;
};
