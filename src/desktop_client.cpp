#include "qcae/desktop_client.hpp"

#include "qcae/local_endpoint.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <QPointer>
#include <QUuid>
#include <algorithm>
#include <vector>

namespace qcae {
namespace {
constexpr auto api_version = "1.1";
constexpr int max_pending = 64;
} // namespace

DesktopClient::DesktopClient(Options options, QObject* parent)
    : QObject(parent), options_(std::move(options)) {
    socket_.setReadBufferSize(transport::max_frame_bytes + 1);
    reconnect_.setInterval(500);
    deadlines_.setInterval(200);
    connect(&socket_, &QLocalSocket::connected, this, &DesktopClient::onConnected);
    connect(&socket_, &QLocalSocket::disconnected, this, &DesktopClient::onDisconnected);
    connect(&socket_, &QLocalSocket::readyRead, this, &DesktopClient::consume);
    connect(&socket_, &QLocalSocket::errorOccurred, this, [this] {
        QPointer<DesktopClient> self(this);
        if (!ready_ && options_.auto_start)
            launchEngine();
        if (!self)
            return;
        emit transportError(socket_.errorString());
    });
    connect(&reconnect_, &QTimer::timeout, this, &DesktopClient::connectNow);
    connect(&deadlines_, &QTimer::timeout, this, &DesktopClient::checkTimeouts);
}

DesktopClient::~DesktopClient() {
    reconnect_.stop();
    deadlines_.stop();
    QObject::disconnect(&socket_, nullptr, this, nullptr);
    socket_.abort();
}

void DesktopClient::start() {
    reconnect_.start();
    deadlines_.start();
    connectNow();
}

void DesktopClient::connectNow() {
    if (socket_.state() == QLocalSocket::UnconnectedState)
        socket_.connectToServer(options_.endpoint);
}

void DesktopClient::launchEngine() {
    if (launched_)
        return;
    launched_ = true;
    QString executable = options_.engine;
    if (executable.isEmpty()) {
        executable = QCoreApplication::applicationDirPath() + "/qcae-engine";
#ifdef Q_OS_WIN
        executable += ".exe";
#endif
    }
    if (!QFileInfo(executable).isExecutable()) {
        emit transportError("Engine executable is unavailable: " + executable);
        return;
    }
    QProcess process;
    process.setProgram(executable);
    process.setArguments({"--socket", options_.endpoint, "--workspace", options_.workspace});
    process.setStandardInputFile(QProcess::nullDevice());
    process.setStandardOutputFile(QProcess::nullDevice());
    process.setStandardErrorFile(QProcess::nullDevice());
    if (!process.startDetached())
        emit transportError("Unable to start engine: " + executable);
}

void DesktopClient::onConnected() {
    incoming_.clear();
    ready_ = false;
    handshake_id_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    handshake_deadline_ms_ =
        QDateTime::currentMSecsSinceEpoch() + std::max<qint64>(1, options_.request_timeout_ms);
    QPointer<DesktopClient> self(this);
    const bool sent = send({{"api_version", api_version},
                            {"request_id", handshake_id_},
                            {"operation", "runtime.handshake"}});
    if (!self)
        return;
    if (!sent)
        socket_.abort();
}

void DesktopClient::onDisconnected() {
    incoming_.clear();
    handshake_id_.clear();
    handshake_deadline_ms_ = 0;
    if (ready_) {
        ready_ = false;
        QPointer<DesktopClient> self(this);
        emit readyChanged(false);
        if (!self)
            return;
    }
    failPending("CONNECTION_LOST",
                "Engine connection was lost; query outcome before retrying a write");
}

QString DesktopClient::request(const QString& operation,
                               const QJsonObject& parameters,
                               const QJsonObject& context,
                               Reply callback) {
    const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!ready_ || pending_.size() >= max_pending) {
        if (callback)
            callback(failure(id,
                             ready_ ? "RESOURCE_LIMIT" : "ENGINE_UNAVAILABLE",
                             ready_ ? "Too many pending requests" : "Engine is not connected"));
        return {};
    }
    QJsonObject frame = context;
    frame.insert("api_version", api_version);
    frame.insert("request_id", id);
    frame.insert("operation", operation);
    frame.insert("parameters", parameters);
    QPointer<DesktopClient> self(this);
    const bool sent = send(frame);
    if (!self)
        return {};
    if (!sent) {
        if (callback)
            callback(failure(id, "FRAME_TOO_LARGE", "Request exceeds the IPC frame limit"));
        return {};
    }
    pending_.insert(
        id,
        {std::move(callback),
         QDateTime::currentMSecsSinceEpoch() + std::max<qint64>(1, options_.request_timeout_ms)});
    return id;
}

