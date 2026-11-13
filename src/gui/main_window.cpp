#include "gui/main_window.h"

#include <QApplication>
#include <QDataStream>
#include <QCheckBox>
#include <QDrag>
#include <QDropEvent>
#include <QMimeData>
#include <QUrl>
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
#include <QDockWidget>
#include <QPlainTextEdit>
#include <QPainter>
#include <QMouseEvent>
#include <QStyledItemDelegate>
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
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>

#include "gui/keyring.h"
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

constexpr const char* kRemoteMime = "application/x-vnxfer-remote";

// Paints a clickable "x" on the right edge of every history entry.
class HistoryDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    std::function<void(int)> removeRequested;

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        QStyledItemDelegate::paint(painter, option, index);
        QRect closeRect(option.rect.right() - 18, option.rect.top(), 18, option.rect.height());
        painter->save();
        painter->setPen(option.state & QStyle::State_Selected ? Qt::white : Qt::gray);
        painter->drawText(closeRect, Qt::AlignCenter, QString::fromUtf8("×"));
        painter->restore();
    }


};

class DropTreeView : public QTreeView {
public:
    bool remoteSide = false;
    std::function<void(const QString& path, bool isDir)> dropRemoteItem;
    std::function<void(const QString& localPath)> dropLocalFile;

protected:
    void startDrag(Qt::DropActions actions) override {
        if (!remoteSide) {
            QTreeView::startDrag(actions);
            return;
        }
        const auto indexes = selectionModel()->selectedRows(0);
        if (indexes.isEmpty()) return;
        QByteArray payload;
        QDataStream stream(&payload, QIODevice::WriteOnly);
        for (const auto& index : indexes) {
            stream << index.data(kPathRole).toString() << index.data(kDirRole).toBool();
        }
        auto* mime = new QMimeData();
        mime->setData(kRemoteMime, payload);
        auto* drag = new QDrag(this);
        drag->setMimeData(mime);
        drag->exec(Qt::CopyAction);
    }

