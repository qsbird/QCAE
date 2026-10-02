#include "qcae/desktop.hpp"
#include "qcae/vtk_view.hpp"
#include "qcae/render_wire.hpp"
#include <QCryptographicHash>
#include <QAction>
#include <QApplication>
#include <QSettings>
#include <QComboBox>
#include <QDebug>
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
            peer_->write(documentChangeFrame());
        }
    }
    void
    respondObsoleteCurrent(const QJsonObject& request, bool missing, bool notify_first = false) {
        QJsonObject response{{"request_id", request.value("request_id")},
                             {"status", missing ? "failed" : "success"}};
        if (missing)
            response.insert("error", QJsonObject{{"code", "DOCUMENT_NOT_FOUND"}});
        else
            response.insert("data",
                            QJsonObject{{"document_id", "document"},
                                        {"document_epoch", "epoch"},
                                        {"revision", "7"},
                                        {"name", "Obsolete name"},
                                        {"dirty", true}});
        auto bytes = QJsonDocument(response).toJson(QJsonDocument::Compact) + '\n';
        if (notify_first)
            bytes.prepend(documentChangeFrame());
        // One real socket write exposes the event-before-deferred-current-callback phase.
        peer_->write(bytes);
    }

    void respondResource(const QJsonObject& request) {
        const auto params = request.value("parameters").toObject();
        auto view_revision = params.value("expected_view_revision").toString().toULongLong();
        const auto rev = request.value("expected_revision").toString().toULongLong();
        if (lean_refresh && params.value("allow_model_rebase").toBool()) {
            if (request.value("expected_revision") != revision || view_revision != view_revision_ ||
                params.value("base_view_revision").toString().toULongLong() != view_revision_ ||
                params.value("base_revision").toString().toULongLong() != view_model_revision_ ||
                rev <= view_model_revision_) {
                rejectView(request);
                return;
            }
            view_model_revision_ = rev;
            view_revision = ++view_revision_;
            if (wrong_next_target) {
                wrong_next_target = false;
                ++view_revision;
                hold_render = true;
            }
        } else if (lean_refresh &&
                   (request.value("expected_revision") != revision || rev != view_model_revision_ ||
                    view_revision != view_revision_)) {
            rejectView(request);
            return;
        }
        const qcae::DocumentRef doc{
            qcae::DocumentId(request.value("document_id").toString().toStdString()),
            qcae::DocumentEpoch(request.value("document_epoch").toString().toStdString())};
        const auto resource_view = params.value("view_session_id").toString();
        const bool delta = params.contains("base_revision");
        if (delta && inline_empty && params.value("allow_inline_empty").toBool()) {
            QJsonObject acknowledgement{{"document_id", request.value("document_id")},
                                        {"document_epoch", request.value("document_epoch")},
                                        {"revision", QString::number(rev)},
                                        {"view_session_id", resource_view},
                                        {"view_revision", QString::number(view_revision)},
                                        {"base_revision", params.value("base_revision")},
                                        {"base_view_revision", params.value("base_view_revision")}};
            if (corrupt_next_ack) {
                corrupt_next_ack = false;
                acknowledgement.insert("base_revision", "999");
                hold_render = true;
            }
            respond(request,
                    changedRows({{"mode", "version_only"},
                                 {"acknowledgement", acknowledgement},
                                 {"refresh_tree", false},
                                 {"changed_ids", QJsonArray{"line-a"}}},
                                request,
                                view_revision));
            return;
        }
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
                changedRows({{"mode", delta ? "delta" : "full"},
                             {"manifest", manifest},
                             {"refresh_tree", !delta},
                             {"changed_ids", QJsonArray{"line-a"}}},
                            request,
                            view_revision));
    }
    bool resources_enabled{}, drop_next_manifest{}, drop_next_subscription{}, drop_next_update{};
    bool inline_empty{}, corrupt_next_ack{}, reject_next_update{};
    bool legacy_numeric_versions{};
    bool lean_refresh{}, summary_subscribed{}, wrong_next_target{}, hold_entity_rows{},
        hold_entity_all{};
    QString summary_fault, rows_fault, legacy_reply_fault, notified_revision{"7"};
    QString property_revision_fault;
    QJsonObject last_property_query_data;
    bool organization_enabled{};
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
    QList<QJsonObject> held_other;
    QList<QJsonObject> held_entity_rows;
    bool hold_create{}, hold_view_ack{};
    QList<QJsonObject> held_creates;
    QList<std::pair<QJsonObject, QJsonObject>> held_view_acks;

    void acceptHeldViewUpdates() {
        hold_view_ack = true;
        releaseViewUpdates();
        hold_view_ack = false;
        hold_update = true;
    }
    void releaseViewAcknowledgements() {
        const auto pending = held_view_acks;
        held_view_acks.clear();
        for (const auto& [request, data] : pending)
            respond(request, data);
    }
    void releaseCreates() {
        hold_create = false;
        const auto pending = held_creates;
        held_creates.clear();
        for (const auto& request : pending)
            respondView(request);
    }

    void releaseEntityRows() {
        hold_entity_rows = false;
        hold_entity_all = false;
        const auto pending = held_entity_rows;
        held_entity_rows.clear();
        for (const auto& request : pending)
            dispatch(request);
    }

  private:
    QByteArray documentChangeFrame() {
        ++event_sequence;
        QJsonObject details;
        if (lean_refresh && summary_subscribed) {
            auto summary = documentSummary();
            if (summary_fault == "revision")
                summary.insert("revision", "999");
            if (summary_fault == "document")
                summary.insert("document_id", "other-document");
            if (summary_fault == "epoch")
                summary.insert("document_epoch", "other-epoch");
            if (summary_fault == "name")
                summary.insert("name", 7);
            if (summary_fault == "count")
                summary.insert("material_count", -1);
            if (summary_fault == "oversized")
                summary.insert("name", QString(4097, QChar('X')));
            details = {{"base_revision", notified_revision},
                       {"resync_required", false},
                       {"document_summary", summary}};
            if (summary_fault == "missing")
                details.remove("document_summary");
            if (summary_fault == "malformed")
                details.insert("document_summary", "bad");
            if (summary_fault == "resync")
                details.insert("resync_required", true);
            if (summary_fault == "base")
                details.insert("base_revision", "999");
            notified_revision = revision;
        }
        return QJsonDocument(QJsonObject{{"frame_type", "event"},
                                         {"engine_instance_id", "fake-engine"},
                                         {"sequence", QString::number(event_sequence)},
                                         {"event", "DocumentChanged"},
                                         {"document_id", document_id},
                                         {"document_epoch", epoch},
                                         {"revision", revision},
                                         {"data", details}})
                   .toJson(QJsonDocument::Compact) +
               '\n';
    }
    QJsonObject documentSummary() const {
        return {{"document_id", document_id},
                {"document_epoch", epoch},
                {"revision", revision},
                {"content_state", "content-" + revision},
                {"name", "Selection test"},
                {"material_count", 0},
                {"dirty", false},
                {"durable", false},
                {"project_id", "selection-project"},
                {"saved_path", ""},
                {"saved_content_state", ""}};
    }
    void rejectView(const QJsonObject& request) {
        peer_->write(QJsonDocument(QJsonObject{{"request_id", request.value("request_id")},
                                               {"status", "conflict"},
                                               {"error",
                                                QJsonObject{{"code", "REVISION_CONFLICT"},
                                                            {"message", "View baseline changed"}}}})
                         .toJson(QJsonDocument::Compact) +
                     '\n');
    }
    QJsonObject changedRows(QJsonObject data, const QJsonObject& request, quint64 view_revision) {
        if (!lean_refresh ||
            !request.value("parameters").toObject().value("include_changed_rows").toBool())
            return data;
        const bool complete = !data.value("refresh_tree").toBool();
        data.insert("rows_complete", complete && rows_fault != "complete");
        if (!complete)
            return data;
        QJsonObject version{
            {"document_id", request.value("document_id")},
            {"document_epoch", request.value("document_epoch")},
            {"revision", request.value("expected_revision")},
            {"view_session_id", request.value("parameters").toObject().value("view_session_id")},
            {"view_revision", QString::number(view_revision)}};
        auto row = line("line-a");
        if (!rows_fault.isEmpty())
            row.insert("name", "poison-row");
        if (rows_fault == "version")
            version.insert("revision", "999");
        if (rows_fault == "fields")
            row.insert("fields", "bad-fields");
        if (rows_fault == "position")
            row.insert("start_mm", QJsonArray{0, 1});
        if (rows_fault == "id")
            row.insert("entity_id", "other-id");
        if (rows_fault == "budget")
            row.insert("name", QString(11000, QChar('X')));
        QJsonArray rows{row};
        if (rows_fault == "duplicate")
            rows.append(row);
        data.insert("rows_version", version);
        data.insert("changed_rows", rows);
        if (rows_fault == "missing_rows")
            data.remove("changed_rows");
        if (rows_fault == "missing_ids")
            data.remove("changed_ids");
        if (rows_fault == "null_ids")
            data.insert("changed_ids", QJsonValue(QJsonValue::Null));
        if (rows_fault == "string_ids")
            data.insert("changed_ids", "line-a");
        if (rows_fault == "duplicate_ids")
            data.insert("changed_ids", QJsonArray{"line-a", "line-a"});
        if (rows_fault == "invalid_id")
            data.insert("changed_ids", QJsonArray{""});
        return data;
    }
    QJsonObject line(const QString& id) const {
        const double z = id == "line-b" ? 25.0 : revision == "7" || inline_empty ? 0.0 : 15.0;
        return {{"entity_id", id},
                {"kind", "geometry"},
                {"name",
                 organization_enabled && revision != "7" && id == "line-a"
                     ? QString("renamed-line-a")
                     : id},
                {"start_mm", QJsonArray{0, 0, z}},
                {"end_mm", QJsonArray{100, 0, z}},
                {"sources", QJsonArray{}}};
    }
    QJsonObject renderData(const QJsonObject& request) const {
        QJsonArray lines;
        for (const auto* id : {"line-a", "line-b"})
            if (!hidden_.contains(id))
                lines.append(line(id));
        const auto parameters = request.value("parameters").toObject();
        QJsonObject data{{"document_id", request.value("document_id")},
                         {"document_epoch", request.value("document_epoch")},
                         {"revision", request.value("expected_revision")},
                         {"view_session_id", parameters.value("view_session_id")},
                         {"view_revision", parameters.value("expected_view_revision")},
                         {"points", QJsonArray{}},
                         {"beams", QJsonArray{}},
                         {"geometry_lines", lines}};
        if (legacy_numeric_versions) {
            data.insert("revision", request.value("expected_revision").toString().toLongLong());
            data.insert("view_revision",
                        parameters.value("expected_view_revision").toString().toLongLong());
        }
        return data;
    }
    void respondView(const QJsonObject& request) {
        if (request.value("operation") == "view.update" && reject_next_update) {
            reject_next_update = false;
            rejectView(request);
            return;
        }
        if (lean_refresh && request.value("expected_revision") != revision) {
            rejectView(request);
            return;
        }
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
        view_model_revision_ = request.value("expected_revision").toString().toULongLong();
        hidden_ = request.value("parameters").toObject().value("hidden_ids").toArray();
        if (request.value("operation") == "view.update" && drop_next_update) {
            drop_next_update = false;
            return; // Server updated its disposable view but the acknowledgement was lost.
        }
        const QJsonObject data{{"view_session_id", view_id},
                               {"view_revision", QString::number(view_revision_)}};
        if (hold_view_ack)
            held_view_acks.append({request, data});
        else
            respond(request, data);
    }
    void dispatch(const QJsonObject& request) {
        const auto operation = request.value("operation").toString();
        const auto parameters = request.value("parameters").toObject();
        if (operation == "runtime.handshake")
            respond(request,
                    {{"api_version", "1.1"},
                     {"engine_instance_id", "fake-engine"},
                     {"capabilities",
                      resources_enabled
                          ? QJsonObject{{"resources_version", 1},
                                        {"events_version", 1},
                                        {"render_inline_empty_version", inline_empty ? 1 : 0},
                                        {"events_document_summary_version", lean_refresh ? 1 : 0},
                                        {"render_model_rebase_version", lean_refresh ? 1 : 0},
                                        {"render_changed_rows_version", lean_refresh ? 1 : 0}}
                          : QJsonObject{}}});
        else if (operation == "events.subscribe") {
            summary_subscribed = parameters.value("include_document_summary").toBool();
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
        else if (operation == "test.hold")
            held_other.append(request);
        else if (operation == "test.barrier")
            respond(request, {});
        else if (operation == "project.current") {
            if (hold_current)
                held_current.append(request);
            else
                respond(request, documentSummary());
        } else if (operation == "entity.query") {
            QJsonArray rows;
            const auto ids = parameters.value("ids").toArray();
            if ((hold_entity_rows && !ids.isEmpty()) || (hold_entity_all && ids.isEmpty())) {
                held_entity_rows.append(request);
                return;
            }
            for (const auto* id : {"line-a", "line-b"})
                if (ids.isEmpty() || ids.contains(id))
                    rows.append(line(id));
            if (organization_enabled && parameters.value("view") == "all")
                rows.append(QJsonObject{
                    {"entity_id", "material-owner"}, {"kind", "material"}, {"name", "Steel"}});
            QJsonObject data{{"entities", rows}, {"total", rows.size()}, {"revision", revision}};
            if (!ids.isEmpty()) {
                if (property_revision_fault == "missing")
                    data.remove("revision");
                else if (property_revision_fault == "mismatch")
                    data.insert("revision", "999");
                last_property_query_data = data;
            }
            respond(request, data);
        } else if (operation == "history.list")
            respond(request, {{"items", QJsonArray{}}});
        else if (operation == "view.create" || operation == "view.update") {
            if (operation == "view.create" && hold_create)
                held_creates.append(request);
            else if (operation == "view.update" && hold_update)
                held_updates.append(request);
            else
                respondView(request);
        } else if (operation == "view.render_data") {
            if (hold_render)
                held_renders.append(request);
            else if (!legacy_reply_fault.isEmpty()) {
                hold_render = true;
                if (legacy_reply_fault == "failed")
                    rejectView(request);
                else {
                    auto data = renderData(request);
                    if (legacy_reply_fault == "bad-coordinate")
                        data.insert(
                            "points",
                            QJsonArray{QJsonObject{{"entity_id", "bad-point"},
                                                   {"position_mm", QJsonArray{"bad", 0, 0}}}});
                    else if (legacy_reply_fault == "duplicate-id") {
                        auto lines = data.value("geometry_lines").toArray();
                        lines.append(lines.first());
                        data.insert("geometry_lines", lines);
                    } else if (legacy_reply_fault == "cross-kind-id")
                        data.insert("points",
                                    QJsonArray{QJsonObject{{"entity_id", "line-a"},
                                                           {"position_mm", QJsonArray{0, 0, 0}}}});
                    else if (legacy_reply_fault == "visible-type")
                        data.insert("points",
                                    QJsonArray{QJsonObject{{"entity_id", "new-point"},
                                                           {"position_mm", QJsonArray{0, 0, 0}},
                                                           {"visible", "false"}}});
                    else if (legacy_reply_fault == "missing-document")
                        data.remove("document_id");
                    else if (legacy_reply_fault == "missing-revision")
                        data.remove("revision");
                    else
                        data.remove("view_revision");
                    respond(request, data);
                }
                legacy_reply_fault.clear();
            } else
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
    std::uint64_t view_model_revision_{7};
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

QStringList treeIdentities(const QTreeWidget& tree) {
    QStringList identities;
    for (int index = 0; index < tree.topLevelItemCount(); ++index)
        identities.append(tree.topLevelItem(index)->data(0, Qt::UserRole).toString());
    identities.sort();
    return identities;
}

void reportSelectionSetup(const Window& desktop, const SelectionEngine& engine) {
    qInfo() << "selection setup idle" << qcae::desktop_pipeline_idle(*desktop.window)
            << "viewport timer" << desktop.viewport->pendingCameraUpdate() << "pending requests"
            << desktop.client->pendingRequests() << "camera"
            << desktop.viewport->cameraFingerprint();
    qInfo() << "selection setup view.create" << engine.requests("view.create");
    qInfo() << "selection setup view.update" << engine.requests("view.update");
    qInfo() << "selection setup view.render_data" << engine.requests("view.render_data");
}

class DesktopSelectionTests : public QObject {
    Q_OBJECT
  private slots:
    void idleObservationIncludesDeferredCameraAndQueuedViewWork() {
        SelectionEngine engine;
        engine.resources_enabled = true;
        QVERIFY(engine.listen());
        Window desktop(engine);
        QVERIFY(desktop.valid());
        QVERIFY(QTest::qWaitForWindowExposed(desktop.window.get()));
        QTRY_VERIFY_WITH_TIMEOUT(qcae::desktop_pipeline_idle(*desktop.window), 10000);
        QMainWindow unrelated;
        QVERIFY(!qcae::desktop_pipeline_idle(unrelated));
        QCOMPARE(qcae::desktop_pipeline_diagnostics(unrelated),
                 QJsonObject({{"created_desktop", false}}));
        const auto updates = engine.requests("view.update").size();
        engine.hold_update = true;
        desktop.viewport->standardView(qcae::VtkView::StandardView::front);
        QVERIFY(desktop.viewport->pendingCameraUpdate());
        QVERIFY(!qcae::desktop_pipeline_idle(*desktop.window));
        QTRY_COMPARE(engine.held_updates.size(), 1);
        QVERIFY(!desktop.viewport->pendingCameraUpdate());
        QVERIFY(!qcae::desktop_pipeline_idle(*desktop.window));
        const auto held = qcae::desktop_pipeline_diagnostics(*desktop.window);
        QCOMPARE(held.value("idle"), QJsonValue(false));
        QCOMPARE(held.value("view_update_pending"), QJsonValue(true));
        auto* show_all = action(desktop.window.get(), "Show all");
        QVERIFY(show_all);
        show_all->trigger();
        QVERIFY(!qcae::desktop_pipeline_idle(*desktop.window));
        QCOMPARE(qcae::desktop_pipeline_diagnostics(*desktop.window).value("queued_hidden"),
                 QJsonValue(true));
        engine.releaseViewUpdates();
        QTRY_VERIFY_WITH_TIMEOUT(qcae::desktop_pipeline_idle(*desktop.window), 10000);
        QVERIFY(engine.requests("view.update").size() >= updates + 2);
        const auto settled = qcae::desktop_pipeline_diagnostics(*desktop.window);
        QCOMPARE(settled.value("idle"), QJsonValue(true));
        QCOMPARE(settled.value("view_update_pending"), QJsonValue(false));
        QCOMPARE(settled.value("queued_hidden"), QJsonValue(false));
    }

    void continuousModelEditUsesSameVersionSummaryAndRows_data() {
        QTest::addColumn<bool>("lean");
        QTest::addColumn<bool>("inline_empty");
        QTest::newRow("nonempty-delta") << true << false;
        QTest::newRow("empty-inline-delta") << true << true;
        QTest::newRow("legacy-fallback") << false << false;
    }
    void continuousModelEditUsesSameVersionSummaryAndRows() {
        QFETCH(bool, lean);
        QFETCH(bool, inline_empty);
        SelectionEngine engine;
        engine.resources_enabled = engine.organization_enabled = true;
        engine.lean_refresh = lean;
        engine.inline_empty = inline_empty;
        QVERIFY(engine.listen());
        Window desktop(engine);
        QVERIFY(QTest::qWaitForWindowExposed(desktop.window.get()));
        QTRY_VERIFY_WITH_TIMEOUT(qcae::desktop_pipeline_idle(*desktop.window), 10000);
        auto* tree = desktop.window->findChild<QTreeWidget*>("entityTree");
        QVERIFY(tree);
        const auto initial_ids = treeIdentities(*tree);
        qInfo() << "initial organization identities" << initial_ids;
        QCOMPARE(initial_ids, QStringList({"line-a", "line-b", "material-owner"}));
        auto* original = tree->topLevelItem(0);
        const auto baseline = *desktop.viewport->installedVersion();
        const auto current = engine.requests("project.current").size();
        const auto updates = engine.requests("view.update").size();
        const auto queries = engine.requests("entity.query").size();
        const auto renders = engine.requests("view.render_resource").size();
        const auto reads = engine.requests("resources.read").size();
        const auto releases = engine.requests("resources.release").size();
        QCOMPARE(engine.summary_subscribed, lean);
        engine.revision = "8";
        engine.notifyChange();
        QTRY_COMPARE(tree->topLevelItem(0)->text(0), QString("renamed-line-a"));
        QTRY_VERIFY_WITH_TIMEOUT(qcae::desktop_pipeline_idle(*desktop.window), 10000);
        QCOMPARE(tree->topLevelItem(0), original);
        QCOMPARE(desktop.window->property("treeRevision").toString(), QString("8"));
        QCOMPARE(desktop.viewport->installedVersion()->revision, qcae::Revision(8));
        QCOMPARE(desktop.viewport->installedVersion()->view_revision, baseline.view_revision + 1);
        QCOMPARE(engine.requests("view.render_resource").size(), renders + 1);
        QCOMPARE(engine.requests("project.current").size(), current + (lean ? 0 : 1));
        QCOMPARE(engine.requests("view.update").size(), updates + (lean ? 0 : 1));
        QCOMPARE(engine.requests("entity.query").size(), queries + (lean ? 0 : 1));
        const auto parameters =
            engine.requests("view.render_resource").back().value("parameters").toObject();
        QCOMPARE(parameters.value("base_revision").toString(), QString("7"));
        QCOMPARE(parameters.value("base_view_revision").toString().toULongLong(),
                 baseline.view_revision);
        if (lean) {
            QCOMPARE(parameters.value("allow_model_rebase"), QJsonValue(true));
            QCOMPARE(parameters.value("include_changed_rows"), QJsonValue(true));
            QCOMPARE(parameters.value("expected_view_revision").toString().toULongLong(),
                     baseline.view_revision);
        } else {
            QVERIFY(!parameters.contains("allow_model_rebase"));
            QVERIFY(!parameters.contains("include_changed_rows"));
        }
        QCOMPARE(engine.requests("resources.read").size(), reads + (inline_empty ? 0 : 1));
        QCOMPARE(engine.requests("resources.release").size(), releases + (inline_empty ? 0 : 1));
        QVERIFY(engine.requests("changes.commit").isEmpty());
    }

    void invalidDocumentSummaryUsesAuthoritativeFallback_data() {
        QTest::addColumn<QString>("fault");
        for (const auto* fault : {"revision",
                                  "document",
                                  "epoch",
                                  "name",
                                  "count",
                                  "oversized",
                                  "missing",
                                  "malformed",
                                  "resync",
                                  "base",
                                  "jump"})
            QTest::newRow(fault) << QString::fromLatin1(fault);
    }
    void invalidDocumentSummaryUsesAuthoritativeFallback() {
        QFETCH(QString, fault);
        SelectionEngine engine;
        engine.resources_enabled = engine.organization_enabled = engine.lean_refresh = true;
        QVERIFY(engine.listen());
        Window desktop(engine);
        QVERIFY(QTest::qWaitForWindowExposed(desktop.window.get()));
        QTRY_VERIFY_WITH_TIMEOUT(qcae::desktop_pipeline_idle(*desktop.window), 10000);
        const auto current = engine.requests("project.current").size();
        const auto updates = engine.requests("view.update").size();
        const auto renders = engine.requests("view.render_resource").size();
        engine.summary_fault = fault;
        engine.revision = fault == "jump" ? "9" : "8";
        engine.notifyChange();
        QTRY_COMPARE(desktop.window->property("treeRevision").toString(), engine.revision);
        QTRY_VERIFY_WITH_TIMEOUT(qcae::desktop_pipeline_idle(*desktop.window), 10000);
        QVERIFY(engine.requests("project.current").size() > current);
        QVERIFY(engine.requests("view.update").size() > updates);
        for (const auto& request : engine.requests("view.render_resource").mid(renders)) {
            QVERIFY(!request.value("parameters").toObject().contains("allow_model_rebase"));
            QCOMPARE(request.value("document_id").toString(), QString("document"));
            QCOMPARE(request.value("document_epoch").toString(), QString("epoch"));
            QCOMPARE(request.value("expected_revision").toString(), engine.revision);
        }
        QCOMPARE(desktop.window->windowTitle(), QString("Selection test — QCAE"));
        QVERIFY(engine.requests("changes.commit").isEmpty());
    }

    void invalidChangedRowsCannotPublishPartialTreeState_data() {
        QTest::addColumn<QString>("fault");
        for (const auto* fault : {"complete",
                                  "version",
                                  "fields",
                                  "position",
                                  "id",
                                  "budget",
                                  "duplicate",
                                  "missing_rows",
                                  "missing_ids",
                                  "null_ids",
                                  "string_ids",
                                  "duplicate_ids",
                                  "invalid_id"})
            QTest::newRow(fault) << QString::fromLatin1(fault);
    }
    void invalidChangedRowsCannotPublishPartialTreeState() {
        QFETCH(QString, fault);
        SelectionEngine engine;
        engine.resources_enabled = engine.organization_enabled = engine.lean_refresh = true;
        engine.inline_empty = true;
        QVERIFY(engine.listen());
        Window desktop(engine);
        QVERIFY(QTest::qWaitForWindowExposed(desktop.window.get()));
        QTRY_VERIFY_WITH_TIMEOUT(qcae::desktop_pipeline_idle(*desktop.window), 10000);
        auto* tree = desktop.window->findChild<QTreeWidget*>("entityTree");
        QVERIFY(tree);
        const auto initial_ids = treeIdentities(*tree);
        qInfo() << "initial organization identities" << initial_ids;
        QCOMPARE(initial_ids, QStringList({"line-a", "line-b", "material-owner"}));
        auto* original = tree->topLevelItem(0);
        const bool full_fallback = fault.endsWith("ids") || fault == "invalid_id";
        engine.rows_fault = fault;
        engine.hold_entity_rows = true;
        engine.hold_entity_all = full_fallback;
        engine.revision = "8";
        engine.notifyChange();
        QTRY_COMPARE(engine.held_entity_rows.size(), 1);
        QCOMPARE(desktop.viewport->installedVersion()->revision, qcae::Revision(8));
        QCOMPARE(desktop.window->property("treeRevision").toString(), QString("7"));
        QCOMPARE(original->text(0), QString("line-a"));
        QVERIFY(!qcae::desktop_pipeline_idle(*desktop.window));
        if (full_fallback)
            QVERIFY(
                !engine.held_entity_rows.front().value("parameters").toObject().contains("ids"));
        else
            QCOMPARE(engine.held_entity_rows.front()
                         .value("parameters")
                         .toObject()
                         .value("ids")
                         .toArray(),
                     QJsonArray({"line-a"}));
        engine.releaseEntityRows();
        QTRY_VERIFY_WITH_TIMEOUT(qcae::desktop_pipeline_idle(*desktop.window), 10000);
        if (!full_fallback)
            QCOMPARE(tree->topLevelItem(0), original);
        QCOMPARE(tree->topLevelItem(0)->text(0), QString("renamed-line-a"));
        QCOMPARE(desktop.window->property("treeRevision").toString(), QString("8"));
    }

    void unexpectedRebaseTargetRecreatesViewBeforePublishingRows() {
        SelectionEngine engine;
        engine.resources_enabled = engine.organization_enabled = engine.lean_refresh = true;
        QVERIFY(engine.listen());
        Window desktop(engine);
        QVERIFY(QTest::qWaitForWindowExposed(desktop.window.get()));
        QTRY_VERIFY_WITH_TIMEOUT(qcae::desktop_pipeline_idle(*desktop.window), 10000);
        auto* tree = desktop.window->findChild<QTreeWidget*>("entityTree");
        QVERIFY(tree);
        const auto initial_ids = treeIdentities(*tree);
        qInfo() << "initial organization identities" << initial_ids;
        QCOMPARE(initial_ids, QStringList({"line-a", "line-b", "material-owner"}));
        const auto views = engine.requests("view.create").size();
        const auto releases = engine.requests("resources.release").size();
        engine.wrong_next_target = true;
        engine.revision = "8";
        engine.notifyChange();
        QTRY_VERIFY(!engine.held_renders.isEmpty());
        QCOMPARE(engine.requests("view.create").size(), views + 1);
        QVERIFY(engine.requests("resources.release").size() > releases);
        QCOMPARE(desktop.viewport->installedVersion()->revision, qcae::Revision(7));
        QCOMPARE(desktop.window->property("treeRevision").toString(), QString("7"));
        QCOMPARE(tree->topLevelItem(0)->text(0), QString("line-a"));
        const auto replacement = engine.held_renders.back().value("parameters").toObject();
        QVERIFY(!replacement.contains("base_revision"));
        QVERIFY(!replacement.contains("allow_model_rebase"));
        engine.releaseRender();
        QTRY_VERIFY_WITH_TIMEOUT(qcae::desktop_pipeline_idle(*desktop.window), 10000);
        QCOMPARE(desktop.viewport->installedVersion()->revision, qcae::Revision(8));
        QCOMPARE(desktop.window->property("treeRevision").toString(), QString("8"));
        QCOMPARE(tree->topLevelItem(0)->text(0), QString("renamed-line-a"));
    }

    void cameraAndHiddenQueueUseTheAdmittedRebaseVersion() {
        SelectionEngine engine;
        engine.resources_enabled = engine.organization_enabled = engine.lean_refresh = true;
        QVERIFY(engine.listen());
        Window desktop(engine);
        QVERIFY(QTest::qWaitForWindowExposed(desktop.window.get()));
        QTRY_VERIFY_WITH_TIMEOUT(qcae::desktop_pipeline_idle(*desktop.window), 10000);
        const auto baseline = *desktop.viewport->installedVersion();
        const auto updates = engine.requests("view.update").size();
        engine.hold_render = true;
        engine.revision = "8";
        engine.notifyChange();
        QTRY_COMPARE(engine.held_renders.size(), 1);
        QVERIFY(engine.held_renders.front()
                    .value("parameters")
                    .toObject()
                    .value("allow_model_rebase")
                    .toBool());
        desktop.viewport->standardView(qcae::VtkView::StandardView::top);
        auto* show_all = action(desktop.window.get(), "Show all");
        QVERIFY(show_all);
        show_all->trigger();
        QTest::qWait(450); // Both real camera timers expire while the resource is held.
        QVERIFY(barrier(*desktop.client));
        QCOMPARE(engine.requests("view.update").size(), updates);
        QCOMPARE(desktop.viewport->installedVersion()->revision, qcae::Revision(7));
        desktop.click(0);
        QVERIFY(barrier(*desktop.client));
        QVERIFY(engine.requests("selection.evaluate").isEmpty());
        engine.releaseRender();
        QTRY_VERIFY_WITH_TIMEOUT(qcae::desktop_pipeline_idle(*desktop.window), 10000);
        const auto queued = engine.requests("view.update").mid(updates);
        QVERIFY(!queued.isEmpty());
        QCOMPARE(queued.front()
                     .value("parameters")
                     .toObject()
                     .value("expected_view_revision")
                     .toString()
                     .toULongLong(),
                 baseline.view_revision + 1);
        QCOMPARE(
            queued.front().value("parameters").toObject().value("camera_fingerprint").toString(),
            desktop.viewport->cameraFingerprint());
        QCOMPARE(queued.front().value("parameters").toObject().value("hidden_ids").toArray(),
                 QJsonArray{});
        QCOMPARE(desktop.viewport->installedVersion()->revision, qcae::Revision(8));
    }

    void pendingCurrentCannotOverwriteAcceptedDocumentSummary_data() {
        QTest::addColumn<bool>("inline_empty");
        QTest::addColumn<bool>("coalesced");
        QTest::addColumn<bool>("same_batch");
        QTest::addColumn<bool>("other_pending");
        QTest::addColumn<bool>("missing_current");
        QTest::newRow("delta-held") << false << false << false << false << false;
        QTest::newRow("empty-held") << true << false << false << false << false;
        QTest::newRow("delta-coalesced") << false << true << false << false << false;
        QTest::newRow("empty-coalesced") << true << true << false << false << false;
        QTest::newRow("delta-event-reply-batch") << false << false << true << false << false;
        QTest::newRow("empty-event-reply-batch") << true << false << true << false << false;
        QTest::newRow("delta-batch-other-held") << false << false << true << true << false;
        QTest::newRow("empty-batch-other-held") << true << false << true << true << false;
        QTest::newRow("stale-not-found-held") << false << false << false << false << true;
    }
    void pendingCurrentCannotOverwriteAcceptedDocumentSummary() {
        QFETCH(bool, inline_empty);
        QFETCH(bool, coalesced);
        QFETCH(bool, same_batch);
        QFETCH(bool, other_pending);
        QFETCH(bool, missing_current);
        SelectionEngine engine;
        engine.resources_enabled = engine.organization_enabled = engine.lean_refresh = true;
        engine.inline_empty = inline_empty;
        QVERIFY(engine.listen());
        Window desktop(engine, 15000); // Keep the real request held through a coalesced poll.
        QVERIFY(desktop.valid());
        QVERIFY(QTest::qWaitForWindowExposed(desktop.window.get()));
        QTRY_VERIFY_WITH_TIMEOUT(qcae::desktop_pipeline_idle(*desktop.window), 10000);
        auto* tree = desktop.window->findChild<QTreeWidget*>("entityTree");
        QVERIFY(tree);
        QCOMPARE(treeIdentities(*tree), QStringList({"line-a", "line-b", "material-owner"}));
        auto* original = tree->topLevelItem(0);
        QVERIFY(desktop.viewport->installedVersion());
        const auto baseline = *desktop.viewport->installedVersion();
        const auto current = engine.requests("project.current").size();
        const auto updates = engine.requests("view.update").size();
        const auto creates = engine.requests("view.create").size();
        const auto queries = engine.requests("entity.query").size();
        const auto renders = engine.requests("view.render_resource").size();
        const auto reads = engine.requests("resources.read").size();
        const auto releases = engine.requests("resources.release").size();
        engine.hold_current = true;
        QTRY_COMPARE_WITH_TIMEOUT(engine.held_current.size(), 1, 5000);
        const auto old = engine.held_current.takeFirst();
        const auto old_id = old.value("request_id").toString();
        QVERIFY(!old_id.isEmpty());
        QVERIFY(desktop.client->isRequestPending(old_id));
        QVERIFY(
            qcae::desktop_pipeline_diagnostics(*desktop.window).value("current_pending").toBool());
        QVERIFY(!qcae::desktop_pipeline_idle(*desktop.window));
        if (coalesced) {
            QTRY_VERIFY_WITH_TIMEOUT(qcae::desktop_pipeline_diagnostics(*desktop.window)
                                         .value("current_queued")
                                         .toBool(),
                                     5000);
            QCOMPARE(engine.requests("project.current").size(), current + 1);
            QVERIFY(engine.held_current.isEmpty());
        }
        QString other_id;
        bool other_answered = false;
        bool other_succeeded = false;
        if (other_pending) {
            other_id =
                desktop.client->request("test.hold", {}, {}, [&](const QJsonObject& response) {
                    other_answered = true;
                    other_succeeded = response.value("status") == "success";
                });
            QTRY_COMPARE(engine.held_other.size(), 1);
            QVERIFY(desktop.client->isRequestPending(other_id));
            QCOMPARE(desktop.client->pendingRequests(), qsizetype(2));
        }
        bool event_seen = false;
        bool current_in_transport_at_event = false;
        bool other_in_transport_at_event = false;
        QJsonObject event_diagnostics;
        QObject::connect(
            desktop.client,
            &qcae::DesktopClient::eventReceived,
            desktop.window.get(),
            [&](const QJsonObject& event) {
                if (event.value("event") != "DocumentChanged")
                    return;
                event_seen = true;
                current_in_transport_at_event = desktop.client->isRequestPending(old_id);
                other_in_transport_at_event = desktop.client->isRequestPending(other_id);
                event_diagnostics = qcae::desktop_pipeline_diagnostics(*desktop.window);
            });
        engine.revision = "8";
        if (same_batch) {
            engine.hold_current = false;
            engine.respondObsoleteCurrent(old, missing_current, true);
        } else {
            engine.notifyChange();
        }
        QTRY_VERIFY(event_seen);
        QVERIFY(event_diagnostics.value("current_pending").toBool());
        QVERIFY(!event_diagnostics.value("idle").toBool());
        QCOMPARE(current_in_transport_at_event, !same_batch);
        QCOMPARE(other_in_transport_at_event, other_pending);
        QTRY_COMPARE(desktop.viewport->installedVersion()->revision, qcae::Revision(8));
        QTRY_COMPARE(desktop.window->property("treeRevision").toString(), QString("8"));
        QCOMPARE(desktop.viewport->installedVersion()->view_revision, baseline.view_revision + 1);
        QCOMPARE(tree->topLevelItem(0), original);
        QCOMPARE(original->text(0), QString("renamed-line-a"));
        QCOMPARE(desktop.window->windowTitle(), QString("Selection test — QCAE"));
        QCOMPARE(engine.requests("view.render_resource").size(), renders + 1);
        QCOMPARE(engine.requests("view.update").size(), updates + (other_pending ? 1 : 0));
        QCOMPARE(engine.requests("view.create").size(), creates);
        QCOMPARE(engine.requests("entity.query").size(), queries);
        const auto parameters =
            engine.requests("view.render_resource").back().value("parameters").toObject();
        QCOMPARE(parameters.value("base_revision").toString(), QString("7"));
        QCOMPARE(parameters.value("base_view_revision").toString().toULongLong(),
                 baseline.view_revision);
        QCOMPARE(parameters.value("include_changed_rows"), QJsonValue(true));
        if (other_pending)
            QVERIFY(!parameters.contains("allow_model_rebase"));
        else
            QCOMPARE(parameters.value("allow_model_rebase"), QJsonValue(true));
        QCOMPARE(parameters.value("expected_view_revision").toString().toULongLong(),
                 baseline.view_revision + (other_pending ? 1 : 0));
        QTRY_COMPARE(engine.requests("resources.read").size(), reads + (inline_empty ? 0 : 1));
        QTRY_COMPARE(engine.requests("resources.release").size(),
                     releases + (inline_empty ? 0 : 1));
        if (!same_batch) {
            QVERIFY(desktop.client->isRequestPending(old_id));
            QVERIFY(!qcae::desktop_pipeline_idle(*desktop.window));
            engine.respondObsoleteCurrent(old, missing_current);
            engine.releaseCurrent();
        }
        if (other_pending) {
            QTRY_VERIFY(!qcae::desktop_pipeline_diagnostics(*desktop.window)
                             .value("current_pending")
                             .toBool());
            QTRY_COMPARE(desktop.client->pendingRequests(), qsizetype(1));
            QVERIFY(!desktop.client->isRequestPending(old_id));
            QVERIFY(desktop.client->isRequestPending(other_id));
            QVERIFY(!other_answered);
            QVERIFY(!qcae::desktop_pipeline_idle(*desktop.window));
            engine.respond(engine.held_other.takeFirst(), {});
            QTRY_VERIFY(other_answered);
            QVERIFY(other_succeeded);
        }
        QVERIFY(barrier(*desktop.client));
        QTRY_VERIFY_WITH_TIMEOUT(qcae::desktop_pipeline_idle(*desktop.window), 10000);
        QVERIFY(!desktop.client->isRequestPending(old_id));
        QVERIFY(!desktop.client->isRequestPending(other_id));
        QCOMPARE(desktop.client->pendingRequests(), qsizetype(0));
        if (coalesced)
            QVERIFY(engine.requests("project.current").size() >= current + 2);
        QCOMPARE(desktop.viewport->installedVersion()->revision, qcae::Revision(8));
        QCOMPARE(desktop.viewport->installedVersion()->view_revision, baseline.view_revision + 1);
        QCOMPARE(desktop.window->property("treeRevision").toString(), QString("8"));
        QCOMPARE(tree->topLevelItem(0), original);
        QCOMPARE(original->text(0), QString("renamed-line-a"));
        QCOMPARE(desktop.window->windowTitle(), QString("Selection test — QCAE"));
        QCOMPARE(engine.requests("view.render_resource").size(), renders + 1);
        QCOMPARE(engine.requests("view.update").size(), updates + (other_pending ? 1 : 0));
        QCOMPARE(engine.requests("view.create").size(), creates);
        QCOMPARE(engine.requests("entity.query").size(), queries);
        QVERIFY(engine.requests("changes.commit").isEmpty());
    }

    void modelRefreshPreservesPendingHiddenIntent_data() {
        QTest::addColumn<bool>("old_success");
        QTest::addColumn<bool>("second_intent");
        QTest::newRow("accepted-H1-queued-H2") << true << true;
        QTest::newRow("accepted-H1-alone") << true << false;
        QTest::newRow("conflicted-H1-queued-H2") << false << true;
        QTest::newRow("conflicted-H1-alone") << false << false;
    }
    void modelRefreshPreservesPendingHiddenIntent() {
        QFETCH(bool, old_success);
        QFETCH(bool, second_intent);
        SelectionEngine engine;
        engine.resources_enabled = engine.lean_refresh = true;
        QVERIFY(engine.listen());
        Window desktop(engine);
        QVERIFY(QTest::qWaitForWindowExposed(desktop.window.get()));
        QTRY_VERIFY_WITH_TIMEOUT(qcae::desktop_pipeline_idle(*desktop.window), 10000);
        desktop.orient();
        QTRY_VERIFY_WITH_TIMEOUT(qcae::desktop_pipeline_idle(*desktop.window), 10000);
        QVERIFY(clickWhenReady(desktop, engine, 0));
        engine.respond(engine.requests("selection.evaluate").back(),
                       {{"selection_handle", "hide-choice"}});
        QTRY_COMPARE(engine.requests("selection.get").size(), 1);
        engine.respond(engine.requests("selection.get").back(),
                       {{"entity_ids", QJsonArray{"line-a"}}});
        QTRY_COMPARE(desktop.selected->text(), QString("line-a"));
        QTRY_VERIFY_WITH_TIMEOUT(qcae::desktop_pipeline_idle(*desktop.window), 10000);
        auto* hide = action(desktop.window.get(), "Hide selected");
        auto* isolate = action(desktop.window.get(), "Isolate selected");
        QVERIFY(hide && isolate);
        const auto baseline_view = desktop.viewport->installedVersion()->view_revision;
        engine.hold_update = true;
        hide->trigger();
        QTRY_COMPARE(engine.held_updates.size(), 1);
        QCOMPARE(engine.held_updates.front()
                     .value("parameters")
                     .toObject()
                     .value("hidden_ids")
                     .toArray(),
                 QJsonArray({"line-a"}));
        if (second_intent)
            isolate->trigger();
        QVERIFY(barrier(*desktop.client));
        const QJsonArray desired{second_intent ? "line-b" : "line-a"};
        if (old_success) {
            engine.acceptHeldViewUpdates();
            QCOMPARE(engine.held_view_acks.size(), 1);
        } else
            engine.hold_create = true;
        engine.revision = "8";
        engine.notifyChange();
        QVERIFY(barrier(*desktop.client));
        QCOMPARE(desktop.viewport->installedVersion()->revision, qcae::Revision(7));
        if (old_success) {
            engine.releaseViewAcknowledgements();
            QTRY_COMPARE(engine.held_updates.size(), 1);
            const auto parameters = engine.held_updates.front().value("parameters").toObject();
            QCOMPARE(engine.held_updates.front().value("expected_revision").toString(),
                     QString("8"));
            QCOMPARE(parameters.value("expected_view_revision").toString().toULongLong(),
                     baseline_view + 1);
            QCOMPARE(parameters.value("hidden_ids").toArray(), desired);
            engine.releaseViewUpdates();
        } else {
            engine.releaseViewUpdates();
            QTRY_COMPARE(engine.held_creates.size(), 1);
            QCOMPARE(engine.held_creates.front()
                         .value("parameters")
                         .toObject()
                         .value("hidden_ids")
                         .toArray(),
                     desired);
            engine.releaseCreates();
        }
        QTRY_VERIFY_WITH_TIMEOUT(qcae::desktop_pipeline_idle(*desktop.window), 10000);
        QCOMPARE(desktop.viewport->installedVersion()->revision, qcae::Revision(8));
        QVERIFY(engine.requests("changes.commit").isEmpty());
    }

    void heldRecoveryCreateRetainsNewCameraUntilDisplaySynchronization_data() {
        QTest::addColumn<bool>("resources");
        QTest::addColumn<bool>("queued_hidden");
        QTest::newRow("resources-camera") << true << false;
        QTest::newRow("legacy-camera") << false << false;
        QTest::newRow("resources-camera-and-hidden") << true << true;
        QTest::newRow("legacy-camera-and-hidden") << false << true;
    }
    void heldRecoveryCreateRetainsNewCameraUntilDisplaySynchronization() {
        QFETCH(bool, resources);
        QFETCH(bool, queued_hidden);
        SelectionEngine engine;
        engine.resources_enabled = engine.lean_refresh = resources;
        QVERIFY(engine.listen());
        Window desktop(engine);
        QVERIFY(QTest::qWaitForWindowExposed(desktop.window.get()));
        desktop.orient();
        QTRY_VERIFY_WITH_TIMEOUT(qcae::desktop_pipeline_idle(*desktop.window), 10000);
        const auto views = engine.requests("view.create").size();
        const auto updates = engine.requests("view.update").size();
        engine.hold_create = true;
        engine.reject_next_update = true;
        engine.summary_fault = "missing";
        engine.revision = "8";
        engine.notifyChange();
        QTRY_COMPARE(engine.held_creates.size(), 1);
        const auto old_camera = engine.held_creates.front()
                                    .value("parameters")
                                    .toObject()
                                    .value("camera_fingerprint")
                                    .toString();
        desktop.viewport->standardView(qcae::VtkView::StandardView::top);
        if (queued_hidden) {
            auto* isolate = action(desktop.window.get(), "Isolate selected");
            QVERIFY(isolate);
            isolate->trigger();
        }
        QTest::qWait(450);
        const auto new_camera = desktop.viewport->cameraFingerprint();
        qInfo() << "held create camera C0" << old_camera << "viewport C1" << new_camera;
        QVERIFY(new_camera != old_camera);
        QVERIFY(barrier(*desktop.client));
        QCOMPARE(engine.requests("view.create").size(), views + 1);
        QCOMPARE(engine.requests("view.update").size(), updates + 1);
        QVERIFY(!qcae::desktop_pipeline_idle(*desktop.window));
        engine.hold_update = true;
        engine.hold_render = true;
        engine.releaseCreates();
        QTRY_COMPARE(engine.held_renders.size(), 1);
        engine.releaseRender();
        QTRY_COMPARE(engine.held_updates.size(), 1);
        QCOMPARE(engine.held_updates.front()
                     .value("parameters")
                     .toObject()
                     .value("camera_fingerprint")
                     .toString(),
                 new_camera);
        QCOMPARE(engine.held_updates.front()
                     .value("parameters")
                     .toObject()
                     .value("hidden_ids")
                     .toArray(),
                 queued_hidden ? QJsonArray({"line-a", "line-b"}) : QJsonArray{});
        QVERIFY(!qcae::desktop_pipeline_idle(*desktop.window));
        desktop.click(15);
        QVERIFY(barrier(*desktop.client));
        QVERIFY(engine.requests("selection.evaluate").isEmpty());
        engine.releaseViewUpdates();
        QTRY_VERIFY_WITH_TIMEOUT(qcae::desktop_pipeline_idle(*desktop.window), 10000);
        QCOMPARE(desktop.viewport->installedVersion()->revision, qcae::Revision(8));
        QCOMPARE(engine.requests("view.create").size(), views + 1);
        QCOMPARE(desktop.viewport->cameraFingerprint(), new_camera);
    }

    void legacySameRevisionDisplayFailureResynchronizes_data() {
        QTest::addColumn<QString>("fault");
        QTest::newRow("failed-reply") << QString("failed");
        QTest::newRow("malformed-packet") << QString("malformed");
        QTest::newRow("bad-coordinate") << QString("bad-coordinate");
        QTest::newRow("duplicate-id") << QString("duplicate-id");
        QTest::newRow("cross-kind-id") << QString("cross-kind-id");
        QTest::newRow("visible-type") << QString("visible-type");
        QTest::newRow("missing-document") << QString("missing-document");
        QTest::newRow("missing-revision-at-zero") << QString("missing-revision");
    }
    void legacySameRevisionDisplayFailureResynchronizes() {
        QFETCH(QString, fault);
        SelectionEngine engine;
        if (fault == "missing-revision")
            engine.revision = "0";
        QVERIFY(engine.listen());
        Window desktop(engine);
        QVERIFY(QTest::qWaitForWindowExposed(desktop.window.get()));
        QTRY_VERIFY_WITH_TIMEOUT(qcae::desktop_pipeline_idle(*desktop.window), 10000);
        const auto baseline = *desktop.viewport->installedVersion();
        const auto renders = engine.requests("view.render_data").size();
        engine.legacy_reply_fault = fault;
        auto* show_all = action(desktop.window.get(), "Show all");
        QVERIFY(show_all);
        show_all->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!engine.held_renders.isEmpty(), 5000);
        QVERIFY(engine.requests("view.render_data").size() >= renders + 2);
        QVERIFY(desktop.viewport->installedVersion().has_value());
        QCOMPARE(desktop.viewport->installedVersion()->view_revision, baseline.view_revision);
        QVERIFY(!qcae::desktop_pipeline_idle(*desktop.window));
        desktop.click(0);
        QVERIFY(barrier(*desktop.client));
        QVERIFY(engine.requests("selection.evaluate").isEmpty());
        engine.releaseRender();
        QTRY_VERIFY_WITH_TIMEOUT(qcae::desktop_pipeline_idle(*desktop.window), 10000);
        QCOMPARE(desktop.viewport->installedVersion()->revision, baseline.revision);
        QVERIFY(desktop.viewport->installedVersion()->view_revision > baseline.view_revision);
        QVERIFY(engine.requests("changes.commit").isEmpty());
    }

    void legacyNumericPacketVersionsRemainCompatible() {
        SelectionEngine engine;
        engine.legacy_numeric_versions = true;
        QVERIFY(engine.listen());
        Window desktop(engine);
        QVERIFY(QTest::qWaitForWindowExposed(desktop.window.get()));
        QTRY_VERIFY_WITH_TIMEOUT(qcae::desktop_pipeline_idle(*desktop.window), 10000);
        QCOMPARE(desktop.viewport->installedVersion()->revision, qcae::Revision(7));
        QVERIFY(!engine.requests("view.render_data").isEmpty());
        QVERIFY(engine.requests("changes.commit").isEmpty());
    }

    void cameraChangeFencesPicksAndAnAlreadyRequestedSelectionReply() {
        SelectionEngine engine;
        engine.resources_enabled = engine.lean_refresh = true;
        QVERIFY(engine.listen());
        Window desktop(engine);
        QVERIFY(QTest::qWaitForWindowExposed(desktop.window.get()));
        QTRY_VERIFY_WITH_TIMEOUT(qcae::desktop_pipeline_idle(*desktop.window), 10000);
        desktop.orient();
        QTRY_VERIFY_WITH_TIMEOUT(qcae::desktop_pipeline_idle(*desktop.window), 10000);
        QVERIFY(clickWhenReady(desktop, engine, 0));
        const auto before_camera = engine.requests("selection.evaluate").back();
        engine.respond(before_camera, {{"selection_handle", "before-camera"}});
        QTRY_COMPARE(engine.requests("selection.get").size(), 1);
        const auto old_get = engine.requests("selection.get").back();
        desktop.viewport->standardView(qcae::VtkView::StandardView::top);
        QVERIFY(desktop.viewport->pendingCameraUpdate());
        desktop.click(0);
        engine.respond(old_get, {{"entity_ids", QJsonArray{"line-a"}}});
        QVERIFY(barrier(*desktop.client));
        QCOMPARE(engine.requests("selection.evaluate").size(), 1);
        QCOMPARE(desktop.selected->text(), QString("No selection"));
        QTRY_COMPARE(engine.requests("selection.evaluate").size(), 2);
        const auto current = engine.requests("selection.evaluate").back();
        QVERIFY(current.value("parameters")
                    .toObject()
                    .value("expected_view_revision")
                    .toString()
                    .toULongLong() > before_camera.value("parameters")
                                         .toObject()
                                         .value("expected_view_revision")
                                         .toString()
                                         .toULongLong());
        engine.respond(current, {{"selection_handle", "after-camera"}});
        QTRY_COMPARE(engine.requests("selection.get").size(), 2);
        engine.respond(engine.requests("selection.get").back(),
                       {{"entity_ids", QJsonArray{"line-a"}}});
        QTRY_COMPARE(desktop.selected->text(), QString("line-a"));
        QTRY_VERIFY_WITH_TIMEOUT(qcae::desktop_pipeline_idle(*desktop.window), 10000);
    }

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
        QVERIFY(clickWhenReady(desktop, engine, 15));
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
        QVERIFY(clickWhenReady(desktop, engine, 15));
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
    void ordinaryFieldChangeRefreshesOrganizationRowsByIdentity() {
        SelectionEngine engine;
        engine.resources_enabled = true;
        engine.organization_enabled = true;
        QVERIFY(engine.listen());
        Window desktop(engine);
        QVERIFY(desktop.valid());
        QVERIFY(QTest::qWaitForWindowExposed(desktop.window.get()));
        QTRY_VERIFY(desktop.viewport->hasPacket());
        desktop.orient();
        auto* tree = desktop.window->findChild<QTreeWidget*>("entityTree");
        auto* kind = desktop.window->findChild<QComboBox*>("entityViewKind");
        auto* owner = desktop.window->findChild<QComboBox*>("entityViewOwner");
        QVERIFY(tree && kind && owner);
        kind->setCurrentText("material");
        QTRY_VERIFY(owner->findData("material-owner") > 0);
        owner->setCurrentIndex(owner->findData("material-owner"));
        QTRY_COMPARE(tree->topLevelItemCount(), 2);
        QVERIFY(barrier(*desktop.client));
        QTRY_COMPARE(desktop.client->pendingRequests(), std::size_t(0));
        auto* original = tree->topLevelItem(0);
        const auto queries = engine.requests("entity.query").size();
        engine.revision = "8";
        engine.notifyChange();
        QTRY_COMPARE(tree->topLevelItem(0)->text(0), QString("renamed-line-a"));
        QCOMPARE(tree->topLevelItem(0), original);
        QCOMPARE(owner->currentData().toString(), QString("material-owner"));
        const auto requests = engine.requests("entity.query");
        QVERIFY(requests.size() > queries);
        for (qsizetype index = queries; index < requests.size(); ++index) {
            const auto params = requests[index].value("parameters").toObject();
            QCOMPARE(params.value("ids").toArray(), QJsonArray({"line-a"}));
            QVERIFY(!params.contains("view"));
            QVERIFY(!params.contains("owner_id"));
        }
    }

    void versionAcknowledgementAdvancesSceneAndRowsAndRejectsAStaleBase() {
        SelectionEngine engine;
        engine.resources_enabled = true;
        engine.inline_empty = true;
        QVERIFY(engine.listen());
        Window desktop(engine);
        QVERIFY(QTest::qWaitForWindowExposed(desktop.window.get()));
        QTRY_VERIFY(desktop.viewport->hasPacket());
        desktop.orient();
        QVERIFY(barrier(*desktop.client));
        QTRY_COMPARE(desktop.client->pendingRequests(), std::size_t(0));
        const auto reads = engine.requests("resources.read").size();
        const auto releases = engine.requests("resources.release").size();
        engine.revision = "8";
        engine.notifyChange();
        QTRY_COMPARE(desktop.viewport->installedVersion()->revision, qcae::Revision(8));
        QTRY_COMPARE(desktop.window->property("treeRevision").toString(), QString("8"));
        QTRY_COMPARE(desktop.client->pendingRequests(), std::size_t(0));
        QCOMPARE(engine.requests("resources.read").size(), reads);
        QCOMPARE(engine.requests("resources.release").size(), releases);
        engine.corrupt_next_ack = true;
        engine.revision = "9";
        engine.notifyChange();
        QTRY_VERIFY(!engine.held_renders.isEmpty());
        QCOMPARE(desktop.viewport->installedVersion()->revision, qcae::Revision(8));
        QVERIFY(
            !engine.held_renders.back().value("parameters").toObject().contains("base_revision"));
        engine.releaseRender();
        QTRY_COMPARE(desktop.viewport->installedVersion()->revision, qcae::Revision(9));
        QTRY_COMPARE(desktop.window->property("treeRevision").toString(), QString("9"));
        QTRY_COMPARE(desktop.client->pendingRequests(), std::size_t(0));
        QCOMPARE(engine.requests("resources.read").size(), reads + 1);
    }

    void cameraUpdateWaitsForThePendingResourceInstallation() {
        SelectionEngine engine;
        engine.resources_enabled = true;
        QVERIFY(engine.listen());
        Window desktop(engine);
        QVERIFY(QTest::qWaitForWindowExposed(desktop.window.get()));
        QTRY_VERIFY(desktop.viewport->hasPacket());
        desktop.orient();
        QVERIFY(barrier(*desktop.client));
        QTRY_COMPARE(desktop.client->pendingRequests(), std::size_t(0));
        engine.hold_render = true;
        engine.revision = "8";
        engine.notifyChange();
        QTRY_VERIFY(!engine.held_renders.isEmpty());
        const auto updates = engine.requests("view.update").size();
        const auto renders = engine.requests("view.render_resource").size();
        desktop.viewport->standardView(qcae::VtkView::StandardView::top);
        QTest::qWait(350); // Exercise the real 200 ms camera debounce while the transfer waits.
        QVERIFY(barrier(*desktop.client));
        QCOMPARE(engine.requests("view.update").size(), updates);
        QCOMPARE(engine.requests("view.render_resource").size(), renders);
        engine.releaseRender();
        QTRY_VERIFY(engine.requests("view.update").size() > updates);
        QTRY_COMPARE(desktop.viewport->installedVersion()->revision, qcae::Revision(8));
        QTRY_COMPARE(desktop.client->pendingRequests(), std::size_t(0));
        for (const auto& request : engine.requests("view.render_resource").mid(renders))
            QVERIFY(request.value("parameters").toObject().contains("base_revision"));
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

        reportSelectionSetup(desktop, engine);
        QTRY_VERIFY_WITH_TIMEOUT(qcae::desktop_pipeline_idle(*desktop.window), 10000);
        reportSelectionSetup(desktop, engine);
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

    void propertyQueryNeedsSnapshotRevisionToEnableMeshTools_data() {
        QTest::addColumn<QString>("revision_fault");
        QTest::newRow("missing") << QString("missing");
        QTest::newRow("mismatch") << QString("mismatch");
    }

    void propertyQueryNeedsSnapshotRevisionToEnableMeshTools() {
        QFETCH(QString, revision_fault);
        SelectionEngine engine;
        engine.property_revision_fault = revision_fault;
        QVERIFY(engine.listen());
        Window desktop(engine);
        QVERIFY(desktop.valid());
        QVERIFY(QTest::qWaitForWindowExposed(desktop.window.get()));
        QTRY_VERIFY_WITH_TIMEOUT(qcae::desktop_pipeline_idle(*desktop.window), 10000);
        auto* tree = desktop.window->findChild<QTreeWidget*>("entityTree");
        auto* mode = desktop.window->findChild<QComboBox*>("modelingMode");
        auto* apply = desktop.window->findChild<QPushButton*>("modelingApply");
        auto* property_details = desktop.window->findChild<QLabel*>("propertyDetails");
        QVERIFY(tree && mode && apply && property_details);
        QTRY_COMPARE(tree->topLevelItemCount(), 2);
        mode->setCurrentIndex(1);
        tree->setCurrentItem(tree->topLevelItem(1));
        QTRY_COMPARE(engine.requests("selection.evaluate").size(), 1);
        engine.respond(engine.requests("selection.evaluate").back(),
                       {{"selection_handle", "bad-property-version"}});
        QTRY_COMPARE(engine.requests("selection.get").size(), 1);
        engine.respond(engine.requests("selection.get").back(),
                       {{"entity_ids", QJsonArray{"line-b"}}});
        QTRY_COMPARE(desktop.selected->text(), QString("line-b"));
        // This barrier follows the real entity.query request on the local socket,
        // so the negative assertion observes delivery rather than an empty window.
        QVERIFY(barrier(*desktop.client));
        QCOMPARE(engine.requests("entity.query")
                     .back()
                     .value("parameters")
                     .toObject()
                     .value("ids")
                     .toArray(),
                 QJsonArray({"line-b"}));
        QCOMPARE(engine.last_property_query_data.value("entities").toArray().size(), 1);
        if (revision_fault == "missing")
            QVERIFY(!engine.last_property_query_data.contains("revision"));
        else
            QCOMPARE(engine.last_property_query_data.value("revision").toString(), QString("999"));
        qInfo() << "delivered property query data" << engine.last_property_query_data;
        QVERIFY(property_details->text().isEmpty());
        QVERIFY(!apply->isEnabled());
        QTest::mouseClick(apply, Qt::LeftButton);
        QVERIFY(barrier(*desktop.client));
        QVERIFY(engine.requests("mesh.generate_line").isEmpty());
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
        reportSelectionSetup(desktop, engine);
        QTRY_VERIFY_WITH_TIMEOUT(qcae::desktop_pipeline_idle(*desktop.window), 10000);
        reportSelectionSetup(desktop, engine);
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
    if (!settings.isValid())
        return 2;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settings.filePath("system"));
    DesktopSelectionTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "desktop_selection_tests.moc"
