#include "gui/main_window.h"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QListWidget>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QScreen>
#include <QSettings>
#include <QSortFilterProxyModel>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardItemModel>
#include <QStyleFactory>
#include <QTemporaryFile>
#include <QTreeView>
#include <QVBoxLayout>

#include "gui/session_worker.h"

namespace {

constexpr int kSortRole = Qt::UserRole + 1;
constexpr int kPathRole = Qt::UserRole + 2;
constexpr int kDirRole = Qt::UserRole + 3;
constexpr int kLoadedRole = Qt::UserRole + 4;

QString humanSize(quint64 n) {
    const char* units[] = {"B", "K", "M", "G", "T"};
    double v = static_cast<double>(n);
    int u = 0;
    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        ++u;
    }
    return QString::asprintf(u == 0 ? "%.0f %s" : "%.1f %s", v, units[u]);
}

// Directories first, then the regular sort order (name or the column's sort role).
class FolderFirstProxy : public QSortFilterProxyModel {
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;

protected:
    bool lessThan(const QModelIndex& left, const QModelIndex& right) const override {
        const bool ld = left.data(kDirRole).toBool();
        const bool rd = right.data(kDirRole).toBool();
        if (ld != rd) return ld;
        return QSortFilterProxyModel::lessThan(left, right);
    }
};

}  // namespace

MainWindow::MainWindow() {
    worker_ = new SessionWorker();
    worker_->moveToThread(&thread_);
    connect(&thread_, &QThread::finished, worker_, &QObject::deleteLater);
    connect(worker_, &SessionWorker::connected, this, &MainWindow::onConnected);
    connect(worker_, &SessionWorker::listed, this, &MainWindow::onListed);
    connect(worker_, &SessionWorker::progress, this, &MainWindow::onProgress);
    connect(worker_, &SessionWorker::finished, this, &MainWindow::onFinished);
    connect(worker_, &SessionWorker::failed, this, &MainWindow::onFailed);
    thread_.start();

    buildUi();
    loadSettings();
    setWindowTitle("vnxfer");
    const QRect available = QGuiApplication::primaryScreen()->availableGeometry();
    resize(qMin(1240, available.width() - 80), qMin(780, available.height() - 80));
}

MainWindow::~MainWindow() {
    thread_.quit();
    thread_.wait();
}

QWidget* MainWindow::buildBrowserPane(const QString& title, QLineEdit** pathEdit,
                                      QPushButton** up, QPushButton** refresh, QTreeView** view) {
    auto* pane = new QWidget();
    auto* layout = new QVBoxLayout(pane);
    layout->setContentsMargins(0, 0, 0, 0);

    auto* header = new QHBoxLayout();
    header->addWidget(new QLabel(title));
    *up = new QPushButton("Up");
    *refresh = new QPushButton("Refresh");
    header->addWidget(*up);
    header->addWidget(*refresh);
    *pathEdit = new QLineEdit();
    header->addWidget(*pathEdit, 1);
    layout->addLayout(header);

    *view = new QTreeView();
    (*view)->setSelectionMode(QAbstractItemView::ExtendedSelection);
    (*view)->setSortingEnabled(true);
    layout->addWidget(*view, 1);
    return pane;
}

