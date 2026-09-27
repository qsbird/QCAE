#include "modeling_tools.hpp"
#include <QApplication>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTest>
#include <algorithm>
#include <memory>

namespace {
// A real local wire with deliberately held responses makes timeout and reply ordering explicit.
class TestEngine {
  public:
    TestEngine() : directory_("/private/tmp/qcae-tools-XXXXXX") {
        QObject::connect(&server_, &QLocalServer::newConnection, &server_, [this] {
            peer_ = server_.nextPendingConnection();
            QObject::connect(peer_, &QLocalSocket::readyRead, &server_, [this] {
                incoming_ += peer_->readAll();
                while (true) {
                    const auto newline = incoming_.indexOf('\n');
                    if (newline < 0)
                        return;
                    const auto request = QJsonDocument::fromJson(incoming_.left(newline)).object();
                    incoming_.remove(0, newline + 1);
                    const auto operation = request.value("operation").toString();
                    if (operation == "runtime.handshake")
                        respond(request, {{"api_version", "1.1"}});
                    else if (operation == "test.barrier")
                        respond(request, {});
                    else
                        requests.append(request);
                }
            });
        });
    }
    bool listen() {
        return directory_.isValid() && server_.listen(endpoint());
    }
    QString endpoint() const {
        return directory_.filePath("engine.sock");
    }
    QString workspace() const {
        return directory_.path();
    }
    void disconnect() {
        server_.close();
        peer_->abort();
        incoming_.clear();
    }
    void respond(const QJsonObject& request, const QJsonObject& data) {
        send({{"request_id", request.value("request_id")}, {"status", "success"}, {"data", data}});
    }
    void fail(const QJsonObject& request, const QString& code, const QString& message) {
        send({{"request_id", request.value("request_id")},
              {"status", "failed"},
              {"error", QJsonObject{{"code", code}, {"message", message}}}});
    }
    qsizetype count(const QString& operation) const {
        return std::count_if(requests.begin(), requests.end(), [&](const auto& request) {
            return request.value("operation") == operation;
        });
    }
    QJsonObject latest(const QString& operation) const {
        for (auto it = requests.crbegin(); it != requests.crend(); ++it)
            if (it->value("operation") == operation)
                return *it;
        return {};
    }
    QList<QJsonObject> requests;

