#include "MainWindow.h"
#include "AdjustmentDialog.h"
#include "CanvasView.h"
#include "Dialogs.h"
#include "EffectsDialog.h"
#include "LayersPanel.h"
#include "QtBridge.h"
#include "Session.h"
#include "compositor/adjustment.h"
#include "compositor/edit.h"
#include "compositor/image_io.h"
#include "compositor/project_io.h"
#include "compositor/renderer.h"
#include "compositor/selection.h"
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDir>
#include <QCloseEvent>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QImageReader>
#include <QInputDialog>
#include <QComboBox>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QRandomGenerator>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTabWidget>
#include <QToolBar>
#include <QUrl>
#include <cmath>

using namespace comp;

namespace {
const QString kImageFilter = QStringLiteral("*.png *.jpg *.jpeg *.bmp *.gif *.tga *.psd *.webp *.tif *.tiff");
}

MainWindow::MainWindow() {
    setWindowTitle(QStringLiteral("Compositor"));
    setAcceptDrops(true);
    tabs_ = new QTabWidget;
    tabs_->setTabsClosable(true);
    tabs_->setMovable(true);
    tabs_->setDocumentMode(true);
    auto* welcome = new QLabel(QStringLiteral("<p style='font-size:16px'>新建画布（Ctrl+N）或打开项目（Ctrl+O）</p>"
                                              "<p style='color:gray'>也可以把图片或 .comp 项目文件夹拖到这里</p>"));
    welcome->setAlignment(Qt::AlignCenter);
    stack_ = new QStackedWidget;
    stack_->addWidget(welcome);
    stack_->addWidget(tabs_);
    setCentralWidget(stack_);
    connect(tabs_, &QTabWidget::tabCloseRequested, this, [this](int index) { closeTab(index); });
    connect(tabs_, &QTabWidget::currentChanged, this, [this] {
        stack_->setCurrentIndex(tabs_->count() ? 1 : 0);
        layers_->setSession(currentSession());
        updateActions();
        updateTitle();
        if (CanvasView* canvas = currentCanvas()) {
            zoomLabel_->setText(QStringLiteral("%1%").arg(canvas->zoom() * devicePixelRatioF() * 100, 0, 'f', 1));
            sizeLabel_->setText(QStringLiteral("%1 × %2 像素").arg(canvas->session()->doc().width).arg(canvas->session()->doc().height));
        } else {
            zoomLabel_->clear();
            sizeLabel_->clear();
        }
    });

    zoomLabel_ = new QLabel;
    positionLabel_ = new QLabel;
    sizeLabel_ = new QLabel;
    statusBar()->addPermanentWidget(positionLabel_);
    statusBar()->addPermanentWidget(sizeLabel_);
    statusBar()->addPermanentWidget(zoomLabel_);

    createActions();
    createDocks();
    createMenus();
    createToolBars();
    updateActions();

    QSettings settings;
    restoreGeometry(settings.value(QStringLiteral("window/geometry")).toByteArray());
    restoreState(settings.value(QStringLiteral("window/state")).toByteArray());
    if (!settings.contains(QStringLiteral("window/geometry"))) resize(1400, 900);
}

