#include "qcae/desktop.hpp"
#include "qcae/vtk_view.hpp"
#include "qcae/render_wire.hpp"
#include <QCryptographicHash>
#include <QAction>
#include <QApplication>
#include <QSettings>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
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
            if (resources_enabled)
                respondResource(request);
            else
                respond(request, renderData(request));
        held_renders.clear();
    }
    void releaseViewUpdates() {
        hold_update = false;
        for (const auto& request : held_updates)
            respondView(request);
        held_updates.clear();
    }
    void releaseCurrent() {
        hold_current = false;
        const auto pending = held_current;
        held_current.clear();
        for (const auto& request : pending)
            dispatch(request);
    }
    void notifyChange(bool gap = false) {
        if (gap) {
            event_sequence += 2;
            peer_->write(QJsonDocument(QJsonObject{{"frame_type", "event_gap"},
                                                   {"engine_instance_id", "fake-engine"},
                                                   {"sequence", QString::number(event_sequence)},
                                                   {"resync_required", true},
                                                   {"reason", "test retention gap"}})
                             .toJson(QJsonDocument::Compact) +
                         '\n');
        } else {
            ++event_sequence;
            peer_->write(QJsonDocument(QJsonObject{{"frame_type", "event"},
                                                   {"engine_instance_id", "fake-engine"},
                                                   {"sequence", QString::number(event_sequence)},
                                                   {"event", "DocumentChanged"},
                                                   {"document_id", document_id},
                                                   {"document_epoch", epoch},
                                                   {"revision", revision},
                                                   {"data", QJsonObject{}}})
                             .toJson(QJsonDocument::Compact) +
                         '\n');
        }
    }
    void respondResource(const QJsonObject& request) {
        const auto params = request.value("parameters").toObject();
        const auto view_revision = params.value("expected_view_revision").toString().toULongLong();
        const auto rev = request.value("expected_revision").toString().toULongLong();
        const qcae::DocumentRef doc{
            qcae::DocumentId(request.value("document_id").toString().toStdString()),
            qcae::DocumentEpoch(request.value("document_epoch").toString().toStdString())};
        const auto resource_view = params.value("view_session_id").toString();
        const bool delta = params.contains("base_revision");
        const auto resource = "resource-" + QString::number(++resource_sequence);
        const auto a = qcae::RenderGeometryLine{qcae::EntityId("line-a"),
                                                {0, 0, rev == 7 ? 0.0 : 15.0},
                                                {100, 0, rev == 7 ? 0.0 : 15.0}};
        QByteArray bytes;
        if (delta) {
            qcae::RenderDelta update{doc,
                                     params.value("base_revision").toString().toULongLong(),
                                     rev,
                                     resource_view.toStdString(),
                                     params.value("base_view_revision").toString().toULongLong(),
                                     view_revision,
                                     {},
                                     {}};
            if (update.revision != update.base_revision)
                update.geometry_lines.push_back({0, a});
            bytes = qcae::transport::encode_render_delta(update);
        } else {
            qcae::RenderPacket packet;
            packet.document = doc;
            packet.revision = rev;
            packet.view_session_id = resource_view.toStdString();
            packet.view_revision = view_revision;
            packet.geometry_lines = {a, {qcae::EntityId("line-b"), {0, 0, 25}, {100, 0, 25}}};
            bytes = qcae::transport::encode_render_packet(packet);
        }
        const QJsonObject manifest{
            {"resource_id", resource},
            {"document_id", request.value("document_id")},
            {"document_epoch", request.value("document_epoch")},
            {"revision", QString::number(rev)},
            {"view_session_id", resource_view},
            {"view_revision", QString::number(view_revision)},
            {"byte_length", QString::number(bytes.size())},
            {"chunk_bytes", 131072},
            {"chunk_count", 1},
            {"sha256",
             QString::fromLatin1(
                 QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())},
            {"media_type", delta ? "qcae.render.delta.v1" : "qcae.render.packet.v1"}};
        resource_bytes[resource] = bytes;
        resource_manifests[resource] = manifest;
        respond(request,
                {{"mode", delta ? "delta" : "full"},
                 {"manifest", manifest},
                 {"refresh_tree", !delta},
                 {"changed_ids", QJsonArray{"line-a"}}});
    }
    bool resources_enabled{}, drop_next_manifest{}, drop_next_subscription{}, drop_next_update{};
    quint64 event_sequence{}, resource_sequence{};
    QMap<QString, QByteArray> resource_bytes;
    QMap<QString, QJsonObject> resource_manifests;
    QString revision{"7"};
    QString document_id{"document"}, epoch{"epoch"}, view_id{"view"};
    bool hold_render{};
    bool hold_update{};
    bool hold_current{};
    QList<QJsonObject> held_renders;
    QList<QJsonObject> held_updates;
    QList<QJsonObject> held_current;

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
        if (resources_enabled && request.value("operation") == "view.update" &&
            request.value("parameters")
                    .toObject()
                    .value("expected_view_revision")
                    .toString()
                    .toULongLong() != view_revision_) {
            peer_->write(
                QJsonDocument(QJsonObject{{"request_id", request.value("request_id")},
                                          {"status", "conflict"},
                                          {"error",
                                           QJsonObject{{"code", "REVISION_CONFLICT"},
                                                       {"message", "View revision changed"}}}})
                    .toJson(QJsonDocument::Compact) +
                '\n');
            return;
        }
        if (request.value("operation") == "view.create")
            view_revision_ = 1;
        else
            ++view_revision_;
        hidden_ = request.value("parameters").toObject().value("hidden_ids").toArray();
        if (request.value("operation") == "view.update" && drop_next_update) {
            drop_next_update = false;
            return; // Server updated its disposable view but the acknowledgement was lost.
        }
        respond(request,
                {{"view_session_id", view_id}, {"view_revision", QString::number(view_revision_)}});
    }
    void dispatch(const QJsonObject& request) {
        const auto operation = request.value("operation").toString();
        const auto parameters = request.value("parameters").toObject();
        if (operation == "runtime.handshake")
            respond(
                request,
                {{"api_version", "1.1"},
                 {"engine_instance_id", "fake-engine"},
                 {"capabilities",
                  resources_enabled ? QJsonObject{{"resources_version", 1}, {"events_version", 1}}
                                    : QJsonObject{}}});
        else if (operation == "events.subscribe") {
            if (drop_next_subscription)
                drop_next_subscription = false;
            else
                respond(request,
                        {{"engine_instance_id", "fake-engine"},
                         {"next_sequence", QString::number(event_sequence)},
                         {"resync_required", true}});
        } else if (operation == "view.render_resource") {
            if (drop_next_manifest)
                drop_next_manifest = false; // Actual transport timeout.
            else if (hold_render)
                held_renders.append(request);
            else
                respondResource(request);
        } else if (operation == "resources.describe")
            respond(request, resource_manifests.value(parameters.value("resource_id").toString()));
        else if (operation == "resources.read") {
            const auto resource = parameters.value("resource_id").toString();
            const auto bytes = resource_bytes.value(resource);
            respond(request,
                    {{"manifest", resource_manifests.value(resource)},
                     {"offset", "0"},
                     {"raw_length", bytes.size()},
                     {"encoding", "base64"},
                     {"data_base64", QString::fromLatin1(bytes.toBase64())}});
        } else if (operation == "resources.release")
            respond(request, {{"released", true}});
        else if (operation == "test.barrier")
            respond(request, {});
        else if (operation == "project.current") {
            if (hold_current)
                held_current.append(request);
            else
                respond(request,
                        {{"document_id", document_id},
                         {"document_epoch", epoch},
                         {"revision", revision},
                         {"name", "Selection test"},
                         {"dirty", false}});
        } else if (operation == "entity.query") {
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
    explicit Window(const SelectionEngine& engine, qint64 timeout_ms = 5000)
        : window(qcae::create_desktop_window(
              {engine.endpoint(), engine.workspace(), {}, false, timeout_ms})),
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

bool clickWhenReady(const Window& desktop, const SelectionEngine& engine, double z) {
    const auto before = engine.requests("selection.evaluate").size();
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 5000) {
        desktop.click(z);
        if (!barrier(*desktop.client))
            return false;
        const auto after = engine.requests("selection.evaluate").size();
        if (after != before)
            return after == before + 1;
        // Setup can still be completing a camera update or chained resource transfer.
        // Negative race assertions deliberately use a single click instead.
        QTest::qWait(25);
    }
    return false;
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
    void oldDocumentViewReplyCannotOwnNewDocumentPendingState() {
        SelectionEngine engine;
        engine.resources_enabled = true;
        QVERIFY(engine.listen());
        Window desktop(engine, 30000); // Neither held update may expire during this race.
        QVERIFY(desktop.valid());
        QVERIFY(QTest::qWaitForWindowExposed(desktop.window.get()));
        QTRY_VERIFY(desktop.viewport->hasPacket());
        desktop.orient();
        QVERIFY(clickWhenReady(desktop, engine, 0));
        auto* show_all = action(desktop.window.get(), "Show all");
        QVERIFY(show_all);
        engine.hold_update = true;
        show_all->trigger();
        QTRY_COMPARE(engine.held_updates.size(), 1);
        const auto old_update = engine.held_updates.takeFirst();
        QCOMPARE(old_update.value("document_id").toString(), QString("document"));
        const auto releases = engine.requests("resources.release").size();

        engine.document_id = "document-b";
        engine.epoch = "epoch-b";
        engine.view_id = "view-b";
        engine.notifyChange();
        QTRY_COMPARE(engine.requests("view.create").back().value("document_id").toString(),
                     QString("document-b"));
        QTRY_VERIFY(engine.requests("resources.release").size() > releases);
        QVERIFY(barrier(*desktop.client));
        desktop.click(0);
        QTRY_COMPARE(engine.requests("selection.evaluate").size(), 2);
        const auto selected = engine.requests("selection.evaluate").back();
        QCOMPARE(selected.value("document_id").toString(), engine.document_id);
        QCOMPARE(selected.value("document_epoch").toString(), engine.epoch);
        engine.respond(selected, {{"selection_handle", "document-b-selection"}});
        QTRY_COMPARE(engine.requests("selection.get").size(), 1);
        engine.respond(engine.requests("selection.get").back(),
                       {{"entity_ids", QJsonArray{"line-a"}}});
        QTRY_COMPARE(desktop.selected->text(), QString("line-a"));

        show_all->trigger();
        QTRY_COMPARE(engine.held_updates.size(), 1);
        QCOMPARE(engine.held_updates.front().value("document_id").toString(), engine.document_id);
        const auto updates = engine.requests("view.update").size();
        const auto old_params = old_update.value("parameters").toObject();
        engine.respond(
            old_update,
            {{"view_session_id", old_params.value("view_session_id")},
             {"view_revision",
              QString::number(old_params.value("expected_view_revision").toString().toULongLong() +
                              1)}});
        QVERIFY(barrier(*desktop.client));
        desktop.click(0);
        QVERIFY(barrier(*desktop.client));
        QCOMPARE(engine.requests("selection.evaluate").size(), 2);
        // A second B update must stay queued behind the first B update. An old A
        // callback that clears B's pending flag would immediately send another request.
        show_all->trigger();
        QVERIFY(barrier(*desktop.client));
        QCOMPARE(engine.requests("view.update").size(), updates);
        engine.releaseViewUpdates();
        QTRY_COMPARE(engine.requests("view.update").size(), updates + 1);
        QVERIFY(clickWhenReady(desktop, engine, 0));
        QCOMPARE(engine.requests("selection.evaluate").size(), 3);
        QCOMPARE(engine.requests("selection.evaluate").back().value("document_id").toString(),
                 engine.document_id);
        QVERIFY(engine.requests("changes.commit").isEmpty());
    }

    void documentEventFencesSelectionsBeforeTheAuthoritativeReply() {
        SelectionEngine engine;
        engine.resources_enabled = true;
        QVERIFY(engine.listen());
        Window desktop(engine);
        QVERIFY(QTest::qWaitForWindowExposed(desktop.window.get()));
        QTRY_VERIFY(desktop.viewport->hasPacket());
        desktop.orient();
        QVERIFY(barrier(*desktop.client));
        QVERIFY(clickWhenReady(desktop, engine, 0));
        QTRY_COMPARE(engine.requests("selection.evaluate").size(), 1);
        engine.respond(engine.requests("selection.evaluate").back(),
                       {{"selection_handle", "before-event"}});
        QTRY_COMPARE(engine.requests("selection.get").size(), 1);
        const auto old_get = engine.requests("selection.get").back();
        engine.hold_current = true;
        QTRY_VERIFY(!engine.held_current.isEmpty());
        const auto old_current = engine.held_current.takeFirst();
        const auto renders = engine.requests("view.render_resource").size();
        engine.revision = "8";
        engine.notifyChange();
        // This query started before the event and can legitimately describe the old model.
        engine.respond(old_current,
                       {{"document_id", "document"},
                        {"document_epoch", "epoch"},
                        {"revision", "7"},
                        {"name", "Selection test"},
                        {"dirty", false}});
        QTRY_VERIFY(!engine.held_current.isEmpty());
        engine.respond(old_get, {{"entity_ids", QJsonArray{"line-a"}}});
        QVERIFY(barrier(*desktop.client));
        QCOMPARE(desktop.selected->text(), QString("No selection"));
        desktop.click(0);
        QVERIFY(barrier(*desktop.client));
        QCOMPARE(engine.requests("selection.evaluate").size(), 1);
        QCOMPARE(engine.requests("view.render_resource").size(), renders);
        const auto releases = engine.requests("resources.release").size();
        engine.releaseCurrent();
        QTRY_VERIFY(engine.requests("resources.release").size() > releases);
        QVERIFY(barrier(*desktop.client));
        desktop.click(15);
        QTRY_COMPARE(engine.requests("selection.evaluate").size(), 2);
        QCOMPARE(engine.requests("selection.evaluate").back().value("expected_revision").toString(),
                 QString("8"));
        QVERIFY(engine.requests("changes.commit").isEmpty());
    }
    void subscriptionAndViewTimeoutsRetryDisposableState() {
        SelectionEngine engine;
        engine.resources_enabled = true;
        engine.drop_next_subscription = true;
        QVERIFY(engine.listen());
        Window desktop(engine, 600);
        QVERIFY(QTest::qWaitForWindowExposed(desktop.window.get()));
        QTRY_VERIFY_WITH_TIMEOUT(engine.requests("events.subscribe").size() >= 2, 6000);
        QTRY_VERIFY(desktop.viewport->hasPacket());
        desktop.orient();
        QVERIFY(barrier(*desktop.client));
        const auto views = engine.requests("view.create").size();
        engine.drop_next_update = true;
        engine.revision = "8";
        engine.notifyChange();
        QTRY_VERIFY_WITH_TIMEOUT(engine.requests("view.create").size() > views, 8000);
        QTRY_COMPARE(
            engine.requests("view.render_resource").back().value("expected_revision").toString(),
            QString("8"));
        QTest::qWait(200);
        desktop.click(15);
        QTRY_VERIFY(!engine.requests("selection.evaluate").isEmpty());
        QVERIFY(engine.requests("changes.commit").isEmpty());
        QCOMPARE(engine.revision, QString("8"));
    }
    void resourceTimeoutAndEventGapResynchronizeTheVisibleScene() {
        SelectionEngine engine;
        engine.resources_enabled = true;
        QVERIFY(engine.listen());
        Window desktop(engine, 600);
        QVERIFY(QTest::qWaitForWindowExposed(desktop.window.get()));
        QTRY_VERIFY(desktop.viewport->hasPacket());
        desktop.orient();
        QVERIFY(barrier(*desktop.client));
        const auto before = engine.requests("view.render_resource").size();
        const auto history = engine.requests("history.list").size();
        engine.revision = "8";
        engine.drop_next_manifest = true;
        engine.notifyChange();
        QTRY_VERIFY_WITH_TIMEOUT(engine.requests("view.render_resource").size() >= before + 2,
                                 8000);
        QVERIFY(!engine.requests("view.render_resource")
                     .back()
                     .value("parameters")
                     .toObject()
                     .contains("base_revision"));
        QTRY_VERIFY(engine.requests("resources.release").size() >= before + 1);
        QVERIFY(engine.requests("history.list").size() >
                history); // Explicit recovery reloads history.
        QVERIFY(barrier(*desktop.client));
        QTest::qWait(100);
        desktop.click(15);
        QTRY_VERIFY(!engine.requests("selection.evaluate").isEmpty());
        const auto accepted = engine.requests("selection.evaluate").size();
        engine.hold_render = true;
        engine.notifyChange(true);
        QTRY_VERIFY(!engine.held_renders.isEmpty());
        desktop.click(15);
        QVERIFY(barrier(*desktop.client));
        QCOMPARE(engine.requests("selection.evaluate").size(), accepted);
        engine.releaseRender();
        QTest::qWait(200);
        QVERIFY(barrier(*desktop.client));
        desktop.click(15);
        QTRY_COMPARE(engine.requests("selection.evaluate").size(), accepted + 1);
        QVERIFY(engine.requests("view.render_data").isEmpty());
    }
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
        QVERIFY(clickWhenReady(desktop, engine, 0));
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
    QTemporaryDir settings;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    DesktopSelectionTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "desktop_selection_tests.moc"