void MainWindow::buildUi() {
    auto* central = new QWidget(this);
    auto* root = new QVBoxLayout(central);

    auto* top = new QHBoxLayout();
    top->addWidget(new QLabel("Host:"));
    history_ = new QComboBox();
    history_->setEditable(true);
    history_->setMinimumWidth(200);
    top->addWidget(history_);
    top->addWidget(new QLabel("Port:"));
    port_ = new QSpinBox();
    port_->setRange(1, 65535);
    port_->setValue(5900);
    top->addWidget(port_);
    top->addWidget(new QLabel("Password:"));
    password_ = new QLineEdit();
    password_->setEchoMode(QLineEdit::Password);
    top->addWidget(password_);
    remember_ = new QCheckBox("remember");
    top->addWidget(remember_);
    connect_ = new QPushButton("Connect");
    top->addWidget(connect_);
    top->addStretch();
    root->addLayout(top);

    auto* splitter = new QSplitter(Qt::Horizontal);

    QWidget* left = buildBrowserPane("Local", &localPathEdit_, &localUp_, &localRefresh_,
                                     &localView_);
    localModel_ = new QFileSystemModel();
    localModel_->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot);
    localModel_->setRootPath(QDir::homePath());
    localView_->setModel(localModel_);
    for (int i = 1; i < localModel_->columnCount(); ++i) localView_->hideColumn(i);
    localView_->setRootIndex(localModel_->index(QDir::homePath()));
    localView_->sortByColumn(0, Qt::AscendingOrder);
    localPathEdit_->setText(QDir::homePath());
    localRoot_ = QDir::homePath();
    splitter->addWidget(left);

    auto* middle = new QWidget();
    auto* middleLayout = new QVBoxLayout(middle);
    middleLayout->addStretch();
    upload_ = new QPushButton("Upload →");
    download_ = new QPushButton("← Download");
    newFolder_ = new QPushButton("New folder");
    newFile_ = new QPushButton("New file");
    delete_ = new QPushButton("Delete");
    rename_ = new QPushButton("Rename");
    middleLayout->addWidget(upload_);
    middleLayout->addWidget(download_);
    middleLayout->addSpacing(12);
    middleLayout->addWidget(newFolder_);
    middleLayout->addWidget(newFile_);
    middleLayout->addWidget(delete_);
    middleLayout->addWidget(rename_);
    middleLayout->addStretch();
    splitter->addWidget(middle);

    QWidget* right = buildBrowserPane("Remote", &remotePathEdit_, &remoteUp_, &remoteRefresh_,
                                      &remoteView_);
    remoteModel_ = new QStandardItemModel();
    remoteModel_->setHorizontalHeaderLabels({"Name", "Size", "Modified"});
    remoteProxy_ = new FolderFirstProxy();
    remoteProxy_->setSourceModel(remoteModel_);
    remoteProxy_->setSortRole(kSortRole);
    remoteProxy_->setSortCaseSensitivity(Qt::CaseInsensitive);
    remoteView_->setModel(remoteProxy_);
    remoteView_->setRootIsDecorated(true);
    remoteView_->header()->setStretchLastSection(true);
    remoteView_->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    remoteView_->header()->setSectionResizeMode(1, QHeaderView::Interactive);
    remoteView_->header()->setSectionResizeMode(2, QHeaderView::Interactive);
    remoteView_->setColumnWidth(0, 250);
    remoteView_->setColumnWidth(1, 80);
    remoteView_->sortByColumn(0, Qt::AscendingOrder);
    splitter->addWidget(right);

    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 0);
    splitter->setStretchFactor(2, 1);
    root->addWidget(splitter, 1);

    progress_ = new QProgressBar();
    progress_->setRange(0, 1000);
    progress_->setValue(0);
    status_ = new QLabel("Not connected");
    status_->setMinimumWidth(260);
    status_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    auto* bottom = new QHBoxLayout();
    bottom->addWidget(progress_, 1);
    bottom->addWidget(cancelJob_ = new QPushButton("Cancel"));
    bottom->addWidget(status_);
    root->addLayout(bottom);

    jobList_ = new QListWidget();
    jobList_->setMaximumHeight(90);
    jobList_->setVisible(false);
    root->addWidget(jobList_);

    setCentralWidget(central);

    auto* viewMenu = menuBar()->addMenu("View");
    auto* hiddenAction = viewMenu->addAction("Show hidden files");
    hiddenAction->setCheckable(true);
    connect(hiddenAction, &QAction::toggled, this, &MainWindow::toggleHidden);
    auto* themeMenu = viewMenu->addMenu("Theme");
    auto* systemTheme = themeMenu->addAction("System");
    auto* fusionTheme = themeMenu->addAction("Fusion");
    connect(systemTheme, &QAction::triggered, this, [] {
        QApplication::setStyle(QStyleFactory::create(QApplication::style()->objectName()));
    });
    connect(fusionTheme, &QAction::triggered, this, [] {
        QApplication::setStyle(QStyleFactory::create("Fusion"));
    });

    connect(connect_, &QPushButton::clicked, this, &MainWindow::onConnectClicked);
    connect(cancelJob_, &QPushButton::clicked, this, [this] {
        jobs_.clear();
        jobList_->clear();
        jobList_->setVisible(false);
        jobRunning_ = false;
        QMetaObject::invokeMethod(worker_, "cancel", Qt::QueuedConnection);
        setBusy(false);
        status_->setText("cancelled");
    });
    connect(upload_, &QPushButton::clicked, this, &MainWindow::uploadSelected);
    connect(download_, &QPushButton::clicked, this, &MainWindow::downloadSelected);
    connect(newFolder_, &QPushButton::clicked, this, &MainWindow::makeFolder);
    connect(newFile_, &QPushButton::clicked, this, &MainWindow::makeFile);
    connect(delete_, &QPushButton::clicked, this, &MainWindow::deleteSelected);
    connect(rename_, &QPushButton::clicked, this, &MainWindow::renameSelected);
    connect(localUp_, &QPushButton::clicked, this, &MainWindow::localGoUp);
    connect(localRefresh_, &QPushButton::clicked, this, &MainWindow::refreshLocal);
    connect(remoteUp_, &QPushButton::clicked, this, &MainWindow::remoteGoUp);
    connect(remoteRefresh_, &QPushButton::clicked, this, &MainWindow::refreshRemote);
    connect(remoteView_, &QTreeView::doubleClicked, this, &MainWindow::onRemoteDoubleClicked);
    connect(remoteView_, &QTreeView::expanded, this, &MainWindow::onRemoteExpanded);
    connect(localPathEdit_, &QLineEdit::returnPressed, this, [this] {
        const QString path = localPathEdit_->text();
        if (QDir(path).exists()) {
            localRoot_ = path;
            localView_->setRootIndex(localModel_->index(path));
        }
    });
    connect(remotePathEdit_, &QLineEdit::returnPressed, this, [this] {
        const QString path = remotePathEdit_->text();
        if (!connected_ || path.isEmpty()) return;
        QStandardItem* item = findRemoteItem(path);
        if (item) {
            remoteRoot_ = path;
            remoteView_->setRootIndex(remoteProxy_->mapFromSource(remoteModel_->indexFromItem(item)));
        } else {
            refreshRemote();
        }
    });

    setBusy(false);
}