void MainWindow::createActions() {
    auto add = [this](const QString& key, const QString& text, const QKeySequence& shortcut, auto slot) {
        auto* action = new QAction(text, this);
        action->setObjectName(key);
        if (!shortcut.isEmpty()) action->setShortcut(shortcut);
        connect(action, &QAction::triggered, this, slot);
        actions_.insert(key, action);
        addAction(action);
        return action;
    };
    // 文件
    add("new", QStringLiteral("新建…"), QKeySequence::New, [this] { newDocument(); });
    add("open", QStringLiteral("打开…"), QKeySequence::Open, [this] { openDialog(); });
    add("openFolder", QStringLiteral("打开项目文件夹…"), QKeySequence(QStringLiteral("Ctrl+Shift+O")), [this] { openFolderDialog(); });
    add("save", QStringLiteral("保存"), QKeySequence::Save, [this] { if (auto* s = currentSession()) saveSession(s, false); });
    add("saveAs", QStringLiteral("另存为…"), QKeySequence(QStringLiteral("Ctrl+Shift+S")), [this] { if (auto* s = currentSession()) saveSession(s, true); });
    add("place", QStringLiteral("置入图片为图层…"), QKeySequence(QStringLiteral("Ctrl+Shift+P")), [this] { placeDialog(); });
    add("exportPNG", QStringLiteral("导出为 PNG…"), QKeySequence(QStringLiteral("Ctrl+Alt+E")), [this] { exportPNG(); });
    add("exportJPEG", QStringLiteral("导出为 JPEG…"), QKeySequence(QStringLiteral("Ctrl+Shift+Alt+S")), [this] { exportJPEG(); });
    add("close", QStringLiteral("关闭"), QKeySequence(QStringLiteral("Ctrl+W")), [this] { closeTab(tabs_->currentIndex()); });
    add("quit", QStringLiteral("退出"), QKeySequence(QStringLiteral("Ctrl+Q")), [this] { close(); });
    // 编辑
    add("undo", QStringLiteral("撤销"), QKeySequence::Undo, [this] { if (auto* s = currentSession()) s->undo(); });
    auto* redo = add("redo", QStringLiteral("重做"), QKeySequence(QStringLiteral("Ctrl+Shift+Z")), [this] { if (auto* s = currentSession()) s->redo(); });
    redo->setShortcuts({QKeySequence(QStringLiteral("Ctrl+Shift+Z")), QKeySequence(QStringLiteral("Ctrl+Y"))});
    add("copy", QStringLiteral("拷贝图层"), QKeySequence::Copy, [this] { copyLayer(false); });
    add("copyMerged", QStringLiteral("合并拷贝"), QKeySequence(QStringLiteral("Ctrl+Shift+C")), [this] { copyLayer(true); });
    add("paste", QStringLiteral("粘贴为新图层"), QKeySequence::Paste, [this] { paste(); });
    add("fill", QStringLiteral("用前景色填充"), QKeySequence(QStringLiteral("Alt+Backspace")), [this] { fillCurrent(); });
    add("contentFill", QStringLiteral("内容识别填充"), QKeySequence(QStringLiteral("Shift+Backspace")), [this] { contentFill(); });
    add("cut", QStringLiteral("剪切"), QKeySequence::Cut, [this] { cut(); });
    add("clear", QStringLiteral("清除选区内容"), QKeySequence(), [this] { clearOrDelete(); });
    // 选择
    add("selectAll", QStringLiteral("全部"), QKeySequence::SelectAll, [this] {
        editCurrent(QStringLiteral("全选"), [](Document& d) { d.selection = std::make_shared<GrayImage>(d.width, d.height, 255); });
    });
    add("deselect", QStringLiteral("取消选择"), QKeySequence(QStringLiteral("Ctrl+D")), [this] {
        Session* s = currentSession();
        if (s && s->doc().selection) editCurrent(QStringLiteral("取消选择"), [](Document& d) { d.selection.reset(); });
    });
    add("inverse", QStringLiteral("反选"), QKeySequence(QStringLiteral("Ctrl+Shift+I")), [this] {
        editCurrent(QStringLiteral("反选"), [](Document& d) {
            GrayImage inverted = invertSelection(d.selection.get(), d.width, d.height);
            d.selection = selectionIsEmpty(inverted) ? nullptr : std::make_shared<GrayImage>(std::move(inverted));
        });
    });
    add("feather", QStringLiteral("羽化…"), QKeySequence(QStringLiteral("Shift+F6")), [this] { modifySelection(0); });
    add("expand", QStringLiteral("扩展…"), QKeySequence(), [this] { modifySelection(1); });
    add("contract", QStringLiteral("收缩…"), QKeySequence(), [this] { modifySelection(2); });
    add("loadSelection", QStringLiteral("载入图层选区"), QKeySequence(), [this] {
        Session* s = currentSession();
        if (!s) return;
        const Layer* layer = s->doc().find(s->doc().activeLayerID);
        if (!layer || layer->isGroup || layer->isAdjustment()) return;
        GrayImage selection = selectionFromLayer(s->doc(), *layer);
        editCurrent(QStringLiteral("载入选区"), [&](Document& d) {
            d.selection = selectionIsEmpty(selection) ? nullptr : std::make_shared<GrayImage>(std::move(selection));
        });
    });
    add("crop", QStringLiteral("裁剪到选区"), QKeySequence(), [this] {
        editCurrent(QStringLiteral("裁剪"), [](Document& d) { cropToSelection(d); });
        if (auto* c = currentCanvas()) c->fitToWindow();
    });
    // 图像
    add("canvasSize", QStringLiteral("画布大小…"), QKeySequence(QStringLiteral("Ctrl+Alt+C")), [this] { canvasSize(); });
    add("imageSize", QStringLiteral("图像大小…"), QKeySequence(QStringLiteral("Ctrl+Alt+I")), [this] { imageSize(); });
    add("flipCanvasH", QStringLiteral("水平翻转画布"), QKeySequence(), [this] { editCurrent(QStringLiteral("水平翻转画布"), [](Document& d) { flipCanvas(d, true); }); });
    add("flipCanvasV", QStringLiteral("垂直翻转画布"), QKeySequence(), [this] { editCurrent(QStringLiteral("垂直翻转画布"), [](Document& d) { flipCanvas(d, false); }); });
    add("flatten", QStringLiteral("拼合图像"), QKeySequence(), [this] { editCurrent(QStringLiteral("拼合图像"), [](Document& d) { flattenDocument(d); }); });
    // 图层
    add("newLayer", QStringLiteral("新建图层"), QKeySequence(QStringLiteral("Ctrl+Shift+N")), [this] { editCurrent(QStringLiteral("新建图层"), [](Document& d) { addBlankLayer(d, std::string()); }); });
    add("newGroup", QStringLiteral("新建组"), QKeySequence(), [this] { editCurrent(QStringLiteral("新建组"), [](Document& d) { addGroup(d, std::string()); }); });
    add("groupLayer", QStringLiteral("编组"), QKeySequence(QStringLiteral("Ctrl+G")), [this] {
        editCurrent(QStringLiteral("编组"), [](Document& d) { groupLayer(d, d.activeLayerID); });
    });
    add("duplicate", QStringLiteral("复制图层"), QKeySequence(QStringLiteral("Ctrl+J")), [this] { editCurrent(QStringLiteral("复制图层"), [](Document& d) { duplicateLayer(d, d.activeLayerID); }); });
    add("delete", QStringLiteral("删除图层"), QKeySequence(), [this] {
        Session* s = currentSession();
        if (!s || !s->doc().find(s->doc().activeLayerID)) return;
        editCurrent(QStringLiteral("删除图层"), [](Document& d) { deleteLayer(d, d.activeLayerID); });
    });
    add("mergeDown", QStringLiteral("向下合并"), QKeySequence(QStringLiteral("Ctrl+E")), [this] {
        Session* s = currentSession();
        if (!s) return;
        Document copy = s->doc();
        if (!mergeDown(copy, copy.activeLayerID)) { statusBar()->showMessage(QStringLiteral("下方需要有一个同级的像素图层才能合并。"), 4000); return; }
        editCurrent(QStringLiteral("向下合并"), [&](Document& d) { d = std::move(copy); });
    });
    add("clip", QStringLiteral("创建/释放剪贴蒙版"), QKeySequence(QStringLiteral("Ctrl+Alt+G")), [this] {
        Session* s = currentSession();
        if (!s) return;
        Document copy = s->doc();
        if (!toggleClipping(copy, copy.activeLayerID)) { statusBar()->showMessage(QStringLiteral("下方需要有一个同级的像素图层作为剪贴基底。"), 4000); return; }
        editCurrent(QStringLiteral("剪贴蒙版"), [&](Document& d) { d = std::move(copy); });
    });
    add("raise", QStringLiteral("上移一层"), QKeySequence(QStringLiteral("Ctrl+]")), [this] { editCurrent(QStringLiteral("上移图层"), [](Document& d) { moveLayer(d, d.activeLayerID, 1); }); });
    add("lower", QStringLiteral("下移一层"), QKeySequence(QStringLiteral("Ctrl+[")), [this] { editCurrent(QStringLiteral("下移图层"), [](Document& d) { moveLayer(d, d.activeLayerID, -1); }); });
    add("flipLayerH", QStringLiteral("水平翻转图层"), QKeySequence(), [this] { editCurrent(QStringLiteral("水平翻转图层"), [](Document& d) { flipLayer(d, d.activeLayerID, true); }); });
    add("flipLayerV", QStringLiteral("垂直翻转图层"), QKeySequence(), [this] { editCurrent(QStringLiteral("垂直翻转图层"), [](Document& d) { flipLayer(d, d.activeLayerID, false); }); });
    add("addMask", QStringLiteral("添加蒙版（有选区时显示选区）"), QKeySequence(), [this] {
        editCurrent(QStringLiteral("添加蒙版"), [](Document& d) {
            if (d.selection) maskFromSelection(d, d.activeLayerID, *d.selection);
            else addMask(d, d.activeLayerID, true);
        });
    });
    add("addMaskHide", QStringLiteral("添加蒙版（隐藏全部）"), QKeySequence(), [this] { editCurrent(QStringLiteral("添加蒙版"), [](Document& d) { addMask(d, d.activeLayerID, false); }); });
    add("toggleMask", QStringLiteral("停用/启用蒙版"), QKeySequence(), [this] {
        editCurrent(QStringLiteral("停用蒙版"), [](Document& d) { if (Layer* l = d.find(d.activeLayerID); l && l->mask) l->maskEnabled = !l->maskEnabled; });
    });
    add("invertMask", QStringLiteral("反相蒙版"), QKeySequence(), [this] { editCurrent(QStringLiteral("反相蒙版"), [](Document& d) { invertMask(d, d.activeLayerID); }); });
    add("applyMask", QStringLiteral("应用蒙版"), QKeySequence(), [this] { editCurrent(QStringLiteral("应用蒙版"), [](Document& d) { applyMask(d, d.activeLayerID); }); });
    add("deleteMask", QStringLiteral("删除蒙版"), QKeySequence(), [this] { editCurrent(QStringLiteral("删除蒙版"), [](Document& d) { deleteMask(d, d.activeLayerID); }); });
    add("effects", QStringLiteral("图层效果…"), QKeySequence(QStringLiteral("Ctrl+Shift+F")), [this] { editEffects(); });
    add("clearEffects", QStringLiteral("清除图层效果"), QKeySequence(), [this] {
        editCurrent(QStringLiteral("清除图层效果"), [](Document& d) { if (Layer* l = d.find(d.activeLayerID)) l->extra.erase("effects"); });
    });
    add("editAdjustment", QStringLiteral("编辑调整…"), QKeySequence(), [this] {
        if (Session* s = currentSession()) editAdjustment(QString::fromStdString(s->doc().activeLayerID));
    });
    // 视图
    add("zoomIn", QStringLiteral("放大"), QKeySequence::ZoomIn, [this] { if (auto* c = currentCanvas()) c->zoomIn(); });
    actions_["zoomIn"]->setShortcuts({QKeySequence(QStringLiteral("Ctrl+=")), QKeySequence(QStringLiteral("Ctrl++"))});
    add("zoomOut", QStringLiteral("缩小"), QKeySequence::ZoomOut, [this] { if (auto* c = currentCanvas()) c->zoomOut(); });
    add("fit", QStringLiteral("按屏幕大小缩放"), QKeySequence(QStringLiteral("Ctrl+0")), [this] { if (auto* c = currentCanvas()) c->fitToWindow(); });
    add("actual", QStringLiteral("100%"), QKeySequence(QStringLiteral("Ctrl+1")), [this] { if (auto* c = currentCanvas()) c->actualPixels(); });
    auto* guides = add("guides", QStringLiteral("显示参考线"), QKeySequence(QStringLiteral("Ctrl+;")), [this](bool on) {
        for (int i = 0; i < tabs_->count(); ++i) if (auto* c = qobject_cast<CanvasView*>(tabs_->widget(i))) c->setShowGuides(on);
    });
    guides->setCheckable(true);
    guides->setChecked(true);
    auto* grid = add("pixelGrid", QStringLiteral("显示像素网格"), QKeySequence(), [this](bool on) {
        for (int i = 0; i < tabs_->count(); ++i) if (auto* c = qobject_cast<CanvasView*>(tabs_->widget(i))) c->setShowPixelGrid(on);
    });
    grid->setCheckable(true);
    grid->setChecked(true);
    add("about", QStringLiteral("关于 Compositor"), QKeySequence(), [this] { showAbout(); });

    // 画笔大小 [ ]，颜色 X / D。
    auto* smaller = new QAction(this);
    smaller->setShortcut(QKeySequence(QStringLiteral("[")));
    connect(smaller, &QAction::triggered, this, [this] { brushSize_->setValue(std::max(1, int(brushSize_->value() * 0.85))); });
    addAction(smaller);
    auto* bigger = new QAction(this);
    bigger->setShortcut(QKeySequence(QStringLiteral("]")));
    connect(bigger, &QAction::triggered, this, [this] { brushSize_->setValue(std::max(brushSize_->value() + 1, int(brushSize_->value() * 1.15))); });
    addAction(bigger);
    auto* swap = new QAction(this);
    swap->setShortcut(QKeySequence(QStringLiteral("X")));
    connect(swap, &QAction::triggered, this, [this] {
        std::swap(tools_.foreground, tools_.background);
        foreground_->setColor(tools_.foreground);
        background_->setColor(tools_.background);
    });
    addAction(swap);
    auto* reset = new QAction(this);
    reset->setShortcut(QKeySequence(QStringLiteral("D")));
    connect(reset, &QAction::triggered, this, [this] {
        tools_.foreground = Qt::black; tools_.background = Qt::white;
        foreground_->setColor(tools_.foreground);
        background_->setColor(tools_.background);
    });
    addAction(reset);
    auto* deleteKey = new QAction(this);
    deleteKey->setShortcut(QKeySequence::Delete);
    connect(deleteKey, &QAction::triggered, this, [this] { clearOrDelete(); });
    addAction(deleteKey);

    adjustmentMenu_ = new QMenu(QStringLiteral("新建调整图层"), this);
    for (AdjustmentKind kind : allAdjustmentKinds()) {
        QAction* a = adjustmentMenu_->addAction(QString::fromUtf8(adjustmentKindLabel(kind)));
        a->setObjectName(QStringLiteral("adjust_") + QString::fromUtf8(adjustmentKindName(kind)));
        connect(a, &QAction::triggered, this, [this, kind] { addAdjustment(int(kind)); });
    }
}

