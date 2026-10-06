#include "LayersPanel.h"
#include "QtBridge.h"
#include "compositor/adjustment.h"
#include "compositor/edit.h"
#include "compositor/image_io.h"
#include <cmath>
#include <QCheckBox>
#include <QComboBox>
#include <QDropEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QSlider>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>
#include <functional>

using namespace comp;

namespace {
constexpr int kIDRole = Qt::UserRole + 1;
constexpr int kThumb = 36;

QIcon placeholderIcon(const QString& glyph) {
    QPixmap pixmap(kThumb, kThumb);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QColor(150, 150, 150));
    p.setBrush(QColor(90, 90, 90, 60));
    p.drawRoundedRect(QRectF(1, 1, kThumb - 2, kThumb - 2), 4, 4);
    QFont font = p.font();
    font.setPixelSize(16);
    p.setFont(font);
    p.drawText(pixmap.rect(), Qt::AlignCenter, glyph);
    return QIcon(pixmap);
}

QPixmap framed(const QImage& image) {
    QPixmap pixmap(kThumb, kThumb);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    // 透明处显示棋盘格。
    for (int y = 0; y < kThumb; y += 6) for (int x = 0; x < kThumb; x += 6)
        p.fillRect(x, y, 6, 6, ((x + y) / 6) % 2 ? QColor(204, 204, 204) : Qt::white);
    QSize size = image.size().scaled(kThumb, kThumb, Qt::KeepAspectRatio);
    QRect target((kThumb - size.width()) / 2, (kThumb - size.height()) / 2, size.width(), size.height());
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.drawImage(target, image);
    p.setPen(QColor(0, 0, 0, 90));
    p.drawRect(pixmap.rect().adjusted(0, 0, -1, -1));
    return pixmap;
}
} // namespace

void LayerTree::dropEvent(QDropEvent* event) {
    QList<QTreeWidgetItem*> selected = selectedItems();
    if (selected.isEmpty()) { event->ignore(); return; }
    QString layerID = selected.first()->data(0, kIDRole).toString();
    QTreeWidgetItem* target = itemAt(event->position().toPoint());
    int placement = 1; // 之上
    QString targetID;
    if (!target) {
        // 拖到空白处：放到最底部。
        if (topLevelItemCount() == 0) { event->ignore(); return; }
        QTreeWidgetItem* bottom = topLevelItem(topLevelItemCount() - 1);
        targetID = bottom->data(0, kIDRole).toString();
        placement = -1;
    } else {
        targetID = target->data(0, kIDRole).toString();
        bool isGroup = target->data(0, kIDRole + 1).toBool();
        switch (dropIndicatorPosition()) {
        case AboveItem: placement = 1; break;
        case BelowItem: placement = (isGroup && target->isExpanded()) ? 0 : -1; break;
        case OnItem: placement = isGroup ? 0 : 1; break;
        default: placement = -1; break;
        }
    }
    event->setDropAction(Qt::IgnoreAction);
    event->accept();
    if (layerID != targetID) emit layerDropped(layerID, targetID, placement);
}