  private:
    void send(const QJsonObject& reply) {
        peer_->write(QJsonDocument(reply).toJson(QJsonDocument::Compact) + '\n');
    }
    QTemporaryDir directory_;
    QLocalServer server_;
    QPointer<QLocalSocket> peer_;
    QByteArray incoming_;
};

struct Observations {
    int previews{}, clears{}, refreshes{};
    qcae::RenderPreview preview;
    QStringList operations;
    qcae::ModelingTools::Actions actions() {
        return {[this](const auto& value) {
                    ++previews;
                    preview = value;
                },
                [this] { ++clears; },
                [this] { ++refreshes; },
                [this](const auto& operation, const auto&) { operations.append(operation); }};
    }
};

struct Controls {
    explicit Controls(qcae::ModelingTools& tools)
        : mode(tools.findChild<QComboBox*>("modelingMode")),
          start_x(tools.findChild<QDoubleSpinBox*>("startX")),
          end_x(tools.findChild<QDoubleSpinBox*>("endX")),
          segments(tools.findChild<QSpinBox*>("meshSegments")),
          preview(tools.findChild<QPushButton*>("modelingPreview")),
          apply(tools.findChild<QPushButton*>("modelingApply")),
          cancel(tools.findChild<QPushButton*>("modelingCancel")),
          status(tools.findChild<QLabel*>("modelingStatus")),
          progress(tools.findChild<QProgressBar*>("meshProgress")) {}
    bool valid() const {
        return mode && start_x && end_x && segments && preview && apply && cancel && status &&
               progress;
    }
    QComboBox* mode;
    QDoubleSpinBox *start_x, *end_x;
    QSpinBox* segments;
    QPushButton *preview, *apply, *cancel;
    QLabel* status;
    QProgressBar* progress;
};

QJsonObject context(const QString& revision = "7",
                    const QString& document = "document",
                    const QString& epoch = "epoch") {
    return {{"document_id", document}, {"document_epoch", epoch}, {"expected_revision", revision}};
}

QJsonObject task(const QString& state, double progress = 0.0) {
    return {{"task_id", "mesh-task"}, {"state", state}, {"progress", progress}};
}

void barrier(qcae::DesktopClient& client, bool& reached) {
    (void)client.request("test.barrier", {}, {}, [&](const auto&) { reached = true; });
}

class ModelingToolsTests : public QObject {
    Q_OBJECT
  private slots:
    void previewCancelAndEditsRemainLocal() {
        TestEngine engine;
        QVERIFY(engine.listen());
        qcae::DesktopClient client({engine.endpoint(), engine.workspace(), {}, false});
        client.start();
        QTRY_VERIFY(client.ready());
        Observations observed;
        qcae::ModelingTools tools(client, observed.actions());
        Controls controls(tools);
        QVERIFY(controls.valid());
        tools.setContext(context());
        controls.start_x->setValue(12.5);
        controls.end_x->setValue(43.25);
        controls.preview->click();
        QCOMPARE(observed.previews, 1);
        QVERIFY((observed.preview.points ==
                 std::vector<std::array<double, 3>>({{12.5, 0, 0}, {43.25, 0, 0}})));
        QVERIFY((observed.preview.lines == std::vector<std::array<std::size_t, 2>>({{0, 1}})));
        QVERIFY(controls.apply->isEnabled());
        const auto clears = observed.clears;
        controls.cancel->click();
        QCOMPARE(observed.clears, clears + 1);
        QVERIFY(!controls.apply->isEnabled());
        controls.apply->click();
        controls.preview->click();
        controls.end_x->setValue(50);
        QVERIFY(!controls.apply->isEnabled());
        controls.preview->click();
        tools.setContext(context("8"));
        QVERIFY(!controls.apply->isEnabled());
        QVERIFY(controls.status->text().contains("model changed", Qt::CaseInsensitive));
        controls.end_x->setValue(controls.start_x->value());
        controls.preview->click();
        QVERIFY(!controls.apply->isEnabled());
        QVERIFY(controls.status->text().contains("different"));
        bool drained = false;
        barrier(client, drained);
        QTRY_VERIFY(drained);
        QVERIFY(engine.requests.isEmpty());
        QVERIFY(observed.operations.isEmpty());
        QCOMPARE(observed.refreshes, 0);
    }

    void applyUsesPreviewContextAndSuppressesRepeatedActivation() {
        TestEngine engine;
        QVERIFY(engine.listen());
        qcae::DesktopClient client({engine.endpoint(), engine.workspace(), {}, false});
        client.start();
        QTRY_VERIFY(client.ready());
        Observations observed;
        qcae::ModelingTools tools(client, observed.actions());
        Controls controls(tools);
        QVERIFY(controls.valid());
        tools.setContext(context("8"));
        controls.start_x->setValue(-25);
        controls.end_x->setValue(250);
        controls.preview->click();
        controls.apply->click();
        controls.apply->click();
        QTRY_COMPARE(engine.requests.size(), 1);
        const auto request = engine.requests.front();
        QCOMPARE(request.value("operation").toString(), QString("geometry.create_line"));
        QCOMPARE(request.value("api_version").toString(), QString("1.1"));
        QCOMPARE(request.value("requested_version").toInt(), 1);
        QCOMPARE(request.value("document_id").toString(), QString("document"));
        QCOMPARE(request.value("document_epoch").toString(), QString("epoch"));
        QCOMPARE(request.value("expected_revision").toString(), QString("8"));
        QVERIFY(!request.value("idempotency_key").toString().isEmpty());
        QVERIFY(!request.value("request_id").toString().isEmpty());
        QCOMPARE(
            request.value("parameters").toObject(),
            QJsonObject({{"start_mm", QJsonArray{-25, 0, 0}}, {"end_mm", QJsonArray{250, 0, 0}}}));
        QVERIFY(!controls.apply->isEnabled());
        QVERIFY(!controls.cancel->isEnabled());
        engine.respond(request, {{"entity_id", "created-line"}});
        QTRY_COMPARE(observed.refreshes, 1);
        QCOMPARE(observed.operations, QStringList({"geometry.create_line"}));
        QCOMPARE(controls.status->text(), QString("Line created"));
        QVERIFY(!controls.apply->isEnabled());
        QCOMPARE(engine.requests.size(), 1);
    }

