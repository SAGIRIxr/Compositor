#include "AdjustmentDialog.h"
#include "CurveEditor.h"
#include "Dialogs.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QRandomGenerator>
#include <QSlider>
#include <QVBoxLayout>
#include <cmath>

using namespace comp;

AdjustmentDialog::AdjustmentDialog(Session* session, const std::string& layerID, QWidget* parent)
    : QDialog(parent), session_(session), layerID_(layerID) {
    const Layer* layer = session->doc().find(layerID);
    params_ = parseAdjustment(*layer->adjustment);
    setWindowTitle(QStringLiteral("%1 — %2").arg(QString::fromUtf8(adjustmentKindLabel(params_.kind)), QString::fromStdString(layer->name)));
    setMinimumWidth(380);
    session_->beginPreview();
    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout;
    build(form);
    layout->addLayout(form);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Reset);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("确定"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    buttons->button(QDialogButtonBox::Reset)->setText(QStringLiteral("复位"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons->button(QDialogButtonBox::Reset), &QPushButton::clicked, this, [this] {
        AdjustmentParams fresh = parseAdjustment(makeAdjustment(params_.kind, params_.grainSeed));
        fresh.grainSeed = params_.grainSeed;
        fresh.noiseSeed = params_.noiseSeed;
        params_ = fresh;
        refresh();
        apply();
    });
    layout->addWidget(buttons);
    connect(this, &QDialog::finished, this, [this](int result) {
        if (result == QDialog::Accepted) {
            session_->commitPreview(QStringLiteral("编辑%1").arg(QString::fromUtf8(adjustmentKindLabel(params_.kind))));
        } else {
            session_->cancelPreview();
        }
    });
    refresh();
}

void AdjustmentDialog::apply() {
    if (loading_) return;
    Layer* layer = session_->doc().find(layerID_);
    if (!layer || !layer->adjustment) return;
    if (!params_.isValid()) return;
    Json& json = *layer->adjustment;
    // 编辑了某个色彩范围时需要 hsvSettings（macOS 版的分范围格式）。
    if (params_.kind == AdjustmentKind::HueSaturation && !json.contains("hsvSettings")) {
        bool ranged = params_.selectedRange != 0;
        for (int i = 1; i < 7; ++i) {
            const HueRange& r = params_.hueRanges[size_t(i)];
            if (r.hue != 0 || r.saturation != 0 || r.lightness != 0) ranged = true;
        }
        if (ranged) json["hsvSettings"] = Json::object();
    }
    writeAdjustment(params_, json);
    session_->notifyChanged(false);
}

void AdjustmentDialog::refresh() {
    loading_ = true;
    for (auto& r : refreshers_) r();
    loading_ = false;
}

void AdjustmentDialog::addSlider(QFormLayout* form, const QString& label, double minimum, double maximum, int decimals,
                                 std::function<double()> get, std::function<void(double)> set) {
    double factor = std::pow(10.0, decimals);
    auto* slider = new QSlider(Qt::Horizontal);
    slider->setRange(int(std::lround(minimum * factor)), int(std::lround(maximum * factor)));
    auto* box = new QDoubleSpinBox;
    box->setRange(minimum, maximum);
    box->setDecimals(decimals);
    box->setSingleStep(decimals ? 1 / factor * (decimals == 1 ? 1 : 10) : 1);
    box->setMinimumWidth(80);
    auto* row = new QHBoxLayout;
    row->addWidget(slider, 1);
    row->addWidget(box);
    form->addRow(label, row);
    connect(slider, &QSlider::valueChanged, this, [=](int v) {
        if (loading_) return;
        loading_ = true;
        box->setValue(v / factor);
        loading_ = false;
        set(v / factor);
        apply();
    });
    connect(box, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [=](double v) {
        if (loading_) return;
        loading_ = true;
        slider->setValue(int(std::lround(v * factor)));
        loading_ = false;
        set(v);
        apply();
    });
    refreshers_.push_back([=] {
        double v = get();
        box->setValue(v);
        slider->setValue(int(std::lround(v * factor)));
    });
}

void AdjustmentDialog::build(QFormLayout* form) {
    AdjustmentParams& p = params_;
    switch (p.kind) {
    case AdjustmentKind::HueSaturation: {
        auto* range = new QComboBox;
        range->addItems({QStringLiteral("全图"), QStringLiteral("红色"), QStringLiteral("黄色"), QStringLiteral("绿色"),
                         QStringLiteral("青色"), QStringLiteral("蓝色"), QStringLiteral("洋红")});
        form->addRow(QStringLiteral("范围："), range);
        connect(range, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
            if (loading_) return;
            params_.selectedRange = index;
            refresh();
            apply();
        });
        refreshers_.push_back([this, range] { range->setCurrentIndex(params_.selectedRange); });
        auto current = [this]() -> HueRange& { return params_.hueRanges[size_t(params_.colorize ? 0 : params_.selectedRange)]; };
        addSlider(form, QStringLiteral("色相："), -180, 360, 0, [=] { return current().hue; }, [=](double v) { current().hue = v; });
        addSlider(form, QStringLiteral("饱和度："), -100, 100, 0, [=] { return current().saturation; }, [=](double v) { current().saturation = v; });
        addSlider(form, QStringLiteral("明度："), -100, 100, 0, [=] { return current().lightness; }, [=](double v) { current().lightness = v; });
        auto* colorize = new QCheckBox(QStringLiteral("着色"));
        form->addRow(QString(), colorize);
        connect(colorize, &QCheckBox::toggled, this, [this](bool on) {
            if (loading_) return;
            params_.colorize = on;
            if (on) {
                // Photoshop 打开着色时的起点。
                params_.selectedRange = 0;
                params_.hueRanges[0] = {0, 25, 0};
            }
            refresh();
            apply();
        });
        refreshers_.push_back([this, colorize, range] { colorize->setChecked(params_.colorize); range->setEnabled(!params_.colorize); });
        break;
    }
    case AdjustmentKind::Levels: {
        auto* channel = new QComboBox;
        channel->addItems({QStringLiteral("RGB"), QStringLiteral("红"), QStringLiteral("绿"), QStringLiteral("蓝")});
        form->addRow(QStringLiteral("通道："), channel);
        connect(channel, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
            levelsChannel_ = index;
            refresh();
        });
        auto range = [this]() -> LevelRange& { return params_.levels[size_t(levelsChannel_)]; };
        addSlider(form, QStringLiteral("输入黑场："), 0, 254, 0, [=] { return range().black; },
                  [=](double v) { range().black = v; range().white = std::max(range().white, v + 1); });
        addSlider(form, QStringLiteral("灰度系数："), 0.1, 9.99, 2, [=] { return range().gamma; }, [=](double v) { range().gamma = v; });
        addSlider(form, QStringLiteral("输入白场："), 1, 255, 0, [=] { return range().white; },
                  [=](double v) { range().white = v; range().black = std::min(range().black, v - 1); });
        addSlider(form, QStringLiteral("输出黑场："), 0, 255, 0, [=] { return range().outputBlack; }, [=](double v) { range().outputBlack = v; });
        addSlider(form, QStringLiteral("输出白场："), 0, 255, 0, [=] { return range().outputWhite; }, [=](double v) { range().outputWhite = v; });
        break;
    }
    case AdjustmentKind::Curves: {
        auto* channel = new QComboBox;
        channel->addItems({QStringLiteral("RGB"), QStringLiteral("红"), QStringLiteral("绿"), QStringLiteral("蓝")});
        form->addRow(QStringLiteral("通道："), channel);
        auto* editor = new CurveEditor;
        form->addRow(editor);
        form->addRow(new QLabel(QStringLiteral("点击添加控制点，拖动调整，拖出方框删除。")));
        const QColor colors[4] = {Qt::white, QColor(255, 90, 90), QColor(90, 220, 90), QColor(90, 140, 255)};
        connect(channel, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, editor, colors](int index) {
            curvesChannel_ = index;
            editor->setCurveColor(colors[index]);
            refresh();
        });
        connect(editor, &CurveEditor::pointsChanged, this, [this, editor] {
            params_.curves[size_t(curvesChannel_)] = editor->points();
            apply();
        });
        refreshers_.push_back([this, editor] { editor->setPoints(params_.curves[size_t(curvesChannel_)]); });
        break;
    }
    case AdjustmentKind::Exposure:
        addSlider(form, QStringLiteral("曝光度："), -20, 20, 2, [&p] { return p.exposure; }, [&p](double v) { p.exposure = v; });
        addSlider(form, QStringLiteral("位移："), -0.5, 0.5, 3, [&p] { return p.exposureOffset; }, [&p](double v) { p.exposureOffset = v; });
        addSlider(form, QStringLiteral("灰度系数："), 0.01, 9.99, 2, [&p] { return p.exposureGamma; }, [&p](double v) { p.exposureGamma = v; });
        break;
    case AdjustmentKind::GradientMap: {
        auto toColor = [](const double c[3]) { return QColor::fromRgbF(float(c[0]), float(c[1]), float(c[2])); };
        auto* shadows = new ColorButton(toColor(p.shadows));
        auto* highlights = new ColorButton(toColor(p.highlights));
        auto* reversed = new QCheckBox(QStringLiteral("反向"));
        form->addRow(QStringLiteral("暗部颜色："), shadows);
        form->addRow(QStringLiteral("亮部颜色："), highlights);
        form->addRow(QString(), reversed);
        connect(shadows, &ColorButton::colorChanged, this, [this](const QColor& c) {
            params_.shadows[0] = c.redF(); params_.shadows[1] = c.greenF(); params_.shadows[2] = c.blueF(); apply();
        });
        connect(highlights, &ColorButton::colorChanged, this, [this](const QColor& c) {
            params_.highlights[0] = c.redF(); params_.highlights[1] = c.greenF(); params_.highlights[2] = c.blueF(); apply();
        });
        connect(reversed, &QCheckBox::toggled, this, [this](bool on) { if (loading_) return; params_.gradientReversed = on; apply(); });
        refreshers_.push_back([=] {
            shadows->setColor(toColor(params_.shadows));
            highlights->setColor(toColor(params_.highlights));
            reversed->setChecked(params_.gradientReversed);
        });
        break;
    }
    case AdjustmentKind::Grain: {
        addSlider(form, QStringLiteral("数量："), 0, 100, 0, [&p] { return p.grainAmount; }, [&p](double v) { p.grainAmount = v; });
        addSlider(form, QStringLiteral("大小："), 0.5, 20, 1, [&p] { return p.grainSize; }, [&p](double v) { p.grainSize = v; });
        addSlider(form, QStringLiteral("粗糙度："), 0, 100, 0, [&p] { return p.grainRoughness; }, [&p](double v) { p.grainRoughness = v; });
        auto* reseed = new QPushButton(QStringLiteral("换一种图案"));
        form->addRow(QString(), reseed);
        connect(reseed, &QPushButton::clicked, this, [this] { params_.grainSeed = QRandomGenerator::global()->generate(); apply(); });
        break;
    }
    case AdjustmentKind::AddNoise: {
        addSlider(form, QStringLiteral("数量（%）："), 0.1, 400, 1, [&p] { return p.noiseAmount; }, [&p](double v) { p.noiseAmount = v; });
        auto* gaussian = new QCheckBox(QStringLiteral("高斯分布"));
        auto* mono = new QCheckBox(QStringLiteral("单色"));
        form->addRow(QString(), gaussian);
        form->addRow(QString(), mono);
        connect(gaussian, &QCheckBox::toggled, this, [this](bool on) { if (loading_) return; params_.noiseGaussian = on; apply(); });
        connect(mono, &QCheckBox::toggled, this, [this](bool on) { if (loading_) return; params_.noiseMonochromatic = on; apply(); });
        refreshers_.push_back([=] { gaussian->setChecked(params_.noiseGaussian); mono->setChecked(params_.noiseMonochromatic); });
        break;
    }
    case AdjustmentKind::GaussianBlur:
        addSlider(form, QStringLiteral("半径（像素）："), 0.1, 250, 1, [&p] { return p.blurRadius; }, [&p](double v) { p.blurRadius = v; });
        break;
    case AdjustmentKind::MotionBlur:
        addSlider(form, QStringLiteral("角度："), -90, 90, 0, [&p] { return p.motionAngle; }, [&p](double v) { p.motionAngle = v; });
        addSlider(form, QStringLiteral("距离（像素）："), 1, 2000, 0, [&p] { return p.motionDistance; }, [&p](double v) { p.motionDistance = v; });
        break;
    case AdjustmentKind::Invert:
        form->addRow(new QLabel(QStringLiteral("反相没有可调的参数。")));
        break;
    case AdjustmentKind::BlackWhite: {
        const char* names[6] = {"红色：", "黄色：", "绿色：", "青色：", "蓝色：", "洋红："};
        for (int i = 0; i < 6; ++i) {
            addSlider(form, QString::fromUtf8(names[i]), -200, 300, 0, [&p, i] { return p.bw[i]; }, [&p, i](double v) { p.bw[i] = v; });
        }
        auto* tint = new QCheckBox(QStringLiteral("色调"));
        form->addRow(QString(), tint);
        connect(tint, &QCheckBox::toggled, this, [this](bool on) { if (loading_) return; params_.tint = on; apply(); });
        refreshers_.push_back([=] { tint->setChecked(params_.tint); });
        addSlider(form, QStringLiteral("色相："), 0, 360, 0, [&p] { return p.tintHue; }, [&p](double v) { p.tintHue = v; });
        addSlider(form, QStringLiteral("饱和度："), 0, 100, 0, [&p] { return p.tintSaturation; }, [&p](double v) { p.tintSaturation = v; });
        break;
    }
    case AdjustmentKind::ColorBalance: {
        auto* tone = new QComboBox;
        tone->addItems({QStringLiteral("阴影"), QStringLiteral("中间调"), QStringLiteral("高光")});
        tone->setCurrentIndex(1);
        form->addRow(QStringLiteral("色调："), tone);
        connect(tone, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) { balanceTone_ = index; refresh(); });
        const char* names[3] = {"青色 — 红色：", "洋红 — 绿色：", "黄色 — 蓝色："};
        for (int i = 0; i < 3; ++i) {
            addSlider(form, QString::fromUtf8(names[i]), -100, 100, 0, [this, i] { return params_.balance[balanceTone_ * 3 + i]; },
                      [this, i](double v) { params_.balance[balanceTone_ * 3 + i] = v; });
        }
        auto* preserve = new QCheckBox(QStringLiteral("保持明度"));
        form->addRow(QString(), preserve);
        connect(preserve, &QCheckBox::toggled, this, [this](bool on) { if (loading_) return; params_.preserveLuminosity = on; apply(); });
        refreshers_.push_back([=] { preserve->setChecked(params_.preserveLuminosity); });
        break;
    }
    }
}
