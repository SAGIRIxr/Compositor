// 主窗口：菜单、工具栏、项目标签页、图层面板与状态栏。
#pragma once
#include "ToolState.h"
#include <QHash>
#include <QMainWindow>
#include <QPointer>

class CanvasView;
class LayersPanel;
class Session;
class QTabWidget;
class QStackedWidget;
class QLabel;
class QAction;
class QActionGroup;
class QMenu;
class QSpinBox;
class QDoubleSpinBox;
class ColorButton;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow();
    // 打开项目（.comp 文件夹或其中的 manifest.json）或图片。
    bool openPath(const QString& path);

protected:
    void closeEvent(QCloseEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    void createActions();
    void createMenus();
    void createToolBars();
    void createDocks();
    void addSession(Session* session);
    CanvasView* currentCanvas() const;
    Session* currentSession() const;
    void updateActions();
    void updateTitle();
    void updateRecentMenu();
    void rememberRecent(const QString& path);
    bool maybeSave(Session* session);
    bool saveSession(Session* session, bool saveAs);
    bool closeTab(int index);
    void showError(const QString& title, const QString& message);
    // 对当前文档执行一项编辑；没有文档时什么也不做。
    void editCurrent(const QString& name, const std::function<void(comp::Document&)>& change);
    void placeImage(const QImage& image, const QString& name);
    bool loadImageFile(const QString& path, QImage& image, QString& error);

    void newDocument();
    void openDialog();
    void openFolderDialog();
    void placeDialog();
    void exportPNG();
    void exportJPEG();
    void copyLayer(bool merged);
    void cut();
    void clearOrDelete();
    void fillCurrent();
    void contentFill();
    void modifySelection(int kind);
    void updateToolOptions();
    void paste();
    void canvasSize();
    void imageSize();
    void editAdjustment(const QString& layerID);
    void editEffects();
    void addAdjustment(int kind);
    void showAbout();

    ToolState tools_;
    QStackedWidget* stack_;
    QTabWidget* tabs_;
    LayersPanel* layers_;
    QLabel* zoomLabel_;
    QLabel* positionLabel_;
    QLabel* sizeLabel_;
    QMenu* recentMenu_;
    QMenu* adjustmentMenu_;
    QMenu* layerContextMenu_;
    QHash<QString, QAction*> actions_;
    QActionGroup* toolGroup_;
    QSpinBox* brushSize_;
    QSpinBox* brushHardness_;
    QSpinBox* brushOpacity_;
    QHash<int, QList<QAction*>> toolOptions_; // 工具 → 选项栏里属于它的控件
    ColorButton* foreground_;
    ColorButton* background_;
    QPointer<Session> watched_;
};