    void dropEvent(QDropEvent* event) override {
        if (remoteSide) {
            if (event->mimeData()->hasUrls() && dropLocalFile) {
                for (const QUrl& url : event->mimeData()->urls()) {
                    if (url.isLocalFile()) dropLocalFile(url.toLocalFile());
                }
                event->acceptProposedAction();
                return;
            }
        } else if (event->mimeData()->hasFormat(kRemoteMime) && dropRemoteItem) {
            QDataStream stream(event->mimeData()->data(kRemoteMime));
            while (!stream.atEnd()) {
                QString path;
                bool isDir = false;
                stream >> path >> isDir;
                dropRemoteItem(path, isDir);
            }
            event->acceptProposedAction();
            return;
        }
        QTreeView::dropEvent(event);
    }
};

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
    connect(worker_, &SessionWorker::logged, this, [this](const QString& line) {
        if (logView_) logView_->appendPlainText(line);
    });
    connect(worker_, &SessionWorker::capabilitiesReady, this, [this](const QString& text) {
        capabilities_ = text;
    });
    connect(worker_, &SessionWorker::verified, this, &MainWindow::onVerified);
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

    auto* tree = new DropTreeView();
    tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    tree->setSortingEnabled(true);
    tree->setDragEnabled(true);
    tree->setAcceptDrops(true);
    tree->setDropIndicatorShown(true);
    tree->setDragDropMode(QAbstractItemView::DragDrop);
    *view = tree;
    layout->addWidget(tree, 1);
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
    auto* historyDelegate = new HistoryDelegate(this);
    historyDelegate->removeRequested = [this](int row) { removeHistoryAt(row); };
    history_->setItemDelegate(historyDelegate);
    history_->view()->viewport()->installEventFilter(this);
    top->addWidget(history_);
    forgetButton_ = new QToolButton();
    forgetButton_->setText(QString::fromUtf8("×"));
    forgetButton_->setToolTip("Forget this host");
    top->addWidget(forgetButton_);
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
    probe_ = new QPushButton("Probe");
    top->addWidget(probe_);
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

    auto* dock = new QDockWidget("Log", this);
    logView_ = new QPlainTextEdit();
    logView_->setReadOnly(true);
    logView_->setMaximumBlockCount(500);
    dock->setWidget(logView_);
    addDockWidget(Qt::BottomDockWidgetArea, dock);
    dock->hide();

    setCentralWidget(central);

    auto* settingsMenu = menuBar()->addMenu("Settings");
    rateAction_ = settingsMenu->addAction("Rate limit...");
    connect(rateAction_, &QAction::triggered, this, &MainWindow::configureRateLimit);
    verifyAction_ = settingsMenu->addAction("Verify uploads (MD5)");
    verifyAction_->setCheckable(true);
    verifyAction_->setChecked(verifyUploads_);
    connect(verifyAction_, &QAction::toggled, this, [this](bool on) { verifyUploads_ = on; });
    auto* connectionMenu = menuBar()->addMenu("Connection");
    infoAction_ = connectionMenu->addAction("Info");
    connect(infoAction_, &QAction::triggered, this, &MainWindow::showConnectionInfo);

    auto* viewMenu = menuBar()->addMenu("View");
    hiddenAction_ = viewMenu->addAction("Show hidden files");
    hiddenAction_->setCheckable(true);
    connect(hiddenAction_, &QAction::toggled, this, &MainWindow::toggleHidden);
    auto* languageMenu = viewMenu->addMenu("Language");
    auto* englishAction = languageMenu->addAction("English");
    auto* chineseAction = languageMenu->addAction("中文");
    connect(englishAction, &QAction::triggered, this, [this] { applyLanguage(false); });
    connect(chineseAction, &QAction::triggered, this, [this] { applyLanguage(true); });

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
    connect(probe_, &QPushButton::clicked, this, &MainWindow::onProbeClicked);
    connect(worker_, &SessionWorker::probeResult, this, [this](const QString& text) {
        QMessageBox::information(this, "Probe", text);
    });
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
    auto* localTree = static_cast<DropTreeView*>(localView_);
    localTree->remoteSide = false;
    localTree->dropRemoteItem = [this](const QString& path, bool isDir) {
        enqueueDownload(path, isDir);
    };
    auto* remoteTree = static_cast<DropTreeView*>(remoteView_);
    remoteTree->remoteSide = true;
    remoteTree->dropLocalFile = [this](const QString& localPath) { enqueueUpload(localPath); };

    connect(remoteView_, &QTreeView::doubleClicked, this, &MainWindow::onRemoteDoubleClicked);
    connect(remoteView_, &QTreeView::expanded, this, &MainWindow::onRemoteExpanded);
    connect(remoteView_->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [this](const QItemSelection&, const QItemSelection&) { onRemoteSelectionChanged(); });
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
            currentRemoteDir_ = path;
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

void MainWindow::enableUploadTest(const QString& local) { uploadTestLocal_ = local; }

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
        bool stored = false;
        if (vnxfer::keyring::available()) {
            stored = vnxfer::keyring::store(endpoint(), password_->text());
        }
        if (stored) {
            settings.remove("passwords/" + endpoint());
        } else {
            settings.setValue("passwords/" + endpoint(),
                              QString::fromLatin1(password_->text().toUtf8().toBase64()));
        }
    } else {
        settings.remove("passwords/" + endpoint());
        if (vnxfer::keyring::available()) vnxfer::keyring::remove(endpoint());
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
    currentRemoteDir_ = remoteRoot_;
    remotePathEdit_->setText(currentRemoteDir_);
    QSettings().setValue("lastPath/" + endpoint(), currentRemoteDir_);
    QSettings().setValue("lastPort/" + endpoint(), port_->value());
    refreshRemote();

    if (!selftestRemote_.isEmpty()) {
        enqueueJob({"download", selftestRemote_, selftestLocal_, "selftest"});
    }
    if (!uploadTestLocal_.isEmpty()) {
        const QString local = uploadTestLocal_;
        uploadTestLocal_.clear();
        QTimer::singleShot(1500, this, [this, local] { enqueueUpload(local); });
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
        const QStringList expanded = collectExpanded(remoteModel_->invisibleRootItem());
        QStandardItem* item = findRemoteItem(path);
        if (!item) {
            item = new QStandardItem(QFileIconProvider().icon(QFileIconProvider::Folder),
                                     QFileInfo(path).fileName().isEmpty()
                                         ? QString("/")
                                         : QFileInfo(path).fileName());
            item->setData(path, kPathRole);
            item->setData(true, kDirRole);
            item->setData(true, kLoadedRole);
            item->setEditable(false);
            remoteModel_->appendRow(item);
        }
        fillRemoteItem(item, entries);
        remoteView_->setRootIndex(remoteProxy_->mapFromSource(remoteModel_->indexFromItem(item)));
        remoteView_->expand(remoteProxy_->mapFromSource(remoteModel_->indexFromItem(item)));
        restoreExpanded(expanded);
        if (currentRemoteDir_.isEmpty()) currentRemoteDir_ = path;
        remotePathEdit_->setText(currentRemoteDir_);
        setBusy(false);
        return;
    }
    QStandardItem* item = findRemoteItem(path);
    if (item) {
        const QStringList expanded = collectExpanded(item);
        fillRemoteItem(item, entries);
        restoreExpanded(expanded);
    }
    setBusy(false);
}