void MainWindow::setBusy(bool busy) {
    progress_->setEnabled(busy);
    connect_->setEnabled(!busy);
}

QString MainWindow::endpoint() const {
    return QString("%1:%2").arg(history_->currentText().trimmed()).arg(port_->value());
}

void MainWindow::autoConnect(const QString& host, int port, const QString& password) {
    history_->setCurrentText(host);
    port_->setValue(port);
    password_->setText(password);
    QMetaObject::invokeMethod(this, "onConnectClicked", Qt::QueuedConnection);
}

void MainWindow::setInitialPath(const QString& path) { initialPath_ = path; }

void MainWindow::enableSelfTest(const QString& remote, const QString& local) {
    selftestRemote_ = remote;
    selftestLocal_ = local;
}

void MainWindow::onConnectClicked() {
    if (connected_) {
        QMetaObject::invokeMethod(worker_, "disconnectFrom", Qt::QueuedConnection);
        connected_ = false;
        setWindowTitle("vnxfer");
        status_->setText("Disconnected");
        connect_->setText("Connect");
        return;
    }
    const QString host = history_->currentText().trimmed();
    if (host.isEmpty()) return;

    QSettings settings;
    if (remember_->isChecked()) {
        settings.setValue("passwords/" + endpoint(),
                          QString::fromLatin1(password_->text().toUtf8().toBase64()));
    } else {
        settings.remove("passwords/" + endpoint());
    }

    status_->setText("Connecting to " + endpoint() + " ...");
    setBusy(true);
    QMetaObject::invokeMethod(worker_, "connectTo", Qt::QueuedConnection, Q_ARG(QString, host),
                              Q_ARG(quint16, static_cast<quint16>(port_->value())),
                              Q_ARG(QString, password_->text()), Q_ARG(int, 5000));
}

