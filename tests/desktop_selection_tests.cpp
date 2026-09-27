#include "qcae/desktop.hpp"
#include "qcae/vtk_view.hpp"
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMainWindow>
#include <QPointer>
#include <QPushButton>
#include <QSurfaceFormat>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTreeWidget>
#include <QVTKOpenGLNativeWidget.h>
#include <algorithm>
#include <memory>
#include <vtkRenderWindow.h>
#include <vtkRenderer.h>
#include <vtkRendererCollection.h>

namespace {
// Real local wire and a rendered desktop, with selected replies held to expose races.
class SelectionEngine {
  public:
    SelectionEngine() : directory_(QDir("/tmp").canonicalPath() + "/qcae-selection-XXXXXX") {
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
                    requests_.append(request);
                    dispatch(request);
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
        return directory_.filePath("work.sqlite");
    }
    void respond(const QJsonObject& request, const QJsonObject& data) {
        peer_->write(QJsonDocument(QJsonObject{{"request_id", request.value("request_id")},
                                               {"status", "success"},
                                               {"data", data}})
                         .toJson(QJsonDocument::Compact) +
                     '\n');
    }
    QList<QJsonObject> requests(const QString& operation) const {
        QList<QJsonObject> result;
        for (const auto& request : requests_)
            if (request.value("operation") == operation)
                result.append(request);
        return result;
    }
    void releaseRender() {
        hold_render = false;
        for (const auto& request : held_renders)
            respond(request, renderData(request));
        held_renders.clear();
    }
    void releaseViewUpdates() {
        hold_update = false;
        for (const auto& request : held_updates)
            respondView(request);
        held_updates.clear();
    }
    QString revision{"7"};
    bool hold_render{};
    bool hold_update{};
    QList<QJsonObject> held_renders;
    QList<QJsonObject> held_updates;

