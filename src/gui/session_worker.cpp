#include "gui/session_worker.h"

#include <QElapsedTimer>
#include <stdexcept>

SessionWorker::SessionWorker(QObject* parent) : QObject(parent) {}

SessionWorker::~SessionWorker() = default;

bool SessionWorker::reconnect() {
    try {
        session_ = vnxfer::open_session(options_);
        emit connected(QString::fromStdString(session_->describe()),
                       QString::fromStdString(session_->default_root()));
        return true;
    } catch (const std::exception&) {
        session_.reset();
        return false;
    }
}

template <typename Fn>
void SessionWorker::guarded(const QString& what, Fn&& fn) {
    try {
        vnxfer::with_retry(retry_, [&](int attempt) {
            if (attempt > 0 && !reconnect()) {
                throw std::runtime_error("reconnect failed");
            }
            if (!session_) throw std::runtime_error("not connected");
            fn();
        });
        emit finished(what);
    } catch (const std::exception& e) {
        emit failed(what, QString::fromUtf8(e.what()));
    }
}

void SessionWorker::connectTo(const QString& host, quint16 port, const QString& password,
                              int timeoutMs) {
    options_.host = host.toStdString();
    options_.port = port;
    options_.password = password.toStdString();
    options_.timeout_ms = timeoutMs;
    try {
        session_ = vnxfer::open_session(options_);
        emit connected(QString::fromStdString(session_->describe()),
                       QString::fromStdString(session_->default_root()));
    } catch (const std::exception& e) {
        session_.reset();
        emit failed("connect", QString::fromUtf8(e.what()));
    }
}

void SessionWorker::disconnectFrom() {
    session_.reset();
    emit finished("disconnect");
}

void SessionWorker::list(const QString& path) {
    guarded("list", [&] {
        auto entries = session_->list(path.toStdString());
        QVector<RemoteEntryQt> out;
        out.reserve(static_cast<int>(entries.size()));
        for (const auto& entry : entries) {
            out.push_back(RemoteEntryQt{QString::fromStdString(entry.name),
                                        static_cast<qint64>(entry.size),
                                        static_cast<qint64>(entry.mtime), entry.is_dir});
        }
        emit listed(path, out);
    });
}

void SessionWorker::download(const QString& remote, const QString& local) {
    guarded("download", [&] {
        QElapsedTimer timer;
        timer.start();
        quint64 last_emit = 0;
        session_->download(remote.toStdString(), local.toStdString(),
                           [&](uint64_t done, uint64_t total) {
                               if (timer.elapsed() - static_cast<qint64>(last_emit) < 100 &&
                                   done != total) {
                                   return;
                               }
                               last_emit = static_cast<quint64>(timer.elapsed());
                               emit progress(done, total);
                           });
    });
}

void SessionWorker::upload(const QString& local, const QString& remote, bool overwrite) {
    guarded("upload", [&] {
        QElapsedTimer timer;
        timer.start();
        quint64 last_emit = 0;
        session_->upload(local.toStdString(), remote.toStdString(), overwrite,
                         [&](uint64_t done, uint64_t total) {
                             if (timer.elapsed() - static_cast<qint64>(last_emit) < 100 &&
                                 done != total) {
                                 return;
                             }
                             last_emit = static_cast<quint64>(timer.elapsed());
                             emit progress(done, total);
                         });
    });
}

void SessionWorker::makeDir(const QString& path) {
    guarded("mkdir", [&] { session_->mkdir(path.toStdString()); });
}

void SessionWorker::removePath(const QString& path) {
    guarded("remove", [&] { session_->remove(path.toStdString()); });
}

void SessionWorker::renamePath(const QString& from, const QString& to) {
    guarded("rename", [&] { session_->rename(from.toStdString(), to.toStdString()); });
}
