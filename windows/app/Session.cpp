#include "Session.h"
#include "QtBridge.h"
#include "compositor/project_io.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>

int Session::untitledCounter_ = 0;

namespace {
constexpr size_t kMaxUndo = 60;
}

Session::Session(comp::Document document, const QString& path, comp::SaveCache cache, QObject* parent)
    : QObject(parent), doc_(std::move(document)), path_(path), cache_(std::move(cache)) {
    if (path_.isEmpty()) untitled_ = ++untitledCounter_;
    debounce_.setSingleShot(true);
    debounce_.setInterval(350);
    connect(&debounce_, &QTimer::timeout, this, &Session::checkDisk);
    connect(&watcher_, &QFileSystemWatcher::fileChanged, this, [this] { debounce_.start(); });
    connect(&watcher_, &QFileSystemWatcher::directoryChanged, this, [this] { debounce_.start(); });
    if (!path_.isEmpty()) { digest_ = diskDigest(); watch(); }
}

QString Session::title() const {
    if (path_.isEmpty()) return QStringLiteral("未命名 %1").arg(untitled_);
    QString name = QFileInfo(path_).fileName();
    if (name == QLatin1String("manifest.json")) name = QFileInfo(QFileInfo(path_).path()).fileName();
    return name;
}

void Session::touch() {
    bool wasDirty = isDirty();
    revision_ = nextRevision_++;
    if (wasDirty != isDirty()) emit dirtyChanged();
}

void Session::checkpoint(const QString& name) {
    undo_.push_back({doc_, name, revision_});
    if (undo_.size() > kMaxUndo) undo_.erase(undo_.begin());
    redo_.clear();
    touch();
    emit historyChanged();
}

void Session::edit(const QString& name, const std::function<void(comp::Document&)>& change, bool structure) {
    checkpoint(name);
    change(doc_);
    emit documentChanged(structure);
    emit activeLayerChanged();
}

void Session::notifyChanged(bool structure) { emit documentChanged(structure); }
void Session::notifyPixels(const QRectF& rect) { emit pixelsChanged(rect); }

void Session::undo() {
    if (undo_.empty() || previewing_) return;
    bool wasDirty = isDirty();
    redo_.push_back({doc_, undo_.back().name, revision_});
    doc_ = std::move(undo_.back().document);
    revision_ = undo_.back().revision;
    undo_.pop_back();
    if (wasDirty != isDirty()) emit dirtyChanged();
    emit historyChanged();
    emit documentChanged(true);
    emit activeLayerChanged();
}

void Session::redo() {
    if (redo_.empty() || previewing_) return;
    bool wasDirty = isDirty();
    undo_.push_back({doc_, redo_.back().name, revision_});
    doc_ = std::move(redo_.back().document);
    revision_ = redo_.back().revision;
    redo_.pop_back();
    if (wasDirty != isDirty()) emit dirtyChanged();
    emit historyChanged();
    emit documentChanged(true);
    emit activeLayerChanged();
}

void Session::beginPreview() {
    if (previewing_) return;
    previewing_ = true;
    preview_ = {doc_, QString(), revision_};
}

void Session::cancelPreview() {
    if (!previewing_) return;
    previewing_ = false;
    doc_ = std::move(preview_.document);
    emit documentChanged(true);
}

void Session::commitPreview(const QString& name) {
    if (!previewing_) return;
    previewing_ = false;
    undo_.push_back({std::move(preview_.document), name, revision_});
    if (undo_.size() > kMaxUndo) undo_.erase(undo_.begin());
    redo_.clear();
    touch();
    emit historyChanged();
    emit documentChanged(true);
}

void Session::setActiveLayer(const std::string& id) {
    if (doc_.activeLayerID == id) return;
    doc_.activeLayerID = id;
    emit activeLayerChanged();
}

void Session::save(const QString& path) {
    QString target = path;
    if (QFileInfo(target).fileName() == QLatin1String("manifest.json")) target = QFileInfo(target).path();
    if (!target.endsWith(QLatin1String(".comp"), Qt::CaseInsensitive) && !QFileInfo(target).isDir()) target += QStringLiteral(".comp");
    watcher_.removePaths(watcher_.files() + watcher_.directories());
    comp::saveProject(doc_, toPath(target), true, &cache_);
    bool pathChangedNow = target != path_;
    path_ = target;
    savedRevision_ = revision_;
    digest_ = diskDigest();
    watch();
    emit dirtyChanged();
    if (pathChangedNow) emit pathChanged();
}

void Session::reloadFromDisk() {
    comp::SaveCache cache;
    comp::Document loaded = comp::loadProject(toPath(path_), &cache);
    cache_ = std::move(cache);
    // 保持缩放、选中图层（若仍存在）；与重新打开一样清空撤销历史。
    std::string active = doc_.activeLayerID;
    doc_ = std::move(loaded);
    if (doc_.find(active)) doc_.activeLayerID = active;
    undo_.clear();
    redo_.clear();
    revision_ = savedRevision_ = nextRevision_++;
    digest_ = diskDigest();
    emit dirtyChanged();
    emit historyChanged();
    emit documentChanged(true);
    emit activeLayerChanged();
    emit reloadedFromDisk();
}

void Session::watch() {
    if (path_.isEmpty()) return;
    QString manifest = QDir(path_).filePath(QStringLiteral("manifest.json"));
    QStringList paths{path_};
    if (QFile::exists(manifest)) paths << manifest;
    QString images = QDir(path_).filePath(QStringLiteral("images"));
    if (QFileInfo(images).isDir()) paths << images;
    watcher_.addPaths(paths);
}

// 与 macOS 版相同的变化判断：清单内容加上每张图片的文件名与大小。
QByteArray Session::diskDigest() const {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QFile manifest(QDir(path_).filePath(QStringLiteral("manifest.json")));
    if (!manifest.open(QIODevice::ReadOnly)) return QByteArray();
    hash.addData(manifest.readAll());
    QDir images(QDir(path_).filePath(QStringLiteral("images")));
    for (const QFileInfo& info : images.entryInfoList(QDir::Files, QDir::Name)) {
        hash.addData(info.fileName().toUtf8());
        hash.addData(QByteArray::number(info.size()));
    }
    return hash.result();
}

void Session::checkDisk() {
    // 整个包被替换时监视会失效：每次都重新挂上。
    watch();
    QByteArray digest = diskDigest();
    if (digest.isEmpty() || digest == digest_) return;
    if (isDirty()) {
        digest_ = digest; // 只问一次，直到下一次变化
        emit externalChangeConflict();
        return;
    }
    try {
        reloadFromDisk();
    } catch (...) {
        // 写了一半或写坏的项目：忽略，等下一次变化（与 macOS 版一致）。
    }
}
