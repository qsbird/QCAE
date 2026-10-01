#include "qcae/desktop_client.hpp"

#include "qcae/local_endpoint.hpp"
#include "qcae/json_ledger.hpp"
#include "qcae/operation_ledger.hpp"

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
bool unsignedString(const QJsonObject& object, const char* field, quint64& result) {
    const auto value = object.value(QLatin1String(field));
    std::uint64_t parsed{};
    if (!value.isString() ||
        !transport::json_ledger::unsigned_decimal(value.toStringView(), parsed))
        return false;
    result = parsed;
    return true;
}
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
    ++transport_generation_;
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
    ++transport_generation_;
    incoming_.clear();
    handshake_id_.clear();
    handshake_deadline_ms_ = 0;
    events_need_resync_ = true;
    supports_events_ = false;
    supports_resources_ = false;
    supports_inline_empty_render_ = false;
    supports_events_document_summary_ = false;
    supports_render_model_rebase_ = false;
    supports_render_changed_rows_ = false;
    render_wire_version_ = 2;
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

void DesktopClient::setEventCursor(const QString& instance, quint64 sequence) {
    if (!ready_ || !supports_events_ || instance != engine_instance_id_)
        return;
    event_sequence_ = sequence;
    events_need_resync_ = false;
}

QString DesktopClient::request(const QString& operation,
                               const QJsonObject& parameters,
                               const QJsonObject& context,
                               Reply callback,
                               qsizetype max_reply_bytes) {
    const auto id = transport::json_ledger::uuid();
    if (!ready_ || pending_.size() >= max_pending) {
        if (callback)
            callback(failure(id,
                             ready_ ? "RESOURCE_LIMIT" : "ENGINE_UNAVAILABLE",
                             ready_ ? "Too many pending requests" : "Engine is not connected"));
        return {};
    }
    QJsonObject frame;
    transport::json_ledger::ObjectCopies object_copies;
    const std::array<QLatin1StringView, 4> names{QLatin1StringView("api_version"),
                                                 QLatin1StringView("operation"),
                                                 QLatin1StringView("parameters"),
                                                 QLatin1StringView("request_id")};
    const std::array<QJsonValue, 4> values{QStringLiteral("1.1"), operation, parameters, id};
    std::size_t field{};
    const auto append_fixed = [&] {
        frame.insert(names[field], values[field]);
        object_copies.insert(names[field], values[field], true, false, true);
        ++field;
    };
    // Merge already sorted context keys with the reserved fields. Reserved
    // values retain their original precedence. Every insertion appends at
    // the end, with no COW of the original context or temporary owned key.
    for (auto it = context.constBegin(); it != context.constEnd(); ++it) {
        const auto key = it.keyView();
        while (field < names.size() && QAnyStringView::compare(names[field], key) < 0)
            append_fixed();
        if (field < names.size() && QAnyStringView::compare(names[field], key) == 0) {
            append_fixed();
            continue;
        }
        const auto member = it.value();
        const auto owned_key = transport::json_ledger::insert_borrowed_key(frame, key, member);
        object_copies.insert(key, member, false, owned_key, true);
    }
    while (field < names.size())
        append_fixed();
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
         QDateTime::currentMSecsSinceEpoch() + std::max<qint64>(1, options_.request_timeout_ms),
         max_reply_bytes > 0 ? std::min<qsizetype>(max_reply_bytes, transport::max_frame_bytes)
                             : transport::max_frame_bytes});
    return id;
}

bool DesktopClient::send(const QJsonObject& frame) {
    const auto bytes = transport::json_ledger::compact_frame(frame);
    if (bytes.size() > transport::max_frame_bytes ||
        socket_.bytesToWrite() + bytes.size() > transport::max_frame_bytes * 2)
        return false;
    const auto accepted = socket_.write(bytes);
    if (accepted > 0) {
        transport::json_ledger::frame(ledger::Stage::socket_send, frame, accepted);
        ledger::add(ledger::Stage::socket_send, ledger::Metric::socket_bytes, accepted);
        ledger::add(ledger::Stage::socket_send, ledger::Metric::model_copy_bytes, accepted);
    }
    ledger::cover(ledger::Stage::socket_send);
    if (accepted == bytes.size())
        return true;
    socket_.abort();
    return false;
}

void DesktopClient::consume() {
    const auto received = socket_.readAll();
    ledger::add(ledger::Stage::socket_receive, ledger::Metric::socket_bytes, received.size());
    ledger::add(ledger::Stage::socket_receive, ledger::Metric::model_copy_bytes, received.size());
    transport::json_ledger::socket_read(received.size());
    ledger::cover(ledger::Stage::socket_receive);
    const auto previous_size = incoming_.size();
    const auto previous_capacity = incoming_.capacity();
    incoming_ += received;
    ledger::add(ledger::Stage::socket_receive, ledger::Metric::model_copy_bytes, received.size());
    if (incoming_.capacity() != previous_capacity)
        ledger::add(ledger::Stage::socket_receive, ledger::Metric::model_copy_bytes, previous_size);
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
        const auto line = QByteArrayView(incoming_).first(newline);
        QJsonParseError error;
        const auto parsed = QJsonValue::fromJson(line, &error);
        transport::json_ledger::decoding(line);
        ledger::add(ledger::Stage::socket_receive, ledger::Metric::decoded_bytes, line.size());
        // The parsed object owns its strings; the borrowed input ends here.
        transport::json_ledger::remove_prefix(incoming_, newline + 1);
        if (error.error != QJsonParseError::NoError || !parsed.isObject()) {
            socket_.abort();
            return;
        }
        QPointer<DesktopClient> self(this);
        transport::json_ledger::frame(
            ledger::Stage::socket_receive, parsed.toObject(), newline + 1);
        deliver(parsed.toObject(), newline + 1);
        if (!self)
            return;
        if (socket_.state() != QLocalSocket::ConnectedState)
            return;
    }
}