void MainWindow::createMenus() {
    QMenu* file = menuBar()->addMenu(QStringLiteral("文件(&F)"));
    file->addAction(actions_["new"]);
    file->addAction(actions_["open"]);
    file->addAction(actions_["openFolder"]);
    recentMenu_ = file->addMenu(QStringLiteral("最近打开"));
    updateRecentMenu();
    file->addSeparator();
    file->addAction(actions_["save"]);
    file->addAction(actions_["saveAs"]);
    file->addSeparator();
    file->addAction(actions_["place"]);
    file->addAction(actions_["exportPNG"]);
    file->addAction(actions_["exportJPEG"]);
    file->addSeparator();
    file->addAction(actions_["close"]);
    file->addAction(actions_["quit"]);

    QMenu* edit = menuBar()->addMenu(QStringLiteral("编辑(&E)"));
    edit->addAction(actions_["undo"]);
    edit->addAction(actions_["redo"]);
    edit->addSeparator();
    edit->addAction(actions_["cut"]);
    edit->addAction(actions_["copy"]);
    edit->addAction(actions_["copyMerged"]);
    edit->addAction(actions_["paste"]);
    edit->addAction(actions_["clear"]);
    edit->addSeparator();
    edit->addAction(actions_["fill"]);
    edit->addAction(actions_["contentFill"]);

    QMenu* image = menuBar()->addMenu(QStringLiteral("图像(&I)"));
    image->addAction(actions_["imageSize"]);
    image->addAction(actions_["canvasSize"]);
    image->addAction(actions_["crop"]);
    image->addSeparator();
    image->addAction(actions_["flipCanvasH"]);
    image->addAction(actions_["flipCanvasV"]);
    image->addSeparator();
    image->addAction(actions_["flatten"]);

    QMenu* layer = menuBar()->addMenu(QStringLiteral("图层(&L)"));
    layer->addAction(actions_["newLayer"]);
    layer->addAction(actions_["newGroup"]);
    layer->addMenu(adjustmentMenu_);
    layer->addSeparator();
    layer->addAction(actions_["duplicate"]);
    layer->addAction(actions_["delete"]);
    layer->addAction(actions_["groupLayer"]);
    layer->addAction(actions_["mergeDown"]);
    layer->addAction(actions_["clip"]);
    layer->addSeparator();
    layer->addAction(actions_["raise"]);
    layer->addAction(actions_["lower"]);
    layer->addAction(actions_["flipLayerH"]);
    layer->addAction(actions_["flipLayerV"]);
    layer->addSeparator();
    QMenu* mask = layer->addMenu(QStringLiteral("图层蒙版"));
    for (const char* key : {"addMask", "addMaskHide", "toggleMask", "invertMask", "applyMask", "deleteMask"}) mask->addAction(actions_[key]);
    layer->addAction(actions_["effects"]);
    layer->addAction(actions_["clearEffects"]);
    layer->addAction(actions_["editAdjustment"]);

    QMenu* select = menuBar()->addMenu(QStringLiteral("选择(&S)"));
    select->addAction(actions_["selectAll"]);
    select->addAction(actions_["deselect"]);
    select->addAction(actions_["inverse"]);
    select->addSeparator();
    select->addAction(actions_["feather"]);
    select->addAction(actions_["expand"]);
    select->addAction(actions_["contract"]);
    select->addSeparator();
    select->addAction(actions_["loadSelection"]);

    QMenu* view = menuBar()->addMenu(QStringLiteral("视图(&V)"));
    view->addAction(actions_["zoomIn"]);
    view->addAction(actions_["zoomOut"]);
    view->addAction(actions_["fit"]);
    view->addAction(actions_["actual"]);
    view->addSeparator();
    view->addAction(actions_["guides"]);
    view->addAction(actions_["pixelGrid"]);

    QMenu* help = menuBar()->addMenu(QStringLiteral("帮助(&H)"));
    help->addAction(actions_["about"]);

    layerContextMenu_ = new QMenu(this);
    for (const char* key : {"newLayer", "newGroup", "duplicate", "delete", "groupLayer", "mergeDown", "clip"}) layerContextMenu_->addAction(actions_[key]);
    layerContextMenu_->addSeparator();
    for (const char* key : {"addMask", "toggleMask", "invertMask", "applyMask", "deleteMask"}) layerContextMenu_->addAction(actions_[key]);
    layerContextMenu_->addAction(actions_["loadSelection"]);
    layerContextMenu_->addSeparator();
    layerContextMenu_->addAction(actions_["effects"]);
    layerContextMenu_->addAction(actions_["clearEffects"]);
    layerContextMenu_->addAction(actions_["editAdjustment"]);
    layerContextMenu_->addMenu(adjustmentMenu_);
}