    void timeoutRetryKeepsOriginalIntent() {
        TestEngine engine;
        QVERIFY(engine.listen());
        qcae::DesktopClient client({engine.endpoint(), engine.workspace(), {}, false, 250});
        client.start();
        QTRY_VERIFY(client.ready());
        Observations observed;
        qcae::ModelingTools tools(client, observed.actions());
        Controls controls(tools);
        QVERIFY(controls.valid());
        tools.setContext(context());
        controls.preview->click();
        controls.apply->click();
        QTRY_COMPARE(engine.requests.size(), 1);
        const auto first = engine.requests.front();
        QTRY_COMPARE(controls.apply->text(), QString("Retry request"));
        QVERIFY(controls.apply->isEnabled());
        QVERIFY(!controls.end_x->isEnabled());
        QVERIFY(!controls.cancel->isEnabled());
        tools.setContext(context("8"));
        controls.apply->click();
        controls.apply->click();
        QTRY_COMPARE(engine.requests.size(), 2);
        const auto second = engine.requests.back();
        QVERIFY(first.value("request_id") != second.value("request_id"));
        for (const auto* field : {"operation",
                                  "requested_version",
                                  "document_id",
                                  "document_epoch",
                                  "expected_revision",
                                  "idempotency_key",
                                  "parameters"})
            QCOMPARE(first.value(field), second.value(field));
        engine.respond(second, {{"entity_id", "created-line"}});
        QTRY_COMPARE(observed.refreshes, 1);
        engine.respond(first, {{"entity_id", "created-line"}});
        bool drained = false;
        barrier(client, drained);
        QTRY_VERIFY(drained);
        QCOMPARE(observed.refreshes, 1);
        QCOMPARE(observed.operations.size(), 2);
        QCOMPARE(controls.status->text(), QString("Line created"));
        QCOMPARE(engine.requests.size(), 2);
    }

    void pendingReplyCannotChangeAnotherDocumentOrEpoch() {
        for (const bool new_document : {false, true}) {
            TestEngine engine;
            QVERIFY(engine.listen());
            qcae::DesktopClient client({engine.endpoint(), engine.workspace(), {}, false});
            client.start();
            QTRY_VERIFY(client.ready());
            Observations observed;
            qcae::ModelingTools tools(client, observed.actions());
            Controls controls(tools);
            QVERIFY(controls.valid());
            tools.setContext(context());
            controls.preview->click();
            controls.apply->click();
            QTRY_COMPARE(engine.requests.size(), 1);
            tools.setContext(context("1", new_document ? "other-document" : "document", "next"));
            const auto status = controls.status->text();
            engine.respond(engine.requests.front(), {{"entity_id", "old-line"}});
            bool drained = false;
            barrier(client, drained);
            QTRY_VERIFY(drained);
            QCOMPARE(observed.refreshes, 0);
            QVERIFY(observed.operations.isEmpty());
            QCOMPARE(controls.status->text(), status);
            QVERIFY(controls.preview->isEnabled());
            QVERIFY(!controls.apply->isEnabled());
        }
    }