void DesktopClient::deliver(const QJsonObject& response, qsizetype frame_bytes) {
    const auto frame_type = transport::json_ledger::string(response.value("frame_type"));
    if (frame_type == "event" || frame_type == "event_gap") {
        const auto generation = transport_generation_;
        QTimer::singleShot(0, this, [this, generation, response] {
            if (generation == transport_generation_ && ready_)
                deliverEvent(response);
        });
        return;
    }
    const auto id = transport::json_ledger::string(response.value("request_id"));
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
        const auto data = response.value("data").toObject();
        const auto previous_instance = engine_instance_id_;
        engine_instance_id_ = data.value("engine_instance_id").toString();
        supports_events_ =
            !engine_instance_id_.isEmpty() &&
            data.value("capabilities").toObject().value("events_version").toInt() == 1;
        supports_resources_ =
            data.value("capabilities").toObject().value("resources_version").toInt() == 1;
        supports_inline_empty_render_ =
            data.value("capabilities").toObject().value("render_inline_empty_version").toInt() == 1;
        const auto advertised_wire =
            data.value("capabilities").toObject().value("render_wire_version").toInt(2);
        render_wire_version_ = advertised_wire > 0 ? std::min<unsigned>(advertised_wire, 3) : 2;
        supports_events_document_summary_ =
            supports_events_ && data.value("capabilities")
                                        .toObject()
                                        .value("events_document_summary_version")
                                        .toInt() == 1;
        supports_render_model_rebase_ =
            supports_resources_ &&
            data.value("capabilities").toObject().value("render_model_rebase_version").toInt() == 1;
        supports_render_changed_rows_ =
            supports_resources_ &&
            data.value("capabilities").toObject().value("render_changed_rows_version").toInt() == 1;
        events_need_resync_ = true;
        event_sequence_ = 0;
        QPointer<DesktopClient> self(this);
        emit readyChanged(true);
        if (!self)
            return;
        if (supports_events_ && !previous_instance.isEmpty()) {
            const auto generation = transport_generation_;
            const auto reason = previous_instance == engine_instance_id_
                                    ? "Engine connection resumed"
                                    : "Engine instance changed";
            QTimer::singleShot(0, this, [this, generation, reason] {
                if (generation == transport_generation_ && ready_)
                    emit eventGap(reason);
            });
        }
        return;
    }
    auto pending = pending_.take(id);
    if (pending.callback)
        postReply(std::move(pending.callback),
                  frame_bytes > pending.max_reply_bytes
                      ? failure(id, "FRAME_TOO_LARGE", "Reply exceeds its requested frame limit")
                      : response);
}

void DesktopClient::requireEventResync(const QString& reason, quint64 sequence) {
    event_sequence_ = sequence;
    events_need_resync_ = true;
    emit eventGap(reason);
}

void DesktopClient::deliverEvent(const QJsonObject& event) {
    quint64 sequence{};
    if (!supports_events_ || !unsignedString(event, "sequence", sequence) ||
        transport::json_ledger::string(event.value("engine_instance_id")) != engine_instance_id_) {
        requireEventResync("Invalid event instance or sequence", event_sequence_);
        return;
    }
    if (event.value("frame_type") == "event_gap") {
        if (!event.value("resync_required").isBool() || !event.value("resync_required").toBool()) {
            requireEventResync("Malformed event gap frame", event_sequence_);
            return;
        }
        requireEventResync(event.value("reason").toString("Event retention gap"), sequence);
        return;
    }
    quint64 revision{};
    if (!sequence || !event.value("event").isString() ||
        transport::json_ledger::string(event.value("event")).isEmpty() ||
        transport::json_ledger::string(event.value("document_id")).isEmpty() ||
        transport::json_ledger::string(event.value("document_epoch")).isEmpty() ||
        !unsignedString(event, "revision", revision) || !event.value("data").isObject()) {
        requireEventResync("Malformed event frame", event_sequence_);
        return;
    }
    const auto kind = event.value("event").toString();
    if (kind != "DocumentChanged" && kind != "HistoryChanged" && kind != "JobChanged" &&
        kind != "ProjectMetadataChanged") {
        requireEventResync("Unsupported event kind", event_sequence_);
        return;
    }
    if (sequence <= event_sequence_)
        return;
    if (events_need_resync_) {
        event_sequence_ = sequence;
        return;
    }
    if (sequence - event_sequence_ != 1) {
        requireEventResync("Event sequence gap", sequence);
        return;
    }
    event_sequence_ = sequence;
    emit eventReceived(event);
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