  private:
    QJsonObject line(const QString& id) const {
        const double z = id == "line-b" ? 25.0 : revision == "7" ? 0.0 : 15.0;
        return {{"entity_id", id},
                {"kind", "geometry"},
                {"name", id},
                {"start_mm", QJsonArray{0, 0, z}},
                {"end_mm", QJsonArray{100, 0, z}}};
    }
    QJsonObject renderData(const QJsonObject& request) const {
        QJsonArray lines;
        for (const auto* id : {"line-a", "line-b"})
            if (!hidden_.contains(id))
                lines.append(line(id));
        const auto parameters = request.value("parameters").toObject();
        return {{"document_id", request.value("document_id")},
                {"document_epoch", request.value("document_epoch")},
                {"revision", request.value("expected_revision")},
                {"view_session_id", parameters.value("view_session_id")},
                {"view_revision", parameters.value("expected_view_revision")},
                {"points", QJsonArray{}},
                {"beams", QJsonArray{}},
                {"geometry_lines", lines}};
    }
    void respondView(const QJsonObject& request) {
        if (request.value("operation") == "view.create")
            view_revision_ = 1;
        else
            ++view_revision_;
        hidden_ = request.value("parameters").toObject().value("hidden_ids").toArray();
        respond(request,
                {{"view_session_id", "view"}, {"view_revision", QString::number(view_revision_)}});
    }
    void dispatch(const QJsonObject& request) {
        const auto operation = request.value("operation").toString();
        const auto parameters = request.value("parameters").toObject();
        if (operation == "runtime.handshake")
            respond(request, {{"api_version", "1.1"}});
        else if (operation == "test.barrier")
            respond(request, {});
        else if (operation == "project.current")
            respond(request,
                    {{"document_id", "document"},
                     {"document_epoch", "epoch"},
                     {"revision", revision},
                     {"name", "Selection test"},
                     {"dirty", false}});
        else if (operation == "entity.query") {
            QJsonArray rows;
            const auto ids = parameters.value("ids").toArray();
            for (const auto* id : {"line-a", "line-b"})
                if (ids.isEmpty() || ids.contains(id))
                    rows.append(line(id));
            respond(request, {{"entities", rows}, {"total", rows.size()}});
        } else if (operation == "history.list")
            respond(request, {{"items", QJsonArray{}}});
        else if (operation == "view.create" || operation == "view.update") {
            if (operation == "view.update" && hold_update)
                held_updates.append(request);
            else
                respondView(request);
        } else if (operation == "view.render_data") {
            if (hold_render)
                held_renders.append(request);
            else
                respond(request, renderData(request));
        }
        // selection.evaluate/get are answered explicitly by the test.
    }
    QTemporaryDir directory_;
    QLocalServer server_;
    QPointer<QLocalSocket> peer_;
    QByteArray incoming_;
    QList<QJsonObject> requests_;
    QJsonArray hidden_;
    std::uint64_t view_revision_{};
};

struct Window {
    explicit Window(const SelectionEngine& engine)
        : window(qcae::create_desktop_window(
              {engine.endpoint(), engine.workspace(), {}, false, 5000})),
          viewport(window->findChild<qcae::VtkView*>()),
          widget(window->findChild<QVTKOpenGLNativeWidget*>()),
          selected(window->findChild<QLabel*>("selectedEntities")),
          client(window->findChild<qcae::DesktopClient*>()) {
        window->show();
    }
    bool valid() const {
        return viewport && widget && selected && client;
    }
    QPoint position(double z) const {
        auto* renderer = widget->renderWindow()->GetRenderers()->GetFirstRenderer();
        renderer->SetWorldPoint(50, 0, z, 1);
        renderer->WorldToDisplay();
        const auto* display = renderer->GetDisplayPoint();
        const auto scale = widget->devicePixelRatioF();
        return {qRound(display[0] / scale), qRound(widget->height() - display[1] / scale)};
    }
    void click(double z) const {
        QTest::mouseClick(widget, Qt::LeftButton, Qt::NoModifier, position(z));
    }
    void orient() const {
        viewport->standardView(qcae::VtkView::StandardView::front);
        viewport->fit();
        QTest::qWait(500); // Let the desktop's camera sync timers finish before racing replies.
    }
    std::unique_ptr<QMainWindow> window;
    qcae::VtkView* viewport;
    QVTKOpenGLNativeWidget* widget;
    QLabel* selected;
    qcae::DesktopClient* client;
};

bool barrier(qcae::DesktopClient& client) {
    QEventLoop loop;
    QPointer<QEventLoop> guard(&loop);
    auto reached = std::make_shared<bool>(false);
    (void)client.request("test.barrier", {}, {}, [guard, reached](const auto&) {
        *reached = true;
        if (guard)
            guard->quit();
    });
    QTimer::singleShot(5000, &loop, &QEventLoop::quit);
    loop.exec();
    return *reached;
}

QAction* action(QMainWindow* window, const QString& title) {
    for (auto* item : window->findChildren<QAction*>())
        if (item->text() == title)
            return item;
    return nullptr;
}

class DesktopSelectionTests : public QObject {
    Q_OBJECT
  private slots:
    void latestTreeSelectionSurvivesViewRefreshAndEnablesMeshTools() {
        SelectionEngine engine;
        QVERIFY(engine.listen());
        Window desktop(engine);
        QVERIFY(desktop.valid());
        QVERIFY(QTest::qWaitForWindowExposed(desktop.window.get()));
        QTRY_VERIFY(desktop.viewport->hasPacket());
        desktop.orient();
        auto* tree = desktop.window->findChild<QTreeWidget*>("entityTree");
        auto* mode = desktop.window->findChild<QComboBox*>("modelingMode");
        auto* apply = desktop.window->findChild<QPushButton*>("modelingApply");
        auto* show_all = action(desktop.window.get(), "Show all");
        QVERIFY(tree && mode && apply && show_all);
        QTRY_COMPARE(tree->topLevelItemCount(), 2);
        mode->setCurrentIndex(1);

        desktop.click(0);
        QTRY_COMPARE(engine.requests("selection.evaluate").size(), 1);
        engine.respond(engine.requests("selection.evaluate").front(),
                       {{"selection_handle", "before-refresh"}});
        QTRY_COMPARE(engine.requests("selection.get").size(), 1);
        const auto older_get = engine.requests("selection.get").front();
        engine.hold_update = engine.hold_render = true;
        show_all->trigger();
        QTRY_VERIFY(!engine.held_updates.isEmpty());
        engine.respond(older_get, {{"entity_ids", QJsonArray{"line-a"}}});
        QVERIFY(barrier(*desktop.client));
        QCOMPARE(desktop.selected->text(), QString("No selection"));

        tree->setCurrentItem(tree->topLevelItem(0));
        tree->setCurrentItem(tree->topLevelItem(1));
        QVERIFY(barrier(*desktop.client));
        QCOMPARE(engine.requests("selection.evaluate").size(), 1);
        QVERIFY(!apply->isEnabled());
        engine.releaseViewUpdates();
        QTRY_VERIFY(!engine.held_renders.isEmpty());
        QCOMPARE(engine.requests("selection.evaluate").size(), 1);
        engine.releaseRender();
        QTRY_COMPARE(engine.requests("selection.evaluate").size(), 2);
        QCOMPARE(engine.requests("selection.evaluate")
                     .back()
                     .value("parameters")
                     .toObject()
                     .value("scope")
                     .toObject()
                     .value("candidate_ids")
                     .toArray(),
                 QJsonArray({"line-b"}));
        engine.respond(engine.requests("selection.evaluate").back(),
                       {{"selection_handle", "after-refresh"}});
        QTRY_COMPARE(engine.requests("selection.get").size(), 2);
        engine.respond(engine.requests("selection.get").back(),
                       {{"entity_ids", QJsonArray{"line-b"}}});
        QTRY_COMPARE(desktop.selected->text(), QString("line-b"));
        QTRY_VERIFY(apply->isEnabled());
        QCOMPARE(tree->currentItem()->data(0, Qt::UserRole).toString(), QString("line-b"));

        // Once resolved, later view refreshes do not replay the completed intent.
        engine.hold_render = true;
        show_all->trigger();
        QTRY_VERIFY(!engine.held_renders.isEmpty());
        engine.releaseRender();
        QVERIFY(barrier(*desktop.client));
        QCOMPARE(engine.requests("selection.evaluate").size(), 2);
        QVERIFY(apply->isEnabled());
    }