LayersPanel::LayersPanel(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(6);

    auto* row = new QHBoxLayout;
    blend_ = new QComboBox;
    for (const auto& group : blendModeGroups()) {
        if (blend_->count()) blend_->insertSeparator(blend_->count());
        for (BlendMode mode : group) blend_->addItem(QString::fromUtf8(blendModeName(mode)), int(mode));
    }
    blend_->setToolTip(QStringLiteral("混合模式"));
    row->addWidget(blend_, 1);
    opacity_ = new QSpinBox;
    opacity_->setRange(0, 100);
    opacity_->setSuffix(QStringLiteral("%"));
    opacity_->setToolTip(QStringLiteral("不透明度"));
    row->addWidget(opacity_);
    layout->addLayout(row);
    opacitySlider_ = new QSlider(Qt::Horizontal);
    opacitySlider_->setRange(0, 100);
    layout->addWidget(opacitySlider_);

    tree_ = new LayerTree;
    tree_->setHeaderHidden(true);
    tree_->setColumnCount(2);
    tree_->header()->setStretchLastSection(false);
    tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree_->header()->setSectionResizeMode(1, QHeaderView::Fixed);
    tree_->header()->resizeSection(1, kThumb + 8);
    tree_->setIconSize(QSize(kThumb, kThumb));
    tree_->setDragDropMode(QAbstractItemView::InternalMove);
    tree_->setDefaultDropAction(Qt::MoveAction);
    tree_->setSelectionMode(QAbstractItemView::SingleSelection);
    tree_->setEditTriggers(QAbstractItemView::EditKeyPressed);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    tree_->setIndentation(16);
    tree_->setUniformRowHeights(true);
    layout->addWidget(tree_, 1);

    maskTarget_ = new QCheckBox(QStringLiteral("绘制到蒙版"));
    maskTarget_->setToolTip(QStringLiteral("勾选后画笔和橡皮擦作用在选中图层的蒙版上（黑色隐藏，白色显示）"));
    layout->addWidget(maskTarget_);

    auto* buttons = new QHBoxLayout;
    auto button = [&](const QString& text, const QString& tip) {
        auto* b = new QToolButton;
        b->setText(text);
        b->setToolTip(tip);
        b->setAutoRaise(true);
        buttons->addWidget(b);
        return b;
    };
    auto* newLayer = button(QStringLiteral("＋"), QStringLiteral("新建图层"));
    auto* newGroup = button(QString(), QStringLiteral("新建组"));
    newGroup->setIcon(style()->standardIcon(QStyle::SP_FileDialogNewFolder));
    auto* addMask = button(QStringLiteral("◐"), QStringLiteral("添加蒙版"));
    adjustmentButton_ = button(QStringLiteral("◑"), QStringLiteral("新建调整图层"));
    adjustmentButton_->setPopupMode(QToolButton::InstantPopup);
    buttons->addStretch();
    auto* remove = button(QString(), QStringLiteral("删除图层"));
    remove->setIcon(style()->standardIcon(QStyle::SP_TrashIcon));
    layout->addLayout(buttons);

    connect(newLayer, &QToolButton::clicked, this, &LayersPanel::newLayerRequested);
    connect(newGroup, &QToolButton::clicked, this, &LayersPanel::newGroupRequested);
    connect(addMask, &QToolButton::clicked, this, &LayersPanel::addMaskRequested);
    connect(remove, &QToolButton::clicked, this, &LayersPanel::deleteRequested);
    connect(tree_, &QTreeWidget::customContextMenuRequested, this, [this](QPoint p) {
        if (QTreeWidgetItem* item = tree_->itemAt(p)) tree_->setCurrentItem(item);
        emit contextMenuRequested(tree_->viewport()->mapToGlobal(p));
    });

    connect(tree_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item) {
        if (updating_ || !session_ || !item) return;
        session_->setActiveLayer(item->data(0, kIDRole).toString().toStdString());
        refreshControls();
    });
    connect(tree_, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int column) {
        if (updating_ || !session_ || column != 0) return;
        std::string id = item->data(0, kIDRole).toString().toStdString();
        const Layer* layer = session_->doc().find(id);
        if (!layer) return;
        bool visible = item->checkState(0) == Qt::Checked;
        QString name = item->text(0).trimmed();
        if (visible != layer->visible) {
            session_->edit(visible ? QStringLiteral("显示图层") : QStringLiteral("隐藏图层"),
                           [&](Document& d) { d.find(id)->visible = visible; }, false);
        } else if (!name.isEmpty() && name.toStdString() != layer->name) {
            session_->edit(QStringLiteral("重命名图层"), [&](Document& d) { d.find(id)->name = name.toStdString(); });
        } else if (name.isEmpty()) {
            rebuild();
        }
    });
    connect(tree_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int column) {
        if (!session_) return;
        QString id = item->data(0, kIDRole).toString();
        const Layer* layer = session_->doc().find(id.toStdString());
        if (layer && layer->isAdjustment() && column == 0) emit editAdjustment(id);
        else if (column == 0) tree_->editItem(item, 0);
    });
    connect(tree_, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem* item) { collapsed_.remove(item->data(0, kIDRole).toString()); });
    connect(tree_, &QTreeWidget::itemCollapsed, this, [this](QTreeWidgetItem* item) { collapsed_.insert(item->data(0, kIDRole).toString()); });
    connect(tree_, &LayerTree::layerDropped, this, [this](QString layerID, QString targetID, int placement) {
        if (!session_) return;
        Document copy = session_->doc();
        if (!moveLayerTo(copy, layerID.toStdString(), targetID.toStdString(), DropPlacement(placement))) { rebuild(); return; }
        session_->edit(QStringLiteral("移动图层"), [&](Document& d) { d = std::move(copy); d.activeLayerID = layerID.toStdString(); });
    });

    connect(blend_, QOverload<int>::of(&QComboBox::activated), this, [this](int index) {
        if (!session_) return;
        Layer* layer = session_->doc().find(session_->doc().activeLayerID);
        if (!layer || layer->isGroup) return;
        BlendMode mode = BlendMode(blend_->itemData(index).toInt());
        if (mode == layer->blendMode) return;
        std::string id = layer->id;
        session_->edit(QStringLiteral("混合模式"), [&](Document& d) { d.find(id)->blendMode = mode; }, false);
    });
    // 拖动滑块时只预览，松开后记为一步撤销。
    connect(opacitySlider_, &QSlider::sliderPressed, this, [this] {
        if (!session_) return;
        session_->checkpoint(QStringLiteral("不透明度"));
        opacityEditing_ = true;
    });
    connect(opacitySlider_, &QSlider::sliderReleased, this, [this] { opacityEditing_ = false; if (session_) session_->notifyChanged(false); });
    connect(opacitySlider_, &QSlider::valueChanged, this, [this](int value) {
        if (updating_ || !session_) return;
        Layer* layer = session_->doc().find(session_->doc().activeLayerID);
        if (!layer) return;
        updating_ = true;
        opacity_->setValue(value);
        updating_ = false;
        if (opacityEditing_) {
            layer->opacity = value / 100.0;
            session_->notifyChanged(false);
        } else {
            std::string id = layer->id;
            session_->edit(QStringLiteral("不透明度"), [&](Document& d) { d.find(id)->opacity = value / 100.0; }, false);
        }
    });
    connect(opacity_, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int value) {
        if (updating_) return;
        opacitySlider_->setValue(value);
    });
    setEnabled(false);
}

