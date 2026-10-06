// 新建、画布大小、图像大小、JPEG 导出对话框。
#pragma once
#include <QColor>
#include <QDialog>

class QSpinBox;
class QDoubleSpinBox;
class QComboBox;
class QCheckBox;
class QSlider;
class QLabel;
class QPushButton;

class ColorButton;

class NewDocumentDialog : public QDialog {
    Q_OBJECT
public:
    explicit NewDocumentDialog(QWidget* parent = nullptr);
    int documentWidth() const;
    int documentHeight() const;
    double resolution() const;
    // 无效颜色表示透明背景。
    QColor fill() const;

private:
    QSpinBox* width_;
    QSpinBox* height_;
    QDoubleSpinBox* resolution_;
    QComboBox* background_;
    QColor custom_ = Qt::white;
};

class CanvasSizeDialog : public QDialog {
    Q_OBJECT
public:
    CanvasSizeDialog(int width, int height, QWidget* parent = nullptr);
    int newWidth() const;
    int newHeight() const;
    double anchorX() const { return anchorX_; }
    double anchorY() const { return anchorY_; }

private:
    QSpinBox* width_;
    QSpinBox* height_;
    double anchorX_ = 0.5, anchorY_ = 0.5;
};

class ImageSizeDialog : public QDialog {
    Q_OBJECT
public:
    ImageSizeDialog(int width, int height, double resolution, QWidget* parent = nullptr);
    int newWidth() const;
    int newHeight() const;
    double resolution() const;

private:
    QSpinBox* width_;
    QSpinBox* height_;
    QDoubleSpinBox* resolution_;
    QCheckBox* keepRatio_;
    double ratio_;
    bool syncing_ = false;
};

class JpegExportDialog : public QDialog {
    Q_OBJECT
public:
    explicit JpegExportDialog(QWidget* parent = nullptr);
    int quality() const;
    QColor matte() const { return matte_; }

private:
    QSlider* quality_;
    QColor matte_ = Qt::white;
};

// 颜色按钮：点开系统取色器。
#include <QToolButton>
class ColorButton : public QToolButton {
    Q_OBJECT
public:
    explicit ColorButton(const QColor& color, QWidget* parent = nullptr);
    QColor color() const { return color_; }
    void setColor(const QColor& color);
signals:
    void colorChanged(const QColor& color);
private:
    QColor color_;
};
