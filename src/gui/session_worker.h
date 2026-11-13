#pragma once

#include <QElapsedTimer>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVector>
#include <memory>

#include "core/ft/open.h"
#include "core/ft/retry.h"
#include "core/ft/session.h"

// Qt-facing copy of vnxfer::RemoteEntry (keeps the core free of Qt types).
struct RemoteEntryQt {
    QString name;
    qint64 size = 0;
    qint64 mtime = 0;
    bool isDir = false;
};

Q_DECLARE_METATYPE(RemoteEntryQt)
Q_DECLARE_METATYPE(QVector<RemoteEntryQt>)

class SessionWorker : public QObject {
    Q_OBJECT

public:
    explicit SessionWorker(QObject* parent = nullptr);
    ~SessionWorker() override;

public slots:
    void connectTo(const QString& host, quint16 port, const QString& password, int timeoutMs);
    void disconnectFrom();
    void list(const QString& path);
    void download(const QString& remote, const QString& local);
    void downloadResume(const QString& remote, const QString& local, quint64 offset);
    void upload(const QString& local, const QString& remote, bool overwrite);
    void uploadResume(const QString& local, const QString& remote, quint64 offset);
    void downloadTree(const QString& remote, const QString& local);
    void uploadTree(const QString& local, const QString& remote);
    void makeDir(const QString& path);
    void removePath(const QString& path);
    void renamePath(const QString& from, const QString& to);
    void setRateLimit(int bytesPerSecond);
    void probe(const QString& host, quint16 port, const QString& password);
    void verifyUpload(const QString& local, const QString& remote);
    void cancel();

signals:
    void connected(const QString& protocol, const QString& root);
    void listed(const QString& path, const QVector<RemoteEntryQt>& entries);
    void progress(quint64 done, quint64 total);
    void finished(const QString& what);
    void failed(const QString& what, const QString& message);
    void logged(const QString& line);
    void verified(const QString& remote, bool ok, const QString& detail);
    void capabilitiesReady(const QString& text);
    void probeResult(const QString& text);

private:
    template <typename Fn>
    void guarded(const QString& what, Fn&& fn);

    bool reconnect();
    void pingOnce();
    void throttle(quint64 done);

    std::unique_ptr<vnxfer::FtSession> session_;
    vnxfer::ConnectionOptions options_;
    vnxfer::RetryPolicy retry_;
    bool canceling_ = false;
    int rate_limit_ = 0;
    QElapsedTimer throttle_timer_;
    QTimer* ping_timer_ = nullptr;
};