    void transportSuspendRetainsUncertainAdmission() {
        for (const auto* resumed_revision : {"7", "8"}) {
            TestEngine engine;
            QVERIFY(engine.listen());
            qcae::DesktopClient client({engine.endpoint(), engine.workspace(), {}, false});
            client.start();
            QTRY_VERIFY(client.ready());
            Observations observed;
            qcae::ModelingTools tools(client, observed.actions());
            Controls controls(tools);
            QVERIFY(controls.valid());
            connect(&client, &qcae::DesktopClient::readyChanged, &tools, [&](bool ready) {
                if (!ready)
                    tools.suspend();
            });
            tools.setContext(context());
            controls.preview->click();
            controls.apply->click();
            QTRY_COMPARE(engine.requests.size(), 1);
            auto original = engine.requests.front();
            engine.disconnect();
            QTRY_VERIFY(!client.ready());
            QVERIFY(!controls.apply->isEnabled());
            QVERIFY(!controls.preview->isEnabled());
            QVERIFY(!controls.cancel->isEnabled());
            QVERIFY(!controls.mode->isEnabled());
            QVERIFY(!controls.end_x->isEnabled());
            controls.apply->click();
            controls.cancel->click();
            controls.preview->click();
            QCOMPARE(engine.requests.size(), 1);
            QCOMPARE(observed.previews, 1);
            QVERIFY(engine.listen());
            QTRY_VERIFY(client.ready());
            // Transport readiness alone must not reactivate the old tool or replay a write.
            QVERIFY(!controls.apply->isEnabled());
            tools.setContext(context(resumed_revision));
            QCOMPARE(controls.apply->text(), QString("Retry request"));
            QVERIFY(controls.apply->isEnabled());
            bool drained = false;
            barrier(client, drained);
            QTRY_VERIFY(drained);
            QCOMPARE(engine.requests.size(), 1);
            QVERIFY(observed.operations.isEmpty());
            QCOMPARE(observed.refreshes, 0);
            controls.apply->click();
            controls.apply->click();
            QTRY_COMPARE(engine.requests.size(), 2);
            auto retried = engine.requests.back();
            QVERIFY(original.value("request_id") != retried.value("request_id"));
            original.remove("request_id");
            retried.remove("request_id");
            QCOMPARE(QJsonDocument(original).toJson(QJsonDocument::Compact),
                     QJsonDocument(retried).toJson(QJsonDocument::Compact));
            engine.respond(engine.requests.back(), {{"entity_id", "created-line"}});
            QTRY_COMPARE(observed.refreshes, 1);
            QCOMPARE(observed.operations, QStringList({"geometry.create_line"}));
            QCOMPARE(controls.status->text(), QString("Line created"));
        }
    }

    void resumedEpochDiscardsOldUncertainIntent() {
        TestEngine engine;
        QVERIFY(engine.listen());
        qcae::DesktopClient client({engine.endpoint(), engine.workspace(), {}, false});
        client.start();
        QTRY_VERIFY(client.ready());
        Observations observed;
        qcae::ModelingTools tools(client, observed.actions());
        Controls controls(tools);
        QVERIFY(controls.valid());
        connect(&client, &qcae::DesktopClient::readyChanged, &tools, [&](bool ready) {
            if (!ready)
                tools.suspend();
        });
        tools.setContext(context());
        controls.preview->click();
        controls.apply->click();
        QTRY_COMPARE(engine.requests.size(), 1);
        const auto original = engine.requests.front();
        engine.disconnect();
        QTRY_VERIFY(!client.ready());
        QVERIFY(engine.listen());
        QTRY_VERIFY(client.ready());
        tools.setContext(context("1", "document", "next-epoch"));
        QVERIFY(controls.preview->isEnabled());
        QVERIFY(!controls.apply->isEnabled());
        QCOMPARE(controls.apply->text(), QString("Apply"));
        QCOMPARE(engine.requests.size(), 1);
        controls.preview->click();
        controls.apply->click();
        QTRY_COMPARE(engine.requests.size(), 2);
        const auto next = engine.requests.back();
        QCOMPARE(next.value("document_epoch").toString(), QString("next-epoch"));
        QCOMPARE(next.value("expected_revision").toString(), QString("1"));
        QVERIFY(original.value("idempotency_key") != next.value("idempotency_key"));
        engine.respond(next, {{"entity_id", "new-epoch-line"}});
        QTRY_COMPARE(observed.refreshes, 1);
        QCOMPARE(observed.operations, QStringList({"geometry.create_line"}));
    }