void MainWindow::createToolBars() {
    QToolBar* toolsBar = new QToolBar(QStringLiteral("工具"));
    toolsBar->setObjectName(QStringLiteral("tools"));
    toolsBar->setMovable(false);
    toolsBar->setToolButtonStyle(Qt::ToolButtonTextOnly);
    addToolBar(Qt::LeftToolBarArea, toolsBar);
    toolGroup_ = new QActionGroup(this);
    struct ToolInfo { Tool tool; const char* name; const char* key; const char* tip; };
    const ToolInfo infos[] = {
        {Tool::Move, "移动", "V", "移动工具（V）：拖动移动图层，拖动手柄缩放（Shift 等比，Alt 以中心），在角外拖动旋转"},
        {Tool::Marquee, "选框", "M", "选框工具（M）：拖出矩形或椭圆选区。按下时 Shift 添加、Alt 减去、Shift+Alt 交叉；拖动中 Shift 约束比例、Alt 从中心"},
        {Tool::Lasso, "套索", "L", "套索工具（L）：自由或多边形。多边形模式下单击加点，双击、回车或点回起点闭合，Esc 取消"},
        {Tool::Wand, "魔棒", "W", "魔棒工具（W）：选取相近的颜色"},
        {Tool::Brush, "画笔", "B", "画笔工具（B）：Shift 点击画直线，[ ] 调整大小"},
        {Tool::Eraser, "橡皮", "E", "橡皮擦工具（E）"},
        {Tool::Heal, "修复", "J", "污点修复画笔（J）：涂抹瑕疵，松开后用周围的纹理修补"},
        {Tool::Clone, "仿制", "S", "仿制图章（S）：按住 Alt 点击设置取样点，再在别处涂抹"},
        {Tool::Gradient, "渐变", "G", "渐变工具（G）：拖出渐变，Shift 约束角度；有选区时只填选区"},
        {Tool::Eyedropper, "吸管", "I", "吸管工具（I）：点击取前景色，Alt 点击取背景色"},
        {Tool::Hand, "抓手", "H", "抓手工具（H）：拖动画布；任何工具下按住空格也可以"},
        {Tool::Zoom, "缩放", "Z", "缩放工具（Z）：点击放大，Alt 点击缩小"},
    };
    for (const auto& info : infos) {
        QAction* action = toolsBar->addAction(QString::fromUtf8(info.name));
        action->setObjectName(QStringLiteral("tool_") + QString::fromLatin1(info.key));
        action->setCheckable(true);
        action->setShortcut(QKeySequence(QString::fromLatin1(info.key)));
        action->setToolTip(QString::fromUtf8(info.tip));
        action->setChecked(info.tool == tools_.tool);
        toolGroup_->addAction(action);
        Tool tool = info.tool;
        connect(action, &QAction::triggered, this, [this, tool] { tools_.setTool(tool); });
    }
    toolsBar->addSeparator();
    foreground_ = new ColorButton(tools_.foreground);
    foreground_->setToolTip(QStringLiteral("前景色（X 交换，D 复位）"));
    background_ = new ColorButton(tools_.background);
    background_->setToolTip(QStringLiteral("背景色"));
    toolsBar->addWidget(foreground_);
    toolsBar->addWidget(background_);
    connect(foreground_, &ColorButton::colorChanged, this, [this](const QColor& c) { tools_.foreground = c; });
    connect(background_, &ColorButton::colorChanged, this, [this](const QColor& c) { tools_.background = c; });

    QToolBar* options = new QToolBar(QStringLiteral("工具选项"));
    options->setObjectName(QStringLiteral("options"));
    addToolBar(Qt::TopToolBarArea, options);
    auto spin = [&](const QString& label, int minimum, int maximum, int value, const QString& suffix) {
        options->addWidget(new QLabel(QStringLiteral("  %1 ").arg(label)));
        auto* box = new QSpinBox;
        box->setRange(minimum, maximum);
        box->setValue(value);
        box->setSuffix(suffix);
        box->setAccelerated(true);
        options->addWidget(box);
        return box;
    };
    auto track = [&](Tool tool, QAction* action) { toolOptions_[int(tool)].append(action); };
    auto label = [&](const QString& text) { return options->addWidget(new QLabel(QStringLiteral("  %1 ").arg(text))); };
    // 画笔与橡皮
    int before = options->actions().size();
    brushSize_ = spin(QStringLiteral("大小"), 1, 5000, int(tools_.brush.size), QStringLiteral(" 像素"));
    brushHardness_ = spin(QStringLiteral("硬度"), 0, 100, int(tools_.brush.hardness * 100), QStringLiteral("%"));
    brushOpacity_ = spin(QStringLiteral("不透明度"), 1, 100, int(tools_.brush.opacity * 100), QStringLiteral("%"));
    for (int i = before; i < options->actions().size(); ++i) {
        for (Tool t : {Tool::Brush, Tool::Eraser, Tool::Heal, Tool::Clone}) track(t, options->actions()[i]);
    }
    connect(brushSize_, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int v) { tools_.brush.size = v; tools_.notify(); });
    connect(brushHardness_, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int v) { tools_.brush.hardness = v / 100.0; tools_.notify(); });
    connect(brushOpacity_, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int v) { tools_.brush.opacity = v / 100.0; tools_.notify(); });
    // 选框
    track(Tool::Marquee, label(QStringLiteral("形状")));
    auto* shape = new QComboBox;
    shape->addItems({QStringLiteral("矩形"), QStringLiteral("椭圆")});
    track(Tool::Marquee, options->addWidget(shape));
    connect(shape, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int i) { tools_.marqueeEllipse = i == 1; tools_.notify(); });
    // 套索
    track(Tool::Lasso, label(QStringLiteral("方式")));
    auto* lasso = new QComboBox;
    lasso->addItems({QStringLiteral("自由"), QStringLiteral("多边形")});
    track(Tool::Lasso, options->addWidget(lasso));
    connect(lasso, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int i) { tools_.lassoPolygon = i == 1; tools_.notify(); });
    // 魔棒
    QSpinBox* tolerance = nullptr;
    {
        int start = options->actions().size();
        tolerance = spin(QStringLiteral("容差"), 0, 255, tools_.wandTolerance, QString());
        for (int i = start; i < options->actions().size(); ++i) track(Tool::Wand, options->actions()[i]);
    }
    connect(tolerance, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int v) { tools_.wandTolerance = v; });
    auto* contiguous = new QCheckBox(QStringLiteral("连续"));
    contiguous->setChecked(tools_.wandContiguous);
    track(Tool::Wand, options->addWidget(contiguous));
    connect(contiguous, &QCheckBox::toggled, this, [this](bool on) { tools_.wandContiguous = on; });
    auto* allLayers = new QCheckBox(QStringLiteral("对所有图层取样"));
    track(Tool::Wand, options->addWidget(allLayers));
    connect(allLayers, &QCheckBox::toggled, this, [this](bool on) { tools_.wandAllLayers = on; });
    // 污点修复
    track(Tool::Heal, label(QStringLiteral("类型")));
    auto* healMode = new QComboBox;
    healMode->addItems({QStringLiteral("内容识别"), QStringLiteral("创建纹理"), QStringLiteral("近似匹配")});
    track(Tool::Heal, options->addWidget(healMode));
    connect(healMode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int i) { tools_.healMode = i; });
    // 仿制图章
    auto* aligned = new QCheckBox(QStringLiteral("对齐"));
    aligned->setChecked(tools_.cloneAligned);
    track(Tool::Clone, options->addWidget(aligned));
    connect(aligned, &QCheckBox::toggled, this, [this](bool on) { tools_.cloneAligned = on; tools_.cloneOffset.reset(); });
    auto* cloneAll = new QCheckBox(QStringLiteral("对所有图层取样"));
    track(Tool::Clone, options->addWidget(cloneAll));
    connect(cloneAll, &QCheckBox::toggled, this, [this](bool on) { tools_.cloneAllLayers = on; });
    track(Tool::Clone, label(QStringLiteral("<span style='color:gray'>Alt 点击设置取样点</span>")));
    // 渐变
    track(Tool::Gradient, label(QStringLiteral("形状")));
    auto* gradientShape = new QComboBox;
    gradientShape->addItems({QStringLiteral("线性"), QStringLiteral("径向")});
    track(Tool::Gradient, options->addWidget(gradientShape));
    connect(gradientShape, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int i) { tools_.gradientRadial = i == 1; });
    auto* gradientStyle = new QComboBox;
    gradientStyle->addItems({QStringLiteral("前景色到背景色"), QStringLiteral("前景色到透明")});
    track(Tool::Gradient, options->addWidget(gradientStyle));
    connect(gradientStyle, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int i) { tools_.gradientToTransparent = i == 1; });
    auto* gradientReverse = new QCheckBox(QStringLiteral("反向"));
    track(Tool::Gradient, options->addWidget(gradientReverse));
    connect(gradientReverse, &QCheckBox::toggled, this, [this](bool on) { tools_.gradientReversed = on; });
    {
        int start = options->actions().size();
        QSpinBox* gradientOpacity = spin(QStringLiteral("不透明度"), 1, 100, 100, QStringLiteral("%"));
        for (int i = start; i < options->actions().size(); ++i) track(Tool::Gradient, options->actions()[i]);
        connect(gradientOpacity, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int v) { tools_.gradientOpacity = v / 100.0; });
    }
    // 选区工具共用的提示。
    QAction* hint = label(QStringLiteral("<span style='color:gray'>Shift 添加 · Alt 减去 · Shift+Alt 交叉</span>"));
    for (Tool t : {Tool::Marquee, Tool::Lasso, Tool::Wand}) track(t, hint);
    connect(&tools_, &ToolState::changed, this, [this] { updateToolOptions(); });
    updateToolOptions();
    options->addSeparator();
    options->addAction(actions_["undo"]);
    options->addAction(actions_["redo"]);
}