void LayersPanel::setAdjustmentMenu(QMenu* menu) { adjustmentButton_->setMenu(menu); }

void LayersPanel::setSession(Session* session) {
    disconnect(documentConnection_);
    disconnect(activeConnection_);
    session_ = session;
    thumbnails_.clear();
    setEnabled(session != nullptr);
    if (session) {
        documentConnection_ = connect(session, &Session::documentChanged, this, [this](bool) {
            // 正在重命名时不要重建，免得打断输入。
            if (tree_->isEditing()) return;
            rebuild();
        });
        activeConnection_ = connect(session, &Session::activeLayerChanged, this, [this] {
            if (updating_ || !session_) return;
            updating_ = true;
            QString id = QString::fromStdString(session_->doc().activeLayerID);
            QTreeWidgetItemIterator it(tree_);
            while (*it) {
                if ((*it)->data(0, kIDRole).toString() == id) { tree_->setCurrentItem(*it); break; }
                ++it;
            }
            updating_ = false;
            refreshControls();
        });
    }
    rebuild();
}

QIcon LayersPanel::thumbnail(const Layer& layer) {
    if (layer.isGroup) return style()->standardIcon(QStyle::SP_DirIcon);
    if (layer.isAdjustment()) return placeholderIcon(QStringLiteral("◑"));
    if (!layer.image) return placeholderIcon(QString());
    auto it = thumbnails_.find(layer.image->serial);
    if (it != thumbnails_.end()) return *it;
    QIcon icon(framed(toQImage(scaledToFit(*layer.image, kThumb * 2))));
    if (thumbnails_.size() > 2000) thumbnails_.clear();
    thumbnails_.insert(layer.image->serial, icon);
    return icon;
}