void MainWindow::onConnected(const QString& protocol, const QString& root) {
    connected_ = true;
    setBusy(false);
    setWindowTitle(QString("vnxfer - %1 [%2]").arg(endpoint(), protocol));
    status_->setText(QString("Connected: %1").arg(protocol));
    connect_->setText("Disconnect");
    addHistory(history_->currentText().trimmed());

    remoteModel_->clear();
    remoteModel_->setHorizontalHeaderLabels({"Name", "Size", "Modified"});
    remoteRoot_ = !initialPath_.isEmpty() ? initialPath_ : (root.isEmpty() ? "/" : root);
    initialPath_.clear();
    if (remoteRoot_.isEmpty()) remoteRoot_ = "/";
    if (!remoteRoot_.startsWith('/')) remoteRoot_.prepend('/');
    refreshRemote();

    if (!selftestRemote_.isEmpty()) {
        enqueueJob({"download", selftestRemote_, selftestLocal_, "selftest"});
    }
}

void MainWindow::onListed(const QString& path, const QVector<RemoteEntryQt>& entries) {
    // The header width is only meaningful once the view has a viewport, so
    // re-apply it on every refresh.
    remoteView_->setColumnWidth(0, 250);
    remoteView_->setColumnWidth(1, 80);
    if (path == remoteRoot_) {
        remoteModel_->removeRows(0, remoteModel_->rowCount());
        remoteRoot_ = path;
        auto* item = new QStandardItem(QFileIconProvider().icon(QFileIconProvider::Folder),
                                       QFileInfo(path).fileName().isEmpty() ? QString("/")
                                                                            : QFileInfo(path).fileName());
        item->setData(path, kPathRole);
        item->setData(true, kDirRole);
        item->setData(true, kLoadedRole);
        item->setEditable(false);
        remoteModel_->appendRow(item);
        fillRemoteItem(item, entries);
        remoteView_->setRootIndex(remoteProxy_->mapFromSource(remoteModel_->indexFromItem(item)));
        remoteView_->expand(remoteProxy_->mapFromSource(remoteModel_->indexFromItem(item)));
        remotePathEdit_->setText(path);
        setBusy(false);
        return;
    }
    auto* item = findRemoteItem(path);
    if (item) fillRemoteItem(item, entries);
    setBusy(false);
}

void MainWindow::fillRemoteItem(QStandardItem* item, const QVector<RemoteEntryQt>& entries) {
    item->removeRows(0, item->rowCount());
    const QString base = item->data(kPathRole).toString();
    QFileIconProvider icons;
    for (const auto& entry : entries) {
        if (entry.name == ".") continue;
        if (!showHidden_ && entry.name.startsWith('.')) continue;
        const QString path = base.endsWith('/') ? base + entry.name : base + "/" + entry.name;
        auto* nameItem = new QStandardItem(
            icons.icon(entry.isDir ? QFileIconProvider::Folder : QFileIconProvider::File),
            entry.name);
        nameItem->setData(entry.name, kSortRole);
        nameItem->setData(path, kPathRole);
        nameItem->setData(entry.isDir, kDirRole);
        nameItem->setData(false, kLoadedRole);
        nameItem->setEditable(false);
        if (entry.isDir) {
            // A disabled placeholder makes the tree show an expander; it is
            // replaced by the real children on first expansion.
            auto* placeholder = new QStandardItem(QStringLiteral("…"));
            placeholder->setEnabled(false);
            placeholder->setEditable(false);
            nameItem->appendRow(placeholder);
        }

        auto* sizeItem = new QStandardItem(entry.isDir ? QString() : humanSize(entry.size));
        sizeItem->setData(static_cast<qulonglong>(entry.isDir ? 0 : entry.size), kSortRole);
        sizeItem->setEditable(false);

        auto* timeItem = new QStandardItem(
            entry.mtime > 0
                ? QDateTime::fromSecsSinceEpoch(entry.mtime).toString("yyyy-MM-dd HH:mm")
                : QString());
        timeItem->setData(static_cast<qlonglong>(entry.mtime), kSortRole);
        timeItem->setEditable(false);

        item->appendRow({nameItem, sizeItem, timeItem});
    }
}

