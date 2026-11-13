#include "gui/main_window.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QItemSelectionModel>
#include <QVariant>
#include <QFile>
#include <QFileSystemModel>
#include <QFormLayout>
#include <QGuiApplication>
#include <QScreen>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSortFilterProxyModel>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardItemModel>
#include <QStatusBar>
#include <QTemporaryFile>
#include <QTreeView>
#include <QVBoxLayout>

#include "gui/session_worker.h"

namespace {
constexpr int kSortRole = Qt::UserRole + 1;
constexpr int kPathRole = Qt::UserRole + 2;
constexpr int kDirRole = Qt::UserRole + 3;

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
    resize(qMin(1180, available.width() - 80), qMin(740, available.height() - 80));
}

MainWindow::~MainWindow() {
    thread_.quit();
    thread_.wait();
}

void MainWindow::setInitialPath(const QString& path) { initialPath_ = path; }

void MainWindow::enableSelfTest(const QString& remote, const QString& local) {
    selftestRemote_ = remote;
    selftestLocal_ = local;
}

void MainWindow::autoConnect(const QString& host, int port, const QString& password) {
    history_->setCurrentText(host);
    port_->setValue(port);
    password_->setText(password);
    QMetaObject::invokeMethod(this, "onConnectClicked", Qt::QueuedConnection);
}

