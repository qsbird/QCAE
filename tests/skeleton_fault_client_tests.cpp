#include "qcae/desktop_client.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalServer>
#include <QPointer>
#include <QTemporaryDir>
#include <QUuid>
#include <QTest>
#include <iostream>
#include <stdexcept>

namespace {
QByteArray wire(const QJsonObject& object) {
    return QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
}
QJsonObject reply(const QString& id, QJsonObject data = {}) {
    return {{"request_id", id}, {"status", "success"}, {"data", data}};
}
QJsonObject event(quint64 sequence, quint64 revision) {
    return {{"frame_type", "event"},
            {"engine_instance_id", "fault-engine"},
            {"sequence", QString::number(sequence)},
            {"event", "DocumentChanged"},
            {"document_id", "document"},
            {"document_epoch", "epoch"},
            {"revision", QString::number(revision)},
            {"data", QJsonObject{}}};
}
void wait(const std::function<bool()>& condition, const char* label) {
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < 5000)
        QTest::qWait(2);
    if (!condition())
        throw std::runtime_error(label);
}
struct Audit {
    QJsonArray assertions;
    QJsonObject actual;
    void require(bool passed, const QString& label) {
        assertions.append(QJsonObject{{"expected", label}, {"actual", passed}, {"passed", passed}});
        if (!passed)
            throw std::runtime_error(label.toStdString());
    }
};
struct Engine : QObject {
    QTemporaryDir directory{QDir("/tmp").canonicalPath() + "/qcae-fault-client-XXXXXX"};
    QLocalServer server;
    QPointer<QLocalSocket> peer;
    QJsonArray requests;
    quint64 revision{3}, cursor{};
    unsigned write_count{};
    bool snapshots{true};
    Engine() {
        connect(&server, &QLocalServer::newConnection, &server, [this] {
            peer = server.nextPendingConnection();
            auto* connection = peer.data();
            connect(
                connection,
                &QLocalSocket::readyRead,
                &server,
                [this, connection, buffer = QByteArray{}]() mutable {
                    buffer += connection->readAll();
                    while (true) {
                        const auto newline = buffer.indexOf('\n');
                        if (newline < 0)
                            return;
                        const auto request = QJsonDocument::fromJson(buffer.left(newline)).object();
                        buffer.remove(0, newline + 1);
                        const auto operation = request.value("operation").toString();
                        const auto id = request.value("request_id").toString();
                        if (operation == "runtime.handshake") {
                            connection->write(wire(reply(
                                id,
                                {{"api_version", "1.1"},
                                 {"engine_instance_id", "fault-engine"},
                                 {"capabilities",
                                  QJsonObject{{"events_version", 1}, {"resources_version", 1}}}})));
                        } else {
                            requests.append(request);
                            if (operation == "state.current" && snapshots)
                                connection->write(wire(
                                    reply(id,
                                          {{"revision", QString::number(revision)},
                                           {"cursor", QString::number(cursor)},
                                           {"model",
                                            QJsonArray{QString("node-a@%1").arg(revision),
                                                       QString("node-b@%1").arg(revision)}}})));
                            if (operation == "test.write") {
                                ++write_count;
                                ++revision;
                            }
                        }
                    }
                });
        });
        if (!directory.isValid() || !server.listen(directory.filePath("engine.sock")))
            throw std::runtime_error("Cannot listen on local client fixture socket");
    }
    qcae::DesktopClient::Options options() const {
        return {directory.filePath("engine.sock"), directory.path(), {}, false, 5000};
    }
    void send(const QList<QJsonObject>& frames) {
        QByteArray bytes;
        for (const auto& frame : frames)
            bytes += wire(frame);
        if (peer)
            peer->write(bytes);
    }
};
void event_gap(Audit& audit) {
    Engine engine;
    qcae::DesktopClient client(engine.options());
    QJsonObject installed;
    unsigned gaps{}, accepted_events{}, syncs{};
    bool syncing{};
    auto resync = [&] {
        if (syncing || !client.ready())
            return;
        syncing = true;
        (void)client.request("state.current", {}, {}, [&](const QJsonObject& response) {
            const auto data = response.value("data").toObject();
            installed = data;
            client.setEventCursor("fault-engine", data.value("cursor").toString().toULongLong());
            ++syncs;
            syncing = false;
        });
    };
    QObject::connect(&client, &qcae::DesktopClient::eventGap, &engine, [&](const QString&) {
        ++gaps;
        resync();
    });
    QObject::connect(&client,
                     &qcae::DesktopClient::eventReceived,
                     &engine,
                     [&](const QJsonObject&) { ++accepted_events; });
    QObject::connect(&client, &qcae::DesktopClient::readyChanged, &engine, [&](bool ready) {
        if (ready)
            resync();
    });
    client.start();
    wait([&] { return syncs == 1; }, "Initial snapshot not installed");
    engine.cursor = 1;
    engine.send({event(1, 3)});
    wait([&] { return accepted_events == 1; }, "Initial event not accepted");
    // Sequence 2 is deliberately discarded. Sequence 3 arrives with a newer model.
    engine.revision = 5;
    engine.cursor = 3;
    engine.send({event(3, 5), event(4, 5)});
    wait([&] { return gaps == 1 && syncs == 2; },
         "Dropped event did not trigger snapshot resynchronization");
    audit.require(
        accepted_events == 1,
        "Gap and subsequent events are withheld until full current snapshot is installed");
    audit.require(installed.value("revision") == "5" &&
                      installed.value("model").toArray() == QJsonArray{"node-a@5", "node-b@5"},
                  "Resynchronization installs both rows from the same authoritative revision");
    engine.peer->abort();
    wait([&] { return !client.ready(); }, "Disconnect not detected");
    engine.revision = 6;
    engine.cursor = 4;
    wait([&] { return client.ready() && syncs == 3; }, "Reconnect did not requery current model");
    audit.require(installed.value("revision") == "6" &&
                      installed.value("model").toArray() == QJsonArray{"node-a@6", "node-b@6"},
                  "Reconnect replaces the stale model with a single current snapshot");
    audit.require(engine.write_count == 0, "Reconnect and resync produce no model write or replay");
    audit.actual = {{"gap_count", int(gaps)},
                    {"snapshot_queries", int(syncs)},
                    {"accepted_events", int(accepted_events)},
                    {"installed", installed},
                    {"requests", engine.requests},
                    {"model_writes", int(engine.write_count)}};
}
void synchronous_destruction(Audit& audit) {
    QJsonArray variants;
    // Reply destruction is the contract's primary fault. Event and gap destruction also
    // exercise the posted-notification path with another buffered notification.
    for (const auto& mode : {QString("reply"), QString("event"), QString("gap")}) {
        Engine engine;
        engine.snapshots = false;
        QPointer<qcae::DesktopClient> client = new qcae::DesktopClient(engine.options());
        client->start();
        wait([&] { return client && client->ready(); }, "Client never became ready");
        client->setEventCursor("fault-engine", 0);
        unsigned first_calls{}, late_calls{};
        const auto write = client->request(
            "test.write", {}, {{"idempotency_key", "single-intent"}}, [&](const QJsonObject&) {
                ++first_calls;
                if (mode == "reply")
                    delete client.data();
            });
        const auto late =
            client->request("late.read", {}, {}, [&](const QJsonObject&) { ++late_calls; });
        if (mode == "event")
            QObject::connect(
                client, &qcae::DesktopClient::eventReceived, &engine, [&](const QJsonObject&) {
                    ++first_calls;
                    delete client.data();
                });
        if (mode == "gap")
            QObject::connect(client, &qcae::DesktopClient::eventGap, &engine, [&](const QString&) {
                ++first_calls;
                delete client.data();
            });
        wait([&] { return engine.requests.size() == 2; },
             "Fixture did not receive both actual requests");
        if (mode == "reply")
            engine.send({reply(write, {{"transaction_id", "tx-once"}}), reply(late), reply(write)});
        else if (mode == "event")
            engine.send({event(1, 4), reply(late), event(2, 4)});
        else
            engine.send({event(2, 4), reply(late), event(3, 4)});
        wait([&] { return client.isNull(); }, "Callback failed to destroy client synchronously");
        // A later turn of the local socket also attempts a reply after destruction.
        engine.send({reply(late), reply(write)});
        QTest::qWait(20);
        audit.require(first_calls == 1 && late_calls == 0 && client.isNull(),
                      mode + ": destroyed context suppresses buffered/late callbacks");
        audit.require(
            engine.write_count == 1 && engine.revision == 4 && engine.requests.size() == 2,
            mode +
                ": exactly one original write reaches the local authority and no replay is sent");
        variants.append(QJsonObject{{"mode", mode},
                                    {"first_callbacks", int(first_calls)},
                                    {"late_callbacks", int(late_calls)},
                                    {"model_writes", int(engine.write_count)},
                                    {"revision", QString::number(engine.revision)},
                                    {"requests", engine.requests}});
    }
    audit.actual.insert("variants", variants);
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
    audit.actual.insert("asan_instrumented", true);
#else
    audit.actual.insert("asan_instrumented", false);
#endif
#if __has_feature(undefined_behavior_sanitizer)
    audit.actual.insert("ubsan_instrumented", true);
#else
    audit.actual.insert("ubsan_instrumented", false);
#endif
#else
    audit.actual.insert("asan_instrumented", false);
    audit.actual.insert("ubsan_instrumented", false);
#endif
}
} // namespace
int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    QFile evidence(argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString{});
    if (!evidence.fileName().isEmpty() &&
        !evidence.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return 2;
    const auto source = argc > 2 ? QString::fromLocal8Bit(argv[2]) : QString("mutable-diagnostic");
    const auto nonce = QUuid::createUuid().toString(QUuid::WithoutBraces);
    unsigned failures{};
    for (unsigned id : {16U, 17U}) {
        for (unsigned repeat = 1; repeat <= 10; ++repeat) {
            Audit audit;
            QString error;
            try {
                if (id == 16)
                    event_gap(audit);
                else
                    synchronous_destruction(audit);
            } catch (const std::exception& problem) {
                error = QString::fromLocal8Bit(problem.what());
                ++failures;
            }
            QJsonObject record{
                {"case_id", QString("F%1").arg(id)},
                {"run_id", QString("native-client-%1-F%2-%3").arg(nonce).arg(id).arg(repeat)},
                {"source_tree_sha256", source},
                {"input",
                 id == 16 ? "Discard sequence 2, deliver 3/4, reconnect and query "
                            "authoritative revision 6"
                          : "Synchronously delete DesktopClient inside reply/event/gap "
                            "callback with buffered and late replies"},
                {"assertions", audit.assertions},
                {"actual", audit.actual},
                {"passed", error.isEmpty()},
                {"error", error}};
            const auto bytes = wire(record);
            if (evidence.isOpen()) {
                evidence.write(bytes);
                evidence.flush();
            }
            std::cout << bytes.constData();
        }
    }
    return failures ? 1 : 0;
}