QStringList MainWindow::collectExpanded(QStandardItem* item) const {
    QStringList paths;
    if (!item) return paths;
    const QModelIndex index = remoteModel_->indexFromItem(item);
    if (index.isValid() && remoteView_->isExpanded(remoteProxy_->mapFromSource(index))) {
        const QString path = item->data(kPathRole).toString();
        if (!path.isEmpty()) paths << path;
    }
    for (int i = 0; i < item->rowCount(); ++i) paths += collectExpanded(item->child(i));
    return paths;
}

void MainWindow::restoreExpanded(const QStringList& paths) {
    for (const QString& path : paths) {
        QStandardItem* item = findRemoteItem(path);
        if (!item) continue;
        remoteView_->expand(remoteProxy_->mapFromSource(remoteModel_->indexFromItem(item)));
    }
}

void MainWindow::refreshPath(const QString& path) {
    if (!connected_ || path.isEmpty()) return;
    setBusy(true);
    QMetaObject::invokeMethod(worker_, "list", Qt::QueuedConnection, Q_ARG(QString, path));
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

void MainWindow::onRemoteSelectionChanged() {
    const QModelIndex index = remoteView_->currentIndex();
    if (!index.isValid()) return;
    const QString path = index.data(kPathRole).toString();
    if (path.isEmpty()) return;
    const bool isDir = index.data(kDirRole).toBool();
    currentRemoteDir_ = isDir ? path : QFileInfo(path).path();
    if (currentRemoteDir_.isEmpty()) currentRemoteDir_ = "/";
    remotePathEdit_->setText(currentRemoteDir_);
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
    currentRemoteDir_ = remoteRoot_;
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

void MainWindow::applyLanguage(bool chinese) {
    chinese_ = chinese;
    QSettings().setValue("language", chinese ? "zh" : "en");
    auto t = [chinese](const char* en, const char* zh) {
        return chinese ? QString::fromUtf8(zh) : QString::fromUtf8(en);
    };
    connect_->setText(connected_ ? t("Disconnect", "断开") : t("Connect", "连接"));
    probe_->setText(t("Probe", "探测"));
    rateAction_->setText(t("Rate limit...", "限速..."));
    verifyAction_->setText(t("Verify uploads (MD5)", "上传 MD5 校验"));
    infoAction_->setText(t("Info", "信息"));
    hiddenAction_->setText(t("Show hidden files", "显示隐藏文件"));

    upload_->setText(t("Upload →", "上传 →"));
    download_->setText(t("← Download", "← 下载"));
    newFolder_->setText(t("New folder", "新建文件夹"));
    newFile_->setText(t("New file", "新建文件"));
    delete_->setText(t("Delete", "删除"));
    rename_->setText(t("Rename", "重命名"));
    cancelJob_->setText(t("Cancel", "取消"));
    localUp_->setText(t("Up", "上级"));
    localRefresh_->setText(t("Refresh", "刷新"));
    remoteUp_->setText(t("Up", "上级"));
    remoteRefresh_->setText(t("Refresh", "刷新"));
    remoteModel_->setHorizontalHeaderLabels(
        {t("Name", "名称"), t("Size", "大小"), t("Modified", "修改时间")});
}

void MainWindow::onProbeClicked() {
    const QString host = history_->currentText().trimmed();
    if (host.isEmpty()) return;
    status_->setText("probing " + endpoint() + " ...");
    QMetaObject::invokeMethod(worker_, "probe", Qt::QueuedConnection, Q_ARG(QString, host),
                              Q_ARG(quint16, static_cast<quint16>(port_->value())),
                              Q_ARG(QString, password_->text()));
}

void MainWindow::removeHistoryAt(int row) {
    const QString host = history_->itemText(row).trimmed();
    if (!host.isEmpty()) removeHost(host);
}

void MainWindow::removeHost(const QString& host) {
    if (host.isEmpty()) return;
    if (QMessageBox::question(this, "vnxfer", QString("Forget %1?").arg(host)) !=
        QMessageBox::Yes) {
        return;
    }
    QSettings settings;
    QStringList history = settings.value("history").toStringList();
    history.removeAll(host);
    settings.setValue("history", history);

    // Drop every saved endpoint that belongs to this host.
    const QString prefixPw = "passwords/" + host + ":";
    const QString prefixPath = "lastPath/" + host + ":";
    const QString prefixPort = "lastPort/" + host + ":";
    for (const QString& key : settings.allKeys()) {
        if (key.startsWith(prefixPw) || key.startsWith(prefixPath) || key.startsWith(prefixPort)) {
            const QString ep = key.section('/', 1);
            if (vnxfer::keyring::available()) vnxfer::keyring::remove(ep);
            settings.remove(key);
        }
    }

    history_->blockSignals(true);
    history_->clear();
    history_->addItems(history);
    history_->setCurrentText(host);
    history_->blockSignals(false);
    status_->setText("forgot " + host);
}

void MainWindow::configureRateLimit() {
    bool ok = false;
    const int current = rateLimit_ / 1024;
    const int kb = QInputDialog::getInt(this, "Rate limit", "KB/s (0 = unlimited):", current, 0,
                                        1024 * 1024, 64, &ok);
    if (!ok) return;
    rateLimit_ = kb * 1024;
    QMetaObject::invokeMethod(worker_, "setRateLimit", Qt::QueuedConnection, Q_ARG(int, rateLimit_));
    status_->setText(kb == 0 ? "rate limit off" : QString("rate limit %1 KB/s").arg(kb));
}

void MainWindow::showConnectionInfo() {
    QMessageBox::information(this, "Connection info",
                             QString("Endpoint: %1\n\n%2").arg(
                                 endpoint(), capabilities_.isEmpty() ? "no capability data"
                                                                    : capabilities_));
}

void MainWindow::onVerified(const QString& remote, bool ok, const QString& detail) {
    status_->setText(ok ? "md5 ok" : "md5 MISMATCH");
    if (logView_) logView_->appendPlainText(QString("%1: %2 (%3)").arg(remote, ok ? "ok" : "MISMATCH", detail));
    if (!ok) QMessageBox::warning(this, "Checksum", "MD5 mismatch for " + remote + "\n" + detail);
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

quint64 MainWindow::selectedRemoteSize() const {
    const auto indexes = remoteView_->selectionModel()->selectedRows(0);
    if (indexes.isEmpty()) return 0;
    const QModelIndex index = indexes.first();
    const QModelIndex sizeIndex = index.sibling(index.row(), 1);
    return sizeIndex.data(kSortRole).toULongLong();
}

void MainWindow::uploadSelected() {
    if (!connected_) return;
    const auto indexes = localView_->selectionModel()->selectedRows(0);
    if (indexes.isEmpty()) return;
    enqueueUpload(localModel_->filePath(indexes.first()));
}

void MainWindow::enqueueDownload(const QString& remote, bool isDir) {
    const QString name = QFileInfo(remote).fileName();
    const QString local = QDir(localRoot_).filePath(name);
    if (isDir) {
        enqueueJob({"download-tree", remote, local, "↓ " + name + "/"});
    } else {
        enqueueJob({"download", remote, local, "↓ " + name});
    }
}

void MainWindow::enqueueUpload(const QString& local) {
    QFileInfo info(local);
    const QString targetDir = currentRemoteDir_.isEmpty() ? remoteRoot_ : currentRemoteDir_;
    QString remote = targetDir;
    if (!remote.endsWith('/')) remote += '/';
    remote += info.fileName();

    QStandardItem* existing = findRemoteItem(remote);
    if (existing) {
        const bool existingIsDir = existing->data(kDirRole).toBool();
        if (info.isDir() && existingIsDir) {
            if (QMessageBox::question(
                    this, "Merge",
                    QString("Folder %1 already exists. Merge into it?").arg(remote)) !=
                QMessageBox::Yes) {
                return;
            }
        } else if (!info.isDir() && !existingIsDir) {
            if (QMessageBox::question(
                    this, "Overwrite",
                    QString("%1 already exists. Overwrite it?").arg(remote)) !=
                QMessageBox::Yes) {
                return;
            }
        }
    }

    if (info.isDir()) {
        enqueueJob({"upload-tree", local, remote, "↑ " + info.fileName() + "/"});
        return;
    }

    // If the remote file is already partially there, offer to continue it.
    QStandardItem* item = findRemoteItem(remote);
    quint64 remoteSize = 0;
    if (item && !item->data(kDirRole).toBool()) {
        const QModelIndex index = remoteModel_->indexFromItem(item);
        remoteSize = index.sibling(index.row(), 1).data(kSortRole).toULongLong();
    }
    const quint64 localSize = static_cast<quint64>(info.size());
    if (remoteSize > 0 && remoteSize < localSize) {
        const auto answer = QMessageBox::question(
            this, "Resume",
            QString("Remote file has %1 of %2 bytes. Continue uploading?").arg(remoteSize).arg(localSize));
        if (answer == QMessageBox::Yes) {
            enqueueJob({"upload-resume", local, remote, "↻ " + info.fileName()});
            return;
        }
    }
    enqueueJob({"upload", local, remote, "↑ " + info.fileName()});
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
    currentJob_ = job;
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
    } else if (job.kind == "upload-resume") {
        QStandardItem* item = findRemoteItem(job.b);
        quint64 offset = 0;
        if (item) {
            const QModelIndex index = remoteModel_->indexFromItem(item);
            offset = index.sibling(index.row(), 1).data(kSortRole).toULongLong();
        }
        QMetaObject::invokeMethod(worker_, "uploadResume", Qt::QueuedConnection,
                                  Q_ARG(QString, job.a), Q_ARG(QString, job.b),
                                  Q_ARG(quint64, offset));
    } else if (job.kind == "download-resume") {
        const quint64 offset = static_cast<quint64>(QFileInfo(job.b).size());
        QMetaObject::invokeMethod(worker_, "downloadResume", Qt::QueuedConnection,
                                  Q_ARG(QString, job.a), Q_ARG(QString, job.b),
                                  Q_ARG(quint64, offset));
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
    if (jobs_.isEmpty() && jobList_->count() == 0) jobList_->setVisible(false);
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
    const bool isDir = selectedRemoteIsDir();
    const QString name = QFileInfo(remote).fileName();
    const QString local = QDir(localDir).filePath(name);
    if (isDir) {
        enqueueJob({"download-tree", remote, local, "↓ " + name + "/"});
        return;
    }
    const quint64 have = QFileInfo(local).size();
    const quint64 remoteSize = selectedRemoteSize();
    if (have > 0 && remoteSize > have) {
        const auto answer = QMessageBox::question(
            this, "Resume",
            QString("Local file has %1 of %2 bytes. Resume?")
                .arg(have)
                .arg(remoteSize));
        if (answer == QMessageBox::Yes) {
            enqueueJob({"download-resume", remote, local, "↻ " + name});
            return;
        }
    }
    enqueueJob({"download", remote, local, "↓ " + name});
}

void MainWindow::makeFolder() {
    if (!connected_) return;
    bool ok = false;
    const QString name = QInputDialog::getText(this, "New folder", "Name:", QLineEdit::Normal,
                                               "new-folder", &ok);
    if (!ok || name.trimmed().isEmpty()) return;
    QString path = currentRemoteDir_.isEmpty() ? remoteRoot_ : currentRemoteDir_;
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

    QString path = currentRemoteDir_.isEmpty() ? remoteRoot_ : currentRemoteDir_;
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
    // Refresh only the directory that changed so expanded branches stay open.
    QString changedDir;
    if (what == "upload") {
        changedDir = QFileInfo(currentJob_.b).path();
    } else if (what == "download") {
        changedDir = QFileInfo(currentJob_.a).path();
    } else if (what == "remove" || what == "rename" || what == "mkdir") {
        changedDir = QFileInfo(currentJob_.a).path();
    }
    if (!changedDir.isEmpty() && changedDir != "/") refreshPath(changedDir);
    if (what == "upload" && !pendingTempFile_.isEmpty()) {
        QFile::remove(pendingTempFile_);
        pendingTempFile_.clear();
    }
    if (what == "upload" && verifyUploads_ && !currentJob_.a.isEmpty() &&
        currentJob_.kind == "upload") {
        QMetaObject::invokeMethod(worker_, "verifyUpload", Qt::QueuedConnection,
                                  Q_ARG(QString, currentJob_.a), Q_ARG(QString, currentJob_.b));
    }
    finishCurrentJob();
}

void MainWindow::onFailed(const QString& what, const QString& message) {
    setBusy(false);
    progress_->setValue(0);
    status_->setText(what + " failed");
    if (what == "keepalive") {
        // A dropped keepalive is informational; do not interrupt with a dialog.
        if (logView_) logView_->appendPlainText("keepalive failed: " + message);
        return;
    }
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

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    if (history_ && watched == history_->view()->viewport() &&
        (event->type() == QEvent::MouseButtonPress ||
         event->type() == QEvent::MouseButtonRelease)) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        QAbstractItemView* view = history_->view();
        const QModelIndex index = view->indexAt(mouse->pos());
        if (index.isValid()) {
            const QRect rect = view->visualRect(index);
            if (mouse->pos().x() > rect.right() - 22) {
                if (event->type() == QEvent::MouseButtonPress) removeHistoryAt(index.row());
                return true;
            }
        }
    }
    return QMainWindow::eventFilter(watched, event);
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
    if (settings.value("language", "en").toString() == "zh") applyLanguage(true);

    QString savedPassword;
    if (vnxfer::keyring::available()) savedPassword = vnxfer::keyring::lookup(endpoint());
    if (savedPassword.isEmpty()) {
        const QByteArray saved = settings.value("passwords/" + endpoint()).toByteArray();
        if (!saved.isEmpty()) savedPassword = QString::fromUtf8(QByteArray::fromBase64(saved));
    }
    password_->setText(savedPassword);


    // Editing the password or port refreshes the saved entry for this host.
    connect(password_, &QLineEdit::editingFinished, this, [this] {
        if (!remember_->isChecked()) return;
        const QString host = history_->currentText().trimmed();
        if (host.isEmpty()) return;
        const QString ep = QString("%1:%2").arg(host).arg(port_->value());
        bool stored = false;
        if (vnxfer::keyring::available()) stored = vnxfer::keyring::store(ep, password_->text());
        if (stored) {
            QSettings().remove("passwords/" + ep);
        } else {
            QSettings().setValue("passwords/" + ep,
                                 QString::fromLatin1(password_->text().toUtf8().toBase64()));
        }
        addHistory(host);
    });
    connect(port_, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int value) {
        const QString host = history_->currentText().trimmed();
        if (host.isEmpty()) return;
        QSettings().setValue(QString("lastPort/%1:%2").arg(host).arg(value), value);
    });

    connect(forgetButton_, &QToolButton::clicked, this, [this] {
        removeHost(history_->currentText().trimmed());
    });

    connect(history_, &QComboBox::currentTextChanged, this, [this](const QString& host) {
        if (host.trimmed().isEmpty()) return;
        QSettings s;
        // Each remembered host acts as its own profile: port, password and last path.
        const QString base = QString("%1:%2").arg(host.trimmed()).arg(port_->value());
        if (s.contains("lastPort/" + base)) {
            port_->setValue(s.value("lastPort/" + base).toInt());
        }
        const QString ep = QString("%1:%2").arg(host.trimmed()).arg(port_->value());
        QString pw;
        if (vnxfer::keyring::available()) pw = vnxfer::keyring::lookup(ep);
        if (pw.isEmpty()) {
            const QByteArray savedPw = s.value("passwords/" + ep).toByteArray();
            if (!savedPw.isEmpty()) pw = QString::fromUtf8(QByteArray::fromBase64(savedPw));
        }
        if (!pw.isEmpty()) password_->setText(pw);
        const QString lastPath = s.value("lastPath/" + ep).toString();
        if (!lastPath.isEmpty()) initialPath_ = lastPath;
    });
}

void MainWindow::saveSettings() {
    QSettings settings;
    settings.setValue("port", port_->value());
    settings.setValue("remember", remember_->isChecked());
    settings.setValue("showHidden", showHidden_);
}