void MainWindow::buildUi() {
    auto* central = new QWidget(this);
    auto* root = new QVBoxLayout(central);

    auto* top = new QHBoxLayout();
    top->addWidget(new QLabel("Host:"));
    history_ = new QComboBox();
    history_->setEditable(true);
    history_->setMinimumWidth(220);
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

    auto* left = new QWidget();
    auto* leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->addWidget(new QLabel("Local"));
    localView_ = new QTreeView();
    localModel_ = new QFileSystemModel();
    localModel_->setRootPath(QDir::homePath());
    localView_->setModel(localModel_);
    localView_->setRootIndex(localModel_->index(QDir::homePath()));
    localView_->setSortingEnabled(true);
    localView_->sortByColumn(0, Qt::AscendingOrder);
    localView_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    for (int i = 1; i < localModel_->columnCount(); ++i) localView_->hideColumn(i);
    leftLayout->addWidget(localView_);
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

    auto* right = new QWidget();
    auto* rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    auto* remoteHeader = new QHBoxLayout();
    remoteHeader->addWidget(new QLabel("Remote"));
    up_ = new QPushButton("Up");
    refresh_ = new QPushButton("Refresh");
    remoteHeader->addWidget(up_);
    remoteHeader->addWidget(refresh_);
    remotePathEdit_ = new QLineEdit();
    remoteHeader->addWidget(remotePathEdit_, 1);
    rightLayout->addLayout(remoteHeader);

    remoteView_ = new QTreeView();
    remoteModel_ = new QStandardItemModel();
    remoteModel_->setHorizontalHeaderLabels({"Name", "Size", "Modified"});
    remoteProxy_ = new QSortFilterProxyModel();
    remoteProxy_->setSourceModel(remoteModel_);
    remoteProxy_->setSortRole(kSortRole);
    remoteProxy_->setSortCaseSensitivity(Qt::CaseInsensitive);
    remoteView_->setModel(remoteProxy_);
    remoteView_->setSortingEnabled(true);
    remoteView_->sortByColumn(0, Qt::AscendingOrder);
    remoteView_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    remoteView_->setRootIsDecorated(false);
    remoteView_->header()->setStretchLastSection(true);
    remoteView_->setColumnWidth(0, 240);
    remoteView_->setColumnWidth(1, 90);
    rightLayout->addWidget(remoteView_);
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
    bottom->addWidget(status_);
    root->addLayout(bottom);

    setCentralWidget(central);

    connect(connect_, &QPushButton::clicked, this, &MainWindow::onConnectClicked);
    connect(upload_, &QPushButton::clicked, this, &MainWindow::uploadSelected);
    connect(download_, &QPushButton::clicked, this, &MainWindow::downloadSelected);
    connect(newFolder_, &QPushButton::clicked, this, &MainWindow::makeFolder);
    connect(newFile_, &QPushButton::clicked, this, &MainWindow::makeFile);
    connect(delete_, &QPushButton::clicked, this, &MainWindow::deleteSelected);
    connect(rename_, &QPushButton::clicked, this, &MainWindow::renameSelected);
    connect(up_, &QPushButton::clicked, this, &MainWindow::goUp);
    connect(refresh_, &QPushButton::clicked, this, &MainWindow::refreshRemote);
    connect(remoteView_, &QTreeView::doubleClicked, this, &MainWindow::onRemoteDoubleClicked);
    connect(remotePathEdit_, &QLineEdit::returnPressed, this, [this] {
        remotePath_ = remotePathEdit_->text();
        refreshRemote();
    });
    connect(localView_->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [this](const QItemSelection&, const QItemSelection&) {
                upload_->setEnabled(connected_);
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
    QMetaObject::invokeMethod(worker_, "connectTo", Qt::QueuedConnection,
                              Q_ARG(QString, host), Q_ARG(quint16, static_cast<quint16>(port_->value())),
                              Q_ARG(QString, password_->text()), Q_ARG(int, 5000));
}

void MainWindow::onConnected(const QString& protocol, const QString& root) {
    connected_ = true;
    setBusy(false);
    setWindowTitle(QString("vnxfer - %1 [%2]").arg(endpoint(), protocol));
    status_->setText(QString("Connected: %1").arg(protocol));
    connect_->setText("Disconnect");
    addHistory(history_->currentText().trimmed());
    remotePath_ = !initialPath_.isEmpty() ? initialPath_ : (root.isEmpty() ? "/" : root);
    initialPath_.clear();
    refreshRemote();

    if (!selftestRemote_.isEmpty()) {
        setBusy(true);
        QMetaObject::invokeMethod(worker_, "download", Qt::QueuedConnection,
                                  Q_ARG(QString, selftestRemote_), Q_ARG(QString, selftestLocal_));
    }
}

void MainWindow::onListed(const QString& path, const QVector<RemoteEntryQt>& entries) {
    remotePath_ = path;
    remotePathEdit_->setText(path);
    remoteModel_->removeRows(0, remoteModel_->rowCount());
    for (const auto& entry : entries) {
        if (entry.name == ".") continue;
        auto* nameItem = new QStandardItem(entry.name + (entry.isDir ? "/" : ""));
        nameItem->setEditable(false);
        nameItem->setData(entry.name, kSortRole);
        nameItem->setData(entry.name, kPathRole);
        nameItem->setData(entry.isDir, kDirRole);

        auto* sizeItem = new QStandardItem(entry.isDir ? QString() : humanSize(entry.size));
        sizeItem->setData(static_cast<qulonglong>(entry.isDir ? 0 : entry.size), kSortRole);
        sizeItem->setEditable(false);

        auto* timeItem = new QStandardItem(
            entry.mtime > 0
                ? QDateTime::fromSecsSinceEpoch(entry.mtime).toString("yyyy-MM-dd HH:mm")
                : QString());
        timeItem->setData(static_cast<qlonglong>(entry.mtime), kSortRole);
        timeItem->setEditable(false);

        remoteModel_->appendRow({nameItem, sizeItem, timeItem});
    }
    setBusy(false);
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
}

void MainWindow::onRemoteDoubleClicked(const QModelIndex& index) {
    if (!index.isValid()) return;
    if (!index.data(kDirRole).toBool()) return;
    const QString name = index.data(kPathRole).toString();
    if (name == "..") {
        goUp();
        return;
    }
    QString base = remotePath_;
    if (!base.endsWith('/')) base += '/';
    remotePath_ = base + name;
    refreshRemote();
}

void MainWindow::goUp() {
    QString path = remotePath_;
    if (path.isEmpty() || path == "/") return;
    if (path.endsWith('/')) path.chop(1);
    const int pos = path.lastIndexOf('/');
    if (pos <= 0) {
        remotePath_ = "/";
    } else {
        remotePath_ = path.left(pos);
    }
    refreshRemote();
}

void MainWindow::refreshRemote() {
    if (!connected_) return;
    setBusy(true);
    QMetaObject::invokeMethod(worker_, "list", Qt::QueuedConnection, Q_ARG(QString, remotePath_));
}

QString MainWindow::remotePath() const {
    return remotePath_.isEmpty() ? QString("/") : remotePath_;
}

QString MainWindow::selectedRemotePath() const {
    const auto indexes = remoteView_->selectionModel()->selectedRows(0);
    if (indexes.isEmpty()) return {};
    const QModelIndex index = indexes.first();
    const QString name = index.data(kPathRole).toString();
    if (name.isEmpty()) return {};
    QString base = remotePath();
    if (!base.endsWith('/')) base += '/';
    return base + name;
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
    if (info.isDir()) {
        QMessageBox::information(this, "vnxfer",
                                 "Directory upload is available in the CLI (--recursive).");
        return;
    }
    QString remote = remotePath();
    if (!remote.endsWith('/')) remote += '/';
    remote += info.fileName();
    setBusy(true);
    QMetaObject::invokeMethod(worker_, "upload", Qt::QueuedConnection, Q_ARG(QString, local),
                              Q_ARG(QString, remote), Q_ARG(bool, true));
}

void MainWindow::downloadSelected() {
    if (!connected_) return;
    const QString remote = selectedRemotePath();
    if (remote.isEmpty() || selectedRemoteIsDir()) {
        QMessageBox::information(this, "vnxfer", "Select a remote file to download.");
        return;
    }
    const auto indexes = localView_->selectionModel()->selectedRows(0);
    QString localDir = QDir::homePath();
    if (!indexes.isEmpty()) {
        const QString selected = localModel_->filePath(indexes.first());
        localDir = QFileInfo(selected).isDir() ? selected : QFileInfo(selected).path();
    }
    const QString local = QDir(localDir).filePath(QFileInfo(remote).fileName());
    setBusy(true);
    QMetaObject::invokeMethod(worker_, "download", Qt::QueuedConnection, Q_ARG(QString, remote),
                              Q_ARG(QString, local));
}

void MainWindow::makeFolder() {
    if (!connected_) return;
    bool ok = false;
    const QString name = QInputDialog::getText(this, "New folder", "Name:", QLineEdit::Normal,
                                               "new-folder", &ok);
    if (!ok || name.trimmed().isEmpty()) return;
    QString path = remotePath();
    if (!path.endsWith('/')) path += '/';
    path += name.trimmed();
    setBusy(true);
    QMetaObject::invokeMethod(worker_, "makeDir", Qt::QueuedConnection, Q_ARG(QString, path));
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

    QString path = remotePath();
    if (!path.endsWith('/')) path += '/';
    path += name.trimmed();
    setBusy(true);
    QMetaObject::invokeMethod(worker_, "upload", Qt::QueuedConnection, Q_ARG(QString, tempPath),
                              Q_ARG(QString, path), Q_ARG(bool, true));
}

void MainWindow::deleteSelected() {
    if (!connected_) return;
    const QString path = selectedRemotePath();
    if (path.isEmpty()) return;
    if (QMessageBox::question(this, "Delete", "Delete " + path + "?") != QMessageBox::Yes) return;
    setBusy(true);
    QMetaObject::invokeMethod(worker_, "removePath", Qt::QueuedConnection, Q_ARG(QString, path));
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
    setBusy(true);
    QMetaObject::invokeMethod(worker_, "renamePath", Qt::QueuedConnection, Q_ARG(QString, path),
                              Q_ARG(QString, target));
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

void MainWindow::refreshLocal() {
    localView_->setRootIndex(localModel_->index(QDir::homePath()));
}

void MainWindow::loadSettings() {
    QSettings settings;
    history_->addItems(settings.value("history").toStringList());
    if (history_->count() > 0) history_->setCurrentIndex(0);
    port_->setValue(settings.value("port", 5900).toInt());
    remember_->setChecked(settings.value("remember", false).toBool());

    const QString ep = endpoint();
    const QByteArray saved = settings.value("passwords/" + ep).toByteArray();
    if (!saved.isEmpty()) {
        password_->setText(QString::fromUtf8(QByteArray::fromBase64(saved)));
    }

    connect(history_, &QComboBox::currentTextChanged, this, [this](const QString&) {
        QSettings s;
        const QByteArray saved = s.value("passwords/" + endpoint()).toByteArray();
        if (!saved.isEmpty()) {
            password_->setText(QString::fromUtf8(QByteArray::fromBase64(saved)));
        }
    });
}

void MainWindow::closeEvent(QCloseEvent* event) {
    saveSettings();
    QMainWindow::closeEvent(event);
}

void MainWindow::saveSettings() {
    QSettings settings;
    settings.setValue("port", port_->value());
    settings.setValue("remember", remember_->isChecked());
}