void MainWindow::createDocks() {
    layers_ = new LayersPanel;
    layers_->setAdjustmentMenu(adjustmentMenu_);
    auto* dock = new QDockWidget(QStringLiteral("图层"));
    dock->setObjectName(QStringLiteral("layers"));
    dock->setWidget(layers_);
    dock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    dock->setMinimumWidth(280);
    addDockWidget(Qt::RightDockWidgetArea, dock);
    connect(layers_, &LayersPanel::editAdjustment, this, [this](const QString& id) { editAdjustment(id); });
    connect(layers_, &LayersPanel::editEffects, this, [this] { editEffects(); });
    connect(layers_, &LayersPanel::contextMenuRequested, this, [this](QPoint p) { updateActions(); layerContextMenu_->popup(p); });
    connect(layers_, &LayersPanel::newLayerRequested, actions_["newLayer"], &QAction::trigger);
    connect(layers_, &LayersPanel::newGroupRequested, actions_["newGroup"], &QAction::trigger);
    connect(layers_, &LayersPanel::addMaskRequested, actions_["addMask"], &QAction::trigger);
    connect(layers_, &LayersPanel::deleteRequested, actions_["delete"], &QAction::trigger);
    connect(layers_->maskTargetBox(), &QCheckBox::toggled, this, [this](bool on) { tools_.paintOnMask = on; });
}

CanvasView* MainWindow::currentCanvas() const { return qobject_cast<CanvasView*>(tabs_->currentWidget()); }
Session* MainWindow::currentSession() const { CanvasView* c = currentCanvas(); return c ? c->session() : nullptr; }

void MainWindow::addSession(Session* session) {
    auto* canvas = new CanvasView(session, &tools_);
    session->setParent(canvas);
    canvas->setShowGuides(actions_["guides"]->isChecked());
    canvas->setShowPixelGrid(actions_["pixelGrid"]->isChecked());
    int index = tabs_->addTab(canvas, session->title());
    tabs_->setTabToolTip(index, session->path());
    tabs_->setCurrentIndex(index);
    auto refreshTab = [this, canvas, session] {
        int i = tabs_->indexOf(canvas);
        if (i < 0) return;
        tabs_->setTabText(i, session->title() + (session->isDirty() ? QStringLiteral(" •") : QString()));
        tabs_->setTabToolTip(i, session->path());
        if (canvas == currentCanvas()) updateTitle();
    };
    connect(session, &Session::dirtyChanged, this, refreshTab);
    connect(session, &Session::pathChanged, this, refreshTab);
    connect(session, &Session::historyChanged, this, [this, canvas] { if (canvas == currentCanvas()) updateActions(); });
    connect(session, &Session::activeLayerChanged, this, [this, canvas] { if (canvas == currentCanvas()) updateActions(); });
    connect(session, &Session::documentChanged, this, [this, canvas, session](bool) {
        if (canvas == currentCanvas()) {
            sizeLabel_->setText(QStringLiteral("%1 × %2 像素").arg(session->doc().width).arg(session->doc().height));
            updateActions();
        }
    });
    connect(session, &Session::reloadedFromDisk, this, [this] { statusBar()->showMessage(QStringLiteral("项目已在磁盘上更新，已重新载入。"), 4000); });
    connect(session, &Session::externalChangeConflict, this, [this, session] {
        auto answer = QMessageBox::question(this, QStringLiteral("项目在磁盘上被修改"),
            QStringLiteral("“%1”被其他程序改写了，而你这里有未保存的修改。\n\n要载入磁盘上的版本吗？你的修改会丢失。").arg(session->title()),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer == QMessageBox::Yes) {
            try { session->reloadFromDisk(); } catch (const std::exception& e) { showError(QStringLiteral("无法载入"), QString::fromUtf8(e.what())); }
        }
    });
    connect(canvas, &CanvasView::zoomChanged, this, [this, canvas](double zoom) {
        if (canvas == currentCanvas()) zoomLabel_->setText(QStringLiteral("%1%").arg(zoom * canvas->devicePixelRatioF() * 100, 0, 'f', 1));
    });
    connect(canvas, &CanvasView::cursorMoved, this, [this](QPointF p, bool inside) {
        positionLabel_->setText(inside ? QStringLiteral("X %1  Y %2").arg(int(std::floor(p.x()))).arg(int(std::floor(p.y()))) : QString());
    });
    connect(canvas, &CanvasView::statusMessage, this, [this](const QString& text) { statusBar()->showMessage(text, 5000); });
    connect(canvas, &CanvasView::colorPicked, this, [this](const QColor& color, bool background) {
        QColor opaque = color;
        opaque.setAlpha(255);
        if (background) { tools_.background = opaque; background_->setColor(opaque); }
        else { tools_.foreground = opaque; foreground_->setColor(opaque); }
    });
    if (!session->path().isEmpty()) rememberRecent(session->path());
    canvas->setFocus();
    updateActions();
}

void MainWindow::updateTitle() {
    Session* s = currentSession();
    if (!s) { setWindowTitle(QStringLiteral("Compositor")); setWindowModified(false); return; }
    setWindowTitle(QStringLiteral("%1[*] — Compositor").arg(s->title()));
    setWindowModified(s->isDirty());
}

void MainWindow::updateActions() {
    Session* s = currentSession();
    bool has = s != nullptr;
    const Layer* layer = s ? s->doc().find(s->doc().activeLayerID) : nullptr;
    for (const char* key : {"save", "saveAs", "place", "exportPNG", "exportJPEG", "close", "copyMerged", "canvasSize", "imageSize",
                            "flipCanvasH", "flipCanvasV", "flatten", "newLayer", "newGroup", "zoomIn", "zoomOut", "fit", "actual", "paste"}) {
        actions_[key]->setEnabled(has);
    }
    for (const char* key : {"duplicate", "delete", "groupLayer", "raise", "lower"}) actions_[key]->setEnabled(layer);
    bool pixel = layer && !layer->isGroup && !layer->isAdjustment();
    for (const char* key : {"copy", "mergeDown", "flipLayerH", "flipLayerV", "fill"}) actions_[key]->setEnabled(pixel);
    // 图层效果画在像素周围，所以只给有像素的图层（与 macOS 版一致）。
    actions_["effects"]->setEnabled(pixel && layer->image);
    actions_["clearEffects"]->setEnabled(layer && layer->extra.contains("effects"));
    actions_["clip"]->setEnabled(layer && !layer->isGroup);
    actions_["addMask"]->setEnabled(layer && !layer->mask);
    actions_["addMaskHide"]->setEnabled(layer && !layer->mask);
    for (const char* key : {"toggleMask", "invertMask", "deleteMask"}) actions_[key]->setEnabled(layer && layer->mask);
    actions_["applyMask"]->setEnabled(pixel && layer->mask);
    actions_["editAdjustment"]->setEnabled(layer && layer->isAdjustment());
    adjustmentMenu_->setEnabled(has);
    bool selection = s && s->doc().selection;
    for (const char* key : {"deselect", "feather", "expand", "contract", "crop", "contentFill", "clear", "cut"}) actions_[key]->setEnabled(selection);
    for (const char* key : {"selectAll", "inverse"}) actions_[key]->setEnabled(has);
    actions_["loadSelection"]->setEnabled(pixel);
    if (selection) actions_["copy"]->setEnabled(true);
    actions_["undo"]->setEnabled(s && s->canUndo());
    actions_["redo"]->setEnabled(s && s->canRedo());
    actions_["undo"]->setText(s && s->canUndo() ? QStringLiteral("撤销 %1").arg(s->undoName()) : QStringLiteral("撤销"));
    actions_["redo"]->setText(s && s->canRedo() ? QStringLiteral("重做 %1").arg(s->redoName()) : QStringLiteral("重做"));
    if (layer && !layer->mask && tools_.paintOnMask) layers_->maskTargetBox()->setChecked(false);
}

