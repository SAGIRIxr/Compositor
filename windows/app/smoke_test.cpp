// 界面冒烟测试：启动主窗口，走一遍新建、画笔、图层、调整层、撤销、保存、外部改写重载与导出。
#include "CanvasView.h"
#include "EffectsDialog.h"
#include "LayersPanel.h"
#include "MainWindow.h"
#include "QtBridge.h"
#include "Session.h"
#include "compositor/edit.h"
#include "compositor/project_io.h"
#include "compositor/renderer.h"
#include <cmath>
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QMouseEvent>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>
#include <QListWidget>
#include <QDialogButtonBox>
#include <QPushButton>
#include <iostream>

using namespace comp;

namespace {
int failures = 0;
#define EXPECT(condition) do { if (!(condition)) { ++failures; std::cout << "[失败] " << __LINE__ << ": " #condition "\n"; } else { std::cout << "[通过] " #condition "\n"; } } while (0)

void drag(QWidget* widget, QPoint from, QPoint to) {
    QTest::mousePress(widget, Qt::LeftButton, Qt::NoModifier, from);
    for (int i = 1; i <= 10; ++i) {
        QPointF p = from + (to - from) * (i / 10.0);
        QMouseEvent move(QEvent::MouseMove, p, widget->mapToGlobal(p), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(widget, &move);
    }
    QTest::mouseRelease(widget, Qt::LeftButton, Qt::NoModifier, to);
}

QAction* action(MainWindow& window, const QString& name) {
    QAction* a = window.findChild<QAction*>(name);
    if (!a) std::cout << "找不到动作 " << name.toStdString() << "\n";
    return a;
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("CompositorSmokeTest"));
    QTemporaryDir temp;
    QString package = QDir(temp.path()).filePath(QStringLiteral("冒烟 测试.comp"));
    const uint8_t white[4] = {255, 255, 255, 255};
    Document initial = newDocument(400, 300, white);
    saveProject(initial, toPath(package));

    MainWindow window;
    window.resize(1200, 800);
    window.show();
    if (!QTest::qWaitForWindowExposed(&window)) std::cout << "窗口没有显示出来\n";
    EXPECT(window.openPath(package));
    auto* canvas = window.findChild<CanvasView*>();
    EXPECT(canvas != nullptr);
    if (!canvas) return 1;
    QTest::qWait(50);
    Session* session = canvas->session();
    EXPECT(session->doc().layers.size() == 1);

    // 画笔：在新图层上画一条黑线。
    action(window, QStringLiteral("newLayer"))->trigger();
    EXPECT(session->doc().layers.size() == 2);
    std::string painted = session->doc().activeLayerID;
    action(window, QStringLiteral("tool_B"))->trigger();
    QPoint a = canvas->toWidget(QPointF(100, 150)).toPoint(), b = canvas->toWidget(QPointF(300, 150)).toPoint();
    drag(canvas, a, b);
    const Layer* layer = session->doc().find(painted);
    EXPECT(layer && layer->image && layer->image->at(200, 150)[3] == 255);
    EXPECT(session->isDirty());

    // 吸管取到黑色。
    QColor sampled;
    EXPECT(canvas->sampleColor(QPointF(200, 150), sampled) && sampled.red() < 10);

    // 撤销画笔，再重做。
    action(window, QStringLiteral("undo"))->trigger();
    layer = session->doc().find(painted);
    EXPECT(layer && !layer->image);
    action(window, QStringLiteral("redo"))->trigger();
    layer = session->doc().find(painted);
    EXPECT(layer && layer->image);

    // 移动工具拖动图层。
    action(window, QStringLiteral("tool_V"))->trigger();
    drag(canvas, canvas->toWidget(QPointF(200, 150)).toPoint(), canvas->toWidget(QPointF(200, 200)).toPoint());
    layer = session->doc().find(painted);
    EXPECT(layer && std::abs(layer->transform.y - 50) <= 2);

    // 图层效果：勾选描边后取消，不留痕迹；再勾选后确定，写进图层并可撤销。
    session->setActiveLayer(painted);
    action(window, QStringLiteral("effects"))->trigger();
    auto* effects = window.findChild<EffectsDialog*>();
    EXPECT(effects != nullptr);
    if (effects) {
        auto* list = effects->findChild<QListWidget*>();
        list->item(0)->setCheckState(Qt::Checked);
        EXPECT(session->doc().find(painted)->extra.contains("effects"));
        effects->reject();
        QTest::qWait(20);
        EXPECT(!session->doc().find(painted)->extra.contains("effects"));
    }
    action(window, QStringLiteral("effects"))->trigger();
    QTest::qWait(20);
    effects = nullptr;
    for (auto* d : window.findChildren<EffectsDialog*>()) if (d->isVisible()) effects = d;
    if (effects) {
        effects->findChild<QListWidget*>()->item(0)->setCheckState(Qt::Checked);
        effects->accept();
        QTest::qWait(20);
    }
    EXPECT(session->doc().find(painted)->extra["effects"].contains("stroke"));
    action(window, QStringLiteral("undo"))->trigger();
    EXPECT(!session->doc().find(painted)->extra.contains("effects"));
    action(window, QStringLiteral("redo"))->trigger();
    EXPECT(session->doc().find(painted)->extra.contains("effects"));

    // 反相调整层（无参数，不弹编辑框）。
    action(window, QStringLiteral("adjust_Invert"))->trigger();
    EXPECT(session->doc().layers.size() == 3);
    Image flat = flatten(session->doc());
    EXPECT(flat.at(10, 10)[0] == 0);       // 白底反相成黑
    EXPECT(flat.at(200, 200)[0] == 255);   // 黑线反相成白

    // 图层面板里能看到三个图层。
    auto* tree = window.findChild<QTreeWidget*>();
    EXPECT(tree && tree->topLevelItemCount() == 3);

    // 蒙版与编组。
    action(window, QStringLiteral("addMask"))->trigger();
    EXPECT(session->doc().find(session->doc().activeLayerID)->mask != nullptr);
    action(window, QStringLiteral("groupLayer"))->trigger();
    EXPECT(session->doc().find(session->doc().activeLayerID)->isGroup);

    // 保存、重新读取。
    session->save(package);
    EXPECT(!session->isDirty());
    Document reloaded = loadProject(toPath(package));
    EXPECT(reloaded.layers.size() == 4);

    // 外部程序改写项目（AI 代理的用法）：未修改时自动重新载入。
    Document external = reloaded;
    external.layers[0].name = "被外部改名";
    QTest::qWait(200);
    saveProject(external, toPath(package));
    QTest::qWait(1500);
    EXPECT(session->doc().layers[0].name == "被外部改名");

    // 画面应是黑底（白色反相）加一条白线：中间取样检查画布控件的渲染结果。
    QImage view = canvas->grab().toImage();
    QPoint background = canvas->toWidget(QPointF(50, 50)).toPoint(), line = canvas->toWidget(QPointF(200, 200)).toPoint();
    EXPECT(qGray(view.pixel(background)) < 10);
    EXPECT(qGray(view.pixel(line)) > 245);

    // 截图留档。
    QString shot = QString::fromLocal8Bit(qgetenv("COMPOSITOR_SMOKE_SCREENSHOT"));
    if (!shot.isEmpty()) window.grab().save(shot);

    std::cout << (failures ? "界面冒烟测试失败\n" : "界面冒烟测试通过\n");
    return failures ? 1 : 0;
}
