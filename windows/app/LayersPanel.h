// 图层面板：从上到下列出图层与文件夹，支持拖放排序、显示隐藏、重命名，以及混合模式和不透明度。
#pragma once
#include "Session.h"
#include <QHash>
#include <QIcon>
#include <QPointer>
#include <QSet>
#include <QTreeWidget>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QSlider;
class QSpinBox;
class QToolButton;
class QMenu;

class LayerTree : public QTreeWidget {
    Q_OBJECT
public:
    using QTreeWidget::QTreeWidget;
    bool isEditing() const { return state() == EditingState; }
signals:
    void layerDropped(QString layerID, QString targetID, int placement);
protected:
    void dropEvent(QDropEvent* event) override;
};

class LayersPanel : public QWidget {
    Q_OBJECT
public:
    explicit LayersPanel(QWidget* parent = nullptr);
    void setSession(Session* session);
    QCheckBox* maskTargetBox() const { return maskTarget_; }
    void setAdjustmentMenu(QMenu* menu);

signals:
    void editAdjustment(QString layerID);
    void contextMenuRequested(QPoint globalPosition);
    void newLayerRequested();
    void newGroupRequested();
    void addMaskRequested();
    void deleteRequested();

private:
    void rebuild();
    void refreshControls();
    QIcon thumbnail(const comp::Layer& layer);
    QIcon maskThumbnail(const comp::Layer& layer);

    QPointer<Session> session_;
    LayerTree* tree_;
    QComboBox* blend_;
    QSlider* opacitySlider_;
    QSpinBox* opacity_;
    QCheckBox* maskTarget_;
    QToolButton* adjustmentButton_;
    QSet<QString> collapsed_;
    QHash<quint64, QIcon> thumbnails_;
    bool updating_ = false;
    bool opacityEditing_ = false;
    QMetaObject::Connection documentConnection_, activeConnection_;
};