    void oldPacketCannotSubmitPicksDuringModelOrViewRefresh() {
        SelectionEngine engine;
        QVERIFY(engine.listen());
        Window desktop(engine);
        QVERIFY(desktop.valid());
        QVERIFY(QTest::qWaitForWindowExposed(desktop.window.get()));
        QTRY_VERIFY(desktop.viewport->hasPacket());
        desktop.orient();
        desktop.click(0);
        QTRY_COMPARE(engine.requests("selection.evaluate").size(), 1);
        engine.respond(engine.requests("selection.evaluate").front(), {{"selection_handle", "a"}});
        QTRY_COMPARE(engine.requests("selection.get").size(), 1);
        engine.respond(engine.requests("selection.get").front(),
                       {{"entity_ids", QJsonArray{"line-a"}}});
        QTRY_COMPARE(desktop.selected->text(), QString("line-a"));

        engine.hold_render = true;
        engine.revision = "8";
        QTRY_VERIFY(!engine.held_renders.isEmpty());
        QCOMPARE(engine.held_renders.back().value("expected_revision").toString(), QString("8"));
        QCOMPARE(desktop.selected->text(), QString("No selection"));
        desktop.click(0); // The old line is still drawn until the new packet arrives.
        QVERIFY(barrier(*desktop.client));
        QCOMPARE(engine.requests("selection.evaluate").size(), 1);
        engine.releaseRender();
        QVERIFY(barrier(*desktop.client));
        desktop.click(15);
        QTRY_COMPARE(engine.requests("selection.evaluate").size(), 2);
        engine.respond(engine.requests("selection.evaluate").back(), {{"selection_handle", "a8"}});
        QTRY_COMPARE(engine.requests("selection.get").size(), 2);
        engine.respond(engine.requests("selection.get").back(),
                       {{"entity_ids", QJsonArray{"line-a"}}});
        QTRY_COMPARE(desktop.selected->text(), QString("line-a"));

        engine.hold_render = true;
        auto* hide = action(desktop.window.get(), "Hide selected");
        QVERIFY(hide);
        hide->trigger();
        QTRY_VERIFY(!engine.held_renders.isEmpty());
        desktop.click(15); // Model revision matches, but the old view packet must also be fenced.
        QVERIFY(barrier(*desktop.client));
        QCOMPARE(engine.requests("selection.evaluate").size(), 2);
        engine.releaseRender();
        QVERIFY(barrier(*desktop.client));
        desktop.click(25);
        QTRY_COMPARE(engine.requests("selection.evaluate").size(), 3);
        QCOMPARE(engine.requests("selection.evaluate")
                     .back()
                     .value("parameters")
                     .toObject()
                     .value("scope")
                     .toObject()
                     .value("candidate_ids")
                     .toArray(),
                 QJsonArray({"line-b"}));
    }