    void transportResumeReadsTrackedTaskWithoutReadmission() {
        TestEngine engine;
        QVERIFY(engine.listen());
        qcae::DesktopClient client({engine.endpoint(), engine.workspace(), {}, false});
        client.start();
        QTRY_VERIFY(client.ready());
        Observations observed;
        qcae::ModelingTools tools(client, observed.actions());
        Controls controls(tools);
        QVERIFY(controls.valid());
        connect(&client, &qcae::DesktopClient::readyChanged, &tools, [&](bool ready) {
            if (!ready)
                tools.suspend();
        });
        tools.setContext(context());
        controls.mode->setCurrentIndex(1);
        tools.setSelectedGeometry("line-id", "Selected line");
        controls.apply->click();
        QTRY_COMPARE(engine.count("mesh.generate_line"), 1);
        engine.respond(engine.latest("mesh.generate_line"), task("queued"));
        QTRY_COMPARE(engine.count("task.status"), 1);
        engine.disconnect();
        QTRY_VERIFY(!client.ready());
        QVERIFY(!controls.apply->isEnabled());
        QVERIFY(!controls.cancel->isEnabled());
        QVERIFY(!controls.mode->isEnabled());
        controls.apply->click();
        controls.cancel->click();
        QVERIFY(engine.listen());
        QTRY_VERIFY(client.ready());
        tools.setContext(context("8"));
        QTRY_COMPARE(engine.count("task.status"), 2);
        const auto resumed = engine.latest("task.status");
        QCOMPARE(resumed.value("parameters").toObject(), QJsonObject({{"task_id", "mesh-task"}}));
        QCOMPARE(resumed.value("document_id").toString(), QString("document"));
        QCOMPARE(resumed.value("document_epoch").toString(), QString("epoch"));
        QCOMPARE(resumed.value("expected_revision").toString(), QString("8"));
        QCOMPARE(engine.count("mesh.generate_line"), 1);
        QCOMPARE(engine.count("task.cancel"), 0);
        QCOMPARE(observed.refreshes, 0);
        engine.respond(resumed, task("running", 0.6));
        QTRY_COMPARE(controls.status->text(), QString("Generating mesh"));
        QCOMPARE(controls.progress->value(), 60);
        QTRY_COMPARE(engine.count("task.status"), 3);
        engine.respond(engine.latest("task.status"), task("succeeded", 1.0));
        QTRY_COMPARE(controls.status->text(), QString("Mesh created"));
        QCOMPARE(observed.refreshes, 1);
        QCOMPARE(engine.count("mesh.generate_line"), 1);
        QCOMPARE(observed.operations, QStringList({"mesh.generate_line"}));
    }

    void deletedWidgetIgnoresPendingReply() {
        TestEngine engine;
        QVERIFY(engine.listen());
        qcae::DesktopClient client({engine.endpoint(), engine.workspace(), {}, false});
        client.start();
        QTRY_VERIFY(client.ready());
        Observations observed;
        auto tools = std::make_unique<qcae::ModelingTools>(client, observed.actions());
        Controls controls(*tools);
        QVERIFY(controls.valid());
        tools->setContext(context());
        controls.preview->click();
        controls.apply->click();
        QTRY_COMPARE(engine.requests.size(), 1);
        tools.reset();
        engine.respond(engine.requests.front(), {{"entity_id", "old-line"}});
        bool drained = false;
        barrier(client, drained);
        QTRY_VERIFY(drained);
        QCOMPARE(observed.refreshes, 0);
        QVERIFY(observed.operations.isEmpty());
    }

