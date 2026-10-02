#include "qcae/desktop.hpp"
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QDoubleSpinBox>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QMainWindow>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QSurfaceFormat>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTreeWidget>
#include <QUuid>
#include <QVTKOpenGLNativeWidget.h>
#include <memory>
#include <set>

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
    if (reply->isEmpty()) {
        QTimer::singleShot(10000, &loop, &QEventLoop::quit);
        loop.exec();
    }
    return *reply;
}
QJsonObject data(const QJsonObject& reply) {
    return reply.value("data").toObject();
}
struct Observation {
    bool valid{};
    quint64 revision{};
    QJsonObject context, history;
    QJsonArray entities;
};
Observation observe(qcae::DesktopClient& client) {
    Observation result;
    const auto current = call(client, "project.current");
    if (current.value("status") != "success")
        return result;
    const auto info = data(current);
    bool revision_valid = false;
    result.revision = info.value("revision").toString().toULongLong(&revision_valid);
    result.context = {{"document_id", info.value("document_id")},
                      {"document_epoch", info.value("document_epoch")},
                      {"expected_revision", info.value("revision")}};
    const auto entities = call(client, "entity.query", {{"limit", 1000}}, result.context);
    const auto history = call(client, "history.list", {}, result.context);
    result.entities = data(entities).value("entities").toArray();
    result.history = data(history);
    result.valid = revision_valid && entities.value("status") == "success" &&
                   history.value("status") == "success" &&
                   data(entities).value("revision") == info.value("revision") &&
                   data(entities).value("total").toInt(-1) == result.entities.size() &&
                   result.history.value("revision") == info.value("revision");
    return result;
}
QAction* action(QMainWindow& window, const QString& title) {
    for (auto* candidate : window.findChildren<QAction*>())
        if (candidate->text() == title)
            return candidate;
    return nullptr;
}
QStringList treeIds(const QTreeWidget& tree) {
    QStringList ids;
    for (int i = 0; i < tree.topLevelItemCount(); ++i) {
        const auto id = tree.topLevelItem(i)->data(0, Qt::UserRole).toString();
        if (!id.isEmpty())
            ids.append(id);
    }
    return ids;
}
bool saveEvidence(const QJsonObject& evidence) {
    if (!QDir().mkpath(evidence_dir))
        return false;
    QSaveFile output(evidence_dir + "/tool-lifecycle.json");
    if (!output.open(QIODevice::WriteOnly))
        return false;
    const auto bytes = QJsonDocument(evidence).toJson(QJsonDocument::Indented);
    return output.write(bytes) == bytes.size() && output.commit();
}
class ToolLifecycle : public QObject {
    Q_OBJECT
  private slots:
    void thirtyRealEngineLineToolRuns() {
        QFile fixture_file(QFINDTESTDATA("fixtures/c3-tool-lifecycle-v1.json"));
        QVERIFY(fixture_file.open(QIODevice::ReadOnly));
        const auto fixture_bytes = fixture_file.readAll();
        const auto fixture_hash =
            QCryptographicHash::hash(fixture_bytes, QCryptographicHash::Sha256).toHex();
        QCOMPARE(fixture_hash,
                 QByteArray("3f67b5096cc0527aca95177d71ef32006f762778c1a823cfeb55a7f27a72415a"));
        const auto fixture = QJsonDocument::fromJson(fixture_bytes).object();
        QCOMPARE(fixture.value("samples").toInt(), 30);
        QTemporaryDir temp(QDir("/tmp").canonicalPath() + "/qc3-tools-XXXXXX");
        QVERIFY(temp.isValid());
        const auto socket = temp.filePath("engine.sock");
        const auto workspace = temp.filePath("work.sqlite");
        Engine engine;
        engine.process.setProgram(engine_path);
        engine.process.setArguments({"--socket", socket, "--workspace", workspace});
        engine.process.start();
        QVERIFY(engine.process.waitForStarted(5000));
        qcae::DesktopClient observer({socket, workspace, {}, false, 5000});
        observer.start();
        QTRY_VERIFY_WITH_TIMEOUT(observer.ready(), 10000);
        const auto capabilities = call(observer, "capabilities.list");
        QCOMPARE(capabilities.value("status").toString(), QString("success"));
        QCOMPARE(data(capabilities).value("durable").toBool(), true);
        QCOMPARE(data(capabilities).value("storage_mode").toString(), QString("sqlite"));
        const auto created = call(observer,
                                  "project.create",
                                  {{"name", "C3 frozen tool lifecycle"}},
                                  {{"idempotency_key", "c3-tool-lifecycle-project"}});
        QCOMPARE(created.value("status").toString(), QString("success"));
        const auto baseline = observe(observer);
        QVERIFY(baseline.valid);
        QCOMPARE(baseline.revision, quint64(0));
        QCOMPARE(baseline.entities, fixture.value("final_entities").toArray());
        QCOMPARE(baseline.history.value("items").toArray().size(), 0);

        std::unique_ptr<QMainWindow> window(
            qcae::create_desktop_window({socket, workspace, {}, false, 5000}));
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window.get()));
        auto* preview = window->findChild<QPushButton*>("modelingPreview");
        auto* apply = window->findChild<QPushButton*>("modelingApply");
        auto* cancel = window->findChild<QPushButton*>("modelingCancel");
        auto* status = window->findChild<QLabel*>("modelingStatus");
        auto* mode = window->findChild<QComboBox*>("modelingMode");
        auto* tree = window->findChild<QTreeWidget*>("entityTree");
        auto* undo = action(*window, "Undo");
        QVERIFY(preview && apply && cancel && status && mode && tree && undo);
        mode->setCurrentText(fixture.value("mode").toString());
        const auto coordinates = fixture.value("coordinates_mm").toObject();
        for (auto it = coordinates.begin(); it != coordinates.end(); ++it) {
            auto* field = window->findChild<QDoubleSpinBox*>(it.key());
            QVERIFY(field);
            field->setValue(it.value().toDouble());
        }

        const auto session_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QJsonArray runs;
        QJsonObject evidence{{"script", fixture},
                             {"script_sha256", QString::fromLatin1(fixture_hash)},
                             {"session_id", session_id},
                             {"backend", "real qcae-engine / SQLite / Qt desktop / VTK"},
                             {"qt_version", QT_VERSION_STR},
                             {"document_id", baseline.context.value("document_id")},
                             {"document_epoch", baseline.context.value("document_epoch")},
                             {"runs", runs}};
        QVERIFY(saveEvidence(evidence));
        std::set<QString> transactions, geometry_ids;
        for (int sample = 0; sample < fixture.value("samples").toInt(); ++sample) {
            const auto run_id = session_id + QString("-%1").arg(sample + 1, 3, 10, QChar('0'));
            QTRY_VERIFY_WITH_TIMEOUT(preview->isEnabled(), 10000);
            QTRY_VERIFY_WITH_TIMEOUT(treeIds(*tree).isEmpty(), 10000);
            const auto before = observe(observer);
            QVERIFY(before.valid);
            QCOMPARE(before.revision, quint64(sample * 2));
            QCOMPARE(before.entities, baseline.entities);
            QCOMPARE(before.context.value("document_id"), baseline.context.value("document_id"));
            QCOMPARE(before.context.value("document_epoch"),
                     baseline.context.value("document_epoch"));
            QTest::mouseClick(preview, Qt::LeftButton);
            QVERIFY(apply->isEnabled());
            QVERIFY(status->text().startsWith("Line preview"));
            QTest::mouseClick(cancel, Qt::LeftButton);
            QVERIFY(!apply->isEnabled());
            QVERIFY(status->text().startsWith("Preview cancelled"));
            const auto cancelled = observe(observer);
            QVERIFY(cancelled.valid);
            QCOMPARE(cancelled.revision - before.revision,
                     quint64(fixture.value("cancel_revision_delta").toInt()));
            QCOMPARE(cancelled.history, before.history);
            QCOMPARE(cancelled.entities, before.entities);

            QTest::mouseClick(preview, Qt::LeftButton);
            QVERIFY(apply->isEnabled());
            // Both clicks occur before returning to the event loop for an engine response.
            apply->click();
            apply->click();
            QTRY_COMPARE_WITH_TIMEOUT(status->text(), QString("Line created"), 10000);
            QTRY_COMPARE_WITH_TIMEOUT(treeIds(*tree).size(), 1, 10000);
            const auto applied = observe(observer);
            QVERIFY(applied.valid);
            QCOMPARE(applied.revision - before.revision,
                     quint64(fixture.value("apply_revision_delta").toInt()));
            QCOMPARE(applied.entities.size(), 1);
            const auto geometry = applied.entities.at(0).toObject();
            QCOMPARE(geometry.value("kind").toString(), QString("geometry"));
            QCOMPARE(geometry.value("start_mm").toArray(), QJsonArray({10, 20, 30}));
            QCOMPARE(geometry.value("end_mm").toArray(), QJsonArray({1010, 20, 30}));
            const auto entity_id = geometry.value("entity_id").toString();
            QVERIFY(!entity_id.isEmpty());
            QVERIFY(geometry_ids.insert(entity_id).second);
            QCOMPARE(treeIds(*tree), QStringList({entity_id}));
            const auto history_items = applied.history.value("items").toArray();
            QCOMPARE(history_items.size(), fixture.value("commits_per_sample").toInt());
            QCOMPARE(applied.history.value("cursor").toInt(), 1);
            QCOMPARE(history_items.at(0).toObject().value("applied").toBool(), true);
            const auto transaction =
                history_items.at(0).toObject().value("transaction_id").toString();
            QVERIFY(!transaction.isEmpty());
            QVERIFY(transactions.insert(transaction).second);
            QVERIFY(!apply->isEnabled());

            undo->trigger();
            QTRY_VERIFY_WITH_TIMEOUT(treeIds(*tree).isEmpty(), 10000);
            const auto undone = observe(observer);
            QVERIFY(undone.valid);
            QCOMPARE(undone.revision - before.revision,
                     quint64(fixture.value("sample_revision_delta").toInt()));
            QCOMPARE(undone.entities, baseline.entities);
            QCOMPARE(undone.history.value("cursor").toInt(), 0);
            QCOMPARE(undone.history.value("items").toArray().size(), 1);
            QCOMPARE(
                undone.history.value("items").toArray()[0].toObject().value("applied").toBool(),
                false);
            runs.append(QJsonObject{{"run_id", run_id},
                                    {"start_revision", QString::number(before.revision)},
                                    {"cancel_revision", QString::number(cancelled.revision)},
                                    {"apply_revision", QString::number(applied.revision)},
                                    {"end_revision", QString::number(undone.revision)},
                                    {"cancel_revision_delta", 0},
                                    {"cancel_history_unchanged", true},
                                    {"geometry_commit_count", 1},
                                    {"transaction_id", transaction},
                                    {"geometry_id", entity_id},
                                    {"final_entities", undone.entities},
                                    {"final_matches_golden", true},
                                    {"passed", true}});
            evidence.insert("runs", runs);
            evidence.insert("passed_samples", runs.size());
            evidence.insert("final_revision", QString::number(undone.revision));
            QVERIFY(saveEvidence(evidence));
            qInfo().noquote() << run_id << "cancel_delta=0 commits=1 empty_golden=true revisions="
                              << before.revision << "->" << undone.revision;
        }
        QCOMPARE(runs.size(), 30);
        QCOMPARE(transactions.size(), std::size_t(30));
        window.reset();
    }
};
} // namespace
int main(int argc, char** argv) {
    if (argc != 3)
        return 2;
    engine_path = QString::fromLocal8Bit(argv[1]);
    evidence_dir = QString::fromLocal8Bit(argv[2]);
    QSurfaceFormat::setDefaultFormat(QVTKOpenGLNativeWidget::defaultFormat());
    QApplication app(argc, argv);
    QTemporaryDir settings;
    if (!settings.isValid())
        return 2;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settings.filePath("system"));
    ToolLifecycle tests;
    return QTest::qExec(&tests, 1, argv);
}
#include "c3_tool_lifecycle_tests.moc"