QIcon LayersPanel::maskThumbnail(const Layer& layer) {
    if (!layer.mask) return QIcon();
    quint64 key = layer.mask->serial | (quint64(1) << 63);
    auto it = thumbnails_.find(key);
    if (it != thumbnails_.end()) return *it;
    QImage gray = toQImage(*layer.mask);
    QPixmap pixmap = framed(gray.scaled(kThumb * 2, kThumb * 2, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    if (!layer.maskEnabled) {
        QPainter p(&pixmap);
        p.setPen(QPen(Qt::red, 2));
        p.drawLine(4, 4, kThumb - 4, kThumb - 4);
        p.drawLine(kThumb - 4, 4, 4, kThumb - 4);
    }
    QIcon icon(pixmap);
    thumbnails_.insert(key, icon);
    return icon;
}

void LayersPanel::rebuild() {
    updating_ = true;
    tree_->clear();
    if (!session_) { updating_ = false; refreshControls(); return; }
    const Document& doc = session_->doc();
    QTreeWidgetItem* current = nullptr;
    std::function<void(QTreeWidgetItem*, const std::string&)> add = [&](QTreeWidgetItem* parent, const std::string& parentID) {
        std::vector<const Layer*> children;
        for (const auto& l : doc.layers) if (l.parentID == parentID) children.push_back(&l);
        // 面板从上到下：反过来。
        for (auto it = children.rbegin(); it != children.rend(); ++it) {
            const Layer& layer = **it;
            auto* item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(tree_);
            QString name = QString::fromStdString(layer.name);
            item->setText(0, name);
            item->setData(0, kIDRole, QString::fromStdString(layer.id));
            item->setData(0, kIDRole + 1, layer.isGroup);
            item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable | Qt::ItemIsEditable
                           | Qt::ItemIsDragEnabled | (layer.isGroup ? Qt::ItemIsDropEnabled : Qt::NoItemFlags));
            item->setCheckState(0, layer.visible ? Qt::Checked : Qt::Unchecked);
            item->setIcon(0, thumbnail(layer));
            if (layer.mask) item->setIcon(1, maskThumbnail(layer));
            QStringList notes;
            if (!layer.maskSourceID.empty()) notes << QStringLiteral("剪贴到下方图层");
            if (layer.blendMode != BlendMode::Normal) notes << QString::fromUtf8(blendModeName(layer.blendMode));
            if (layer.opacity < 1) notes << QStringLiteral("%1%").arg(int(std::lround(layer.opacity * 100)));
            if (layer.hasText()) notes << QStringLiteral("文字（以像素显示）");
            if (layer.extra.contains("effects")) notes << QStringLiteral("图层效果（保留，暂不显示）");
            if (layer.isAdjustment()) notes << QStringLiteral("双击编辑");
            item->setToolTip(0, notes.isEmpty() ? name : name + QStringLiteral("\n") + notes.join(QStringLiteral("，")));
            if (!layer.maskSourceID.empty()) item->setText(0, QStringLiteral("↳ ") + name);
            if (!doc.isEffectivelyVisible(layer)) item->setForeground(0, QColor(140, 140, 140));
            if (layer.id == doc.activeLayerID) current = item;
            if (layer.isGroup) {
                add(item, layer.id);
                item->setExpanded(!collapsed_.contains(QString::fromStdString(layer.id)));
            }
        }
    };
    add(nullptr, std::string());
    if (current) tree_->setCurrentItem(current);
    updating_ = false;
    refreshControls();
}

void LayersPanel::refreshControls() {
    updating_ = true;
    const Layer* layer = session_ ? session_->doc().find(session_->doc().activeLayerID) : nullptr;
    blend_->setEnabled(layer && !layer->isGroup);
    opacity_->setEnabled(layer);
    opacitySlider_->setEnabled(layer);
    if (layer) {
        int index = blend_->findData(int(layer->blendMode));
        if (index >= 0) blend_->setCurrentIndex(index);
        int value = int(std::lround(layer->opacity * 100));
        opacity_->setValue(value);
        if (!opacityEditing_) opacitySlider_->setValue(value);
    }
    maskTarget_->setEnabled(layer && layer->mask);
    updating_ = false;
}