QStandardItem* MainWindow::findRemoteItem(const QString& path) {
    const QModelIndexList matches = remoteModel_->match(remoteModel_->index(0, 0), kPathRole, path,
                                                        1, Qt::MatchRecursive);
    if (matches.isEmpty()) return nullptr;
    return remoteModel_->itemFromIndex(matches.first());
}

void MainWindow::onRemoteExpanded(const QModelIndex& index) {
    const QModelIndex source = remoteProxy_->mapToSource(index);
    QStandardItem* item = remoteModel_->itemFromIndex(source);
    if (!item || !item->data(kDirRole).toBool()) return;
    if (item->data(kLoadedRole).toBool()) return;
    const bool placeholderOnly = item->rowCount() == 1 && item->child(0)->data(kPathRole).isNull();
    if (!placeholderOnly && item->rowCount() > 0) {
        item->setData(true, kLoadedRole);
        return;
    }
    item->setData(true, kLoadedRole);
    setBusy(true);
    QMetaObject::invokeMethod(worker_, "list", Qt::QueuedConnection,
                              Q_ARG(QString, item->data(kPathRole).toString()));
}

void MainWindow::onRemoteDoubleClicked(const QModelIndex& index) {
    if (!index.isValid()) return;
    // Directories toggle through the default expander; the expanded() signal
    // triggers the lazy load.
    if (index.data(kDirRole).toBool()) return;
    downloadSelected();
}

void MainWindow::remoteGoUp() {
    QString path = remoteRoot_;
    if (path.isEmpty() || path == "/") return;
    if (path.endsWith('/')) path.chop(1);
    const int pos = path.lastIndexOf('/');
    remoteRoot_ = pos <= 0 ? "/" : path.left(pos);
    setBusy(true);
    QMetaObject::invokeMethod(worker_, "list", Qt::QueuedConnection, Q_ARG(QString, remoteRoot_));
}

void MainWindow::refreshRemote() {
    if (!connected_) return;
    setBusy(true);
    QMetaObject::invokeMethod(worker_, "list", Qt::QueuedConnection, Q_ARG(QString, remoteRoot_));
}

void MainWindow::localGoUp() {
    QDir dir(localRoot_);
    if (dir.cdUp()) {
        localRoot_ = dir.absolutePath();
        localPathEdit_->setText(localRoot_);
        localView_->setRootIndex(localModel_->index(localRoot_));
    }
}

void MainWindow::refreshLocal() {
    localModel_->setRootPath(localRoot_);
    localView_->setRootIndex(localModel_->index(localRoot_));
}

void MainWindow::toggleHidden(bool show) {
    showHidden_ = show;
    localModel_->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot | (show ? QDir::Hidden : QDir::NoFilter));
    refreshLocal();
    if (connected_) refreshRemote();
}

QString MainWindow::selectedRemotePath() const {
    const auto indexes = remoteView_->selectionModel()->selectedRows(0);
    if (indexes.isEmpty()) return {};
    return indexes.first().data(kPathRole).toString();
}

bool MainWindow::selectedRemoteIsDir() const {
    const auto indexes = remoteView_->selectionModel()->selectedRows(0);
    return !indexes.isEmpty() && indexes.first().data(kDirRole).toBool();
}

void MainWindow::uploadSelected() {
    if (!connected_) return;
    const auto indexes = localView_->selectionModel()->selectedRows(0);
    if (indexes.isEmpty()) return;
    const QString local = localModel_->filePath(indexes.first());
    QFileInfo info(local);
    QString remote = remoteRoot_;
    if (!remote.endsWith('/')) remote += '/';
    remote += info.fileName();

    TransferJob job;
    if (info.isDir()) {
        job = {"upload-tree", local, remote, "↑ " + info.fileName() + "/"};
    } else {
        job = {"upload", local, remote, "↑ " + info.fileName()};
    }
    enqueueJob(job);
}

