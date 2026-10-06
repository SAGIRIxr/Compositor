// 一个打开的项目：文档、文件路径、撤销历史和磁盘变化监视。
#pragma once
#include "compositor/document.h"
#include "compositor/project_io.h"
#include <QFileSystemWatcher>
#include <QObject>
#include <QRectF>
#include <QTimer>
#include <functional>
#include <vector>

class Session : public QObject {
    Q_OBJECT
public:
    explicit Session(comp::Document document, const QString& path = QString(), comp::SaveCache cache = {}, QObject* parent = nullptr);

    comp::Document& doc() { return doc_; }
    const comp::Document& doc() const { return doc_; }
    QString path() const { return path_; }
    QString title() const;
    bool isDirty() const { return revision_ != savedRevision_; }

    // 先记录撤销点再修改。structure 为真表示图层列表变了（面板要重建）。
    void edit(const QString& name, const std::function<void(comp::Document&)>& change, bool structure = true);
    // 只记录撤销点（之后由调用方逐步修改，例如画笔、拖动）。
    void checkpoint(const QString& name);
    // 修改已经发生后通知界面。
    void notifyChanged(bool structure);
    void notifyPixels(const QRectF& documentRect);

    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }
    QString undoName() const { return undo_.empty() ? QString() : undo_.back().name; }
    QString redoName() const { return redo_.empty() ? QString() : redo_.back().name; }
    void undo();
    void redo();

    // 预览：对话框打开时记下原状态，取消时恢复，确定时作为一步撤销提交。
    void beginPreview();
    void cancelPreview();
    void commitPreview(const QString& name);
    bool isPreviewing() const { return previewing_; }

    void setActiveLayer(const std::string& id);

    // 保存与读取。出错时抛出 comp::ProjectError。
    void save(const QString& path);
    void reloadFromDisk();

signals:
    void documentChanged(bool structure);
    void pixelsChanged(QRectF documentRect);
    void dirtyChanged();
    void historyChanged();
    void activeLayerChanged();
    void pathChanged();
    // 磁盘上的项目被其他程序（例如 AI 代理）改写了，而本地有未保存的修改。
    void externalChangeConflict();
    void reloadedFromDisk();

private:
    struct Step { comp::Document document; QString name; quint64 revision; };
    void touch();
    void watch();
    void checkDisk();
    QByteArray diskDigest() const;

    comp::Document doc_;
    QString path_;
    std::vector<Step> undo_, redo_;
    quint64 revision_ = 0, savedRevision_ = 0, nextRevision_ = 1;
    bool previewing_ = false;
    Step preview_;
    QFileSystemWatcher watcher_;
    QTimer debounce_;
    QByteArray digest_;
    comp::SaveCache cache_; // 跳过没变的图片
    static int untitledCounter_;
    int untitled_ = 0;
};