void MainWindow::editCurrent(const QString& name, const std::function<void(Document&)>& change) {
    Session* s = currentSession();
    if (!s || s->isPreviewing()) return;
    s->edit(name, change);
}

void MainWindow::showError(const QString& title, const QString& message) {
    QMessageBox::warning(this, title, message);
}

void MainWindow::newDocument() {
    NewDocumentDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted) return;
    QColor fill = dialog.fill();
    uint8_t rgba[4] = {uint8_t(fill.red()), uint8_t(fill.green()), uint8_t(fill.blue()), 255};
    Document doc = comp::newDocument(dialog.documentWidth(), dialog.documentHeight(), fill.isValid() ? rgba : nullptr);
    doc.resolution = dialog.resolution();
    addSession(new Session(std::move(doc)));
}

bool MainWindow::loadImageFile(const QString& path, QImage& image, QString& error) {
    std::vector<uint8_t> bytes;
    if (readFile(toPath(path), bytes, size_t(1) << 31)) {
        std::string message;
        if (auto decoded = decodeImage(bytes, &message)) {
            image = toQImage(*decoded);
            return true;
        }
    }
    // 交给 Qt 的图片插件（WebP、TIFF 等）。
    QImageReader reader(path);
    reader.setAutoTransform(true);
    image = reader.read();
    if (image.isNull()) { error = reader.errorString(); return false; }
    return true;
}

bool MainWindow::openPath(const QString& input) {
    QFileInfo info(input);
    QString path = info.absoluteFilePath();
    bool isProject = info.isDir() || info.fileName() == QLatin1String("manifest.json");
    // 已经打开的项目直接切过去。
    for (int i = 0; i < tabs_->count(); ++i) {
        if (auto* c = qobject_cast<CanvasView*>(tabs_->widget(i))) {
            QString open = c->session()->path();
            QString openPath = QFileInfo(open).absoluteFilePath();
            bool same = openPath == path || (info.fileName() == QLatin1String("manifest.json") && openPath == info.absolutePath());
            if (!open.isEmpty() && same) {
                tabs_->setCurrentIndex(i);
                return true;
            }
        }
    }
    if (isProject) {
        try {
            QString package = info.isDir() ? path : info.absolutePath();
            SaveCache cache;
            Document doc = loadProject(toPath(package), &cache);
            addSession(new Session(std::move(doc), package, std::move(cache)));
            return true;
        } catch (const std::exception& e) {
            showError(QStringLiteral("无法打开项目"), QString::fromUtf8(e.what()));
            return false;
        }
    }
    QImage image;
    QString error;
    if (!loadImageFile(path, image, error)) {
        showError(QStringLiteral("无法打开"), QStringLiteral("“%1”无法读取：%2").arg(info.fileName(), error));
        return false;
    }
    if (image.width() > kMaxSide || image.height() > kMaxSide) {
        showError(QStringLiteral("图片太大"), QStringLiteral("画布每边最多 30000 像素。"));
        return false;
    }
    Document doc = documentFromImage(fromQImage(image), info.completeBaseName().toStdString());
    addSession(new Session(std::move(doc)));
    rememberRecent(path);
    return true;
}

void MainWindow::openDialog() {
    QSettings settings;
    QString dir = settings.value(QStringLiteral("lastDirectory"), QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)).toString();
    QStringList files = QFileDialog::getOpenFileNames(this, QStringLiteral("打开"), dir,
        QStringLiteral("所有支持的文件 (manifest.json %1);;Compositor 项目 (manifest.json);;图片 (%1)").arg(kImageFilter));
    for (const QString& file : files) {
        settings.setValue(QStringLiteral("lastDirectory"), QFileInfo(file).absolutePath());
        openPath(file);
    }
}

void MainWindow::openFolderDialog() {
    QSettings settings;
    QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("打开 .comp 项目文件夹"),
                                                    settings.value(QStringLiteral("lastDirectory")).toString());
    if (dir.isEmpty()) return;
    settings.setValue(QStringLiteral("lastDirectory"), QFileInfo(dir).absolutePath());
    openPath(dir);
}

void MainWindow::placeImage(const QImage& image, const QString& name) {
    Session* s = currentSession();
    if (!s || image.isNull()) return;
    if (image.width() > kMaxSide || image.height() > kMaxSide) { showError(QStringLiteral("图片太大"), QStringLiteral("每边最多 30000 像素。")); return; }
    Image pixels = fromQImage(image);
    editCurrent(QStringLiteral("置入"), [&](Document& d) { addImageLayer(d, std::move(pixels), name.toStdString()); });
}

void MainWindow::placeDialog() {
    if (!currentSession()) return;
    QSettings settings;
    QStringList files = QFileDialog::getOpenFileNames(this, QStringLiteral("置入图片"), settings.value(QStringLiteral("lastDirectory")).toString(),
                                                      QStringLiteral("图片 (%1)").arg(kImageFilter));
    for (const QString& file : files) {
        QImage image;
        QString error;
        if (!loadImageFile(file, image, error)) { showError(QStringLiteral("无法置入"), error); continue; }
        placeImage(image, QFileInfo(file).completeBaseName());
    }
}

bool MainWindow::saveSession(Session* session, bool saveAs) {
    QString path = session->path();
    if (saveAs || path.isEmpty()) {
        QSettings settings;
        QString suggestion = QDir(settings.value(QStringLiteral("lastDirectory"), QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)).toString())
            .filePath(session->title().remove(QStringLiteral(" •")) + (session->title().endsWith(QStringLiteral(".comp")) ? QString() : QStringLiteral(".comp")));
        // .comp 是文件夹；用保存对话框取名字即可。
        path = QFileDialog::getSaveFileName(this, QStringLiteral("保存项目"), suggestion, QStringLiteral("Compositor 项目 (*.comp)"),
                                            nullptr, QFileDialog::DontConfirmOverwrite);
        if (path.isEmpty()) return false;
        if (!path.endsWith(QStringLiteral(".comp"), Qt::CaseInsensitive)) path += QStringLiteral(".comp");
        QFileInfo info(path);
        if (info.exists() && !info.isDir()) { showError(QStringLiteral("无法保存"), QStringLiteral("同名文件已存在。")); return false; }
        if (info.exists() && path != session->path()) {
            if (QMessageBox::question(this, QStringLiteral("替换项目"), QStringLiteral("“%1”已存在，要替换它吗？").arg(info.fileName()))
                != QMessageBox::Yes) return false;
        }
        settings.setValue(QStringLiteral("lastDirectory"), info.absolutePath());
    }
    try {
        QApplication::setOverrideCursor(Qt::WaitCursor);
        session->save(path);
        QApplication::restoreOverrideCursor();
    } catch (const std::exception& e) {
        QApplication::restoreOverrideCursor();
        showError(QStringLiteral("保存失败"), QString::fromUtf8(e.what()));
        return false;
    }
    rememberRecent(session->path());
    statusBar()->showMessage(QStringLiteral("已保存到 %1").arg(QDir::toNativeSeparators(session->path())), 4000);
    return true;
}