void MainWindow::enqueueJob(const TransferJob& job) {
    jobs_.enqueue(job);
    jobList_->addItem("queued: " + job.label);
    jobList_->setVisible(true);
    if (!jobRunning_) startNextJob();
}

void MainWindow::startNextJob() {
    if (jobs_.isEmpty()) {
        jobRunning_ = false;
        return;
    }
    const TransferJob job = jobs_.dequeue();
    jobRunning_ = true;
    if (jobList_->count() > 0) delete jobList_->takeItem(0);
    jobList_->insertItem(0, "running: " + job.label);
    setBusy(true);
    status_->setText(job.label);

    if (job.kind == "upload") {
        QMetaObject::invokeMethod(worker_, "upload", Qt::QueuedConnection, Q_ARG(QString, job.a),
                                  Q_ARG(QString, job.b), Q_ARG(bool, true));
    } else if (job.kind == "download") {
        QMetaObject::invokeMethod(worker_, "download", Qt::QueuedConnection, Q_ARG(QString, job.a),
                                  Q_ARG(QString, job.b));
    } else if (job.kind == "upload-tree") {
        QMetaObject::invokeMethod(worker_, "uploadTree", Qt::QueuedConnection,
                                  Q_ARG(QString, job.a), Q_ARG(QString, job.b));
    } else if (job.kind == "download-tree") {
        QMetaObject::invokeMethod(worker_, "downloadTree", Qt::QueuedConnection,
                                  Q_ARG(QString, job.a), Q_ARG(QString, job.b));
    } else if (job.kind == "mkdir") {
        QMetaObject::invokeMethod(worker_, "makeDir", Qt::QueuedConnection, Q_ARG(QString, job.a));
    } else if (job.kind == "remove") {
        QMetaObject::invokeMethod(worker_, "removePath", Qt::QueuedConnection, Q_ARG(QString, job.a));
    } else if (job.kind == "rename") {
        QMetaObject::invokeMethod(worker_, "renamePath", Qt::QueuedConnection, Q_ARG(QString, job.a),
                                  Q_ARG(QString, job.b));
    }
}

void MainWindow::finishCurrentJob() {
    jobRunning_ = false;
    if (jobList_->count() > 0) delete jobList_->takeItem(0);
    startNextJob();
}

void MainWindow::downloadSelected() {
    if (!connected_) return;
    const QString remote = selectedRemotePath();
    if (remote.isEmpty()) return;
    const auto indexes = localView_->selectionModel()->selectedRows(0);
    QString localDir = localRoot_;
    if (!indexes.isEmpty()) {
        const QString selected = localModel_->filePath(indexes.first());
        localDir = QFileInfo(selected).isDir() ? selected : QFileInfo(selected).path();
    }
    const QString name = QFileInfo(remote).fileName();
    const QString local = QDir(localDir).filePath(name);

    TransferJob job;
    if (selectedRemoteIsDir()) {
        job = {"download-tree", remote, local, "↓ " + name + "/"};
    } else {
        job = {"download", remote, local, "↓ " + name};
    }
    enqueueJob(job);
}

void MainWindow::makeFolder() {
    if (!connected_) return;
    bool ok = false;
    const QString name = QInputDialog::getText(this, "New folder", "Name:", QLineEdit::Normal,
                                               "new-folder", &ok);
    if (!ok || name.trimmed().isEmpty()) return;
    QString path = remoteRoot_;
    if (!path.endsWith('/')) path += '/';
    path += name.trimmed();
    enqueueJob({"mkdir", path, {}, "mkdir " + name.trimmed()});
}

void MainWindow::makeFile() {
    if (!connected_) return;
    bool ok = false;
    const QString name = QInputDialog::getText(this, "New file", "Name:", QLineEdit::Normal,
                                               "new-file.txt", &ok);
    if (!ok || name.trimmed().isEmpty()) return;

    auto* temp = new QTemporaryFile(QDir::temp().filePath("vnxfer-empty-XXXXXX.tmp"));
    temp->setAutoRemove(false);
    temp->open();
    const QString tempPath = temp->fileName();
    temp->close();
    delete temp;
    pendingTempFile_ = tempPath;

    QString path = remoteRoot_;
    if (!path.endsWith('/')) path += '/';
    path += name.trimmed();
    enqueueJob({"upload", tempPath, path, "new file " + name.trimmed()});
}

