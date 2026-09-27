#include "qcae/desktop.hpp"
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QEventLoop>
#include <QJsonArray>
#include <QLabel>
#include <QMainWindow>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QSpinBox>
#include <QSurfaceFormat>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTreeWidget>
#include <QVTKOpenGLNativeWidget.h>
#include <memory>

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
class Workflow : public QObject {
    Q_OBJECT
  private slots:
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
    if (argc != 3)
        return 2;
    engine_path = QString::fromLocal8Bit(argv[1]);
    evidence_dir = QString::fromLocal8Bit(argv[2]);
    QSurfaceFormat::setDefaultFormat(QVTKOpenGLNativeWidget::defaultFormat());
    QApplication app(argc, argv);
    Workflow tests;
    return QTest::qExec(&tests, 1, argv);
}
#include "c3_desktop_workflow_tests.moc"
