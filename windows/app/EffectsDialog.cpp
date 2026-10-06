#include "EffectsDialog.h"
#include "Dialogs.h"
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSlider>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <cmath>

using namespace comp;

EffectsDialog::EffectsDialog(Session* session, const std::string& layerID, const QColor& newColor, QWidget* parent)
    : QDialog(parent), session_(session), layerID_(layerID) {
    const Layer* layer = session->doc().find(layerID);
    if (layer && layer->extra.contains("effects")) {
        try { params_ = parseEffects(layer->extra["effects"]); } catch (...) {}
    }
    newColor_[0] = newColor.redF(); newColor_[1] = newColor.greenF(); newColor_[2] = newColor.blueF();
    setWindowTitle(QStringLiteral("图层效果 — %1").arg(layer ? QString::fromStdString(layer->name) : QString()));
    setMinimumSize(560, 360);
    session_->beginPreview();

    list_ = new QListWidget;
    list_->setMaximumWidth(160);
    pages_ = new QStackedWidget;
    for (EffectKind kind : allEffectKinds()) {
        auto* item = new QListWidgetItem(QString::fromUtf8(effectLabel(kind)), list_);
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
        pages_->addWidget(buildPage(kind));
    }
    connect(list_, &QListWidget::currentRowChanged, pages_, &QStackedWidget::setCurrentIndex);
    connect(list_, &QListWidget::itemChanged, this, [this](QListWidgetItem* item) {
        if (loading_) return;
        EffectKind kind = EffectKind(list_->row(item));
        EffectParams& e = params_[kind];
        bool on = item->checkState() == Qt::Checked;
        if (on && !e.present) e = defaultEffect(kind, newColor_);
        else e.enabled = on;
        list_->setCurrentItem(item);
        refresh();
        apply();
    });
    auto* body = new QHBoxLayout;
    body->addWidget(list_);
    body->addWidget(pages_, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("确定"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    auto* remove = buttons->addButton(QStringLiteral("移除此效果"), QDialogButtonBox::ResetRole);
    connect(remove, &QPushButton::clicked, this, [this] {
        int row = list_->currentRow();
        if (row < 0) return;
        params_.effects[row] = EffectParams();
        refresh();
        apply();
    });
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(this, &QDialog::finished, this, [this](int result) {
        if (result == QDialog::Accepted) session_->commitPreview(QStringLiteral("图层效果"));
        else session_->cancelPreview();
    });

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(body, 1);
    layout->addWidget(new QLabel(QStringLiteral("尺寸以图层像素计。隐藏的效果保留参数，以后可以再打开。")));
    layout->addWidget(buttons);
    // 先选中第一个已有的效果，没有就选投影。
    int first = int(EffectKind::Shadow);
    for (int i = 0; i < 6; ++i) if (params_.effects[i].present) { first = i; break; }
    refresh();
    list_->setCurrentRow(first);
}

void EffectsDialog::addSlider(QFormLayout* form, const QString& label, double minimum, double maximum, int decimals,
                              std::function<double()> get, std::function<void(double)> set) {
    double factor = std::pow(10.0, decimals);
    auto* slider = new QSlider(Qt::Horizontal);
    slider->setRange(int(std::lround(minimum * factor)), int(std::lround(maximum * factor)));
    auto* box = new QDoubleSpinBox;
    box->setRange(minimum, maximum);
    box->setDecimals(decimals);
    box->setMinimumWidth(80);
    auto* row = new QHBoxLayout;
    row->addWidget(slider, 1);
    row->addWidget(box);
    form->addRow(label, row);
    connect(slider, &QSlider::valueChanged, this, [=](int v) {
        if (loading_) return;
        loading_ = true; box->setValue(v / factor); loading_ = false;
        set(v / factor);
        apply();
    });
    connect(box, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [=](double v) {
        if (loading_) return;
        loading_ = true; slider->setValue(int(std::lround(v * factor))); loading_ = false;
        set(v);
        apply();
    });
    refreshers_.push_back([=] { double v = get(); box->setValue(v); slider->setValue(int(std::lround(v * factor))); });
}

QWidget* EffectsDialog::buildPage(EffectKind kind) {
    auto* page = new QWidget;
    auto* form = new QFormLayout(page);
    auto e = [this, kind]() -> EffectParams& { return params_[kind]; };
    auto* color = new ColorButton(Qt::black);
    form->addRow(QStringLiteral("颜色："), color);
    connect(color, &ColorButton::colorChanged, this, [this, e](const QColor& c) {
        e().color[0] = c.redF(); e().color[1] = c.greenF(); e().color[2] = c.blueF();
        apply();
    });
    refreshers_.push_back([=] { color->setColor(QColor::fromRgbF(float(e().color[0]), float(e().color[1]), float(e().color[2]))); });
    addSlider(form, QStringLiteral("不透明度（%）："), 0, 100, 0,
              [e] { return e().opacity * 100; }, [e](double v) { e().opacity = v / 100; });
    switch (kind) {
    case EffectKind::Stroke: {
        addSlider(form, QStringLiteral("大小："), 0, 500, 0, [e] { return e().size; }, [e](double v) { e().size = v; });
        auto* inside = new QCheckBox(QStringLiteral("在边缘内侧"));
        form->addRow(QString(), inside);
        connect(inside, &QCheckBox::toggled, this, [this, e](bool on) { if (loading_) return; e().inside = on; apply(); });
        refreshers_.push_back([=] { inside->setChecked(e().inside); });
        break;
    }
    case EffectKind::Shadow:
    case EffectKind::InnerShadow:
        addSlider(form, QStringLiteral("角度："), -180, 180, 0, [e] { return e().angle; }, [e](double v) { e().angle = v; });
        addSlider(form, QStringLiteral("距离："), 0, 500, 0, [e] { return e().distance; }, [e](double v) { e().distance = v; });
        addSlider(form, QStringLiteral("模糊："), 0, 500, 0, [e] { return e().blur; }, [e](double v) { e().blur = v; });
        break;
    case EffectKind::OuterGlow:
    case EffectKind::InnerGlow:
        addSlider(form, QStringLiteral("大小："), 0, 500, 0, [e] { return e().size; }, [e](double v) { e().size = v; });
        break;
    case EffectKind::ColorOverlay:
        break;
    }
    // 没有这个效果时参数不可编辑。
    refreshers_.push_back([=] { page->setEnabled(e().present); });
    return page;
}

void EffectsDialog::refresh() {
    loading_ = true;
    for (int i = 0; i < 6; ++i) {
        const EffectParams& e = params_.effects[i];
        list_->item(i)->setCheckState(e.present && e.enabled ? Qt::Checked : Qt::Unchecked);
        QFont font = list_->item(i)->font();
        font.setItalic(e.present && !e.enabled);
        list_->item(i)->setFont(font);
    }
    for (auto& r : refreshers_) r();
    loading_ = false;
}

void EffectsDialog::apply() {
    if (loading_ || !params_.isValid()) return;
    Layer* layer = session_->doc().find(layerID_);
    if (!layer) return;
    Json effects = layer->extra.contains("effects") ? layer->extra["effects"] : Json::object();
    writeEffects(params_, effects);
    if (effects.empty()) layer->extra.erase("effects");
    else layer->extra["effects"] = effects;
    session_->notifyChanged(false);
}