void MainWindow::deleteSelected() {
    if (!connected_) return;
    const QString path = selectedRemotePath();
    if (path.isEmpty()) return;
    if (QMessageBox::question(this, "Delete", "Delete " + path + "?") != QMessageBox::Yes) return;
    enqueueJob({"remove", path, {}, "delete " + QFileInfo(path).fileName()});
}

void MainWindow::renameSelected() {
    if (!connected_) return;
    const QString path = selectedRemotePath();
    if (path.isEmpty()) return;
    const QString oldName = QFileInfo(path).fileName();
    bool ok = false;
    const QString name = QInputDialog::getText(this, "Rename", "New name:", QLineEdit::Normal,
                                               oldName, &ok);
    if (!ok || name.trimmed().isEmpty()) return;
    QString parent = path.left(path.lastIndexOf('/'));
    if (parent.isEmpty()) parent = "/";
    const QString target = parent + "/" + name.trimmed();
    enqueueJob({"rename", path, target, "rename " + oldName});
}

void MainWindow::onProgress(quint64 done, quint64 total) {
    if (total == 0) {
        progress_->setRange(0, 0);
    } else {
        progress_->setRange(0, 1000);
        progress_->setValue(static_cast<int>(done * 1000 / total));
    }
}

void MainWindow::onFinished(const QString& what) {
    setBusy(false);
    status_->setText(what + " done");
    progress_->setValue(0);
    if (what == "upload" || what == "download" || what == "remove" || what == "rename" ||
        what == "mkdir") {
        refreshRemote();
    }
    if (what == "upload" && !pendingTempFile_.isEmpty()) {
        QFile::remove(pendingTempFile_);
        pendingTempFile_.clear();
    }
    finishCurrentJob();
}

void MainWindow::onFailed(const QString& what, const QString& message) {
    setBusy(false);
    progress_->setValue(0);
    status_->setText(what + " failed");
    if (what == "connect") {
        connected_ = false;
        connect_->setText("Connect");
    }
    QMessageBox::warning(this, "vnxfer", QString("%1 failed:\n%2").arg(what, message));
    finishCurrentJob();
}

void MainWindow::addHistory(const QString& entry) {
    if (entry.isEmpty()) return;
    QSettings settings;
    QStringList history = settings.value("history").toStringList();
    history.removeAll(entry);
    history.prepend(entry);
    while (history.size() > 15) history.removeLast();
    settings.setValue("history", history);

    const QString current = history_->currentText();
    history_->blockSignals(true);
    history_->clear();
    history_->addItems(history);
    history_->setCurrentText(current);
    history_->blockSignals(false);
}

void MainWindow::closeEvent(QCloseEvent* event) {
    saveSettings();
    QMainWindow::closeEvent(event);
}

void MainWindow::loadSettings() {
    QSettings settings;
    history_->addItems(settings.value("history").toStringList());
    if (history_->count() > 0) history_->setCurrentIndex(0);
    port_->setValue(settings.value("port", 5900).toInt());
    remember_->setChecked(settings.value("remember", false).toBool());
    showHidden_ = settings.value("showHidden", false).toBool();

    const QByteArray saved = settings.value("passwords/" + endpoint()).toByteArray();
    if (!saved.isEmpty()) password_->setText(QString::fromUtf8(QByteArray::fromBase64(saved)));

    connect(history_, &QComboBox::currentTextChanged, this, [this](const QString&) {
        QSettings s;
        const QByteArray savedPw = s.value("passwords/" + endpoint()).toByteArray();
        if (!savedPw.isEmpty()) {
            password_->setText(QString::fromUtf8(QByteArray::fromBase64(savedPw)));
        }
    });
}

void MainWindow::saveSettings() {
    QSettings settings;
    settings.setValue("port", port_->value());
    settings.setValue("remember", remember_->isChecked());
    settings.setValue("showHidden", showHidden_);
}