bool DesktopClient::send(const QJsonObject& frame) {
    const auto bytes = QJsonDocument(frame).toJson(QJsonDocument::Compact) + '\n';
    if (bytes.size() > transport::max_frame_bytes ||
        socket_.bytesToWrite() + bytes.size() > transport::max_frame_bytes * 2)
        return false;
    if (socket_.write(bytes) == bytes.size())
        return true;
    socket_.abort();
    return false;
}

void DesktopClient::consume() {
    incoming_ += socket_.readAll();
    while (true) {
        const auto newline = incoming_.indexOf('\n');
        if (newline < 0) {
            if (incoming_.size() > transport::max_frame_bytes)
                socket_.abort();
            return;
        }
        if (newline > transport::max_frame_bytes) {
            socket_.abort();
            return;
        }
        const auto line = incoming_.left(newline);
        incoming_.remove(0, newline + 1);
        QJsonParseError error;
        const auto parsed = QJsonDocument::fromJson(line, &error);
        if (error.error != QJsonParseError::NoError || !parsed.isObject()) {
            socket_.abort();
            return;
        }
        QPointer<DesktopClient> self(this);
        deliver(parsed.object());
        if (!self)
            return;
        if (socket_.state() != QLocalSocket::ConnectedState)
            return;
    }
}

void DesktopClient::deliver(const QJsonObject& response) {
    const auto id = response.value("request_id").toString();
    if (id.isEmpty() || !response.value("status").isString()) {
        socket_.abort();
        return;
    }
    if (id == handshake_id_) {
        handshake_id_.clear();
        handshake_deadline_ms_ = 0;
        if (response.value("status").toString() != "success" ||
            response.value("data").toObject().value("api_version").toString() != api_version) {
            QPointer<DesktopClient> self(this);
            emit transportError("Engine API handshake failed");
            if (!self)
                return;
            socket_.abort();
            return;
        }
        ready_ = true;
        launched_ = false;
        emit readyChanged(true);
        return;
    }
    auto pending = pending_.take(id);
    if (pending.callback)
        postReply(std::move(pending.callback), response);
}

void DesktopClient::postReply(Reply callback, QJsonObject response) {
    // A callback may close the window. Run it after QLocalSocket/QTimer signals unwind.
    QTimer::singleShot(0, this, [callback = std::move(callback), response = std::move(response)] {
        callback(response);
    });
}

void DesktopClient::failPending(const QString& code, const QString& message) {
    auto pending = std::move(pending_);
    pending_.clear();
    for (auto it = pending.begin(); it != pending.end(); ++it)
        if (it.value().callback)
            postReply(std::move(it.value().callback), failure(it.key(), code, message));
}

void DesktopClient::checkTimeouts() {
    const auto now = QDateTime::currentMSecsSinceEpoch();
    if (!handshake_id_.isEmpty() && now >= handshake_deadline_ms_) {
        QPointer<DesktopClient> self(this);
        emit transportError("Engine handshake timed out");
        if (!self)
            return;
        socket_.abort();
        if (!self)
            return;
    }
    std::vector<std::pair<QString, Reply>> expired;
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (it.value().deadline_ms > now) {
            ++it;
            continue;
        }
        expired.emplace_back(it.key(), std::move(it.value().callback));
        it = pending_.erase(it);
    }
    for (auto& [id, callback] : expired)
        if (callback)
            postReply(std::move(callback),
                      failure(id,
                              "TIMEOUT",
                              "Response timed out; query outcome before retrying a write"));
}

QJsonObject DesktopClient::failure(const QString& id, const QString& code, const QString& message) {
    return {{"request_id", id},
            {"status", "failed"},
            {"error", QJsonObject{{"code", code}, {"message", message}}}};
}
} // namespace qcae
