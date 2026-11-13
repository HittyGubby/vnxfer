#pragma once

#include <QMainWindow>
#include <QThread>
#include <QVector>

#include "gui/session_worker.h"

class QComboBox;
class QFileSystemModel;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QSortFilterProxyModel;
class QSpinBox;
class QStandardItemModel;
class QTreeView;
class QCheckBox;
class SessionWorker;

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
    void goUp();
    void refreshRemote();
    void uploadSelected();
    void downloadSelected();
    void makeFolder();
    void makeFile();
    void deleteSelected();
    void renameSelected();

private:
    void buildUi();
    void loadSettings();
    void saveSettings();
    void setBusy(bool busy);
    QString remotePath() const;
    QString selectedRemotePath() const;
    bool selectedRemoteIsDir() const;
    QString endpoint() const;
    void addHistory(const QString& entry);
    void refreshLocal();

    QThread thread_;
    SessionWorker* worker_ = nullptr;

    QComboBox* history_ = nullptr;
    QSpinBox* port_ = nullptr;
    QLineEdit* password_ = nullptr;
    QCheckBox* remember_ = nullptr;
    QPushButton* connect_ = nullptr;

    QTreeView* localView_ = nullptr;
    QFileSystemModel* localModel_ = nullptr;

    QLineEdit* remotePathEdit_ = nullptr;
    QTreeView* remoteView_ = nullptr;
    QStandardItemModel* remoteModel_ = nullptr;
    QSortFilterProxyModel* remoteProxy_ = nullptr;

    QPushButton* upload_ = nullptr;
    QPushButton* download_ = nullptr;
    QPushButton* newFolder_ = nullptr;
    QPushButton* newFile_ = nullptr;
    QPushButton* delete_ = nullptr;
    QPushButton* rename_ = nullptr;
    QPushButton* up_ = nullptr;
    QPushButton* refresh_ = nullptr;

    QProgressBar* progress_ = nullptr;
    QLabel* status_ = nullptr;

    QString remotePath_ = "/";
    QString initialPath_;
    QString pendingTempFile_;
    QString selftestRemote_;
    QString selftestLocal_;
    bool connected_ = false;
};
