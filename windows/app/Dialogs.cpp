#include "Dialogs.h"
#include "compositor/document.h"
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QPainter>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>
#include <cmath>

namespace {
QSpinBox* sizeBox(int value) {
    auto* box = new QSpinBox;
    box->setRange(1, comp::kMaxSide);
    box->setValue(value);
    box->setSuffix(QStringLiteral(" 像素"));
    box->setAccelerated(true);
    return box;
}
QDialogButtonBox* okCancel(QDialog* dialog) {
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("确定"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    QObject::connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    return buttons;
}
} // namespace

ColorButton::ColorButton(const QColor& color, QWidget* parent) : QToolButton(parent) {
    setMinimumSize(40, 24);
    setColor(color);
    connect(this, &QToolButton::clicked, this, [this] {
        QColor chosen = QColorDialog::getColor(color_, this, QStringLiteral("选择颜色"));
        if (chosen.isValid()) { setColor(chosen); emit colorChanged(chosen); }
    });
}

void ColorButton::setColor(const QColor& color) {
    color_ = color;
    QPixmap swatch(28, 16);
    swatch.fill(color);
    QPainter(&swatch).drawRect(swatch.rect().adjusted(0, 0, -1, -1));
    setIcon(QIcon(swatch));
    setIconSize(swatch.size());
    setToolTip(color.name().toUpper());
}

NewDocumentDialog::NewDocumentDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(QStringLiteral("新建画布"));
    auto* form = new QFormLayout;
    auto* preset = new QComboBox;
    struct Preset { const char* name; int w, h; };
    static const Preset presets[] = {
        {"自定义", 0, 0}, {"1920 × 1080（全高清）", 1920, 1080}, {"3840 × 2160（4K）", 3840, 2160},
        {"1080 × 1080（方形）", 1080, 1080}, {"1080 × 1920（竖屏 9:16）", 1080, 1920},
        {"2480 × 3508（A4 300 ppi）", 2480, 3508}, {"1200 × 630（社交分享图）", 1200, 630},
    };
    for (const auto& p : presets) preset->addItem(QString::fromUtf8(p.name));
    width_ = sizeBox(1920);
    height_ = sizeBox(1080);
    resolution_ = new QDoubleSpinBox;
    resolution_->setRange(1, 9600);
    resolution_->setValue(72);
    resolution_->setSuffix(QStringLiteral(" ppi"));
    background_ = new QComboBox;
    background_->addItems({QStringLiteral("白色"), QStringLiteral("黑色"), QStringLiteral("透明"), QStringLiteral("自定义颜色…")});
    form->addRow(QStringLiteral("预设："), preset);
    form->addRow(QStringLiteral("宽度："), width_);
    form->addRow(QStringLiteral("高度："), height_);
    form->addRow(QStringLiteral("分辨率："), resolution_);
    form->addRow(QStringLiteral("背景："), background_);
    connect(preset, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        if (index <= 0) return;
        width_->setValue(presets[index].w);
        height_->setValue(presets[index].h);
        resolution_->setValue(QString::fromUtf8(presets[index].name).contains(QStringLiteral("300")) ? 300 : 72);
    });
    connect(background_, QOverload<int>::of(&QComboBox::activated), this, [this](int index) {
        if (index != 3) return;
        QColor chosen = QColorDialog::getColor(custom_, this, QStringLiteral("背景颜色"));
        if (chosen.isValid()) custom_ = chosen;
    });
    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(okCancel(this));
}

int NewDocumentDialog::documentWidth() const { return width_->value(); }
int NewDocumentDialog::documentHeight() const { return height_->value(); }
double NewDocumentDialog::resolution() const { return resolution_->value(); }
QColor NewDocumentDialog::fill() const {
    switch (background_->currentIndex()) {
    case 0: return Qt::white;
    case 1: return Qt::black;
    case 2: return QColor();
    default: return custom_;
    }
}

