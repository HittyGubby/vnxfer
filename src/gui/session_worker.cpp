#include "gui/session_worker.h"

#include "core/ft/backend.h"
#include "core/rfb_client.h"
#include "core/rfb_protocol.h"

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QThread>
#include <stdexcept>

SessionWorker::SessionWorker(QObject* parent) : QObject(parent) {
    ping_timer_ = new QTimer(this);
    ping_timer_->setInterval(45000);
    connect(ping_timer_, &QTimer::timeout, this, &SessionWorker::pingOnce);
}

SessionWorker::~SessionWorker() = default;

bool SessionWorker::reconnect() {
    try {
        session_ = vnxfer::open_session(options_);
        emit connected(QString::fromStdString(session_->describe()),
                       QString::fromStdString(session_->default_root()));
        emit capabilitiesReady(QString::fromStdString(session_->capabilities()));
        return true;
    } catch (const std::exception&) {
        session_.reset();
        return false;
    }
}

template <typename Fn>
void SessionWorker::guarded(const QString& what, Fn&& fn) {
    canceling_ = false;
    try {
        const vnxfer::RetryPolicy policy = retry_;
        vnxfer::with_retry(policy, [&](int attempt) {
            if (canceling_) throw std::runtime_error("cancelled");
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
        emit capabilitiesReady(QString::fromStdString(session_->capabilities()));
        ping_timer_->start();
    } catch (const std::exception& e) {
        session_.reset();
        emit failed("connect", QString::fromUtf8(e.what()));
    }
}

void SessionWorker::disconnectFrom() {
    ping_timer_->stop();
    session_.reset();
    emit finished("disconnect");
}

void SessionWorker::throttle(quint64 done) {
    if (rate_limit_ <= 0) return;
    if (!throttle_timer_.isValid()) {
        throttle_timer_.start();
        return;
    }
    const double expected = static_cast<double>(done) / static_cast<double>(rate_limit_);
    const double actual = static_cast<double>(throttle_timer_.elapsed()) / 1000.0;
    if (expected > actual) {
        QThread::msleep(static_cast<unsigned long>((expected - actual) * 1000.0));
    }
}

void SessionWorker::pingOnce() {
    if (!session_) return;
    try {
        session_->ping();
    } catch (const std::exception& e) {
        emit logged("keepalive failed: " + QString::fromUtf8(e.what()));
        emit failed("keepalive", QString::fromUtf8(e.what()));
        ping_timer_->stop();
    }
}

void SessionWorker::setRateLimit(int bytesPerSecond) { rate_limit_ = bytesPerSecond; }

void SessionWorker::probe(const QString& host, quint16 port, const QString& password) {
    QString report;
    try {
        vnxfer::RfbClient client(host.toStdString(), port, password.toStdString(), 5000);
        client.connect();
        client.handshake(8, true);
        report += QString("banner: %1\n").arg(QString::fromStdString(client.server_version()));
        report += QString("server: %1x%2 \"%3\"\n")
                      .arg(client.server_info().width)
                      .arg(client.server_info().height)
                      .arg(QString::fromStdString(client.server_info().name));

        bool found = false;
        if (client.caps().present &&
            client.caps().hasClient(vnxfer::rfb::kTv2CompressionSupportRequest)) {
            auto r = vnxfer::ft::probe_tightvnc2(client);
            report += QString("file transfer: tightvnc2 (%1)\n")
                          .arg(QString::fromStdString(r.detail));
            found = true;
        } else if (client.caps().present &&
                   client.caps().hasClient(vnxfer::rfb::kTv13FileListRequest)) {
            auto r = vnxfer::ft::probe_tightvnc13(client);
            report += QString("file transfer: tightvnc1.3 (%1)\n")
                          .arg(QString::fromStdString(r.detail));
            found = true;
        }
        if (!found) {
            client.socket().close();
            vnxfer::RfbClient plain(host.toStdString(), port, password.toStdString(), 5000);
            plain.connect();
            plain.handshake(8, false);
            auto r = vnxfer::ft::probe_ultravnc(plain);
            report += QString("file transfer: %1 (%2)\n")
                          .arg(QString::fromStdString(vnxfer::ft::to_string(r.kind)))
                          .arg(QString::fromStdString(r.detail));
        }
    } catch (const std::exception& e) {
        report += QString("probe failed: %1").arg(QString::fromUtf8(e.what()));
    }
    emit probeResult(report);
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
        throttle_timer_.invalidate();
        quint64 last_emit = 0;
        session_->download(remote.toStdString(), local.toStdString(),
                           [&](uint64_t done, uint64_t total) {
                               if (timer.elapsed() - static_cast<qint64>(last_emit) < 100 &&
                                   done != total) {
                                   return;
                               }
                               last_emit = static_cast<quint64>(timer.elapsed());
                               throttle(done);
                               emit progress(done, total);
                           });
    });
}

