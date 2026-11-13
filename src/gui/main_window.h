#pragma once

#include <QMainWindow>
#include <QQueue>
#include <QThread>
#include <QVector>

#include "gui/session_worker.h"

class QCheckBox;
class QComboBox;
class QListWidget;
class QFileSystemModel;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QSortFilterProxyModel;
class QSpinBox;
class QStandardItem;
class QStandardItemModel;
class QTreeView;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    MainWindow();
    ~MainWindow() override;
    void autoConnect(const QString& host, int port, const QString& password);
    void setInitialPath(const QString& path);
    void enableSelfTest(const QString& remote, const QString& local);

protected:
    void closeEvent(QCloseEvent* event) override;

private slots:
    void onConnectClicked();
    void onConnected(const QString& protocol, const QString& root);
    void onListed(const QString& path, const QVector<RemoteEntryQt>& entries);
    void onProgress(quint64 done, quint64 total);
    void onFinished(const QString& what);
    void onFailed(const QString& what, const QString& message);
    void onRemoteDoubleClicked(const QModelIndex& index);
    void onRemoteExpanded(const QModelIndex& index);
    void remoteGoUp();
    void refreshRemote();
    void localGoUp();
    void refreshLocal();
    void toggleHidden(bool show);
    void uploadSelected();
    void downloadSelected();
    void makeFolder();
    void makeFile();
    void deleteSelected();
    void renameSelected();

private:
    struct TransferJob {
        QString kind;
        QString a;
        QString b;
        QString label;
    };

    void enqueueDownload(const QString& remote, bool isDir);
    void enqueueUpload(const QString& local);
    void enqueueJob(const TransferJob& job);
    void startNextJob();
    void finishCurrentJob();

    void buildUi();
    QWidget* buildBrowserPane(const QString& title, QLineEdit** pathEdit, QPushButton** up,
                              QPushButton** refresh, QTreeView** view);
    void loadSettings();
    void saveSettings();
    void setBusy(bool busy);
    void addHistory(const QString& entry);
    QString endpoint() const;
    QString selectedRemotePath() const;
    bool selectedRemoteIsDir() const;
    void fillRemoteItem(QStandardItem* item, const QVector<RemoteEntryQt>& entries);
    QStandardItem* findRemoteItem(const QString& path);

    QThread thread_;
    SessionWorker* worker_ = nullptr;

    QComboBox* history_ = nullptr;
    QSpinBox* port_ = nullptr;
    QLineEdit* password_ = nullptr;
    QCheckBox* remember_ = nullptr;
    QPushButton* connect_ = nullptr;

    QLineEdit* localPathEdit_ = nullptr;
    QPushButton* localUp_ = nullptr;
    QPushButton* localRefresh_ = nullptr;
    QTreeView* localView_ = nullptr;
    QFileSystemModel* localModel_ = nullptr;

    QLineEdit* remotePathEdit_ = nullptr;
    QPushButton* remoteUp_ = nullptr;
    QPushButton* remoteRefresh_ = nullptr;
    QTreeView* remoteView_ = nullptr;
    QStandardItemModel* remoteModel_ = nullptr;
    QSortFilterProxyModel* remoteProxy_ = nullptr;

    QPushButton* upload_ = nullptr;
    QPushButton* download_ = nullptr;
    QPushButton* newFolder_ = nullptr;
    QPushButton* newFile_ = nullptr;
    QPushButton* delete_ = nullptr;
    QPushButton* rename_ = nullptr;

    QProgressBar* progress_ = nullptr;
    QLabel* status_ = nullptr;
    QListWidget* jobList_ = nullptr;
    QPushButton* cancelJob_ = nullptr;
    QQueue<TransferJob> jobs_;
    bool jobRunning_ = false;

    QString localRoot_;
    QString remoteRoot_ = "/";
    QString initialPath_;
    QString pendingTempFile_;
    QString selftestRemote_;
    QString selftestLocal_;
    bool connected_ = false;
    bool showHidden_ = false;
};