void MainWindow::exportPNG() {
    Session* s = currentSession();
    if (!s) return;
    QSettings settings;
    QString suggestion = QDir(settings.value(QStringLiteral("lastDirectory")).toString()).filePath(QFileInfo(s->title()).completeBaseName() + QStringLiteral(".png"));
    QString path = QFileDialog::getSaveFileName(this, QStringLiteral("导出 PNG"), suggestion, QStringLiteral("PNG 图片 (*.png)"));
    if (path.isEmpty()) return;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    Image image = flatten(s->doc());
    bool ok = writeFileAtomic(toPath(path), encodePNG(image, s->doc().effectiveResolution()));
    QApplication::restoreOverrideCursor();
    if (!ok) showError(QStringLiteral("导出失败"), QStringLiteral("无法写入 %1").arg(path));
    else statusBar()->showMessage(QStringLiteral("已导出 %1").arg(QDir::toNativeSeparators(path)), 4000);
}

void MainWindow::exportJPEG() {
    Session* s = currentSession();
    if (!s) return;
    JpegExportDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted) return;
    QSettings settings;
    QString suggestion = QDir(settings.value(QStringLiteral("lastDirectory")).toString()).filePath(QFileInfo(s->title()).completeBaseName() + QStringLiteral(".jpg"));
    QString path = QFileDialog::getSaveFileName(this, QStringLiteral("导出 JPEG"), suggestion, QStringLiteral("JPEG 图片 (*.jpg *.jpeg)"));
    if (path.isEmpty()) return;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    Image image = flatten(s->doc());
    QColor m = dialog.matte();
    const uint8_t matte[3] = {uint8_t(m.red()), uint8_t(m.green()), uint8_t(m.blue())};
    bool ok = writeFileAtomic(toPath(path), encodeJPEG(image, dialog.quality(), matte, s->doc().effectiveResolution()));
    QApplication::restoreOverrideCursor();
    if (!ok) showError(QStringLiteral("导出失败"), QStringLiteral("无法写入 %1").arg(path));
    else statusBar()->showMessage(QStringLiteral("已导出 %1").arg(QDir::toNativeSeparators(path)), 4000);
}

void MainWindow::updateToolOptions() {
    for (auto it = toolOptions_.begin(); it != toolOptions_.end(); ++it) {
        for (QAction* a : it.value()) a->setVisible(false);
    }
    for (QAction* a : toolOptions_.value(int(tools_.tool))) a->setVisible(true);
}

void MainWindow::clearOrDelete() {
    Session* s = currentSession();
    if (!s) return;
    if (!s->doc().selection) { actions_["delete"]->trigger(); return; }
    const Layer* layer = s->doc().find(s->doc().activeLayerID);
    if (!layer || layer->isGroup || layer->isAdjustment()) {
        statusBar()->showMessage(QStringLiteral("请选择一个像素图层再清除选区内容。"), 4000);
        return;
    }
    editCurrent(QStringLiteral("清除"), [](Document& d) { clearSelection(d, d.activeLayerID, *d.selection); });
}

void MainWindow::fillCurrent() {
    Session* s = currentSession();
    if (!s) return;
    QColor c = tools_.foreground;
    const uint8_t color[4] = {uint8_t(c.red()), uint8_t(c.green()), uint8_t(c.blue()), 255};
    const Layer* l = s->doc().find(s->doc().activeLayerID);
    if (!l || l->isGroup || l->isAdjustment()) { statusBar()->showMessage(QStringLiteral("只能填充像素图层。"), 3000); return; }
    editCurrent(QStringLiteral("填充"), [&](Document& d) {
        if (d.selection) fillSelection(d, d.activeLayerID, *d.selection, color);
        else fillLayer(d, d.activeLayerID, color);
    });
}

void MainWindow::contentFill() {
    Session* s = currentSession();
    if (!s) return;
    if (!s->doc().selection) { statusBar()->showMessage(QStringLiteral("先选出要填充的区域。"), 4000); return; }
    Document copy = s->doc();
    QApplication::setOverrideCursor(Qt::WaitCursor);
    int result = contentAwareFill(copy, copy.activeLayerID, *copy.selection);
    QApplication::restoreOverrideCursor();
    if (result == 1) { editCurrent(QStringLiteral("内容识别填充"), [&](Document& d) { d = std::move(copy); }); return; }
    if (result == -2) showError(QStringLiteral("内容识别填充"), QStringLiteral("请选择一个有像素的图层。"));
    else if (result == 0) showError(QStringLiteral("内容识别填充"), QStringLiteral("选区周围没有足够的不透明像素可以取样。换一个小一点、周围有画面的选区试试。"));
    else showError(QStringLiteral("内容识别填充"), QStringLiteral("内存不足。"));
}

void MainWindow::modifySelection(int kind) {
    Session* s = currentSession();
    if (!s || !s->doc().selection) return;
    bool ok = false;
    double value = kind == 0
        ? QInputDialog::getDouble(this, QStringLiteral("羽化选区"), QStringLiteral("羽化半径（像素）："), 5, 0.1, 1000, 1, &ok)
        : QInputDialog::getInt(this, kind == 1 ? QStringLiteral("扩展选区") : QStringLiteral("收缩选区"),
                               kind == 1 ? QStringLiteral("扩展量（像素）：") : QStringLiteral("收缩量（像素）："), 5, 1, 500, 1, &ok);
    if (!ok) return;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    GrayImage selection = *s->doc().selection;
    if (kind == 0) featherSelection(selection, value);
    else growSelection(selection, kind == 1 ? int(value) : -int(value));
    QApplication::restoreOverrideCursor();
    const QString names[3] = {QStringLiteral("羽化"), QStringLiteral("扩展"), QStringLiteral("收缩")};
    editCurrent(names[kind], [&](Document& d) {
        d.selection = selectionIsEmpty(selection) ? nullptr : std::make_shared<GrayImage>(std::move(selection));
    });
}

void MainWindow::cut() {
    Session* s = currentSession();
    if (!s || !s->doc().selection) { copyLayer(false); return; }
    copyLayer(false);
    clearOrDelete();
}

void MainWindow::copyLayer(bool merged) {
    Session* s = currentSession();
    if (!s) return;
    if (s->doc().selection) {
        // 有选区：只拷贝选区里的部分，裁到选区大小。
        const Layer* layer = s->doc().find(s->doc().activeLayerID);
        Image source;
        if (merged || !layer || layer->isGroup || layer->isAdjustment()) source = flatten(s->doc());
        else source = renderLayerAlone(s->doc(), *layer);
        int x = 0, y = 0;
        Image piece = copySelection(source, *s->doc().selection, x, y);
        if (piece.empty()) return;
        QApplication::clipboard()->setImage(toQImage(piece));
        statusBar()->showMessage(QStringLiteral("已拷贝选区（%1 × %2）。").arg(piece.width).arg(piece.height), 3000);
        return;
    }
    if (merged) {
        QApplication::clipboard()->setImage(toQImage(flatten(s->doc())));
        statusBar()->showMessage(QStringLiteral("已拷贝合并后的画面。"), 3000);
        return;
    }
    const Layer* layer = s->doc().find(s->doc().activeLayerID);
    if (!layer || !layer->image) return;
    // 只拷贝这个图层（含蒙版、变换）在画布上的样子。
    Document single = s->doc();
    Layer copy = *layer;
    copy.parentID.clear();
    copy.visible = true;
    copy.opacity = 1;
    copy.blendMode = BlendMode::Normal;
    copy.maskSourceID.clear();
    single.layers = {copy};
    QApplication::clipboard()->setImage(toQImage(flatten(single)));
    statusBar()->showMessage(QStringLiteral("已拷贝图层。"), 3000);
}

