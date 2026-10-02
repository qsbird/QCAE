#include "qcae/desktop.hpp"
#include "qcae/operations.hpp"
#include "qcae/vtk_view.hpp"
#include "../ui/desktop/src/analysis_tools.hpp"
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QDialog>
#include <QDockWidget>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeySequenceEdit>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QDialogButtonBox>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMainWindow>
#include <QMap>
#include <QMessageBox>
#include <QMouseEvent>
#include <QFontInfo>
#include <QStyle>
#include <QAbstractButton>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QScrollArea>
#include <QSpinBox>
#include <QSurfaceFormat>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolBar>
#include <QTreeWidget>
#include <QVTKOpenGLNativeWidget.h>
#include <vtkCamera.h>
#include <vtkOpenGLRenderWindow.h>
#include <vtkRenderer.h>
#include <vtkRendererCollection.h>
#include <vtkRenderWindow.h>
#include <QCryptographicHash>
#include <QUuid>
#include <memory>
#include <filesystem>
#include <algorithm>

namespace {
QString engine_path, evidence_dir;
struct Engine {
    QProcess process;
    ~Engine() {
        process.terminate();
        if (!process.waitForFinished(5000)) {
            process.kill();
            process.waitForFinished(5000);
        }
    }
};
// Real DesktopClient framing; hold the report reply to exercise the UI fences.
class AnalysisReportEngine {
  public:
    AnalysisReportEngine() : directory_("/tmp/qcae-analysis-report-XXXXXX") {
        QObject::connect(&server_, &QLocalServer::newConnection, &server_, [this] {
            peer_ = server_.nextPendingConnection();
            QObject::connect(peer_, &QLocalSocket::readyRead, &server_, [this] {
                incoming_ += peer_->readAll();
                for (;;) {
                    const auto newline = incoming_.indexOf('\n');
                    if (newline < 0)
                        return;
                    request = QJsonDocument::fromJson(incoming_.left(newline)).object();
                    incoming_.remove(0, newline + 1);
                    if (request.value("operation") == "runtime.handshake")
                        reply("success", {{"api_version", "1.1"}});
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
    void reply(const QString& status, const QJsonObject& report) {
        peer_->write(QJsonDocument(QJsonObject{{"request_id", request.value("request_id")},
                                               {"status", status},
                                               {"data", report},
                                               {"error",
                                                QJsonObject{{"code", "TEST_DIAGNOSTIC"},
                                                            {"message", "Report probe"}}}})
                         .toJson(QJsonDocument::Compact) +
                     '\n');
        request = {};
    }
    QJsonObject request;

  private:
    QTemporaryDir directory_;
    QLocalServer server_;
    QLocalSocket* peer_{};
    QByteArray incoming_;
};
// Transparent production-engine transport with selected replies held. The test
// injects event/metadata faults at the wire, never changes the engine's model.
class PropertyReplyProxy {
  public:
    explicit PropertyReplyProxy(QString backend) : backend_(std::move(backend)) {
        QObject::connect(&server_, &QLocalServer::newConnection, &server_, [this] {
            peer_ = server_.nextPendingConnection();
            QObject::connect(peer_, &QLocalSocket::readyRead, &server_, [this] {
                from_client_ += peer_->readAll();
                consume(from_client_, [this](const QJsonObject& request) {
                    transcript.append(QJsonObject{{"direction", "request"}, {"frame", request}});
                    requests_.insert(request.value("request_id").toString(), request);
                    if (request.value("operation") == "changes.commit") {
                        ++commit_requests;
                        if (block_commits)
                            return;
                    }
                    const auto bytes = encode(request);
                    if (engine_.state() == QLocalSocket::ConnectedState)
                        engine_.write(bytes);
                    else
                        pending_backend_ += bytes;
                });
            });
            engine_.connectToServer(backend_);
        });
        QObject::connect(&engine_, &QLocalSocket::connected, &server_, [this] {
            engine_.write(pending_backend_);
            pending_backend_.clear();
        });
        QObject::connect(&engine_, &QLocalSocket::readyRead, &server_, [this] {
            from_engine_ += engine_.readAll();
            consume(from_engine_, [this](const QJsonObject& response) {
                const auto request = requests_.value(response.value("request_id").toString());
                const auto operation = request.value("operation").toString();
                if (hold_current && operation == "project.current") {
                    held_current.append(response);
                    return;
                }
                const auto ids = request.value("parameters").toObject().value("ids").toArray();
                const bool property_row =
                    operation == "entity.query" && ids == QJsonArray{force_id};
                const bool property_preview = operation == "force.preview_vector";
                if ((hold_row && property_row) || (hold_preview && property_preview)) {
                    held_property.append(response);
                    hold_row = hold_preview = false;
                    return;
                }
                send(response);
            });
        });
    }
    bool listen(const QString& endpoint) {
        return server_.listen(endpoint);
    }
    void send(const QJsonObject& frame) {
        transcript.append(QJsonObject{{"direction", "response"}, {"frame", frame}});
        peer_->write(encode(frame));
    }
    void releaseProperty(const QString& fault = {}) {
        const auto pending = held_property;
        held_property.clear();
        for (auto frame : pending) {
            auto data = frame.value("data").toObject();
            const auto revision = data.value("revision").toString().toULongLong();
            if (fault == "data-new")
                data.insert("revision", QString::number(revision + 1));
            else if (fault == "envelope-new")
                frame.insert("revision", QString::number(revision + 1));
            else if (fault == "missing") {
                data.remove("revision");
                frame.remove("revision");
            } else if (fault == "fraction")
                data.insert("revision", static_cast<double>(revision) + 0.5);
            else if (fault == "negative")
                data.insert("revision", -1);
            else if (fault == "document")
                data.insert("document_id", "foreign-document");
            else if (fault == "epoch")
                data.insert("document_epoch", "foreign-epoch");
            else if (fault == "legacy-numeric") {
                data.insert("revision", static_cast<qint64>(revision));
                frame.remove("revision");
            }
            frame.insert("data", data);
            send(frame);
        }
    }
    QString force_id;
    bool hold_current{}, hold_row{}, hold_preview{}, block_commits{};
    int commit_requests{};
    QList<QJsonObject> held_current, held_property;
    QJsonArray transcript;

  private:
    static QByteArray encode(const QJsonObject& frame) {
        return QJsonDocument(frame).toJson(QJsonDocument::Compact) + '\n';
    }
    template <typename Callback> static void consume(QByteArray& buffer, Callback callback) {
        while (true) {
            const auto newline = buffer.indexOf('\n');
            if (newline < 0)
                return;
            const auto frame = QJsonDocument::fromJson(buffer.left(newline)).object();
            buffer.remove(0, newline + 1);
            callback(frame);
        }
    }
    QString backend_;
    QLocalServer server_;
    QLocalSocket engine_;
    QLocalSocket* peer_{};
    QByteArray from_client_, from_engine_, pending_backend_;
    QMap<QString, QJsonObject> requests_;
};
QJsonObject reportContext() {
    return {
        {"document_id", "document-1"}, {"document_epoch", "epoch-1"}, {"expected_revision", "5"}};
}
QJsonObject checkReport(const QString& outcome) {
    const QJsonObject version{
        {"document_id", "document-1"}, {"document_epoch", "epoch-1"}, {"revision", "5"}};
    QJsonArray issues;
    if (outcome != "success") {
        const QStringList rules = outcome == "needs_input"
                                      ? QStringList{"missing-load", "missing-constraint"}
                                      : QStringList{"cantilever-transverse-load"};
        for (const auto& rule : rules)
            issues.append(QJsonObject{{"rule_id", rule},
                                      {"rule_version", "1"},
                                      {"severity", "error"},
                                      {"entity_id", "analysis-1"},
                                      {"entity_type", "8"},
                                      {"input_version", version},
                                      {"state", "current"},
                                      {"field", "load_cases"},
                                      {"actual", "0"},
                                      {"expected", ">=1"},
                                      {"message", "Required physical fact is absent"}});
    }
    return {{"check_id", "check-1"},
            {"analysis_id", "analysis-1"},
            {"input_version", version},
            {"current_version", version},
            {"catalog_version", "qcae.analysis.rules.v2"},
            {"check_execution", "completed"},
            {"outcome", outcome},
            {"analysis_kind", "linear_static"},
            {"profile",
             QJsonObject{{"profile_id", "nastran"},
                         {"profile_version", "1"},
                         {"definition_digest", "test-profile-digest"}}},
            {"state", "current"},
            {"issues", issues}};
}
void staleReport(QJsonObject& report, const QString& current_revision) {
    auto version = report.value("current_version").toObject();
    version.insert("revision", current_revision);
    report.insert("current_version", version);
    report.insert("state", "stale");
    auto issues = report.value("issues").toArray();
    for (qsizetype index = 0; index < issues.size(); ++index) {
        auto issue = issues[index].toObject();
        issue.insert("state", "stale");
        issues[index] = issue;
    }
    report.insert("issues", issues);
}
QJsonArray analysisRows() {
    return {
        QJsonObject{{"entity_id", "analysis-1"}, {"kind", "analysis"}, {"name", "Report probe"}}};
}
bool saveCheckEvidence(const QString& invocation, const QString& phase, const QJsonObject& reply) {
    if (!QDir().mkpath(evidence_dir))
        return false;
    QFile file(evidence_dir + "/" + invocation + "-check-" + phase + ".json");
    const auto bytes = QJsonDocument(reply).toJson();
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
QJsonObject call(qcae::DesktopClient& client,
                 const QString& operation,
                 const QJsonObject& parameters = {},
                 const QJsonObject& context = {}) {
    auto reply = std::make_shared<QJsonObject>();
    QEventLoop loop;
    QPointer<QEventLoop> guard(&loop);
    (void)client.request(operation, parameters, context, [guard, reply](const auto& response) {
        *reply = response;
        if (guard)
            guard->quit();
    });
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    loop.exec();
    return *reply;
}
QJsonObject data(const QJsonObject& reply) {
    return reply.value("data").toObject();
}
QJsonObject current(qcae::DesktopClient& client, const QString& key = {}) {
    const auto info = data(call(client, "project.current"));
    QJsonObject result{{"document_id", info.value("document_id")},
                       {"document_epoch", info.value("document_epoch")},
                       {"expected_revision", info.value("revision")}};
    if (!key.isEmpty())
        result.insert("idempotency_key", key);
    return result;
}
QJsonArray entities(qcae::DesktopClient& client, const QString& kind) {
    return data(call(client, "entity.query", {{"kind", kind}}, current(client)))
        .value("entities")
        .toArray();
}
QTreeWidgetItem* geometryItem(QTreeWidget* tree) {
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        auto* item = tree->topLevelItem(i);
        if (item->text(1) == "geometry")
            return item;
    }
    return nullptr;
}
QAction* action(QMainWindow* window, const QString& title) {
    for (auto* candidate : window->findChildren<QAction*>())
        if (candidate->text() == title)
            return candidate;
    return nullptr;
}
void click(QWidget* widget) {
    for (auto* parent = widget->parentWidget(); parent; parent = parent->parentWidget())
        if (auto* scroll = qobject_cast<QScrollArea*>(parent))
            scroll->ensureWidgetVisible(widget);
    QTest::mouseClick(widget, Qt::LeftButton);
}
void chooseFile(const QString& path) {
    auto* driver = new QTimer(qApp);
    driver->setInterval(50);
    auto dialog = std::make_shared<QPointer<QFileDialog>>();
    auto ticks = std::make_shared<int>(0);
    auto selected_at = std::make_shared<int>(0);
    QObject::connect(driver, &QTimer::timeout, driver, [path, driver, dialog, ticks, selected_at] {
        ++*ticks;
        if (!*dialog && *selected_at == 0) {
            *dialog = qobject_cast<QFileDialog*>(QApplication::activeModalWidget());
            if (!*dialog)
                for (auto* candidate : QApplication::topLevelWidgets())
                    if (auto* file = qobject_cast<QFileDialog*>(candidate);
                        file && file->isVisible()) {
                        *dialog = file;
                        break;
                    }
            if (*dialog) {
                (*dialog)->setDirectory(QFileInfo(path).absolutePath());
                (*dialog)->selectFile(QFileInfo(path).fileName());
                *selected_at = *ticks;
            }
        }
        if (*dialog && *ticks >= *selected_at + 5) {
            if (auto* filename = (*dialog)->findChild<QLineEdit*>("fileNameEdit"))
                if (filename->text() != QFileInfo(path).fileName())
                    filename->setText(QFileInfo(path).fileName());
            auto* buttons = (*dialog)->findChild<QDialogButtonBox*>();
            if (buttons) {
                auto* button = buttons->button((*dialog)->acceptMode() == QFileDialog::AcceptSave
                                                   ? QDialogButtonBox::Save
                                                   : QDialogButtonBox::Open);
                if (!button)
                    for (auto* candidate : buttons->buttons())
                        if (buttons->buttonRole(candidate) == QDialogButtonBox::AcceptRole) {
                            button = qobject_cast<QPushButton*>(candidate);
                            break;
                        }
                if (*ticks == *selected_at + 5)
                    qInfo() << "File dialog driver" << (*dialog)->windowTitle()
                            << (*dialog)->selectedFiles() << "accept button"
                            << (button ? button->text() : QString("missing")) << "enabled"
                            << (button && button->isEnabled());
                if (button && button->isEnabled())
                    QTest::mouseClick(button, Qt::LeftButton);
            }
        }
        if ((*selected_at && (!*dialog || !(*dialog)->isVisible())) || *ticks > 100) {
            if (*ticks > 100) {
                qWarning() << "File dialog did not accept" << path;
                if (*dialog) {
                    for (auto* field : (*dialog)->findChildren<QLineEdit*>())
                        qWarning() << "file field" << field->objectName() << field->text();
                    for (auto* button : (*dialog)->findChildren<QAbstractButton*>())
                        qWarning() << "file button" << button->text() << button->isEnabled();
                    (*dialog)->reject();
                }
            }
            driver->stop();
            driver->deleteLater();
        }
    });
    driver->start();
}
QJsonObject modelImage(qcae::DesktopClient& observer) {
    const auto context = current(observer);
    const auto rows = data(call(observer, "entity.query", {{"limit", 1000}}, context))
                          .value("entities")
                          .toArray();
    QJsonObject image;
    for (const auto& item : rows) {
        const auto row = item.toObject();
        const auto identity = row.value("entity_id").toString();
        image.insert(
            identity,
            QJsonObject{{"kind", row.value("kind")},
                        {"fields",
                         data(call(observer, "entity.fields", {{"entity_id", identity}}, context))
                             .value("fields")}});
    }
    return image;
}
QJsonArray modelRows(qcae::DesktopClient& observer) {
    return data(call(observer, "entity.query", {{"limit", 1000}}, current(observer)))
        .value("entities")
        .toArray();
}
QJsonObject externalWrite(qcae::DesktopClient& observer,
                          const QString& endpoint,
                          const QString& request_path,
                          const QString& operation,
                          const QJsonObject& parameters,
                          bool thin_script) {
    auto request = current(observer, QString("mixed-%1").arg(operation));
    request.insert("api_version",
                   QString::fromUtf8(qcae::api_version.data(),
                                     static_cast<qsizetype>(qcae::api_version.size())));
    request.insert("request_id", QString("mixed-request-%1").arg(operation));
    request.insert("operation", operation);
    request.insert("parameters", parameters);
    request.insert("requested_version", 1);
    QProcess process;
    if (thin_script) {
        QFile file(request_path);
        if (!file.open(QIODevice::WriteOnly))
            return {};
        file.write(QJsonDocument(request).toJson());
        file.close();
        process.setProgram("python3");
        process.setArguments(
            {QString::fromStdString(
                 (std::filesystem::path(__FILE__).parent_path() / "thin_script_write.py").string()),
             endpoint,
             request_path});
    } else {
        process.setProgram(QFileInfo(engine_path).absoluteDir().filePath("qcae-cli"));
        process.setArguments({"--socket", endpoint, "--no-start"});
    }
    process.start();
    if (!process.waitForStarted(5000))
        return {};
    if (!thin_script) {
        process.write(QJsonDocument(request).toJson(QJsonDocument::Compact));
        process.closeWriteChannel();
    }
    if (!process.waitForFinished(10000) || process.exitCode() != 0) {
        qWarning() << "External write failed" << process.readAllStandardError();
        return {};
    }
    const auto output = QJsonDocument::fromJson(process.readAllStandardOutput()).object();
    if (thin_script)
        return output;
    return {{"entry", "CLI"}, {"request", request}, {"response", output}};
}
class Workflow : public QObject {
    Q_OBJECT

    void exercisePropertyReplyFence(const QString& stage,
                                    const QString& invalidation,
                                    const QString& metadata) {
        QTemporaryDir temp("/tmp/qcae-property-fence-XXXXXX");
        QVERIFY(temp.isValid());
        const auto backend = temp.filePath("backend.sock"),
                   workspace = temp.filePath("work.sqlite");
        Engine engine;
        engine.process.setProgram(engine_path);
        engine.process.setArguments({"--socket", backend, "--workspace", workspace});
        engine.process.start();
        QVERIFY(engine.process.waitForStarted(5000));
        qcae::DesktopClient observer({backend, workspace, {}, false, 5000});
        observer.start();
        QTRY_VERIFY_WITH_TIMEOUT(observer.ready(), 10000);
        QCOMPARE(call(observer,
                      "project.create",
                      {{"name", "Property fence fixture"}},
                      {{"idempotency_key", "property-fence-project"}})
                     .value("status"),
                 QJsonValue("success"));
        const auto line =
            data(call(observer,
                      "geometry.create_line",
                      {{"start_mm", QJsonArray{0, 0, 0}}, {"end_mm", QJsonArray{100, 0, 0}}},
                      current(observer, "property-fence-line")))
                .value("entity_id");
        QVERIFY(line.isString());
        const auto mesh = data(call(observer,
                                    "mesh.generate_line",
                                    {{"geometry_id", line}, {"segments", 1}},
                                    current(observer, "property-fence-mesh")));
        const auto task = mesh.value("task_id");
        QVERIFY(task.isString());
        QTRY_COMPARE_WITH_TIMEOUT(
            data(call(observer, "task.status", {{"task_id", task}}, current(observer)))
                .value("state"),
            QJsonValue("succeeded"),
            10000);
        const auto nodes = entities(observer, "node");
        QCOMPARE(nodes.size(), 2);
        const auto node = nodes.at(0).toObject().value("entity_id");
        const auto force = data(call(observer,
                                     "force.create",
                                     {{"node_id", node},
                                      {"x", QJsonObject{{"value", 0}, {"unit", "N"}}},
                                      {"y", QJsonObject{{"value", -1}, {"unit", "N"}}},
                                      {"z", QJsonObject{{"value", 0}, {"unit", "N"}}}},
                                     current(observer, "property-fence-force")))
                               .value("entity_id")
                               .toString();
        QVERIFY(!force.isEmpty());
        const auto before = current(observer), before_model = modelImage(observer);
        const auto before_history = data(call(observer, "history.list", {}, before));
        PropertyReplyProxy proxy(backend);
        proxy.force_id = force;
        const auto endpoint = temp.filePath("proxy.sock");
        QVERIFY(proxy.listen(endpoint));
        auto window = std::unique_ptr<QMainWindow>(
            qcae::create_desktop_window({endpoint, workspace, {}, false, 5000}));
        window->resize(1440, 900);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window.get()));
        auto* client = window->findChild<qcae::DesktopClient*>();
        auto* tree = window->findChild<QTreeWidget*>("entityTree");
        auto* properties = window->findChild<QDockWidget*>("selectionPropertiesDock");
        QVERIFY(client && tree && properties);
        QTRY_VERIFY_WITH_TIMEOUT(qcae::desktop_pipeline_idle(*window), 15000);
        QVERIFY(client->supportsEvents());
        properties->show();
        properties->raise();
        const auto editors = properties->findChildren<QLineEdit*>();
        QCOMPARE(editors.size(), 4);
        proxy.hold_row = stage == "row";
        bool selected = false;
        for (int index = 0; index < tree->topLevelItemCount(); ++index) {
            auto* item = tree->topLevelItem(index);
            if (item->data(0, Qt::UserRole).toString() != force)
                continue;
            tree->scrollToItem(item);
            QTest::mouseClick(tree->viewport(),
                              Qt::LeftButton,
                              Qt::NoModifier,
                              tree->visualItemRect(item).center());
            selected = true;
            break;
        }
        QVERIFY(selected);
        if (stage != "row") {
            QTRY_COMPARE_WITH_TIMEOUT(editors[1]->text(), QString("-1"), 10000);
            editors[1]->selectAll();
            QTest::keyClicks(editors[1], "-2");
            auto* button = properties->findChild<QPushButton*>(
                stage == "automatic" ? "propertyApply" : "propertyPreview");
            QVERIFY(button);
            proxy.hold_preview = stage != "ready";
            click(button);
        }
        if (stage == "ready")
            QTRY_VERIFY_WITH_TIMEOUT(!window->property("propertyPreviewId").toString().isEmpty(),
                                     10000);
        else
            QTRY_COMPARE_WITH_TIMEOUT(proxy.held_property.size(), 1, 10000);
        const auto held = proxy.held_property;
        QJsonArray held_frames;
        for (const auto& frame : held)
            held_frames.append(frame);
        if (!invalidation.isEmpty()) {
            proxy.hold_current = proxy.block_commits = true;
            QJsonObject event{
                {"frame_type", "event"},
                {"engine_instance_id", client->engineInstanceId()},
                {"sequence", QString::number(client->eventSequence() + 1)},
                {"event", "DocumentChanged"},
                {"document_id", before.value("document_id")},
                {"document_epoch", before.value("document_epoch")},
                {"revision",
                 QString::number(before.value("expected_revision").toString().toULongLong() + 1)},
                {"data", QJsonObject{{"resync_required", true}}}};
            if (invalidation == "document")
                event.insert("document_id", "foreign-document");
            else if (invalidation == "epoch")
                event.insert("document_epoch", "foreign-epoch");
            else if (invalidation == "missing-summary")
                event.insert("data",
                             QJsonObject{{"resync_required", false},
                                         {"base_revision", before.value("expected_revision")}});
            else if (invalidation == "gap")
                event = {{"frame_type", "event_gap"},
                         {"engine_instance_id", client->engineInstanceId()},
                         {"sequence", QString::number(client->eventSequence() + 2)},
                         {"resync_required", true},
                         {"reason", "Controlled property reply gap"}};
            proxy.send(event);
            QTRY_VERIFY_WITH_TIMEOUT(!proxy.held_current.isEmpty(), 10000);
        } else
            proxy.block_commits = true;
        proxy.releaseProperty(metadata);
        // Flush actual DesktopClient queued delivery and any automatic commit request.
        QTest::qWait(100);
        const auto visible = properties->findChild<QLabel*>("propertyDetails")->text();
        const auto preview_id = window->property("propertyPreviewId").toString();
        const auto invocation = "property-fence-" + stage + "-" + invalidation + "-" + metadata +
                                "-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
        QVERIFY(saveCheckEvidence(invocation,
                                  "wire-widget",
                                  {{"stage", stage},
                                   {"invalidation", invalidation},
                                   {"metadata_fault", metadata},
                                   {"held_property_reply", held_frames},
                                   {"transcript", proxy.transcript},
                                   {"held_current_count", proxy.held_current.size()},
                                   {"property_text", visible},
                                   {"property_y", editors[1]->text()},
                                   {"preview_id", preview_id},
                                   {"commit_requests", proxy.commit_requests},
                                   {"before", before},
                                   {"model_before", before_model},
                                   {"history_before", before_history}}));
        QVERIFY(window->grab().save(evidence_dir + "/" + invocation + ".png"));
        QCOMPARE(proxy.commit_requests, 0);
        if (metadata == "legacy-numeric") {
            if (stage == "row")
                QCOMPARE(editors[1]->text(), QString("-1"));
            else if (stage == "manual")
                QVERIFY(!preview_id.isEmpty());
        } else {
            QVERIFY(preview_id.isEmpty());
            if (stage == "row" || !invalidation.isEmpty()) {
                QVERIFY(editors[1]->text().isEmpty());
                QVERIFY(!editors[1]->isEnabled());
                QVERIFY(visible.isEmpty());
            }
        }
        QCOMPARE(current(observer), before);
        QCOMPARE(modelImage(observer), before_model);
        QCOMPARE(data(call(observer, "history.list", {}, before)), before_history);
    }

  private slots:
    void propertyRepliesWaitForAuthoritativeRefresh_data() {
        QTest::addColumn<QString>("stage");
        QTest::addColumn<QString>("invalidation");
        for (const auto* stage : {"row", "manual", "automatic", "ready"})
            for (const auto* invalidation :
                 {"revision", "missing-summary", "document", "epoch", "gap"})
                QTest::newRow(qPrintable(QString(stage) + "-" + invalidation))
                    << QString(stage) << QString(invalidation);
    }
    void propertyRepliesWaitForAuthoritativeRefresh() {
        QFETCH(QString, stage);
        QFETCH(QString, invalidation);
        exercisePropertyReplyFence(stage, invalidation, {});
    }
    void propertyReplyVersionsMatchRequestedContext_data() {
        QTest::addColumn<QString>("stage");
        QTest::addColumn<QString>("metadata");
        for (const auto* stage : {"row", "manual", "automatic"})
            for (const auto* fault : {"data-new",
                                      "envelope-new",
                                      "missing",
                                      "fraction",
                                      "negative",
                                      "document",
                                      "epoch"})
                QTest::newRow(qPrintable(QString(stage) + "-" + fault))
                    << QString(stage) << QString(fault);
        for (const auto* stage : {"row", "manual"})
            QTest::newRow(qPrintable(QString(stage) + "-legacy-numeric"))
                << QString(stage) << QString("legacy-numeric");
    }
    void propertyReplyVersionsMatchRequestedContext() {
        QFETCH(QString, stage);
        QFETCH(QString, metadata);
        exercisePropertyReplyFence(stage, {}, metadata);
    }
    void aiSnapshotUnitsAndCameraAreConsistent() {
        const auto snapshot = qEnvironmentVariable("QCAE_AI_UNITS_SNAPSHOT");
        if (snapshot.isEmpty())
            QSKIP("Set QCAE_AI_UNITS_SNAPSHOT to an actual AI-created .qcae snapshot.");
        QFile source(snapshot);
        QVERIFY(source.open(QIODevice::ReadOnly));
        const auto original_bytes = source.readAll();
        const auto snapshot_sha = QString::fromLatin1(
            QCryptographicHash::hash(original_bytes, QCryptographicHash::Sha256).toHex());
        const auto invocation = "AI-units-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
        QVERIFY(QDir().mkpath(evidence_dir));
        QTemporaryDir temp("/tmp/qcae-ai-units-gui-XXXXXX");
        QVERIFY(temp.isValid());
        const auto endpoint = temp.filePath("engine.sock"),
                   workspace = temp.filePath("work.sqlite");
        Engine engine;
        engine.process.setProgram(engine_path);
        engine.process.setArguments({"--socket", endpoint, "--workspace", workspace});
        engine.process.start();
        QVERIFY(engine.process.waitForStarted(5000));
        auto window = std::unique_ptr<QMainWindow>(
            qcae::create_desktop_window({endpoint, workspace, {}, false, 5000}));
        window->resize(1440, 900);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window.get()));
        auto* client = window->findChild<qcae::DesktopClient*>();
        QVERIFY(client);
        QTRY_VERIFY_WITH_TIMEOUT(client->ready(), 10000);
        qcae::DesktopClient observer({endpoint, workspace, {}, false, 5000});
        observer.start();
        QTRY_VERIFY_WITH_TIMEOUT(observer.ready(), 10000);
        auto* open = action(window.get(), "Open project…");
        QVERIFY(open);
        chooseFile(snapshot);
        open->trigger();
        auto* tree = window->findChild<QTreeWidget*>("entityTree");
        auto* viewport = window->findChild<qcae::VtkView*>();
        auto* widget = window->findChild<QVTKOpenGLNativeWidget*>();
        QVERIFY(tree && viewport && widget);
        QTRY_VERIFY_WITH_TIMEOUT(viewport->hasPacket(), 15000);
        QTRY_VERIFY_WITH_TIMEOUT(qcae::desktop_pipeline_idle(*window), 15000);
        const auto context = current(observer);
        const auto nodes = entities(observer, "node"), beams = entities(observer, "beam"),
                   materials = entities(observer, "material"),
                   sections = entities(observer, "section"), forces = entities(observer, "force");
        QCOMPARE(nodes.size(), 21);
        QCOMPARE(beams.size(), 20);
        QCOMPARE(materials.size(), 1);
        QCOMPARE(sections.size(), 1);
        QCOMPARE(forces.size(), 1);
        const auto material = materials.at(0).toObject(), section = sections.at(0).toObject(),
                   force = forces.at(0).toObject();
        QCOMPARE(material.value("young_modulus_mpa").toDouble(), 210000.0);
        QCOMPARE(section.value("area_mm2").toDouble(), 100.0);
        QCOMPARE(section.value("i1_mm4").toDouble(), 833.333);
        QCOMPARE(section.value("i2_mm4").toDouble(), 833.333);
        QCOMPARE(section.value("torsion_mm4").toDouble(), 1400.0);
        QCOMPARE(section.value("material_id"), material.value("entity_id"));
        QCOMPARE(force.value("force_n").toArray(), QJsonArray({0, -1, 0}));
        std::vector<QJsonObject> sorted_nodes;
        for (const auto& row : nodes)
            sorted_nodes.push_back(row.toObject());
        std::sort(sorted_nodes.begin(), sorted_nodes.end(), [](const auto& a, const auto& b) {
            return a.value("position_mm").toArray()[0].toDouble() <
                   b.value("position_mm").toArray()[0].toDouble();
        });
        for (std::size_t index = 0; index < sorted_nodes.size(); ++index)
            QCOMPARE(sorted_nodes[index].value("position_mm").toArray(),
                     QJsonArray({50.0 * static_cast<double>(index), 0, 0}));
        const auto tip = sorted_nodes.back().value("entity_id").toString();
        QCOMPARE(force.value("node_id").toString(), tip);
        for (const auto& row : beams) {
            QCOMPARE(row.toObject().value("section_id"), section.value("entity_id"));
        }
        const auto original_model = modelImage(observer);
        const auto history_before = data(call(observer, "history.list", {}, context));
        const auto backend_window = widget->renderWindow();
        QVERIFY(backend_window);
        auto* renderer = backend_window->GetRenderers()->GetFirstRenderer();
        QVERIFY(renderer);
        const auto camera_state = [&] {
            auto* camera = renderer->GetActiveCamera();
            const auto* p = camera->GetPosition();
            const auto* f = camera->GetFocalPoint();
            const auto* u = camera->GetViewUp();
            return QJsonObject{{"position", QJsonArray{p[0], p[1], p[2]}},
                               {"focal_point", QJsonArray{f[0], f[1], f[2]}},
                               {"view_up", QJsonArray{u[0], u[1], u[2]}}};
        };
        auto* gl = vtkOpenGLRenderWindow::SafeDownCast(backend_window);
        QVERIFY(gl);
        const QJsonObject backend{
            {"qt_version", qVersion()},
            {"qt_platform", QGuiApplication::platformName()},
            {"qt_style", QApplication::style()->metaObject()->className()},
            {"font_family", QFontInfo(window->font()).family()},
            {"font_engine_override", qEnvironmentVariable("QT_QPA_FONTENGINE")},
            {"vtk_window_class", backend_window->GetClassName()},
            {"opengl_capabilities", QString::fromUtf8(gl->ReportCapabilities())}};
        const QJsonObject initial{{"snapshot_sha256", snapshot_sha},
                                  {"snapshot_path", snapshot},
                                  {"context", context},
                                  {"nodes", nodes},
                                  {"beams", beams},
                                  {"material", material},
                                  {"section", section},
                                  {"force", force},
                                  {"model", original_model},
                                  {"history", history_before},
                                  {"backend", backend}};
        QVERIFY(saveCheckEvidence(invocation, "units-original", initial));
        QVERIFY(window->grab().save(evidence_dir + "/" + invocation + "-initial-visible.png"));
        auto* properties = window->findChild<QDockWidget*>("selectionPropertiesDock");
        QVERIFY(properties);
        properties->show();
        properties->raise();
        const auto select = [&](const QString& identity) {
            for (int index = 0; index < tree->topLevelItemCount(); ++index) {
                auto* item = tree->topLevelItem(index);
                if (item->data(0, Qt::UserRole).toString() != identity)
                    continue;
                tree->scrollToItem(item);
                QTest::mouseClick(tree->viewport(),
                                  Qt::LeftButton,
                                  Qt::NoModifier,
                                  tree->visualItemRect(item).center());
                return true;
            }
            return false;
        };
        auto* selected = window->findChild<QLabel*>("selectedEntities");
        QVERIFY(selected);
        QVERIFY(select(tip));
        QTRY_VERIFY_WITH_TIMEOUT(selected->text().contains(tip), 10000);
        const auto editors = properties->findChildren<QLineEdit*>();
        QCOMPARE(editors.size(), 4);
        QTRY_COMPARE_WITH_TIMEOUT(editors[0]->text(), QString("1000"), 10000);
        QCOMPARE(editors[1]->text(), QString("0"));
        QCOMPARE(editors[2]->text(), QString("0"));
        QVERIFY(window->grab().save(evidence_dir + "/" + invocation + "-tip-1000mm.png"));
        QVERIFY(select(material.value("entity_id").toString()));
        QTRY_VERIFY_WITH_TIMEOUT(selected->text().contains(material.value("entity_id").toString()),
                                 10000);
        QTRY_COMPARE_WITH_TIMEOUT(editors[3]->text(), QString("210000"), 10000);
        QVERIFY(window->grab().save(evidence_dir + "/" + invocation + "-material-210000MPa.png"));
        QVERIFY(select(section.value("entity_id").toString()));
        QTRY_VERIFY_WITH_TIMEOUT(selected->text().contains(section.value("entity_id").toString()),
                                 10000);
        auto* details = properties->findChild<QLabel*>("propertyDetails");
        QVERIFY2(details, "Section and force physical values have no visible property display.");
        QTRY_VERIFY_WITH_TIMEOUT(details->text().contains("100 mm²"), 10000);
        QVERIFY(details->text().contains("I1 = 833.333 mm⁴"));
        QVERIFY(details->text().contains("I2 = 833.333 mm⁴"));
        QVERIFY(details->text().contains("J = 1400 mm⁴"));
        QVERIFY(window->grab().save(evidence_dir + "/" + invocation + "-section-visible.png"));
        QVERIFY(select(force.value("entity_id").toString()));
        QTRY_VERIFY_WITH_TIMEOUT(selected->text().contains(force.value("entity_id").toString()),
                                 10000);
        QTRY_VERIFY_WITH_TIMEOUT(details->text().contains("(0, -1, 0) N"), 10000);
        QVERIFY(
            window->grab().save(evidence_dir + "/" + invocation + "-force-before-rotation.png"));
        QTRY_VERIFY_WITH_TIMEOUT(qcae::desktop_pipeline_idle(*window), 10000);
        const auto fingerprint_before = viewport->cameraFingerprint();
        const auto camera_before = camera_state();
        const auto start = widget->rect().center();
        QTest::mousePress(widget, Qt::LeftButton, Qt::NoModifier, start);
        for (int step = 1; step <= 12; ++step) {
            const auto position = start + QPoint(7 * step, 3 * step);
            QMouseEvent move(QEvent::MouseMove,
                             QPointF(position),
                             QPointF(widget->mapToGlobal(position)),
                             Qt::NoButton,
                             Qt::LeftButton,
                             Qt::NoModifier);
            QApplication::sendEvent(widget, &move);
            QTest::qWait(16);
        }
        QTest::mouseRelease(widget, Qt::LeftButton, Qt::NoModifier, start + QPoint(84, 36));
        QTRY_VERIFY_WITH_TIMEOUT(viewport->cameraFingerprint() != fingerprint_before, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(qcae::desktop_pipeline_idle(*window), 15000);
        const auto camera_after = camera_state();
        QVERIFY(camera_after != camera_before);
        const auto after = current(observer);
        QCOMPARE(after.value("expected_revision"), context.value("expected_revision"));
        QCOMPARE(entities(observer, "force"), forces);
        QCOMPARE(modelImage(observer), original_model);
        const auto history_after = data(call(observer, "history.list", {}, after));
        QCOMPARE(history_after, history_before);
        QCOMPARE(details->text().contains("(0, -1, 0) N"), true);
        QVERIFY(window->grab().save(evidence_dir + "/" + invocation + "-force-after-rotation.png"));
        QVERIFY(saveCheckEvidence(invocation,
                                  "actual-camera",
                                  {{"snapshot_sha256", snapshot_sha},
                                   {"before_fingerprint", fingerprint_before},
                                   {"after_fingerprint", viewport->cameraFingerprint()},
                                   {"before_camera", camera_before},
                                   {"after_camera", camera_after},
                                   {"context_before", context},
                                   {"context_after", after},
                                   {"force_before", force},
                                   {"force_after", forces.at(0)},
                                   {"history_before", history_before},
                                   {"history_after", history_after},
                                   {"property_visible", details->text()},
                                   {"backend", backend},
                                   {"model_unchanged", true},
                                   {"actual_gui_drag", true}}));
        source.seek(0);
        QCOMPARE(source.readAll(), original_bytes);
    }
    void aiSnapshotForcePreviewUndoAndRecoveryUseSharedHistory() {
        const auto snapshot = qEnvironmentVariable("QCAE_AI_UNITS_SNAPSHOT");
        if (snapshot.isEmpty())
            QSKIP("Set QCAE_AI_UNITS_SNAPSHOT to an actual AI-created .qcae snapshot.");
        QFile source(snapshot);
        QVERIFY(source.open(QIODevice::ReadOnly));
        const auto original_bytes = source.readAll();
        const auto snapshot_sha = QString::fromLatin1(
            QCryptographicHash::hash(original_bytes, QCryptographicHash::Sha256).toHex());
        const auto invocation = "AI-force-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
        QVERIFY(QDir().mkpath(evidence_dir));
        QTemporaryDir temp("/tmp/qcae-ai-force-gui-XXXXXX");
        QVERIFY(temp.isValid());
        const auto endpoint = temp.filePath("engine.sock"),
                   workspace = temp.filePath("work.sqlite");
        Engine engine;
        engine.process.setProgram(engine_path);
        engine.process.setArguments({"--socket", endpoint, "--workspace", workspace});
        engine.process.start();
        QVERIFY(engine.process.waitForStarted(5000));
        auto window = std::unique_ptr<QMainWindow>(
            qcae::create_desktop_window({endpoint, workspace, {}, false, 5000}));
        window->resize(1440, 900);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window.get()));
        auto* client = window->findChild<qcae::DesktopClient*>();
        QVERIFY(client);
        QTRY_VERIFY_WITH_TIMEOUT(client->ready(), 10000);
        qcae::DesktopClient observer({endpoint, workspace, {}, false, 5000});
        observer.start();
        QTRY_VERIFY_WITH_TIMEOUT(observer.ready(), 10000);
        auto* open = action(window.get(), "Open project…");
        QVERIFY(open);
        chooseFile(snapshot);
        open->trigger();
        auto* viewport = window->findChild<qcae::VtkView*>();
        auto* tree = window->findChild<QTreeWidget*>("entityTree");
        auto* properties = window->findChild<QDockWidget*>("selectionPropertiesDock");
        QVERIFY(viewport && tree && properties);
        QTRY_VERIFY_WITH_TIMEOUT(viewport->hasPacket(), 15000);
        QTRY_VERIFY_WITH_TIMEOUT(qcae::desktop_pipeline_idle(*window), 15000);
        const auto before = current(observer);
        const auto forces = entities(observer, "force");
        QCOMPARE(forces.size(), 1);
        const auto original_force = forces.at(0).toObject();
        const auto force_id = original_force.value("entity_id").toString();
        QCOMPARE(original_force.value("force_n").toArray(), QJsonArray({0, -1, 0}));
        const auto original_model = modelImage(observer);
        const auto original_history = data(call(observer, "history.list", {}, before));
        QVERIFY(original_history.value("items").isArray());
        properties->show();
        properties->raise();
        bool selected = false;
        for (int index = 0; index < tree->topLevelItemCount(); ++index) {
            auto* item = tree->topLevelItem(index);
            if (item->data(0, Qt::UserRole).toString() != force_id)
                continue;
            tree->scrollToItem(item);
            QTest::mouseClick(tree->viewport(),
                              Qt::LeftButton,
                              Qt::NoModifier,
                              tree->visualItemRect(item).center());
            selected = true;
            break;
        }
        QVERIFY(selected);
        const auto editors = properties->findChildren<QLineEdit*>();
        QCOMPARE(editors.size(), 4);
        QTRY_COMPARE_WITH_TIMEOUT(editors[1]->text(), QString("-1"), 10000);
        QCOMPARE(editors[0]->text(), QString("0"));
        QCOMPARE(editors[2]->text(), QString("0"));
        for (int index = 0; index < 3; ++index) {
            QVERIFY(editors[index]->isEnabled());
            const auto name = QString("property%1Unit").arg(QChar('X' + index));
            auto* label = properties->findChild<QLabel*>(name);
            QVERIFY(label);
            QCOMPARE(label->text(), QString("%1 (N)").arg(QChar('X' + index)));
        }
        QVERIFY(!editors[3]->isEnabled());
        auto* preview = properties->findChild<QPushButton*>("propertyPreview");
        auto* cancel = properties->findChild<QPushButton*>("propertyCancel");
        auto* apply = properties->findChild<QPushButton*>("propertyApply");
        QVERIFY(preview && cancel && apply);
        editors[1]->selectAll();
        QTest::keyClicks(editors[1], "-2");
        click(preview);
        QTRY_VERIFY_WITH_TIMEOUT(!window->property("propertyPreviewId").toString().isEmpty(),
                                 10000);
        QCOMPARE(current(observer), before);
        QCOMPARE(modelImage(observer), original_model);
        QCOMPARE(data(call(observer, "history.list", {}, before)), original_history);
        QVERIFY(window->grab().save(evidence_dir + "/" + invocation + "-preview-minus2N.png"));
        click(cancel);
        QVERIFY(window->property("propertyPreviewId").toString().isEmpty());
        QVERIFY(!cancel->isEnabled());
        QCOMPARE(current(observer), before);
        QCOMPARE(modelImage(observer), original_model);
        QCOMPARE(data(call(observer, "history.list", {}, before)), original_history);
        click(preview);
        QTRY_VERIFY_WITH_TIMEOUT(!window->property("propertyPreviewId").toString().isEmpty(),
                                 10000);
        click(apply);
        const auto revision_before = before.value("expected_revision").toString().toULongLong();
        QTRY_COMPARE_WITH_TIMEOUT(current(observer).value("expected_revision").toString(),
                                  QString::number(revision_before + 1),
                                  10000);
        QTRY_VERIFY_WITH_TIMEOUT(qcae::desktop_pipeline_idle(*window), 15000);
        const auto changed = current(observer);
        auto expected_model = original_model;
        auto expected_force = expected_model.value(force_id).toObject();
        auto expected_fields = expected_force.value("fields").toObject();
        expected_fields.insert("force_n", QJsonArray{0, -2, 0});
        expected_force.insert("fields", expected_fields);
        expected_model.insert(force_id, expected_force);
        QCOMPARE(modelImage(observer), expected_model);
        auto changed_force = original_force;
        changed_force.insert("force_n", QJsonArray{0, -2, 0});
        QCOMPARE(entities(observer, "force"), QJsonArray{changed_force});
        const auto changed_history = data(call(observer, "history.list", {}, changed));
        QCOMPARE(changed_history.value("items").toArray().size(),
                 original_history.value("items").toArray().size() + 1);
        QCOMPARE(changed_history.value("cursor").toInt(),
                 original_history.value("cursor").toInt() + 1);
        QVERIFY(window->property("propertyPreviewId").toString().isEmpty());
        // Revision admission clears the old property selection. Select another
        // tree row, then the same force through the normal user selection path.
        QVERIFY(tree->topLevelItemCount() > 1);
        auto* other_item = tree->topLevelItem(0);
        QVERIFY(other_item->data(0, Qt::UserRole).toString() != force_id);
        tree->scrollToItem(other_item);
        QTest::mouseClick(tree->viewport(),
                          Qt::LeftButton,
                          Qt::NoModifier,
                          tree->visualItemRect(other_item).center());
        auto* selected_label = properties->findChild<QLabel*>("selectedEntities");
        QVERIFY(selected_label);
        QTRY_VERIFY_WITH_TIMEOUT(
            selected_label->text().contains(other_item->data(0, Qt::UserRole).toString()), 10000);
        selected = false;
        for (int index = 0; index < tree->topLevelItemCount(); ++index) {
            auto* item = tree->topLevelItem(index);
            if (item->data(0, Qt::UserRole).toString() != force_id)
                continue;
            tree->scrollToItem(item);
            QTest::mouseClick(tree->viewport(),
                              Qt::LeftButton,
                              Qt::NoModifier,
                              tree->visualItemRect(item).center());
            selected = true;
            break;
        }
        QVERIFY(selected);
        QTRY_VERIFY_WITH_TIMEOUT(selected_label->text().contains(force_id), 10000);
        QTRY_COMPARE_WITH_TIMEOUT(editors[1]->text(), QString("-2"), 10000);
        QCOMPARE(editors[0]->text(), QString("0"));
        QCOMPARE(editors[2]->text(), QString("0"));
        auto* details = properties->findChild<QLabel*>("propertyDetails");
        QVERIFY(details);
        QTRY_VERIFY_WITH_TIMEOUT(details->text().contains("(0, -2, 0) N"), 10000);
        const auto changed_visible = details->text();
        QTRY_VERIFY_WITH_TIMEOUT(qcae::desktop_pipeline_idle(*window), 10000);
        QCOMPARE(current(observer), changed);
        QCOMPARE(data(call(observer, "history.list", {}, changed)), changed_history);
        QVERIFY(window->grab().save(evidence_dir + "/" + invocation + "-committed-minus2N.png"));
        auto* undo = action(window.get(), "Undo");
        QVERIFY(undo && undo->isEnabled());
        undo->trigger();
        QTRY_COMPARE_WITH_TIMEOUT(current(observer).value("expected_revision").toString(),
                                  QString::number(revision_before + 2),
                                  10000);
        QTRY_VERIFY_WITH_TIMEOUT(qcae::desktop_pipeline_idle(*window), 15000);
        const auto undone = current(observer);
        QCOMPARE(modelImage(observer), original_model);
        QCOMPARE(entities(observer, "force"), forces);
        const auto undone_history = data(call(observer, "history.list", {}, undone));
        auto undone_items = changed_history.value("items").toArray();
        auto undone_item = undone_items.last().toObject();
        QVERIFY(undone_item.value("applied").toBool());
        undone_item.insert("applied", false);
        undone_items.replace(undone_items.size() - 1, undone_item);
        QCOMPARE(undone_history.value("items"), QJsonValue(undone_items));
        QCOMPARE(undone_history.value("cursor"), original_history.value("cursor"));
        engine.process.kill();
        QVERIFY(engine.process.waitForFinished(5000));
        QTRY_VERIFY_WITH_TIMEOUT(!client->ready(), 10000);
        engine.process.start();
        QVERIFY(engine.process.waitForStarted(5000));
        QTRY_VERIFY_WITH_TIMEOUT(client->ready(), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(observer.ready(), 10000);
        auto* recover = action(window.get(), "Recover workspace");
        QVERIFY(recover);
        recover->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(
            current(observer).value("document_epoch") != undone.value("document_epoch"), 10000);
        const auto recovered = current(observer);
        QCOMPARE(recovered.value("document_id"), undone.value("document_id"));
        QCOMPARE(recovered.value("expected_revision"), undone.value("expected_revision"));
        QTRY_COMPARE_WITH_TIMEOUT(window->property("treeRevision").toString(),
                                  recovered.value("expected_revision").toString(),
                                  10000);
        QTRY_VERIFY_WITH_TIMEOUT(qcae::desktop_pipeline_idle(*window), 15000);
        QCOMPARE(modelImage(observer), original_model);
        QCOMPARE(entities(observer, "force"), forces);
        const auto recovered_history = data(call(observer, "history.list", {}, recovered));
        QCOMPARE(recovered_history, undone_history);
        QVERIFY(window->grab().save(evidence_dir + "/" + invocation + "-recovered-after-undo.png"));
        QVERIFY(saveCheckEvidence(invocation,
                                  "force-shared-history",
                                  {{"snapshot_sha256", snapshot_sha},
                                   {"before", before},
                                   {"changed", changed},
                                   {"undone", undone},
                                   {"recovered", recovered},
                                   {"force_before", original_force},
                                   {"force_changed", changed_force},
                                   {"force_changed_visible", changed_visible},
                                   {"force_recovered", forces.at(0)},
                                   {"history_before", original_history},
                                   {"history_changed", changed_history},
                                   {"history_undone", undone_history},
                                   {"history_recovered", recovered_history},
                                   {"model_before", original_model},
                                   {"model_changed", expected_model},
                                   {"model_recovered", modelImage(observer)},
                                   {"one_gui_transaction", true},
                                   {"preview_cancel_did_not_commit", true},
                                   {"actual_process_kill_and_gui_recover", true}}));
        source.seek(0);
        QCOMPARE(source.readAll(), original_bytes);
    }
    void completedAnalysisReportsRemainReadable_data() {
        QTest::addColumn<QString>("outcome");
        QTest::addColumn<bool>("stale");
        for (const auto& outcome : {QString("success"), QString("needs_input"), QString("failed")})
            for (const bool stale : {false, true})
                QTest::newRow(qPrintable(outcome + (stale ? "-stale-read" : "-current-read")))
                    << outcome << stale;
    }
    void completedAnalysisReportsRemainReadable() {
        QFETCH(QString, outcome);
        QFETCH(bool, stale);
        AnalysisReportEngine engine;
        QVERIFY(engine.listen());
        qcae::DesktopClient client({engine.endpoint(), {}, {}, false, 5000});
        client.start();
        QTRY_VERIFY(client.ready());
        qcae::AnalysisTools tools(client,
                                  {[] {},
                                   [](const auto&, const auto&) {},
                                   [](const auto&, const auto&) {},
                                   analysisRows});
        auto context = reportContext();
        tools.setContext(context);
        tools.setRowsContext(context);
        tools.findChild<QPushButton*>("analysisRefresh")->click();
        auto* check = tools.findChild<QPushButton*>("analysisCheck");
        QVERIFY(check->isEnabled());
        check->click();
        QTRY_COMPARE(engine.request.value("operation").toString(), QString("analysis.check"));
        auto report = checkReport(outcome);
        engine.reply(outcome, report);
        QTRY_COMPARE(tools.property("checkId").toString(), QString("check-1"));
        auto* issues = tools.findChild<QTreeWidget*>("analysisIssues");
        const auto count = report.value("issues").toArray().size();
        QCOMPARE(issues->topLevelItemCount(), count);
        if (count)
            QVERIFY(issues->topLevelItem(0)->text(0).endsWith("/1"));
        QCOMPARE(tools.property("checkState").toString(), QString("current"));
        if (stale) {
            context.insert("expected_revision", "6");
            tools.setContext(context);
            staleReport(report, "6");
            QCOMPARE(tools.property("checkState").toString(), QString("stale"));
        }
        auto* refresh = tools.findChild<QPushButton*>("analysisIssuesRefresh");
        QVERIFY(refresh->isEnabled());
        refresh->click();
        QTRY_COMPARE(engine.request.value("operation").toString(), QString("analysis.get_issues"));
        QCOMPARE(engine.request.value("parameters").toObject().value("check_id").toString(),
                 QString("check-1"));
        engine.reply("success", report);
        QTRY_VERIFY(refresh->isEnabled());
        QCOMPARE(issues->topLevelItemCount(), count);
        QCOMPARE(tools.property("checkState").toString(),
                 stale ? QString("stale") : QString("current"));
        QVERIFY(
            tools.findChild<QLabel*>("analysisCheckStatus")->text().contains("outcome " + outcome));
    }
    void malformedAnalysisReportsAreRejected_data() {
        QTest::addColumn<QString>("fault");
        QTest::addColumn<bool>("reading");
        for (const char* fault : {"rule-zero",
                                  "rule-number",
                                  "rule-leading-zero",
                                  "rule-overflow",
                                  "unknown-outcome",
                                  "status",
                                  "empty-issues",
                                  "non-object-issue",
                                  "duplicate-rule",
                                  "issue-state",
                                  "issue-version",
                                  "entity-type",
                                  "document",
                                  "current-epoch",
                                  "revision",
                                  "current-revision",
                                  "profile",
                                  "catalog",
                                  "check-id"})
            for (const bool reading : {false, true})
                QTest::newRow(qPrintable(QString(fault) + (reading ? "-read" : "-run")))
                    << QString(fault) << reading;
    }
    void malformedAnalysisReportsAreRejected() {
        QFETCH(QString, fault);
        QFETCH(bool, reading);
        AnalysisReportEngine engine;
        QVERIFY(engine.listen());
        qcae::DesktopClient client({engine.endpoint(), {}, {}, false, 5000});
        client.start();
        QTRY_VERIFY(client.ready());
        qcae::AnalysisTools tools(client,
                                  {[] {},
                                   [](const auto&, const auto&) {},
                                   [](const auto&, const auto&) {},
                                   analysisRows});
        const auto context = reportContext();
        tools.setContext(context);
        tools.setRowsContext(context);
        tools.findChild<QPushButton*>("analysisRefresh")->click();
        auto* check = tools.findChild<QPushButton*>("analysisCheck");
        check->click();
        QTRY_COMPARE(engine.request.value("operation").toString(), QString("analysis.check"));
        auto report = checkReport("needs_input");
        if (reading) {
            engine.reply("needs_input", report);
            QTRY_COMPARE(tools.property("checkId").toString(), QString("check-1"));
            tools.findChild<QPushButton*>("analysisIssuesRefresh")->click();
            QTRY_COMPARE(engine.request.value("operation").toString(),
                         QString("analysis.get_issues"));
        }
        auto status = reading ? QString("success") : QString("needs_input");
        auto issues = report.value("issues").toArray();
        auto issue = issues[0].toObject();
        auto input = report.value("input_version").toObject();
        auto current = report.value("current_version").toObject();
        if (fault == "rule-zero")
            issue.insert("rule_version", "0");
        else if (fault == "rule-number")
            issue.insert("rule_version", 1);
        else if (fault == "rule-leading-zero")
            issue.insert("rule_version", "01");
        else if (fault == "rule-overflow")
            issue.insert("rule_version", "4294967296");
        else if (fault == "unknown-outcome")
            report.insert("outcome", "conflict");
        else if (fault == "status")
            status = reading ? "needs_input" : "success";
        else if (fault == "empty-issues")
            issues = {};
        else if (fault == "non-object-issue")
            issues[0] = "invalid issue";
        else if (fault == "duplicate-rule")
            issues[1] = issue;
        else if (fault == "issue-state")
            issue.insert("state", "stale");
        else if (fault == "issue-version") {
            auto wrong = input;
            wrong.insert("revision", "4");
            issue.insert("input_version", wrong);
        } else if (fault == "entity-type")
            issue.insert("entity_type", "0");
        else if (fault == "document")
            input.insert("document_id", "different-document");
        else if (fault == "current-epoch")
            current.insert("document_epoch", "different-epoch");
        else if (fault == "revision")
            input.insert("revision", "05");
        else if (fault == "current-revision")
            current.insert("revision", "4");
        else if (fault == "profile")
            report.insert("profile", QJsonObject{});
        else if (fault == "catalog")
            report.insert("catalog_version", "qcae.analysis.rules.v1");
        else if (fault == "check-id")
            report.insert("check_id", reading ? "different-check" : "");
        if (!issues.isEmpty() && fault != "non-object-issue")
            issues[0] = issue;
        report.insert("issues", issues);
        report.insert("input_version", input);
        report.insert("current_version", current);
        engine.reply(status, report);
        QTRY_VERIFY(
            tools.findChild<QLabel*>("analysisCheckStatus")->text().startsWith("TEST_DIAGNOSTIC"));
        QCOMPARE(tools.findChild<QTreeWidget*>("analysisIssues")->topLevelItemCount(),
                 reading ? 2 : 0);
        QCOMPARE(tools.property("checkId").toString(), reading ? QString("check-1") : QString{});
    }
    void lateAnalysisReportsCannotCrossContext_data() {
        QTest::addColumn<QString>("change");
        QTest::addColumn<bool>("reading");
        for (const char* change : {"document", "epoch", "suspend", "destroy"})
            for (const bool reading : {false, true})
                QTest::newRow(qPrintable(QString(change) + (reading ? "-read" : "-run")))
                    << QString(change) << reading;
    }
    void lateAnalysisReportsCannotCrossContext() {
        QFETCH(QString, change);
        QFETCH(bool, reading);
        AnalysisReportEngine engine;
        QVERIFY(engine.listen());
        qcae::DesktopClient client({engine.endpoint(), {}, {}, false, 5000});
        client.start();
        QTRY_VERIFY(client.ready());
        int completed{};
        auto tools = std::make_unique<qcae::AnalysisTools>(
            client,
            qcae::AnalysisTools::Actions{[] {},
                                         [&](const auto&, const auto&) { ++completed; },
                                         [](const auto&, const auto&) {},
                                         analysisRows});
        auto context = reportContext();
        tools->setContext(context);
        tools->setRowsContext(context);
        tools->findChild<QPushButton*>("analysisRefresh")->click();
        tools->findChild<QPushButton*>("analysisCheck")->click();
        QTRY_COMPARE(engine.request.value("operation").toString(), QString("analysis.check"));
        const auto report = checkReport("needs_input");
        if (reading) {
            engine.reply("needs_input", report);
            QTRY_COMPARE(tools->property("checkId").toString(), QString("check-1"));
            tools->findChild<QPushButton*>("analysisIssuesRefresh")->click();
            QTRY_COMPARE(engine.request.value("operation").toString(),
                         QString("analysis.get_issues"));
        }
        if (change == "destroy")
            tools.reset();
        else if (change == "suspend")
            tools->suspend();
        else {
            context.insert(change == "document" ? "document_id" : "document_epoch", "new-context");
            tools->setContext(context);
        }
        engine.reply(reading ? "success" : "needs_input", report);
        QTRY_COMPARE(client.pendingRequests(), 0);
        QCoreApplication::sendPostedEvents();
        QCoreApplication::processEvents();
        QCOMPARE(completed, reading ? 1 : 0);
        if (tools && change != "suspend") {
            QVERIFY(tools->property("checkId").toString().isEmpty());
            QCOMPARE(tools->findChild<QTreeWidget*>("analysisIssues")->topLevelItemCount(), 0);
        }
    }
    void realFailedAnalysisReportRemainsReviewable() {
        QTemporaryDir temp("/tmp/qcae-failed-report-XXXXXX");
        QVERIFY(temp.isValid());
        Engine engine;
        const auto endpoint = temp.filePath("engine.sock");
        engine.process.setProgram(engine_path);
        engine.process.setArguments(
            {"--socket", endpoint, "--workspace", temp.filePath("work.sqlite")});
        engine.process.start();
        QVERIFY(engine.process.waitForStarted(5000));
        qcae::DesktopClient client({endpoint, {}, {}, false, 5000});
        client.start();
        QTRY_VERIFY_WITH_TIMEOUT(client.ready(), 10000);
        QCOMPARE(call(client,
                      "project.create",
                      {{"name", "Failed report fixture"}},
                      {{"idempotency_key", "failed-report-project"}})
                     .value("status")
                     .toString(),
                 QString("success"));
        const auto line =
            call(client,
                 "geometry.create_line",
                 {{"start_mm", QJsonArray{0, 0, 0}}, {"end_mm", QJsonArray{1000, 0, 0}}},
                 current(client, "failed-report-line"));
        QCOMPARE(line.value("status").toString(), QString("success"));
        const auto mesh = call(client,
                               "mesh.generate_line",
                               {{"geometry_id", data(line).value("entity_id")}, {"segments", 1}},
                               current(client, "failed-report-mesh"));
        QCOMPARE(mesh.value("status").toString(), QString("success"));
        QTRY_COMPARE_WITH_TIMEOUT(data(call(client,
                                            "task.status",
                                            {{"task_id", data(mesh).value("task_id")}},
                                            current(client)))
                                      .value("state")
                                      .toString(),
                                  QString("succeeded"),
                                  15000);
        const auto nodes = entities(client, "node");
        QCOMPARE(nodes.size(), 2);
        QJsonArray forces;
        for (int index = 0; index < 2; ++index) {
            const auto created =
                call(client,
                     "force.create",
                     {{"node_id", nodes[0].toObject().value("entity_id")},
                      {"x", QJsonObject{{"value", 0}, {"unit", "N"}}},
                      {"y", QJsonObject{{"value", -1}, {"unit", "N"}}},
                      {"z", QJsonObject{{"value", 0}, {"unit", "N"}}}},
                     current(client, QString("failed-report-force-%1").arg(index)));
            QCOMPARE(created.value("status").toString(), QString("success"));
            forces.append(data(created).value("entity_id"));
        }
        const auto load_case = call(client,
                                    "load_case.create",
                                    {{"name", "Wrong two-force scenario"},
                                     {"force_ids", forces},
                                     {"constraint_ids", QJsonArray{}}},
                                    current(client, "failed-report-case"));
        QCOMPARE(load_case.value("status").toString(), QString("success"));
        const auto profile_reply = call(client, "nastran.ui");
        QCOMPARE(profile_reply.value("status").toString(), QString("success"));
        QVERIFY(data(profile_reply).value("profile").isObject());
        auto create_context = current(client, "failed-report-analysis");
        create_context.insert("expected_profile", data(profile_reply).value("profile"));
        const auto created =
            call(client,
                 "analysis.create",
                 {{"name", "Wrong two-force scenario"},
                  {"load_case_ids", QJsonArray{data(load_case).value("entity_id")}}},
                 create_context);
        QVERIFY2(created.value("status") == "success", QJsonDocument(created).toJson().constData());
        const auto identity = data(created).value("entity_id").toString();
        const auto rows = entities(client, "analysis");
        QCOMPARE(rows.size(), 1);
        QJsonObject actual_check;
        qcae::AnalysisTools tools(client,
                                  {[] {},
                                   [&](const auto&, const auto& reply) { actual_check = reply; },
                                   [](const auto&, const auto&) {},
                                   [&] { return rows; }});
        const auto context = current(client);
        const auto before_history = data(call(client, "history.list", {}, context));
        tools.setContext(context);
        tools.setRowsContext(context);
        tools.findChild<QPushButton*>("analysisRefresh")->click();
        auto* check = tools.findChild<QPushButton*>("analysisCheck");
        QVERIFY(check->isEnabled());
        check->click();
        QTRY_VERIFY_WITH_TIMEOUT(!actual_check.isEmpty(), 10000);
        const auto invocation =
            "failed-report-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
        QVERIFY(saveCheckEvidence(invocation, "run", actual_check));
        QCOMPARE(actual_check.value("status").toString(), QString("failed"));
        QCOMPARE(data(actual_check).value("outcome").toString(), QString("failed"));
        QCOMPARE(data(actual_check).value("analysis_id").toString(), identity);
        QCOMPARE(data(actual_check).value("issues").toArray().size(), 3);
        auto* issues = tools.findChild<QTreeWidget*>("analysisIssues");
        QCOMPARE(issues->topLevelItemCount(), 3);
        QCOMPARE(issues->topLevelItem(0)->text(0), QString("cantilever-single-load/1"));
        QCOMPARE(tools.property("checkState").toString(), QString("current"));
        QCOMPARE(current(client), context);
        QCOMPARE(data(call(client, "history.list", {}, context)), before_history);
        auto* refresh = tools.findChild<QPushButton*>("analysisIssuesRefresh");
        refresh->click();
        QTRY_VERIFY(refresh->isEnabled());
        QCOMPARE(issues->topLevelItemCount(), 3);
        QVERIFY(tools.findChild<QLabel*>("analysisCheckStatus")->text().contains("outcome failed"));
        const auto later = call(client,
                                "load_case.create",
                                {{"name", "Unrelated later case"},
                                 {"force_ids", QJsonArray{}},
                                 {"constraint_ids", QJsonArray{}}},
                                current(client, "later-case"));
        QCOMPARE(later.value("status").toString(), QString("success"));
        const auto later_context = current(client);
        const auto later_history = data(call(client, "history.list", {}, later_context));
        tools.setContext(later_context);
        QCOMPARE(tools.property("checkState").toString(), QString("stale"));
        refresh->click();
        QTRY_VERIFY(refresh->isEnabled());
        QCOMPARE(issues->topLevelItemCount(), 3);
        QVERIFY(tools.findChild<QLabel*>("analysisCheckStatus")->text().contains("outcome failed"));
        const auto reread = call(client,
                                 "analysis.get_issues",
                                 {{"check_id", tools.property("checkId").toString()}},
                                 later_context);
        QVERIFY(saveCheckEvidence(invocation, "stale-read", reread));
        QCOMPARE(reread.value("status").toString(), QString("success"));
        QCOMPARE(data(reread).value("outcome").toString(), QString("failed"));
        QCOMPARE(data(reread).value("state").toString(), QString("stale"));
        QCOMPARE(data(reread).value("input_version"), data(actual_check).value("input_version"));
        QCOMPARE(current(client), later_context);
        QCOMPARE(data(call(client, "history.list", {}, later_context)), later_history);
    }
    void guiCreatesChecksRepairsAndPublishesM_data() {
        QTest::addColumn<int>("run");
        QTest::addColumn<bool>("mixed");
        for (int run = 0; run < 3; ++run)
            QTest::newRow(qPrintable(QString("GUI-M-%1").arg(run))) << run << false;
        for (int run = 0; run < 3; ++run)
            QTest::newRow(qPrintable(QString("MIX-M-%1").arg(run))) << run << true;
    }
    void guiCreatesChecksRepairsAndPublishesM() {
        QFETCH(int, run);
        QFETCH(bool, mixed);
        const auto mode_name = mixed ? QString("MIX") : QString("GUI");
        const auto invocation = QString("%1-M-%2-%3")
                                    .arg(mode_name)
                                    .arg(run)
                                    .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        QJsonArray external_writes;
        QJsonObject semantic_checkpoints;
        QTemporaryDir temp("/tmp/qcae-gui-M-XXXXXX");
        QVERIFY(temp.isValid());
        const auto endpoint = temp.filePath("engine.sock"),
                   workspace = temp.filePath("work.sqlite");
        Engine engine;
        engine.process.setProgram(engine_path);
        engine.process.setArguments({"--socket", endpoint, "--workspace", workspace});
        engine.process.start();
        QVERIFY(engine.process.waitForStarted(5000));
        std::unique_ptr<QMainWindow> window(
            qcae::create_desktop_window({endpoint, workspace, {}, false, 5000}));
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window.get()));
        auto* client = window->findChild<qcae::DesktopClient*>();
        QVERIFY(client);
        QTRY_VERIFY_WITH_TIMEOUT(client->ready(), 10000);
        // The observer only reads facts. The mixed rows invoke separate CLI and
        // thin-script processes; the GUI rows write through production controls.
        qcae::DesktopClient observer({endpoint, workspace, {}, false, 5000});
        observer.start();
        QTRY_VERIFY_WITH_TIMEOUT(observer.ready(), 10000);
        QTimer::singleShot(100, [] {
            if (auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) {
                dialog->setTextValue("M from GUI");
                dialog->accept();
            }
        });
        action(window.get(), "New project")->trigger();
        auto* tree = window->findChild<QTreeWidget*>("entityTree");
        auto* line_preview = window->findChild<QPushButton*>("modelingPreview");
        auto* line_apply = window->findChild<QPushButton*>("modelingApply");
        auto* line_cancel = window->findChild<QPushButton*>("modelingCancel");
        QTRY_VERIFY_WITH_TIMEOUT(line_preview->isEnabled(), 10000);
        QCOMPARE(modelImage(observer).size(), 0);
        click(line_preview);
        QTRY_VERIFY(line_apply->isEnabled());
        click(line_cancel);
        QCOMPARE(current(observer).value("expected_revision").toString(), QString("0"));
        click(line_preview);
        QTRY_VERIFY(line_apply->isEnabled());
        click(line_apply);
        QTRY_VERIFY_WITH_TIMEOUT(geometryItem(tree), 10000);
        const auto geometry_revision = current(observer).value("expected_revision").toString();
        QTRY_COMPARE_WITH_TIMEOUT(
            window->property("treeRevision").toString(), geometry_revision, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(qcae::desktop_pipeline_idle(*window), 10000);
        tree->setCurrentItem(geometryItem(tree));
        QTRY_VERIFY_WITH_TIMEOUT(!window->findChild<QLabel*>("meshGeometry")
                                      ->text()
                                      .startsWith("Select a geometry line"),
                                 10000);
        window->findChild<QComboBox*>("modelingMode")->setCurrentIndex(1);
        window->findChild<QSpinBox*>("meshSegments")->setValue(10);
        QTRY_VERIFY_WITH_TIMEOUT(line_apply->isEnabled(), 10000);
        click(line_apply);
        QTRY_COMPARE_WITH_TIMEOUT(entities(observer, "beam").size(), 10, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(
            window->findChild<QLabel*>("modelingStatus")->text().startsWith("Mesh created"), 10000);
        auto nodes = entities(observer, "node");
        std::vector<QJsonObject> sorted_nodes;
        for (const auto& node : nodes)
            sorted_nodes.push_back(node.toObject());
        std::sort(sorted_nodes.begin(), sorted_nodes.end(), [](const auto& a, const auto& b) {
            return a.value("position_mm").toArray()[0].toDouble() <
                   b.value("position_mm").toArray()[0].toDouble();
        });
        QCOMPARE(sorted_nodes.size(), std::size_t(11));
        for (std::size_t index = 0; index < sorted_nodes.size(); ++index)
            QCOMPARE(sorted_nodes[index].value("position_mm").toArray(),
                     QJsonArray({100.0 * index, 0, 0}));
        const auto first_node = sorted_nodes.front().value("entity_id").toString();
        const auto last_node = sorted_nodes.back().value("entity_id").toString();
        window->findChild<QAction*>("analysisToolsAction")->trigger();
        auto* tools = window->findChild<QWidget*>("analysisTools");
        auto* mode = tools->findChild<QComboBox*>("analysisMode");
        auto* refresh = tools->findChild<QPushButton*>("analysisRefresh");
        auto* apply = tools->findChild<QPushButton*>("analysisApply");
        const auto select_mode = [&](const QString& operation) {
            mode->setCurrentIndex(mode->findData(operation));
            QTRY_VERIFY_WITH_TIMEOUT(refresh->isEnabled(), 10000);
            click(refresh);
        };
        const auto write = [&] {
            tools->setProperty("lastStatus", QVariant{});
            QTRY_VERIFY_WITH_TIMEOUT(apply->isEnabled(), 10000);
            click(apply);
            QTRY_COMPARE_WITH_TIMEOUT(
                tools->property("lastStatus").toString(), QString("success"), 10000);
            const auto expected = current(observer).value("expected_revision").toString();
            QTRY_COMPARE_WITH_TIMEOUT(window->property("treeRevision").toString(), expected, 10000);
            QTRY_VERIFY_WITH_TIMEOUT(refresh->isEnabled(), 10000);
        };
        const auto select_all = [&](const QString& name) {
            auto* list = tools->findChild<QListWidget*>("analysisField_" + name);
            QVERIFY(list);
            for (int index = 0; index < list->count(); ++index)
                list->item(index)->setSelected(true);
        };
        const auto select_identity = [&](const QString& name, const QString& identity) {
            auto* list = tools->findChild<QListWidget*>("analysisField_" + name);
            QVERIFY(list);
            list->clearSelection();
            for (int index = 0; index < list->count(); ++index)
                if (list->item(index)->data(Qt::UserRole) == identity)
                    list->item(index)->setSelected(true);
            QCOMPARE(list->selectedItems().size(), 1);
        };
        select_mode("material.create");
        if (mixed) {
            const auto external =
                externalWrite(observer,
                              endpoint,
                              temp.filePath("cli-request.json"),
                              "material.create",
                              {{"name", "Steel"},
                               {"young_modulus", QJsonObject{{"value", 210}, {"unit", "GPa"}}},
                               {"poisson_ratio", .3}},
                              false);
            QCOMPARE(external.value("response").toObject().value("status").toString(),
                     QString("success"));
            external_writes.append(external);
            const auto material_id = external.value("response")
                                         .toObject()
                                         .value("data")
                                         .toObject()
                                         .value("entity_id")
                                         .toString();
            const auto section_external = externalWrite(observer,
                                                        endpoint,
                                                        temp.filePath("thin-section-request.json"),
                                                        "section.create",
                                                        {{"name", "M section"},
                                                         {"material_id", material_id},
                                                         {"area_mm2", 100},
                                                         {"i1_mm4", 833.333},
                                                         {"i2_mm4", 833.333},
                                                         {"torsion_mm4", 1400}},
                                                        true);
            QCOMPARE(section_external.value("response").toObject().value("status").toString(),
                     QString("success"));
            external_writes.append(section_external);
            const auto expected = current(observer).value("expected_revision").toString();
            QTRY_COMPARE_WITH_TIMEOUT(window->property("treeRevision").toString(), expected, 10000);
        } else
            write();
        if (!mixed) {
            select_mode("section.create");
            tools->findChild<QLineEdit*>("analysisField_name")->setText("M section");
            for (const auto& [name, value] : {std::pair{"area_mm2", 100.0},
                                              {"i1_mm4", 833.333},
                                              {"i2_mm4", 833.333},
                                              {"torsion_mm4", 1400.0}})
                tools->findChild<QDoubleSpinBox*>("analysisField_" + QString(name))
                    ->setValue(value);
            write();
        }
        select_mode("beam.assign_section");
        select_all("beam_ids");
        write();
        select_mode("part.upsert");
        tools->findChild<QLineEdit*>("analysisField_name")->setText("Beam");
        auto* members = tools->findChild<QListWidget*>("analysisField_members");
        QStringList beam_ids;
        for (const auto& row : entities(observer, "beam"))
            beam_ids.append(row.toObject().value("entity_id").toString());
        for (int index = 0; index < members->count(); ++index)
            if (beam_ids.contains(members->item(index)->data(Qt::UserRole).toString()))
                members->item(index)->setSelected(true);
        QCOMPARE(members->selectedItems().size(), 10);
        click(tools->findChild<QPushButton*>("analysisPreview"));
        QTRY_VERIFY(apply->isEnabled());
        const auto before_cancel = current(observer).value("expected_revision");
        click(tools->findChild<QPushButton*>("analysisCancel"));
        QVERIFY(!apply->isEnabled());
        QCOMPARE(current(observer).value("expected_revision"), before_cancel);
        click(tools->findChild<QPushButton*>("analysisPreview"));
        QTRY_VERIFY(apply->isEnabled());
        write();
        select_mode("assembly.upsert");
        select_all("children");
        click(tools->findChild<QPushButton*>("analysisPreview"));
        QTRY_VERIFY(apply->isEnabled());
        write();
        for (const auto& [name, identity] :
             {std::pair{QString("Fixed"), first_node}, {QString("Loaded"), last_node}}) {
            select_mode("set.upsert");
            tools->findChild<QLineEdit*>("analysisField_name")->setText(name);
            select_identity("members", identity);
            click(tools->findChild<QPushButton*>("analysisPreview"));
            QTRY_VERIFY(apply->isEnabled());
            write();
        }
        QCOMPARE(entities(observer, "part").size(), 1);
        QCOMPARE(entities(observer, "assembly").size(), 1);
        QCOMPARE(entities(observer, "set").size(), 2);
        QCOMPARE(entities(observer, "node").size(), 11);
        select_mode("load_case.create");
        write();
        select_mode("analysis.create");
        tools->findChild<QLineEdit*>("analysisField_name")->setText("M static");
        select_all("load_case_ids");
        write();
        click(refresh);
        auto* check = tools->findChild<QPushButton*>("analysisCheck");
        QTRY_VERIFY(check->isEnabled());
        click(check);
        auto* issues = tools->findChild<QTreeWidget*>("analysisIssues");
        QTRY_VERIFY2(!tools->property("checkId").toString().isEmpty(),
                     qPrintable(tools->findChild<QLabel*>("analysisCheckStatus")->text()));
        QTRY_COMPARE(issues->topLevelItemCount(), 2);
        const auto missing_check_id = tools->property("checkId").toString();
        const auto missing_report = call(
            observer, "analysis.get_issues", {{"check_id", missing_check_id}}, current(observer));
        QVERIFY(saveCheckEvidence(invocation, "missing-current", missing_report));
        QCOMPARE(missing_report.value("status").toString(), QString("success"));
        QCOMPARE(data(missing_report).value("outcome").toString(), QString("needs_input"));
        QCOMPARE(data(missing_report).value("state").toString(), QString("current"));
        QCOMPARE(data(missing_report).value("issues").toArray().size(), 2);
        for (int index = 0; index < 2; ++index) {
            const auto issue = data(missing_report).value("issues").toArray()[index].toObject();
            QCOMPARE(issue.value("rule_version").toString(), QString("1"));
            QCOMPARE(issues->topLevelItem(index)->text(0),
                     issue.value("rule_id").toString() + "/1");
        }
        auto* refresh_issues = tools->findChild<QPushButton*>("analysisIssuesRefresh");
        QVERIFY(refresh_issues->isEnabled());
        click(refresh_issues);
        QTRY_VERIFY(refresh_issues->isEnabled());
        QCOMPARE(issues->topLevelItemCount(), 2);
        QVERIFY(tools->findChild<QLabel*>("analysisCheckStatus")
                    ->text()
                    .contains("outcome needs_input"));
        const auto analysis_id =
            entities(observer, "analysis").at(0).toObject().value("entity_id").toString();
        issues->setCurrentItem(issues->topLevelItem(0));
        issues->scrollToItem(issues->topLevelItem(0));
        for (auto* parent = issues->parentWidget(); parent; parent = parent->parentWidget())
            if (auto* scroll = qobject_cast<QScrollArea*>(parent))
                scroll->ensureWidgetVisible(issues);
        click(tools->findChild<QPushButton*>("analysisLocateIssue"));
        QTRY_VERIFY_WITH_TIMEOUT(
            window->findChild<QLabel*>("selectedEntities")->text().contains(analysis_id), 10000);
        qInfo() << "GUI M issue navigation completed";
        select_mode("force.create");
        auto* force_node = tools->findChild<QComboBox*>("analysisField_node_id");
        force_node->setCurrentIndex(force_node->findData(last_node));
        tools->findChild<QDoubleSpinBox*>("analysisField_y")->setValue(-1);
        if (mixed) {
            const auto external = externalWrite(observer,
                                                endpoint,
                                                temp.filePath("thin-request.json"),
                                                "force.create",
                                                {{"node_id", last_node},
                                                 {"x", QJsonObject{{"value", 0}, {"unit", "N"}}},
                                                 {"y", QJsonObject{{"value", -1}, {"unit", "N"}}},
                                                 {"z", QJsonObject{{"value", 0}, {"unit", "N"}}}},
                                                true);
            QCOMPARE(external.value("response").toObject().value("status").toString(),
                     QString("success"));
            external_writes.append(external);
            const auto expected = current(observer).value("expected_revision").toString();
            QTRY_COMPARE_WITH_TIMEOUT(window->property("treeRevision").toString(), expected, 10000);
        } else
            write();
        QCOMPARE(tools->property("checkState").toString(), QString("stale"));
        click(refresh_issues);
        QTRY_VERIFY(refresh_issues->isEnabled());
        QCOMPARE(issues->topLevelItemCount(), 2);
        QCOMPARE(tools->property("checkId").toString(), missing_check_id);
        QCOMPARE(tools->property("checkState").toString(), QString("stale"));
        QVERIFY(tools->findChild<QLabel*>("analysisCheckStatus")
                    ->text()
                    .contains("outcome needs_input"));
        const auto stale_report = call(
            observer, "analysis.get_issues", {{"check_id", missing_check_id}}, current(observer));
        QVERIFY(saveCheckEvidence(invocation, "missing-stale", stale_report));
        QCOMPARE(stale_report.value("status").toString(), QString("success"));
        QCOMPARE(data(stale_report).value("outcome").toString(), QString("needs_input"));
        QCOMPARE(data(stale_report).value("state").toString(), QString("stale"));
        QCOMPARE(data(stale_report).value("input_version"),
                 data(missing_report).value("input_version"));
        for (const auto& issue : data(stale_report).value("issues").toArray())
            QCOMPARE(issue.toObject().value("state").toString(), QString("stale"));
        select_mode("constraint.create");
        select_identity("node_ids", first_node);
        write();
        click(tools->findChild<QPushButton*>("analysisRepair"));
        QTRY_VERIFY(refresh->isEnabled());
        click(refresh);
        select_all("force_ids");
        select_all("constraint_ids");
        write();
        click(refresh);
        const auto repaired = modelImage(observer);
        action(window.get(), "Undo")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(modelImage(observer) != repaired, 10000);
        action(window.get(), "Redo")->trigger();
        QTRY_COMPARE_WITH_TIMEOUT(modelImage(observer), repaired, 10000);
        const auto ready_revision = current(observer).value("expected_revision").toString();
        QTRY_COMPARE_WITH_TIMEOUT(
            window->property("treeRevision").toString(), ready_revision, 10000);
        QTRY_VERIFY(refresh->isEnabled());
        click(refresh);
        QTRY_VERIFY(check->isEnabled());
        click(check);
        QTRY_COMPARE(issues->topLevelItemCount(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(
            tools->property("checkState").toString(), QString("current"), 10000);
        qInfo() << "GUI M repaired, undo/redo and clean check completed";
        semantic_checkpoints.insert("M_baseline", modelRows(observer));
        const auto middle_node = sorted_nodes[5].value("entity_id").toString();
        for (int index = 0; index < tree->topLevelItemCount(); ++index)
            if (tree->topLevelItem(index)->data(0, Qt::UserRole) == middle_node)
                tree->setCurrentItem(tree->topLevelItem(index));
        QTRY_VERIFY_WITH_TIMEOUT(
            window->findChild<QLabel*>("selectedEntities")->text().contains(middle_node), 10000);
        auto* properties = window->findChild<QDockWidget*>("selectionPropertiesDock");
        properties->show();
        properties->raise();
        const auto node_fields = properties->findChildren<QLineEdit*>();
        QCOMPARE(node_fields.size(), 4);
        QTRY_VERIFY_WITH_TIMEOUT(node_fields[1]->isEnabled(), 10000);
        node_fields[1]->setText("1");
        click(properties->findChild<QPushButton*>("propertyPreview"));
        QTRY_VERIFY_WITH_TIMEOUT(!window->property("propertyPreviewId").toString().isEmpty(),
                                 10000);
        const auto before_property_cancel = current(observer).value("expected_revision");
        click(properties->findChild<QPushButton*>("propertyCancel"));
        QVERIFY(window->property("propertyPreviewId").toString().isEmpty());
        QCOMPARE(current(observer).value("expected_revision"), before_property_cancel);
        QCOMPARE(modelImage(observer), repaired);
        click(properties->findChild<QPushButton*>("propertyPreview"));
        QTRY_VERIFY_WITH_TIMEOUT(!window->property("propertyPreviewId").toString().isEmpty(),
                                 10000);
        click(properties->findChild<QPushButton*>("propertyApply"));
        QTRY_COMPARE_WITH_TIMEOUT(modelImage(observer)
                                      .value(middle_node)
                                      .toObject()
                                      .value("fields")
                                      .toObject()
                                      .value("position")
                                      .toArray(),
                                  QJsonArray({500, 1, 0}),
                                  10000);
        const auto selected_image = modelImage(observer);
        semantic_checkpoints.insert("selection_applied", modelRows(observer));
        action(window.get(), "Undo")->trigger();
        QTRY_COMPARE_WITH_TIMEOUT(modelImage(observer), repaired, 10000);
        semantic_checkpoints.insert("selection_undone", modelRows(observer));
        action(window.get(), "Redo")->trigger();
        QTRY_COMPARE_WITH_TIMEOUT(modelImage(observer), selected_image, 10000);
        action(window.get(), "Undo")->trigger();
        QTRY_COMPARE_WITH_TIMEOUT(modelImage(observer), repaired, 10000);
        window->findChild<QDockWidget*>("analysisDock")->raise();
        const auto after_selection = current(observer).value("expected_revision").toString();
        QTRY_COMPARE_WITH_TIMEOUT(
            window->property("treeRevision").toString(), after_selection, 10000);
        click(refresh);
        QTRY_VERIFY(check->isEnabled());
        click(check);
        QTRY_COMPARE(issues->topLevelItemCount(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(
            tools->property("checkState").toString(), QString("current"), 10000);
        semantic_checkpoints.insert("check_repaired", modelRows(observer));
        qInfo() << "GUI M property preview/cancel/apply/undo/redo completed";
        const auto save_path = temp.filePath("M.qcae"), save_as = temp.filePath("M-as.qcae");
        chooseFile(save_path);
        action(window.get(), "Save")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(save_path), 10000);
        qInfo() << "GUI M save completed";
        chooseFile(save_as);
        action(window.get(), "Save as…")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(save_as), 10000);
        QCOMPARE(modelImage(observer), repaired);
        qInfo() << "GUI M save-as completed";
        QTimer::singleShot(100, [] {
            if (auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
                for (auto* button : dialog->buttons())
                    if (button->text() == "Discard")
                        QTest::mouseClick(button, Qt::LeftButton);
        });
        action(window.get(), "Close project…")->trigger();
        QTRY_COMPARE(tree->topLevelItemCount(), 0);
        qInfo() << "GUI M close completed";
        chooseFile(save_as);
        action(window.get(), "Open project…")->trigger();
        QTRY_COMPARE_WITH_TIMEOUT(modelImage(observer), repaired, 10000);
        semantic_checkpoints.insert("normal_open_M", modelRows(observer));
        qInfo() << "GUI M normal open completed";
        engine.process.kill();
        QVERIFY(engine.process.waitForFinished(5000));
        QTRY_VERIFY_WITH_TIMEOUT(!client->ready(), 10000);
        engine.process.start();
        QVERIFY(engine.process.waitForStarted(5000));
        QTRY_VERIFY_WITH_TIMEOUT(client->ready(), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(observer.ready(), 10000);
        action(window.get(), "Recover workspace")->trigger();
        QTRY_COMPARE_WITH_TIMEOUT(modelImage(observer), repaired, 10000);
        const auto recovered_revision = current(observer).value("expected_revision").toString();
        QTRY_COMPARE_WITH_TIMEOUT(
            window->property("treeRevision").toString(), recovered_revision, 10000);
        semantic_checkpoints.insert("recovered_M", modelRows(observer));
        qInfo() << "GUI M process termination/recovery completed";
        window->findChild<QAction*>("nastranExportAction")->trigger();
        QTRY_VERIFY(window->findChild<QDialog*>("nastranExportForm"));
        auto* export_form = window->findChild<QDialog*>("nastranExportForm");
        export_form->findChild<QLineEdit*>("nastranExportDirectory")
            ->setText(temp.filePath("artifact"));
        click(export_form->findChild<QPushButton*>("nastranExportRun"));
        QTRY_VERIFY_WITH_TIMEOUT(!export_form->property("manifestPath").toString().isEmpty(),
                                 10000);
        qInfo() << "GUI M artifact publication completed";
        QFile manifest_file(export_form->property("manifestPath").toString());
        QVERIFY(manifest_file.open(QIODevice::ReadOnly));
        const auto manifest = QJsonDocument::fromJson(manifest_file.readAll()).object();
        QFile r_template(QString::fromStdString((std::filesystem::path(__FILE__).parent_path() /
                                                 "fixtures/results/R-displacement-v1.json")
                                                    .string()));
        QVERIFY(r_template.open(QIODevice::ReadOnly));
        auto r = QJsonDocument::fromJson(r_template.readAll()).object();
        r.insert("input_fingerprint", manifest.value("physical_signature_hex"));
        r.insert("identities", manifest.value("export_id_map"));
        QString first_number, last_number;
        for (const auto& mapping : manifest.value("export_id_map").toArray()) {
            const auto item = mapping.toObject();
            if (item.value("entity_id") == first_node)
                first_number = item.value("number").toString();
            if (item.value("entity_id") == last_node)
                last_number = item.value("number").toString();
        }
        r.insert("values",
                 QJsonArray{
                     QJsonObject{{"solver_number", first_number}, {"value", QJsonArray{0, 0, 0}}},
                     QJsonObject{{"solver_number", last_number}, {"value", QJsonArray{0, -1, 0}}}});
        QFile fixture_file(temp.filePath("R.json"));
        QVERIFY(fixture_file.open(QIODevice::WriteOnly));
        fixture_file.write(QJsonDocument(r).toJson());
        fixture_file.close();
        chooseFile(fixture_file.fileName());
        click(export_form->findChild<QPushButton*>("nastranReadFixture"));
        QTRY_COMPARE_WITH_TIMEOUT(
            export_form->property("resultState").toString(), QString("current"), 10000);
        auto* values = export_form->findChild<QTreeWidget*>("nastranResultValues");
        QCOMPARE(values->topLevelItemCount(), 2);
        QCOMPARE(values->topLevelItem(1)->text(2), QString("-1"));
        const auto result = export_form->property("resultValue").toJsonObject();
        QCOMPARE(result.value("source_kind").toString(), QString("fixture"));
        semantic_checkpoints.insert("published_M", modelRows(observer));
        export_form->hide();
        select_mode("material.create");
        const auto material_id =
            entities(observer, "material").at(0).toObject().value("entity_id").toString();
        for (int index = 0; index < tree->topLevelItemCount(); ++index)
            if (tree->topLevelItem(index)->data(0, Qt::UserRole) == material_id)
                tree->setCurrentItem(tree->topLevelItem(index));
        QTRY_VERIFY_WITH_TIMEOUT(
            window->findChild<QLabel*>("selectedEntities")->text().contains(material_id), 10000);
        const auto before_edit = current(observer).value("expected_revision").toString();
        properties->show();
        properties->raise();
        const auto property_fields = properties->findChildren<QLineEdit*>();
        QCOMPARE(property_fields.size(), 4);
        QTRY_VERIFY_WITH_TIMEOUT(property_fields.back()->isEnabled(), 10000);
        property_fields.back()->setText("200000");
        click(properties->findChild<QPushButton*>("propertyApply"));
        QTRY_VERIFY_WITH_TIMEOUT(
            current(observer).value("expected_revision").toString() != before_edit, 10000);
        export_form->show();
        click(export_form->findChild<QPushButton*>("nastranRefreshResult"));
        QTRY_COMPARE_WITH_TIMEOUT(
            export_form->property("resultState").toString(), QString("stale"), 10000);
        semantic_checkpoints.insert("stale_result_input_M", modelRows(observer));
        QDir().mkpath(evidence_dir);
        QFile evidence(evidence_dir + QString("/%1-M-run-%2.json").arg(mode_name).arg(run));
        QVERIFY(evidence.open(QIODevice::WriteOnly));
        evidence.write(
            QJsonDocument(
                QJsonObject{{"mode", mode_name},
                            {"run_id", invocation},
                            {"source_tree_sha256",
                             qEnvironmentVariable("QCAE_SOURCE_TREE_SHA256", "mutable-diagnostic")},
                            {"external_writes", external_writes},
                            {"observer_writes", 0},
                            {"missing_report_current", missing_report},
                            {"missing_report_stale", stale_report},
                            {"initial_entity_count", 0},
                            {"semantic_checkpoints", semantic_checkpoints},
                            {"M", repaired},
                            {"manifest", manifest},
                            {"R", r},
                            {"result_current", result},
                            {"result_stale", export_form->property("resultValue").toJsonObject()}})
                .toJson());
        QVERIFY(window->grab().save(evidence_dir +
                                    QString("/%1-M-run-%2.png").arg(mode_name).arg(run)));
    }
    void packageExportFormPublishesVerifiedFilesFromAPreloadedFixture() {
        QTemporaryDir temp("/tmp/qcae-export-ui-XXXXXX");
        QVERIFY(temp.isValid());
        const auto endpoint = temp.filePath("engine.sock"),
                   workspace = temp.filePath("work.sqlite");
        Engine engine;
        engine.process.setProgram(engine_path);
        engine.process.setArguments({"--socket", endpoint, "--workspace", workspace});
        engine.process.start();
        QVERIFY(engine.process.waitForStarted(5000));
        qcae::DesktopClient observer({endpoint, workspace, {}, false, 5000});
        observer.start();
        QTRY_VERIFY_WITH_TIMEOUT(observer.ready(), 10000);
        const auto capabilities = data(call(observer, "capabilities.list"));
        const auto profiles = capabilities.value("declared_solver_profiles").toArray();
        QVERIFY(!profiles.isEmpty());
        const auto profile = profiles.at(0).toObject().value("profile_ref").toObject();
        QCOMPARE(call(observer,
                      "project.create",
                      {{"name", "Export UI fixture"}},
                      {{"idempotency_key", "create-export-ui"}})
                     .value("status")
                     .toString(),
                 QString("success"));
        // IPC loads the fixed existing model. The assertion below exercises the actual
        // export action/form; this setup is not evidence of creating a model through GUI.
        QJsonArray resources;
        for (const auto* path :
             {"cantilever.bdf", "mesh/nodes.bdf", "mesh/beams.bdf", "properties.bdf"}) {
            const auto source = QString::fromStdString(
                (std::filesystem::path(__FILE__).parent_path() / "fixtures/nastran" / path)
                    .string());
            QFile file(source);
            QVERIFY(file.open(QIODevice::ReadOnly));
            resources.append(
                QJsonObject{{"path", path}, {"text", QString::fromUtf8(file.readAll())}});
        }
        const auto input = current(observer);
        const auto preview = call(observer,
                                  "changes.preview",
                                  {{"command", "model.import"},
                                   {"root_resource", "cantilever.bdf"},
                                   {"resources", resources},
                                   {"source_profile_ref", profile},
                                   {"unit_system", "mm-N-MPa"}},
                                  input);
        QCOMPARE(preview.value("status").toString(), QString("success"));
        auto commit_context = input;
        commit_context.insert("idempotency_key", "import-export-ui");
        QCOMPARE(call(observer,
                      "changes.commit",
                      {{"preview_id", data(preview).value("preview_id")}},
                      commit_context)
                     .value("status")
                     .toString(),
                 QString("success"));
        auto window = std::unique_ptr<QMainWindow>(
            qcae::create_desktop_window({endpoint, workspace, {}, false, 5000}));
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window.get()));
        const auto context = current(observer);
        QTRY_COMPARE(window->property("treeRevision").toString(),
                     context.value("expected_revision").toString());
        auto* export_action = window->findChild<QAction*>("nastranExportAction");
        QVERIFY(export_action);
        export_action->trigger();
        QTRY_VERIFY(window->findChild<QDialog*>("nastranExportForm"));
        auto* dialog = window->findChild<QDialog*>("nastranExportForm");
        auto* analysis = dialog->findChild<QComboBox*>("nastranExportAnalysis");
        auto* directory = dialog->findChild<QLineEdit*>("nastranExportDirectory");
        auto* run = dialog->findChild<QPushButton*>("nastranExportRun");
        QVERIFY(analysis && directory && run);
        QCOMPARE(analysis->count(), 1);
        QVERIFY(run->isEnabled());
        directory->setText(temp.filePath("delivered"));
        QTest::mouseClick(run, Qt::LeftButton);
        QTRY_COMPARE(dialog->property("taskState").toString(), QString("succeeded"));
        QTRY_VERIFY(!dialog->property("manifestPath").toString().isEmpty());
        QFile manifest_file(dialog->property("manifestPath").toString());
        QVERIFY(manifest_file.open(QIODevice::ReadOnly));
        const auto manifest = QJsonDocument::fromJson(manifest_file.readAll()).object();
        QVERIFY(manifest.value("complete").toBool());
        QCOMPARE(manifest.value("files").toArray().size(), 4);
        for (const auto& value : manifest.value("files").toArray()) {
            const auto descriptor = value.toObject();
            QFile file(QDir(directory->text()).filePath(descriptor.value("path").toString()));
            QVERIFY(file.open(QIODevice::ReadOnly));
            const auto bytes = file.readAll();
            QCOMPARE(QString::fromLatin1(
                         QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()),
                     descriptor.value("sha256").toString());
        }
        QCOMPARE(current(observer).value("expected_revision"), context.value("expected_revision"));
    }

    void layoutRoundTripDoesNotChangeDocument() {
        QTemporaryDir temp(QDir::tempPath().startsWith("/private/")
                               ? "/private/tmp/qc3-layout-XXXXXX"
                               : "/tmp/qc3-layout-XXXXXX");
        QVERIFY(temp.isValid());
        const auto socket = temp.filePath("engine.sock");
        Engine engine;
        engine.process.setProgram(engine_path);
        engine.process.setArguments(
            {"--socket", socket, "--workspace", temp.filePath("work.sqlite")});
        engine.process.start();
        QVERIFY(engine.process.waitForStarted(5000));
        qcae::DesktopClient observer({socket, temp.filePath("work.sqlite"), {}, false, 5000});
        observer.start();
        QTRY_VERIFY_WITH_TIMEOUT(observer.ready(), 10000);
        const auto created = call(observer,
                                  "project.create",
                                  {{"name", "C3 Layout"}},
                                  {{"idempotency_key", "create-c3-layout"}});
        QCOMPARE(created.value("status").toString(), QString("success"));
        const auto material = call(observer,
                                   "material.create",
                                   {{"name", "Layout steel"},
                                    {"young_modulus", QJsonObject{{"value", 210}, {"unit", "GPa"}}},
                                    {"poisson_ratio", 0.3}},
                                   current(observer, "material-c3-layout"));
        QCOMPARE(material.value("status").toString(), QString("success"));
        const auto before_context = current(observer);
        const auto before_history = data(call(observer, "history.list", {}, before_context));
        const auto before_revision = before_context.value("expected_revision");
        const auto before_document = before_context.value("document_id");
        const auto before_epoch = before_context.value("document_epoch");

        auto window = std::unique_ptr<QMainWindow>(
            qcae::create_desktop_window({socket, temp.filePath("work.sqlite"), {}, false, 5000}));
        QVERIFY(window);
        window->resize(1210, 790);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window.get()));
        const QStringList expected_docks{
            "modelViewsDock", "selectionPropertiesDock", "modelingDock", "historyDock"};
        const QStringList expected_toolbars{"workspaceToolbar", "selectionToolbar"};
        for (const auto& name : expected_docks)
            QVERIFY2(window->findChild<QDockWidget*>(name), qPrintable(name));
        for (const auto& name : expected_toolbars)
            QVERIFY2(window->findChild<QToolBar*>(name), qPrintable(name));

        auto* undo_action = window->findChild<QAction*>("historyUndoAction");
        auto* new_action = window->findChild<QAction*>("projectNewAction");
        QVERIFY(undo_action && new_action);
        const auto old_shortcut = undo_action->shortcut();
        window->findChild<QAction*>("shortcutPreferencesAction")->trigger();
        auto* shortcuts = window->findChild<QDialog*>("shortcutPreferencesForm");
        QVERIFY(shortcuts);
        auto* shortcut_action = shortcuts->findChild<QComboBox*>("shortcutAction");
        auto* sequence = shortcuts->findChild<QKeySequenceEdit*>("shortcutSequence");
        auto* shortcut_apply = shortcuts->findChild<QPushButton*>("shortcutApply");
        auto* shortcut_status = shortcuts->findChild<QLabel*>("shortcutStatus");
        shortcut_action->setCurrentIndex(shortcut_action->findData("historyUndoAction"));
        sequence->clear();
        sequence->setFocus();
        const auto duplicate = new_action->shortcut()[0];
        QTest::keyClick(sequence, duplicate.key(), duplicate.keyboardModifiers());
        QTest::qWait(1100);
        click(shortcut_apply);
        QVERIFY(shortcut_status->text().startsWith("Shortcut already belongs to"));
        QCOMPARE(undo_action->shortcut(), old_shortcut);
        sequence->clear();
        sequence->setFocus();
        QTest::keyClick(sequence, Qt::Key_U, Qt::ControlModifier);
        QTest::qWait(1100);
        click(shortcut_apply);
        const QKeySequence changed_shortcut(Qt::CTRL | Qt::Key_U);
        QCOMPARE(undo_action->shortcut(), changed_shortcut);
        QVERIFY(shortcut_status->text().startsWith("Saved shortcut for"));
        shortcuts->close();

        auto* views = window->findChild<QDockWidget*>("modelViewsDock");
        auto* selection = window->findChild<QDockWidget*>("selectionPropertiesDock");
        auto* modeling = window->findChild<QDockWidget*>("modelingDock");
        auto* history = window->findChild<QDockWidget*>("historyDock");
        auto* selection_toolbar = window->findChild<QToolBar*>("selectionToolbar");
        QVERIFY(views && selection && modeling && history && selection_toolbar);
        window->addDockWidget(Qt::LeftDockWidgetArea, views);
        window->addDockWidget(Qt::RightDockWidgetArea, selection);
        window->addDockWidget(Qt::RightDockWidgetArea, modeling);
        window->tabifyDockWidget(selection, modeling);
        selection->raise();
        window->addDockWidget(Qt::BottomDockWidgetArea, history);
        history->show();
        window->resizeDocks({views, selection}, {280, 360}, Qt::Horizontal);
        window->resizeDocks({history}, {170}, Qt::Vertical);
        QTest::qWait(50);
        qInfo() << "BP17 requested panel sizes" << views->width() << selection->width()
                << history->height() << "minimum selection width"
                << selection->minimumSizeHint().width();
        QTRY_VERIFY_WITH_TIMEOUT(views->width() >= 276 && views->width() <= 284, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(selection->width() >= 356 && selection->width() <= 364, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(history->height() >= 166 && history->height() <= 174, 5000);
        const QJsonObject panel_golden{{"modelViewsDock_width", 280},
                                       {"selectionPropertiesDock_width", 360},
                                       {"historyDock_height", 170},
                                       {"logical_pixel_tolerance", 4}};
        history->hide();
        window->addToolBar(Qt::BottomToolBarArea, selection_toolbar);
        selection_toolbar->hide();
        QTest::qWait(50);

        const auto saved_state = window->saveState(1);
        const auto saved_geometry = window->saveGeometry();
        QVERIFY(!saved_state.isEmpty());
        QVERIFY(!saved_geometry.isEmpty());
        QVERIFY(history->isHidden());
        QVERIFY(selection_toolbar->isHidden());
        const auto views_area = window->dockWidgetArea(views);
        const auto selection_area = window->dockWidgetArea(selection);
        const auto modeling_area = window->dockWidgetArea(modeling);
        const auto history_area = window->dockWidgetArea(history);
        const auto toolbar_area = window->toolBarArea(selection_toolbar);
        QVERIFY(window->close());
        window.reset();

        QSettings saved_settings(
            QSettings::defaultFormat(), QSettings::UserScope, "QCAE", "Desktop");
        saved_settings.sync();
        QCOMPARE(saved_settings.format(), QSettings::IniFormat);
        QVERIFY(saved_settings.fileName().startsWith(QDir::tempPath().startsWith("/private/")
                                                         ? "/private/tmp/qc3-settings-"
                                                         : "/tmp/qc3-settings-"));
        QCOMPARE(saved_settings.value("layout/state").toByteArray(), saved_state);
        QCOMPARE(saved_settings.value("layout/geometry").toByteArray(), saved_geometry);

        auto restored = std::unique_ptr<QMainWindow>(
            qcae::create_desktop_window({socket, temp.filePath("work.sqlite"), {}, false, 5000}));
        QVERIFY(restored);
        restored->show();
        QVERIFY(QTest::qWaitForWindowExposed(restored.get()));
        auto* restored_views = restored->findChild<QDockWidget*>("modelViewsDock");
        auto* restored_selection = restored->findChild<QDockWidget*>("selectionPropertiesDock");
        auto* restored_modeling = restored->findChild<QDockWidget*>("modelingDock");
        auto* restored_history = restored->findChild<QDockWidget*>("historyDock");
        auto* restored_toolbar = restored->findChild<QToolBar*>("selectionToolbar");
        QVERIFY(restored_views && restored_selection && restored_modeling && restored_history &&
                restored_toolbar);
        QCOMPARE(restored->dockWidgetArea(restored_views), views_area);
        QCOMPARE(restored->dockWidgetArea(restored_selection), selection_area);
        QCOMPARE(restored->dockWidgetArea(restored_modeling), modeling_area);
        QCOMPARE(restored->dockWidgetArea(restored_history), history_area);
        QCOMPARE(restored->toolBarArea(restored_toolbar), toolbar_area);
        QVERIFY(restored_history->isHidden());
        QVERIFY(restored_toolbar->isHidden());
        QVERIFY(restored->tabifiedDockWidgets(restored_selection).contains(restored_modeling));
        QCOMPARE(restored->saveState(1), saved_state);
        QCOMPARE(restored->saveGeometry(), saved_geometry);
        restored_history->show();
        QTRY_VERIFY_WITH_TIMEOUT(restored_views->width() >= 276 && restored_views->width() <= 284,
                                 5000);
        QTRY_VERIFY_WITH_TIMEOUT(
            restored_selection->width() >= 356 && restored_selection->width() <= 364, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(
            restored_history->height() >= 166 && restored_history->height() <= 174, 5000);
        const QJsonObject restored_panels{
            {"modelViewsDock_width", restored_views->width()},
            {"selectionPropertiesDock_width", restored_selection->width()},
            {"historyDock_height", restored_history->height()}};
        restored_history->hide();
        QTest::qWait(50);
        QCOMPARE(restored->saveState(1), saved_state);
        QCOMPARE(restored->findChild<QAction*>("historyUndoAction")->shortcut(), changed_shortcut);

        const auto after_context = current(observer);
        QCOMPARE(after_context.value("document_id"), before_document);
        QCOMPARE(after_context.value("document_epoch"), before_epoch);
        QCOMPARE(after_context.value("expected_revision"), before_revision);
        const auto after_history = data(call(observer, "history.list", {}, after_context));
        QCOMPARE(after_history.value("revision"), before_history.value("revision"));
        QCOMPARE(after_history.value("cursor"), before_history.value("cursor"));
        QCOMPARE(after_history.value("items"), before_history.value("items"));
        QDir().mkpath(evidence_dir);
        QFile layout_evidence(evidence_dir + "/BP17-layout.json");
        QVERIFY(layout_evidence.open(QIODevice::WriteOnly));
        layout_evidence.write(
            QJsonDocument(
                QJsonObject{
                    {"panel_golden", panel_golden},
                    {"restored_panels", restored_panels},
                    {"serialized_state_golden", QString::fromLatin1(saved_state.toBase64())},
                    {"serialized_state_restored",
                     QString::fromLatin1(restored->saveState(1).toBase64())},
                    {"shortcut_action", "historyUndoAction"},
                    {"shortcut", changed_shortcut.toString(QKeySequence::PortableText)},
                    {"document_revision_increment", 0}})
                .toJson());
        restored.reset();
    }

    void realEngineModelingAndHistory() {
        QTemporaryDir temp(QDir::tempPath().startsWith("/private/") ? "/private/tmp/qc3-ui-XXXXXX"
                                                                    : "/tmp/qc3-ui-XXXXXX");
        QVERIFY(temp.isValid());
        const auto socket = temp.filePath("engine.sock");
        Engine engine;
        engine.process.setProgram(engine_path);
        engine.process.setArguments(
            {"--socket", socket, "--workspace", temp.filePath("work.sqlite")});
        engine.process.start();
        QVERIFY(engine.process.waitForStarted(5000));
        qcae::DesktopClient observer({socket, temp.filePath("work.sqlite"), {}, false, 5000});
        observer.start();
        QTRY_VERIFY_WITH_TIMEOUT(observer.ready(), 10000);
        const auto created = call(observer,
                                  "project.create",
                                  {{"name", "C3 Modeling"}},
                                  {{"idempotency_key", "create-c3-ui"}});
        QCOMPARE(created.value("status").toString(), QString("success"));
        std::unique_ptr<QMainWindow> window(
            qcae::create_desktop_window({socket, temp.filePath("work.sqlite"), {}, false, 5000}));
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window.get()));
        auto* preview = window->findChild<QPushButton*>("modelingPreview");
        auto* apply = window->findChild<QPushButton*>("modelingApply");
        auto* cancel = window->findChild<QPushButton*>("modelingCancel");
        auto* status = window->findChild<QLabel*>("modelingStatus");
        auto* mode = window->findChild<QComboBox*>("modelingMode");
        auto* segments = window->findChild<QSpinBox*>("meshSegments");
        auto* tree = window->findChild<QTreeWidget*>("entityTree");
        QVERIFY(preview && apply && cancel && status && mode && segments && tree);
        QTRY_VERIFY(preview->isEnabled());
        QTest::mouseClick(preview, Qt::LeftButton);
        QVERIFY(apply->isEnabled());
        QCOMPARE(entities(observer, "geometry").size(), 0);
        QTest::mouseClick(cancel, Qt::LeftButton);
        QVERIFY(!apply->isEnabled());
        QCOMPARE(entities(observer, "geometry").size(), 0);
        QCOMPARE(data(call(observer, "project.current")).value("revision").toString(),
                 QString("0"));
        QTest::mouseClick(preview, Qt::LeftButton);
        QDir().mkpath(evidence_dir);
        QTest::qWait(150);
        QVERIFY(window->grab().save(evidence_dir + "/line-preview.png"));
        QTest::mouseClick(apply, Qt::LeftButton);
        QTest::mouseClick(apply, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(entities(observer, "geometry").size(), 1, 10000);
        const auto geometry = entities(observer, "geometry").at(0).toObject().value("entity_id");
        QCOMPARE(data(call(observer, "history.list", {}, current(observer)))
                     .value("items")
                     .toArray()
                     .size(),
                 1);
        QTRY_VERIFY(geometryItem(tree));
        tree->setCurrentItem(geometryItem(tree));
        mode->setCurrentIndex(1);
        segments->setValue(10);
        QTRY_VERIFY(apply->isEnabled());
        QTest::mouseClick(apply, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(entities(observer, "beam").size(), 10, 10000);
        QCOMPARE(entities(observer, "node").size(), 11);
        QCOMPARE(entities(observer, "geometry").at(0).toObject().value("entity_id"), geometry);
        QTRY_VERIFY_WITH_TIMEOUT(status->text().startsWith("Mesh created"), 10000);
        QTest::qWait(700);
        QVERIFY(window->grab().save(evidence_dir + "/line-mesh.png"));
        auto* undo = action(window.get(), "Undo");
        auto* redo = action(window.get(), "Redo");
        QVERIFY(undo && redo);
        const auto beams = entities(observer, "beam");
        undo->trigger();
        QTRY_COMPARE(entities(observer, "beam").size(), 0);
        QCOMPARE(entities(observer, "node").size(), 0);
        QCOMPARE(entities(observer, "geometry").at(0).toObject().value("entity_id"), geometry);
        QTest::qWait(600);
        redo->trigger();
        QTRY_COMPARE(entities(observer, "beam"), beams);
        const auto saved = call(observer,
                                "project.save",
                                {{"path", temp.filePath("model.qcae")}},
                                current(observer, "save-c3-ui"));
        QCOMPARE(saved.value("status").toString(), QString("success"));
        const auto closed = call(
            observer, "project.close", {{"policy", "discard"}}, current(observer, "close-c3-ui"));
        QCOMPARE(closed.value("status").toString(), QString("success"));
        QTRY_COMPARE(tree->topLevelItemCount(), 0);
        QVERIFY(!apply->isEnabled());
        const auto opened = call(observer,
                                 "project.open",
                                 {{"mode", "normal"}, {"path", temp.filePath("model.qcae")}},
                                 {{"idempotency_key", "open-c3-ui"}});
        QCOMPARE(opened.value("status").toString(), QString("success"));
        QVERIFY(data(opened).value("document_id") != data(created).value("document_id"));
        QCOMPARE(entities(observer, "beam"), beams);
        QTRY_VERIFY(geometryItem(tree));
        QCOMPARE(geometryItem(tree)->data(0, Qt::UserRole).toString(), geometry.toString());
        QTest::qWait(700);
        QVERIFY(window->grab().save(evidence_dir + "/saved-reopened.png"));
        window.reset();
    }
};
} // namespace
int main(int argc, char** argv) {
    if (argc < 3)
        return 2;
    engine_path = QString::fromLocal8Bit(argv[1]);
    evidence_dir = QString::fromLocal8Bit(argv[2]);
    QTemporaryDir settings_temp(QDir::tempPath().startsWith("/private/")
                                    ? "/private/tmp/qc3-settings-XXXXXX"
                                    : "/tmp/qc3-settings-XXXXXX");
    if (!settings_temp.isValid())
        return 3;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_temp.path());
    QSettings::setPath(
        QSettings::IniFormat, QSettings::SystemScope, settings_temp.filePath("system"));
    QSurfaceFormat::setDefaultFormat(QVTKOpenGLNativeWidget::defaultFormat());
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    Workflow tests;
    QStringList test_arguments{QString::fromLocal8Bit(argv[0])};
    for (int index = 3; index < argc; ++index)
        test_arguments.append(QString::fromLocal8Bit(argv[index]));
    return QTest::qExec(&tests, test_arguments);
}
#include "c3_desktop_workflow_tests.moc"