void SessionWorker::downloadResume(const QString& remote, const QString& local, quint64 offset) {
    guarded("download", [&] {
        QElapsedTimer timer;
        timer.start();
        throttle_timer_.invalidate();
        quint64 last_emit = 0;
        session_->download_resume(
            remote.toStdString(), local.toStdString(), offset,
            [&](uint64_t done, uint64_t total) {
                if (timer.elapsed() - static_cast<qint64>(last_emit) < 100 && done != total) return;
                last_emit = static_cast<quint64>(timer.elapsed());
                throttle(done);
                emit progress(done, total);
            });
    });
}

void SessionWorker::verifyUpload(const QString& local, const QString& remote) {
    guarded("verify", [&] {
        QFile file(local);
        if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("cannot open local file");
        QCryptographicHash hash(QCryptographicHash::Md5);
        if (!hash.addData(&file)) throw std::runtime_error("md5 failed");
        const QString local_md5 = QString::fromLatin1(hash.result().toHex());

        const int pos = remote.lastIndexOf('/');
        const QString dir = pos <= 0 ? QString("/") : remote.left(pos);
        const QString name = remote.mid(pos + 1);
        uint64_t size = 0;
        for (const auto& entry : session_->list(dir.toStdString())) {
            if (QString::fromStdString(entry.name) == name) size = entry.size;
        }
        if (size == 0) throw std::runtime_error("remote file not found");
        const QString remote_md5 =
            QString::fromStdString(session_->md5(remote.toStdString(), 0, size));
        emit verified(remote, local_md5 == remote_md5,
                      QString("local %1 / remote %2").arg(local_md5, remote_md5));
    });
}

void SessionWorker::upload(const QString& local, const QString& remote, bool overwrite) {
    guarded("upload", [&] {
        QElapsedTimer timer;
        timer.start();
        throttle_timer_.invalidate();
        quint64 last_emit = 0;
        session_->upload(local.toStdString(), remote.toStdString(), overwrite,
                         [&](uint64_t done, uint64_t total) {
                             if (timer.elapsed() - static_cast<qint64>(last_emit) < 100 &&
                                 done != total) {
                                 return;
                             }
                             last_emit = static_cast<quint64>(timer.elapsed());
                             throttle(done);
                             emit progress(done, total);
                         });
    });
}

void SessionWorker::uploadResume(const QString& local, const QString& remote, quint64 offset) {
    guarded("upload", [&] {
        QElapsedTimer timer;
        timer.start();
        throttle_timer_.invalidate();
        quint64 last_emit = 0;
        session_->upload_resume(
            local.toStdString(), remote.toStdString(), true, offset,
            [&](uint64_t done, uint64_t total) {
                if (timer.elapsed() - static_cast<qint64>(last_emit) < 100 && done != total) return;
                last_emit = static_cast<quint64>(timer.elapsed());
                throttle(done);
                emit progress(done, total);
            });
    });
}

void SessionWorker::downloadTree(const QString& remote, const QString& local) {
    guarded("download", [&] {
        QElapsedTimer timer;
        timer.start();
        throttle_timer_.invalidate();
        quint64 last_emit = 0;
        vnxfer::TransferStats stats;
        vnxfer::download_tree(*session_, remote.toStdString(), local.toStdString(),
                              [&](uint64_t done, uint64_t total) {
                                  if (timer.elapsed() - static_cast<qint64>(last_emit) < 100 &&
                                      done != total) {
                                      return;
                                  }
                                  last_emit = static_cast<quint64>(timer.elapsed());
                                  throttle(done);
                                  emit progress(done, total);
                              },
                              stats);
    });
}

void SessionWorker::uploadTree(const QString& local, const QString& remote) {
    guarded("upload", [&] {
        QElapsedTimer timer;
        timer.start();
        throttle_timer_.invalidate();
        quint64 last_emit = 0;
        vnxfer::TransferStats stats;
        vnxfer::upload_tree(*session_, local.toStdString(), remote.toStdString(), true,
                            [&](uint64_t done, uint64_t total) {
                                if (timer.elapsed() - static_cast<qint64>(last_emit) < 100 &&
                                    done != total) {
                                    return;
                                }
                                last_emit = static_cast<quint64>(timer.elapsed());
                                throttle(done);
                                emit progress(done, total);
                            },
                            stats);
    });
}

void SessionWorker::cancel() {
    canceling_ = true;
    session_.reset();
}

void SessionWorker::makeDir(const QString& path) {
    guarded("mkdir", [&] { session_->mkdir(path.toStdString()); });
}

void SessionWorker::removePath(const QString& path) {
    guarded("remove", [&] { session_->remove(path.toStdString()); });
}

void SessionWorker::removeTree(const QString& path) {
    guarded("remove", [&] { vnxfer::remove_tree(*session_, path.toStdString()); });
}

void SessionWorker::renamePath(const QString& from, const QString& to) {
    guarded("rename", [&] { session_->rename(from.toStdString(), to.toStdString()); });
}