    void cancellationCannotBeOverwrittenByAnOlderPoll() {
        TestEngine engine;
        QVERIFY(engine.listen());
        qcae::DesktopClient client({engine.endpoint(), engine.workspace(), {}, false});
        client.start();
        QTRY_VERIFY(client.ready());
        Observations observed;
        qcae::ModelingTools tools(client, observed.actions());
        Controls controls(tools);
        QVERIFY(controls.valid());
        tools.setContext(context());
        controls.mode->setCurrentIndex(1);
        tools.setSelectedGeometry("line-id", "Selected line");
        controls.segments->setValue(7);
        controls.apply->click();
        controls.apply->click();
        QTRY_COMPARE(engine.count("mesh.generate_line"), 1);
        const auto start = engine.latest("mesh.generate_line");
        QCOMPARE(start.value("requested_version").toInt(), 1);
        QCOMPARE(start.value("expected_revision").toString(), QString("7"));
        QCOMPARE(start.value("document_id").toString(), QString("document"));
        QCOMPARE(start.value("document_epoch").toString(), QString("epoch"));
        QVERIFY(!start.value("idempotency_key").toString().isEmpty());
        QCOMPARE(start.value("parameters").toObject(),
                 QJsonObject({{"geometry_id", "line-id"}, {"segments", 7}}));
        engine.respond(start, task("queued"));
        QTRY_COMPARE(controls.status->text(), QString("Mesh queued"));
        QVERIFY(!controls.apply->isEnabled());
        QVERIFY(!controls.mode->isEnabled());
        QTRY_COMPARE(engine.count("task.status"), 1);
        const auto older_poll = engine.latest("task.status");
        controls.cancel->click();
        controls.cancel->click();
        QTRY_COMPARE(engine.count("task.cancel"), 1);
        const auto cancel = engine.latest("task.cancel");
        QCOMPARE(cancel.value("parameters").toObject(), QJsonObject({{"task_id", "mesh-task"}}));
        QCOMPARE(cancel.value("document_id").toString(), QString("document"));
        QCOMPARE(cancel.value("document_epoch").toString(), QString("epoch"));
        engine.respond(cancel, task("cancel_requested"));
        QTRY_COMPARE(controls.status->text(), QString("Cancellation requested"));
        QVERIFY(!controls.cancel->isEnabled());
        engine.respond(older_poll, task("queued"));
        bool drained = false;
        barrier(client, drained);
        QTRY_VERIFY(drained);
        QCOMPARE(controls.status->text(), QString("Cancellation requested"));
        QCOMPARE(observed.refreshes, 0);
        QTRY_COMPARE(engine.count("task.status"), 2);
        engine.respond(engine.latest("task.status"), task("cancelled"));
        QTRY_COMPARE(controls.status->text(), QString("Mesh cancelled — project unchanged"));
        QCOMPARE(observed.refreshes, 1);
        QVERIFY(!controls.status->text().contains("created"));
        QCOMPARE(engine.count("mesh.generate_line"), 1);
        QCOMPARE(engine.count("task.cancel"), 1);
        QCOMPARE(observed.operations, QStringList({"mesh.generate_line", "task.cancel"}));
    }

    void failedPollOffersReadOnlyRefreshWithoutRestartingMesh() {
        TestEngine engine;
        QVERIFY(engine.listen());
        qcae::DesktopClient client({engine.endpoint(), engine.workspace(), {}, false});
        client.start();
        QTRY_VERIFY(client.ready());
        Observations observed;
        qcae::ModelingTools tools(client, observed.actions());
        Controls controls(tools);
        QVERIFY(controls.valid());
        tools.setContext(context());
        controls.mode->setCurrentIndex(1);
        tools.setSelectedGeometry("line-id", "Selected line");
        controls.apply->click();
        QTRY_COMPARE(engine.count("mesh.generate_line"), 1);
        engine.respond(engine.latest("mesh.generate_line"), task("queued"));
        QTRY_COMPARE(engine.count("task.status"), 1);
        engine.fail(engine.latest("task.status"), "TIMEOUT", "Status is temporarily unavailable");
        QTRY_COMPARE(controls.apply->text(), QString("Refresh task"));
        QVERIFY(controls.apply->isEnabled());
        QCOMPARE(observed.refreshes, 0);
        controls.apply->click();
        controls.apply->click();
        QTRY_COMPARE(engine.count("task.status"), 2);
        QCOMPARE(engine.latest("task.status").value("parameters").toObject(),
                 QJsonObject({{"task_id", "mesh-task"}}));
        QCOMPARE(engine.count("mesh.generate_line"), 1);
        engine.respond(engine.latest("task.status"), task("running", 0.5));
        QTRY_COMPARE(controls.status->text(), QString("Generating mesh"));
        QCOMPARE(controls.progress->value(), 50);
        QVERIFY(!controls.apply->isEnabled());
        QTRY_COMPARE(engine.count("task.status"), 3);
        engine.respond(engine.latest("task.status"), task("succeeded", 1.0));
        QTRY_COMPARE(controls.status->text(), QString("Mesh created"));
        QCOMPARE(observed.refreshes, 1);
        QCOMPARE(engine.count("mesh.generate_line"), 1);
    }
};
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    ModelingToolsTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "modeling_tools_tests.moc"