void MainWindow::paste() {
    QImage image = QApplication::clipboard()->image();
    if (image.isNull()) {
        // 剪贴板里是文件时按文件置入。
        const QMimeData* mime = QApplication::clipboard()->mimeData();
        if (mime && mime->hasUrls()) {
            for (const QUrl& url : mime->urls()) {
                QString error;
                if (url.isLocalFile() && loadImageFile(url.toLocalFile(), image, error)) {
                    placeImage(image, QFileInfo(url.toLocalFile()).completeBaseName());
                }
            }
            return;
        }
        statusBar()->showMessage(QStringLiteral("剪贴板里没有图片。"), 3000);
        return;
    }
    if (!currentSession()) {
        Document doc = documentFromImage(fromQImage(image), "粘贴");
        addSession(new Session(std::move(doc)));
        return;
    }
    placeImage(image, QStringLiteral("粘贴"));
}

void MainWindow::canvasSize() {
    Session* s = currentSession();
    if (!s) return;
    CanvasSizeDialog dialog(s->doc().width, s->doc().height, this);
    if (dialog.exec() != QDialog::Accepted) return;
    editCurrent(QStringLiteral("画布大小"), [&](Document& d) { resizeCanvas(d, dialog.newWidth(), dialog.newHeight(), dialog.anchorX(), dialog.anchorY()); });
    if (auto* c = currentCanvas()) c->fitToWindow();
}

void MainWindow::imageSize() {
    Session* s = currentSession();
    if (!s) return;
    ImageSizeDialog dialog(s->doc().width, s->doc().height, s->doc().effectiveResolution(), this);
    if (dialog.exec() != QDialog::Accepted) return;
    editCurrent(QStringLiteral("图像大小"), [&](Document& d) {
        resizeImage(d, dialog.newWidth(), dialog.newHeight());
        d.resolution = dialog.resolution();
    });
    if (auto* c = currentCanvas()) c->fitToWindow();
}

void MainWindow::editAdjustment(const QString& layerID) {
    Session* s = currentSession();
    if (!s || s->isPreviewing()) return;
    const Layer* layer = s->doc().find(layerID.toStdString());
    if (!layer || !layer->isAdjustment()) return;
    try {
        auto* dialog = new AdjustmentDialog(s, layer->id, this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        // 模态：预览期间不能改别的，取消时才能完整恢复。画布仍然可见。
        dialog->setWindowModality(Qt::WindowModal);
        dialog->show();
    } catch (const std::exception& e) {
        showError(QStringLiteral("无法编辑"), QString::fromUtf8(e.what()));
    }
}

void MainWindow::editEffects() {
    Session* s = currentSession();
    if (!s || s->isPreviewing()) return;
    const Layer* layer = s->doc().find(s->doc().activeLayerID);
    if (!layer || !layer->image || layer->isGroup || layer->isAdjustment()) {
        statusBar()->showMessage(QStringLiteral("图层效果需要有像素的图层；空白图层先画点东西。"), 4000);
        return;
    }
    auto* dialog = new EffectsDialog(s, layer->id, tools_.background, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowModality(Qt::WindowModal);
    dialog->show();
}

void MainWindow::addAdjustment(int kind) {
    Session* s = currentSession();
    if (!s) return;
    uint32_t seed = QRandomGenerator::global()->generate();
    std::string id;
    editCurrent(QStringLiteral("新建%1").arg(QString::fromUtf8(adjustmentKindLabel(AdjustmentKind(kind)))),
                [&](Document& d) {
                    id = addAdjustmentLayer(d, AdjustmentKind(kind), seed);
                    // 有选区时调整只作用在选区内（与 Photoshop 一样以选区做蒙版）。
                    if (d.selection) maskFromSelection(d, id, *d.selection);
                });
    if (AdjustmentKind(kind) != AdjustmentKind::Invert && !id.empty()) editAdjustment(QString::fromStdString(id));
}

void MainWindow::showAbout() {
    QMessageBox::about(this, QStringLiteral("关于 Compositor"),
        QStringLiteral("<h3>Compositor %1（Windows 版）</h3>"
                       "<p>面向合成与修图的免费开源图像编辑器。本版本用 C++ 与 Qt 重写了界面与渲染，"
                       "复用原 macOS 版的 C 像素内核，项目文件（.comp）两边通用。</p>"
                       "<p>原作：Robbie Tilton 的 macOS 版 Compositor。</p>").arg(QStringLiteral(COMPOSITOR_VERSION)));
}

void MainWindow::rememberRecent(const QString& path) {
    QSettings settings;
    QStringList recent = settings.value(QStringLiteral("recent")).toStringList();
    recent.removeAll(path);
    recent.prepend(path);
    while (recent.size() > 12) recent.removeLast();
    settings.setValue(QStringLiteral("recent"), recent);
    updateRecentMenu();
}

void MainWindow::updateRecentMenu() {
    recentMenu_->clear();
    QStringList recent = QSettings().value(QStringLiteral("recent")).toStringList();
    for (const QString& path : recent) {
        QAction* a = recentMenu_->addAction(QDir::toNativeSeparators(path));
        connect(a, &QAction::triggered, this, [this, path] { openPath(path); });
    }
    if (!recent.isEmpty()) {
        recentMenu_->addSeparator();
        connect(recentMenu_->addAction(QStringLiteral("清除列表")), &QAction::triggered, this, [this] {
            QSettings().remove(QStringLiteral("recent"));
            updateRecentMenu();
        });
    }
    recentMenu_->setEnabled(!recent.isEmpty());
}

bool MainWindow::maybeSave(Session* session) {
    if (!session->isDirty()) return true;
    QMessageBox box(QMessageBox::Warning, QStringLiteral("保存修改"),
                    QStringLiteral("“%1”有未保存的修改，要保存吗？").arg(session->title()),
                    QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, this);
    box.button(QMessageBox::Save)->setText(QStringLiteral("保存"));
    box.button(QMessageBox::Discard)->setText(QStringLiteral("不保存"));
    box.button(QMessageBox::Cancel)->setText(QStringLiteral("取消"));
    int answer = box.exec();
    if (answer == QMessageBox::Cancel) return false;
    if (answer == QMessageBox::Save) return saveSession(session, false);
    return true;
}

bool MainWindow::closeTab(int index) {
    auto* canvas = qobject_cast<CanvasView*>(tabs_->widget(index));
    if (!canvas) return true;
    if (canvas->session()->isPreviewing()) return false;
    tabs_->setCurrentIndex(index);
    if (!maybeSave(canvas->session())) return false;
    tabs_->removeTab(index);
    canvas->deleteLater();
    return true;
}

void MainWindow::closeEvent(QCloseEvent* event) {
    for (int i = tabs_->count() - 1; i >= 0; --i) {
        auto* canvas = qobject_cast<CanvasView*>(tabs_->widget(i));
        if (!canvas) continue;
        if (!closeTab(i)) { event->ignore(); return; }
    }
    QSettings settings;
    settings.setValue(QStringLiteral("window/geometry"), saveGeometry());
    settings.setValue(QStringLiteral("window/state"), saveState());
    event->accept();
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasUrls() || event->mimeData()->hasImage()) event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* event) {
    const QMimeData* mime = event->mimeData();
    if (mime->hasUrls()) {
        for (const QUrl& url : mime->urls()) {
            if (!url.isLocalFile()) continue;
            QString path = url.toLocalFile();
            QFileInfo info(path);
            bool project = info.isDir() || info.fileName() == QLatin1String("manifest.json");
            // 有打开的画布时，拖进来的图片作为新图层置入；项目总是在新标签打开。
            if (!project && currentSession()) {
                QImage image;
                QString error;
                if (loadImageFile(path, image, error)) placeImage(image, info.completeBaseName());
                else showError(QStringLiteral("无法置入"), error);
            } else {
                openPath(path);
            }
        }
    } else if (mime->hasImage()) {
        placeImage(qvariant_cast<QImage>(mime->imageData()), QStringLiteral("拖入"));
    }
    event->acceptProposedAction();
}
