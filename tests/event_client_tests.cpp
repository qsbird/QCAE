#include "qcae/desktop_client.hpp"
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalServer>
#include <QPointer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

namespace {
QByteArray wire(const QJsonObject& object) {
    return QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
}
QJsonObject eventFrame(quint64 sequence, QString instance = "engine-a") {
    return {{"frame_type", "event"},
            {"engine_instance_id", instance},
            {"sequence", QString::number(sequence)},
            {"event", "DocumentChanged"},
            {"document_id", "document"},
            {"document_epoch", "epoch"},
            {"revision", "3"},
            {"data", QJsonObject{{"resync_required", false}}}};
}
QJsonObject reply(const QString& id, QJsonObject data = {}) {
    return {{"request_id", id}, {"status", "success"}, {"data", data}};
}
struct Engine final : QObject {
    QTemporaryDir directory{QDir("/tmp").canonicalPath() + "/qcae-events-XXXXXX"};
    QLocalServer server;
    QPointer<QLocalSocket> peer;
    QString instance{"engine-a"};
    bool modern{true};
    int summary_version{}, rebase_version{}, rows_version{};
    QList<QJsonObject> requests;
    QList<QByteArray> request_wires;
    Engine() {
        connect(&server, &QLocalServer::newConnection, &server, [this] {
            peer = server.nextPendingConnection();
            auto* connection = peer.data();
            connect(connection,
                    &QLocalSocket::readyRead,
                    &server,
                    [this, connection, buffer = QByteArray{}]() mutable {
                        buffer += connection->readAll();
                        while (true) {
                            const auto newline = buffer.indexOf('\n');
                            if (newline < 0)
                                return;
                            const auto request =
                                QJsonDocument::fromJson(buffer.left(newline)).object();
                            const auto request_wire = buffer.left(newline + 1);
                            buffer.remove(0, newline + 1);
                            if (request.value("operation") == "runtime.handshake") {
                                QJsonObject data{{"api_version", "1.1"}};
                                if (modern) {
                                    data.insert("engine_instance_id", instance);
                                    QJsonObject capabilities{{"events_version", 1},
                                                             {"resources_version", 1}};
                                    if (summary_version)
                                        capabilities.insert("events_document_summary_version",
                                                            summary_version);
                                    if (rebase_version)
                                        capabilities.insert("render_model_rebase_version",
                                                            rebase_version);
                                    if (rows_version)
                                        capabilities.insert("render_changed_rows_version",
                                                            rows_version);
                                    data.insert("capabilities", capabilities);
                                }
                                connection->write(
                                    wire(reply(request.value("request_id").toString(), data)));
                            } else {
                                requests.append(request);
                                request_wires.append(request_wire);
                            }
                        }
                    });
        });
    }
    bool listen() {
        return directory.isValid() && server.listen(directory.filePath("engine.sock"));
    }
    qcae::DesktopClient::Options options() const {
        return {directory.filePath("engine.sock"), directory.path(), {}, false, 5000};
    }
    void send(const QList<QJsonObject>& frames) {
        QByteArray bytes;
        for (const auto& frame : frames)
            bytes += wire(frame);
        peer->write(bytes);
    }
};
class EventClientTests final : public QObject {
    Q_OBJECT
  private slots:
    void requestWireRemainsExactlyQtCompact() {
        Engine engine;
        QVERIFY(engine.listen());
        qcae::DesktopClient client(engine.options());
        client.start();
        QTRY_VERIFY(client.ready());
        const QList<QJsonObject> parameters{
            {{"ascii", "plain text"}, {"integer", 123}},
            {{"nested", QJsonArray{true, false, QJsonValue::Null, -2, "text"}}},
            {{"escape", "quote\" slash\\ line\n"}},
            {{"unicode", QString::fromUtf8("\xe4\xb8\xad\xf0\x9f\x98\x80")}},
            {{"fraction", 0.3}},
            {{"number", QJsonValue::fromJson(QByteArrayView("1e6"))}}};
        for (const auto& values : parameters) {
            const auto id = client.request("wire.probe", values, {}, {});
            QVERIFY(!id.isEmpty());
            QTRY_VERIFY(!engine.requests.isEmpty() &&
                        engine.requests.back().value("request_id") == id);
            const QJsonObject expected{{"api_version", "1.1"},
                                       {"operation", "wire.probe"},
                                       {"parameters", values},
                                       {"request_id", id}};
            QCOMPARE(engine.request_wires.back(), wire(expected));
            engine.send({reply(id)});
        }
    }
    void eventsRemainOrderedAndSeparateFromReplies() {
        Engine engine;
        QVERIFY(engine.listen());
        qcae::DesktopClient client(engine.options());
        QSignalSpy events(&client, &qcae::DesktopClient::eventReceived);
        QSignalSpy gaps(&client, &qcae::DesktopClient::eventGap);
        client.start();
        QTRY_VERIFY(client.ready());
        QVERIFY(client.supportsEvents());
        QVERIFY(client.supportsResources());
        QVERIFY(!client.supportsEventsDocumentSummary());
        QVERIFY(!client.supportsRenderModelRebase());
        QVERIFY(!client.supportsRenderChangedRows());
        QCOMPARE(client.engineInstanceId(), QString("engine-a"));
        bool subscribed = false;
        QStringList replies;
        const auto subscribe =
            client.request("events.subscribe", {}, {}, [&](const QJsonObject& response) {
                const auto data = response.value("data").toObject();
                client.setEventCursor(data.value("engine_instance_id").toString(),
                                      data.value("next_sequence").toString().toULongLong());
                subscribed = true;
            });
        const auto first = client.request("first", {}, {}, [&](const QJsonObject& response) {
            replies.append(response.value("data").toObject().value("value").toString());
        });
        const auto second = client.request("second", {}, {}, [&](const QJsonObject& response) {
            replies.append(response.value("data").toObject().value("value").toString());
        });
        QTRY_COMPARE(engine.requests.size(), 3);
        engine.send(
            {reply(subscribe, {{"engine_instance_id", "engine-a"}, {"next_sequence", "10"}}),
             eventFrame(11),
             reply(second, {{"value", "second"}}),
             eventFrame(12),
             reply(first, {{"value", "first"}}),
             eventFrame(12)});
        QTRY_VERIFY(subscribed && replies.size() == 2 && events.size() == 2);
        QCOMPARE(replies, QStringList({"second", "first"}));
        QCOMPARE(events[0][0].toJsonObject().value("sequence"), QJsonValue("11"));
        QCOMPARE(events[1][0].toJsonObject().value("sequence"), QJsonValue("12"));
        QCOMPARE(client.eventSequence(), quint64(12));
        QCOMPARE(gaps.size(), 0);
        engine.send({eventFrame(14), eventFrame(15)});
        QTRY_COMPARE(gaps.size(), 1);
        QTRY_COMPARE(client.eventSequence(), quint64(15));
        QCOMPARE(events.size(), 2);
        client.setEventCursor("wrong-instance", 15);
        engine.send({eventFrame(16)});
        QTRY_COMPARE(client.eventSequence(), quint64(16));
        QCOMPARE(events.size(), 2);
        client.setEventCursor("engine-a", 16);
        engine.send({eventFrame(17)});
        QTRY_COMPARE(events.size(), 3);
        engine.send({eventFrame(18, "other-engine")});
        QTRY_COMPARE(gaps.size(), 2);
        QVERIFY(client.ready());
        client.setEventCursor("engine-a", 17);
        auto malformed = eventFrame(18);
        malformed.insert("revision", 3);
        engine.send({malformed});
        QTRY_COMPARE(gaps.size(), 3);
        QCOMPARE(events.size(), 3);
        client.setEventCursor("engine-a", 17);
        auto unsupported = eventFrame(18);
        unsupported.insert("event", "UnknownVersion1Event");
        engine.send({unsupported});
        QTRY_COMPARE(gaps.size(), 4);
        QCOMPARE(events.size(), 3);
    }
    void disconnectResetsCapabilitiesAndNeverReplaysWrites() {
        Engine engine;
        engine.summary_version = engine.rebase_version = engine.rows_version = 1;
        QVERIFY(engine.listen());
        qcae::DesktopClient client(engine.options());
        QSignalSpy gaps(&client, &qcae::DesktopClient::eventGap);
        QSignalSpy events(&client, &qcae::DesktopClient::eventReceived);
        client.start();
        QTRY_VERIFY(client.ready());
        QVERIFY(client.supportsEventsDocumentSummary());
        QVERIFY(client.supportsRenderModelRebase());
        QVERIFY(client.supportsRenderChangedRows());
        client.setEventCursor("engine-a", 0);
        engine.send({eventFrame(1)});
        QTRY_COMPARE(events.size(), 1);
        QString failure;
        QVERIFY(!client
                     .request("write",
                              {},
                              {{"idempotency_key", "intent"}},
                              [&](const QJsonObject& response) {
                                  failure =
                                      response.value("error").toObject().value("code").toString();
                              })
                     .isEmpty());
        QTRY_COMPARE(engine.requests.size(), 1);
        engine.peer->abort();
        QTRY_VERIFY(!client.ready());
        QVERIFY(!client.supportsEvents());
        QVERIFY(!client.supportsResources());
        QVERIFY(!client.supportsEventsDocumentSummary());
        QVERIFY(!client.supportsRenderModelRebase());
        QVERIFY(!client.supportsRenderChangedRows());
        QTRY_COMPARE(failure, QString("CONNECTION_LOST"));
        engine.instance = "engine-b";
        QTRY_VERIFY(client.ready());
        QTRY_COMPARE(gaps.size(), 1);
        QCOMPARE(gaps[0][0].toString(), QString("Engine instance changed"));
        QCOMPARE(client.engineInstanceId(), QString("engine-b"));
        QCOMPARE(engine.requests.size(), 1);
        client.setEventCursor("engine-b", 0);
        engine.send({eventFrame(1, "engine-b")});
        QTRY_COMPARE(events.size(), 2);
        engine.peer->abort();
        QTRY_VERIFY(!client.ready());
        QTRY_VERIFY(client.ready());
        QTRY_COMPARE(gaps.size(), 2);
        QCOMPARE(gaps[1][0].toString(), QString("Engine connection resumed"));
        QCOMPARE(engine.requests.size(), 1);
    }
    void legacyHandshakeRetainsOrdinaryReplies() {
        Engine engine;
        engine.modern = false;
        QVERIFY(engine.listen());
        qcae::DesktopClient client(engine.options());
        client.start();
        QTRY_VERIFY(client.ready());
        QVERIFY(!client.supportsEvents());
        QVERIFY(!client.supportsResources());
        QVERIFY(!client.supportsEventsDocumentSummary());
        QVERIFY(!client.supportsRenderModelRebase());
        QVERIFY(!client.supportsRenderChangedRows());
        bool delivered = false;
        const auto id = client.request("legacy", {}, {}, [&](const QJsonObject& response) {
            delivered = response.value("data").toObject().value("value") == "ok";
        });
        QTRY_COMPARE(engine.requests.size(), 1);
        engine.send({reply(id, {{"value", "ok"}})});
        QTRY_VERIFY(delivered);
    }
    void refreshCapabilitiesAreIndependentAndVersioned() {
        for (int capability = 0; capability < 3; ++capability) {
            for (int version : {1, 2}) {
                Engine engine;
                engine.summary_version = capability == 0 ? version : 0;
                engine.rebase_version = capability == 1 ? version : 0;
                engine.rows_version = capability == 2 ? version : 0;
                QVERIFY(engine.listen());
                qcae::DesktopClient client(engine.options());
                client.start();
                QTRY_VERIFY(client.ready());
                QCOMPARE(client.supportsEventsDocumentSummary(), capability == 0 && version == 1);
                QCOMPARE(client.supportsRenderModelRebase(), capability == 1 && version == 1);
                QCOMPARE(client.supportsRenderChangedRows(), capability == 2 && version == 1);
            }
        }
    }
    void envelopeOverridesContextAndPreservesUnicode() {
        Engine engine;
        QVERIFY(engine.listen());
        qcae::DesktopClient client(engine.options());
        client.start();
        QTRY_VERIFY(client.ready());
        const QJsonObject context{
            {"api_version", "wrong"},
            {"request_id", "foreign"},
            {"operation", "forged"},
            {"parameters", QJsonObject{{"old", true}}},
            {"document_id", "document"},
            {QString::fromUtf8("m\xc3\xa9ta"), QString::fromUtf8("\xe4\xb8\xad")}};
        const QJsonObject parameters{{"value", QString::fromUtf8("\xc3\xa9")}};
        const auto id = client.request("correct", parameters, context, {});
        QVERIFY(!id.isEmpty());
        QTRY_COMPARE(engine.requests.size(), 1);
        auto expected = context;
        expected.insert("api_version", "1.1");
        expected.insert("request_id", id);
        expected.insert("operation", "correct");
        expected.insert("parameters", parameters);
        QCOMPARE(engine.requests[0], expected);
        QCOMPARE(engine.request_wires[0], wire(expected));
        QCOMPARE(context.value("request_id"), QJsonValue("foreign"));
        QCOMPARE(context.value("parameters"), QJsonValue(QJsonObject{{"old", true}}));
    }
    void borrowedRepliesOwnFieldsAcrossSplitAndBatchedFrames() {
        Engine engine;
        QVERIFY(engine.listen());
        qcae::DesktopClient client(engine.options());
        client.start();
        QTRY_VERIFY(client.ready());
        QList<QJsonObject> delivered;
        const auto first = client.request("first", {}, {}, [&](const QJsonObject& response) {
            delivered.append(response.value("data").toObject());
        });
        const auto second = client.request("second", {}, {}, [&](const QJsonObject& response) {
            delivered.append(response.value("data").toObject());
        });
        QTRY_COMPARE(engine.requests.size(), 2);
        const auto bytes = QByteArray("{\"request_id\":\"") + first.toUtf8() +
                           "\",\"status\":\"success\",\"data\":{\"name\":\"old\","
                           "\"name\":\"\\u4e2d\\ud83d\\ude00\",\"escaped\":\"\\n\"}}\r\n";
        const auto split = bytes.size() / 2;
        engine.peer->write(bytes.first(split));
        engine.peer->flush();
        QTest::qWait(20);
        QCOMPARE(delivered.size(), 0);
        engine.peer->write(bytes.sliced(split) + wire(reply(second, {{"flag", true}})));
        QTRY_COMPARE(delivered.size(), 2);
        QCOMPARE(delivered[0].value("name"), QJsonValue(QString::fromUtf8("中😀")));
        QCOMPARE(delivered[0].value("escaped"), QJsonValue("\n"));
        QCOMPARE(delivered[1].value("flag"), QJsonValue(true));
        QVERIFY(client.ready());
    }
    void malformedAndNonobjectRepliesRetainConnectionFailure() {
        for (const auto& bytes : {QByteArray("null\n"),
                                  QByteArray("[]\n"),
                                  QByteArray("1\n"),
                                  QByteArray("{\"x\":1}{}\n"),
                                  QByteArray("{\"x\":1,}\n")}) {
            Engine engine;
            QVERIFY(engine.listen());
            qcae::DesktopClient client(engine.options());
            client.start();
            QTRY_VERIFY(client.ready());
            QString failure;
            const auto id = client.request("pending", {}, {}, [&](const QJsonObject& response) {
                failure = response.value("error").toObject().value("code").toString();
            });
            QVERIFY(!id.isEmpty());
            QTRY_COMPARE(engine.requests.size(), 1);
            engine.peer->write(bytes);
            QTRY_COMPARE(failure, QString("CONNECTION_LOST"));
            QVERIFY(!client.ready());
        }
    }
    void postedEventAndGapMayDeleteClientSynchronously() {
        for (bool gap : {false, true}) {
            Engine engine;
            QVERIFY(engine.listen());
            QPointer<qcae::DesktopClient> client = new qcae::DesktopClient(engine.options());
            client->start();
            QTRY_VERIFY(client && client->ready());
            client->setEventCursor("engine-a", 0);
            bool called = false;
            if (gap)
                connect(client, &qcae::DesktopClient::eventGap, &engine, [&](const QString&) {
                    called = true;
                    delete client.data();
                });
            else
                connect(
                    client, &qcae::DesktopClient::eventReceived, &engine, [&](const QJsonObject&) {
                        called = true;
                        delete client.data();
                    });
            auto frame = gap ? QJsonObject{{"frame_type", "event_gap"},
                                           {"engine_instance_id", "engine-a"},
                                           {"sequence", "3"},
                                           {"resync_required", true},
                                           {"reason", "Expired"}}
                             : eventFrame(1);
            engine.send({frame, eventFrame(4)});
            QTRY_VERIFY(called);
            QVERIFY(client.isNull());
        }
    }
};
} // namespace
QTEST_GUILESS_MAIN(EventClientTests)
#include "event_client_tests.moc"