    void newerSelectionWinsWhenEvaluateAndGetRepliesAreReordered() {
        SelectionEngine engine;
        QVERIFY(engine.listen());
        Window desktop(engine);
        QVERIFY(desktop.valid());
        QVERIFY(QTest::qWaitForWindowExposed(desktop.window.get()));
        QTRY_VERIFY(desktop.viewport->hasPacket());
        desktop.orient();
        desktop.click(0);
        desktop.click(25);
        QTRY_COMPARE(engine.requests("selection.evaluate").size(), 2);
        engine.respond(engine.requests("selection.evaluate").back(),
                       {{"selection_handle", "new-evaluation"}});
        QTRY_COMPARE(engine.requests("selection.get").size(), 1);
        engine.respond(engine.requests("selection.get").back(),
                       {{"entity_ids", QJsonArray{"line-b"}}});
        QTRY_COMPARE(desktop.selected->text(), QString("line-b"));
        engine.respond(engine.requests("selection.evaluate").front(),
                       {{"selection_handle", "old-evaluation"}});
        QVERIFY(barrier(*desktop.client));
        QCOMPARE(engine.requests("selection.get").size(), 1);
        QCOMPARE(desktop.selected->text(), QString("line-b"));

        desktop.click(0);
        QTRY_COMPARE(engine.requests("selection.evaluate").size(), 3);
        engine.respond(engine.requests("selection.evaluate").back(),
                       {{"selection_handle", "old-get"}});
        QTRY_COMPARE(engine.requests("selection.get").size(), 2);
        const auto older_get = engine.requests("selection.get").back();
        desktop.click(25);
        QTRY_COMPARE(engine.requests("selection.evaluate").size(), 4);
        engine.respond(engine.requests("selection.evaluate").back(),
                       {{"selection_handle", "new-get"}});
        QTRY_COMPARE(engine.requests("selection.get").size(), 3);
        engine.respond(engine.requests("selection.get").back(),
                       {{"entity_ids", QJsonArray{"line-b"}}});
        QVERIFY(barrier(*desktop.client));
        engine.respond(older_get, {{"entity_ids", QJsonArray{"line-a"}}});
        QVERIFY(barrier(*desktop.client));
        QCOMPARE(desktop.selected->text(), QString("line-b"));
    }
};
} // namespace

int main(int argc, char** argv) {
    QSurfaceFormat::setDefaultFormat(QVTKOpenGLNativeWidget::defaultFormat());
    QApplication app(argc, argv);
    DesktopSelectionTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "desktop_selection_tests.moc"