CanvasSizeDialog::CanvasSizeDialog(int width, int height, QWidget* parent) : QDialog(parent) {
    setWindowTitle(QStringLiteral("画布大小"));
    auto* form = new QFormLayout;
    width_ = sizeBox(width);
    height_ = sizeBox(height);
    form->addRow(QStringLiteral("当前："), new QLabel(QStringLiteral("%1 × %2 像素").arg(width).arg(height)));
    form->addRow(QStringLiteral("宽度："), width_);
    form->addRow(QStringLiteral("高度："), height_);
    // 九宫格锚点。
    auto* grid = new QGridLayout;
    grid->setSpacing(2);
    for (int row = 0; row < 3; ++row) for (int col = 0; col < 3; ++col) {
        auto* b = new QToolButton;
        b->setCheckable(true);
        b->setAutoExclusive(true);
        b->setFixedSize(26, 26);
        b->setChecked(row == 1 && col == 1);
        b->setText(row == 1 && col == 1 ? QStringLiteral("●") : QString());
        grid->addWidget(b, row, col);
        connect(b, &QToolButton::toggled, this, [this, b, row, col](bool on) {
            b->setText(on ? QStringLiteral("●") : QString());
            if (on) { anchorX_ = col / 2.0; anchorY_ = row / 2.0; }
        });
    }
    auto* anchorBox = new QWidget;
    anchorBox->setLayout(grid);
    form->addRow(QStringLiteral("定位："), anchorBox);
    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(okCancel(this));
}

int CanvasSizeDialog::newWidth() const { return width_->value(); }
int CanvasSizeDialog::newHeight() const { return height_->value(); }

ImageSizeDialog::ImageSizeDialog(int width, int height, double resolution, QWidget* parent)
    : QDialog(parent), ratio_(double(width) / height) {
    setWindowTitle(QStringLiteral("图像大小"));
    auto* form = new QFormLayout;
    width_ = sizeBox(width);
    height_ = sizeBox(height);
    resolution_ = new QDoubleSpinBox;
    resolution_->setRange(1, 9600);
    resolution_->setValue(resolution);
    resolution_->setSuffix(QStringLiteral(" ppi"));
    keepRatio_ = new QCheckBox(QStringLiteral("约束比例"));
    keepRatio_->setChecked(true);
    form->addRow(QStringLiteral("宽度："), width_);
    form->addRow(QStringLiteral("高度："), height_);
    form->addRow(QString(), keepRatio_);
    form->addRow(QStringLiteral("分辨率："), resolution_);
    form->addRow(new QLabel(QStringLiteral("图层保留原始像素，只改变它们在画布上的大小，可随时再放大。")));
    connect(width_, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int w) {
        if (syncing_ || !keepRatio_->isChecked()) return;
        syncing_ = true; height_->setValue(std::max(1, int(std::lround(w / ratio_)))); syncing_ = false;
    });
    connect(height_, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int h) {
        if (syncing_ || !keepRatio_->isChecked()) return;
        syncing_ = true; width_->setValue(std::max(1, int(std::lround(h * ratio_)))); syncing_ = false;
    });
    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(okCancel(this));
}

int ImageSizeDialog::newWidth() const { return width_->value(); }
int ImageSizeDialog::newHeight() const { return height_->value(); }
double ImageSizeDialog::resolution() const { return resolution_->value(); }

JpegExportDialog::JpegExportDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(QStringLiteral("导出 JPEG"));
    auto* form = new QFormLayout;
    quality_ = new QSlider(Qt::Horizontal);
    quality_->setRange(1, 100);
    quality_->setValue(85);
    auto* label = new QLabel(QStringLiteral("85"));
    connect(quality_, &QSlider::valueChanged, label, [label](int v) { label->setNum(v); });
    auto* row = new QHBoxLayout;
    row->addWidget(quality_, 1);
    row->addWidget(label);
    form->addRow(QStringLiteral("品质："), row);
    auto* matte = new ColorButton(matte_);
    connect(matte, &ColorButton::colorChanged, this, [this](const QColor& c) { matte_ = c; });
    form->addRow(QStringLiteral("透明处填充："), matte);
    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(okCancel(this));
}

int JpegExportDialog::quality() const { return quality_->value(); }
