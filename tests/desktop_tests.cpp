#include "qcae/desktop_client.hpp"
#include "qcae/vtk_view.hpp"

#include <QFile>
#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPointer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QSurfaceFormat>
#include <QVTKOpenGLNativeWidget.h>
#include <set>
#include <map>
#include <limits>
#include <vtkActor.h>
#include <vtkActorCollection.h>
#include <vtkCallbackCommand.h>
#include <vtkCamera.h>
#include <vtkCommand.h>
#include <vtkDataArray.h>
#include <vtkMapper.h>
#include <vtkMatrix4x4.h>
#include <vtkNew.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkRenderWindow.h>
#include <vtkRenderer.h>
#include <vtkRendererCollection.h>

namespace {
QPoint viewportPosition(QVTKOpenGLNativeWidget* widget, const std::array<double, 3>& position) {
    auto* renderer = widget->renderWindow()->GetRenderers()->GetFirstRenderer();
    renderer->SetWorldPoint(position[0], position[1], position[2], 1.0);
    renderer->WorldToDisplay();
    const auto* display = renderer->GetDisplayPoint();
    const auto scale = widget->devicePixelRatioF();
    return {qRound(display[0] / scale), qRound(widget->height() - display[1] / scale)};
}

void dragBox(QVTKOpenGLNativeWidget* widget, QPoint first, QPoint last) {
    QTest::mousePress(widget, Qt::LeftButton, Qt::ShiftModifier, first);
    QTest::mouseMove(widget, last);
    QTest::mouseRelease(widget, Qt::LeftButton, Qt::ShiftModifier, last);
}

template <class Predicate> int coloredPixels(const QImage& image, Predicate matches) {
    int count = 0;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            if (matches(image.pixelColor(x, y)))
                ++count;
    return count;
}

int highlightedPixels(QVTKOpenGLNativeWidget* widget) {
    return coloredPixels(widget->grab().toImage(), [](const QColor& color) {
        return color.red() > 170 && color.green() > 80 && color.blue() < 100;
    });
}

int previewPixels(QVTKOpenGLNativeWidget* widget) {
    return coloredPixels(widget->grab().toImage(), [](const QColor& color) {
        return color.red() > 150 && color.green() < 150 && color.blue() > 180;
    });
}

bool saveEvidence(QVTKOpenGLNativeWidget* widget, const QString& name) {
    const auto directory = qEnvironmentVariable("QCAE_VTK_EVIDENCE_DIR");
    return directory.isEmpty() ||
           (QDir().mkpath(directory) && widget->grab().save(QDir(directory).filePath(name)));
}

using PointArrays = std::map<vtkPoints*, vtkMTimeType>;
PointArrays pointArrays(QVTKOpenGLNativeWidget* widget) {
    PointArrays result;
    auto* actors = widget->renderWindow()->GetRenderers()->GetFirstRenderer()->GetActors();
    actors->InitTraversal();
    while (auto* actor = actors->GetNextActor()) {
        if (!actor->GetMapper())
            continue;
        auto* data = vtkPolyData::SafeDownCast(actor->GetMapper()->GetInput());
        if (data && data->GetPoints())
            result.emplace(data->GetPoints(), data->GetPoints()->GetMTime());
    }
    return result;
}

qcae::RenderPacket chainPacket(std::size_t nodes) {
    qcae::RenderPacket packet;
    packet.document = {qcae::DocumentId("delta-doc"), qcae::DocumentEpoch("delta-epoch")};
    packet.revision = 7;
    packet.view_session_id = "delta-view";
    packet.view_revision = 3;
    for (std::size_t i = 0; i < nodes; ++i) {
        packet.points.push_back(
            {qcae::EntityId("node-" + std::to_string(i)), {static_cast<double>(i), 0, 0}, true});
        if (i)
            packet.beams.push_back({qcae::EntityId("beam-" + std::to_string(i - 1)), {i - 1, i}});
    }
    return packet;
}

qcae::RenderDelta deltaFor(const qcae::RenderPacket& packet) {
    return {packet.document,
            packet.revision,
            packet.revision + 1,
            packet.view_session_id,
            packet.view_revision,
            packet.view_revision + 1,
            {},
            {}};
}

class DesktopTests : public QObject {
    Q_OBJECT
  private slots:
    void variableArityCellsDrawPickAndUpdateLocally() {
        qcae::RenderPacket packet;
        packet.document = {qcae::DocumentId("area-doc"), qcae::DocumentEpoch("epoch")};
        packet.revision = 1;
        packet.view_session_id = "area-view";
        packet.view_revision = 1;
        const std::vector<std::array<double, 3>> positions{{-2, -2, 0},
                                                           {2, -2, 0},
                                                           {2, 2, 0},
                                                           {-2, 2, 0},
                                                           {-1.8, -1.8, -1},
                                                           {1.8, -1.8, -1},
                                                           {1.8, 1, -1},
                                                           {0, 1.8, -1},
                                                           {-1.8, 1, -1},
                                                           {3, -2, 0},
                                                           {3, 0, 0},
                                                           {3, 2, 0}};
        for (std::size_t i = 0; i < positions.size(); ++i)
            packet.points.push_back(
                {qcae::EntityId("area-node-" + std::to_string(i)), positions[i], false});
        packet.cells = {
            {qcae::EntityId("area-front"), qcae::RenderCellKind::polygon, {0, 1, 2, 3}, true},
            {qcae::EntityId("area-back"), qcae::RenderCellKind::polygon, {4, 5, 6, 7, 8}, true},
            {qcae::EntityId("area-path"), qcae::RenderCellKind::polyline, {9, 10, 11}, true}};
        qcae::VtkView view;
        view.resize(640, 480);
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        view.setPacket(packet);
        auto* widget = view.findChild<QVTKOpenGLNativeWidget*>();
        QVERIFY(widget);
        auto* renderer = widget->renderWindow()->GetRenderers()->GetFirstRenderer();
        auto* camera = renderer->GetActiveCamera();
        camera->SetPosition(0, 0, 10);
        camera->SetFocalPoint(0, 0, 0);
        camera->SetViewUp(0, 1, 0);
        camera->ParallelProjectionOn();
        camera->SetParallelScale(3);
        camera->SetClippingRange(.1, 100);
        widget->renderWindow()->Render();
        QSignalSpy picked(&view, &qcae::VtkView::picked);
        const auto pick = [&](bool through, qcae::VtkView::BoxMode mode) {
            view.setThroughSelection(through);
            view.setBoxMode(mode);
            const auto radius = mode == qcae::VtkView::BoxMode::contained ? 2.1 : .1;
            const auto first = viewportPosition(widget, {-radius, radius, 0});
            const auto last = viewportPosition(widget, {radius, -radius, 0});
            dragBox(widget, first, last);
            if (picked.size() != 1)
                return QStringList{};
            return picked.takeFirst()[0].toStringList();
        };
        for (const auto mode :
             {qcae::VtkView::BoxMode::contained, qcae::VtkView::BoxMode::intersecting}) {
            QCOMPARE(pick(false, mode), QStringList({"area-front"}));
            auto through = pick(true, mode);
            through.sort();
            QCOMPARE(through, QStringList({"area-back", "area-front"}));
        }
        view.setSelectedIds({"area-front"});
        QVERIFY(highlightedPixels(widget) > 100);
        const auto arrays = pointArrays(widget);
        auto hidden = deltaFor(packet);
        hidden.revision = hidden.base_revision;
        hidden.visibility = {{qcae::RenderPrimitive::cell, 0, qcae::EntityId("area-front"), false}};
        QVERIFY(view.applyDelta(hidden));
        QCOMPARE(view.lastUpdateStats().full_rebuilds, std::uint64_t(0));
        QCOMPARE(view.lastUpdateStats().cell_blocks, std::uint64_t(1));
        QCOMPARE(pointArrays(widget), arrays);
        QCOMPARE(pick(true, qcae::VtkView::BoxMode::intersecting), QStringList({"area-back"}));
        auto moved = hidden;
        moved.base_revision = hidden.revision;
        moved.revision = hidden.revision + 1;
        moved.base_view_revision = hidden.view_revision;
        moved.view_revision = hidden.view_revision + 1;
        moved.visibility.clear();
        moved.points = {{0, {packet.points[0].entity, {-3, -2, 0}, false}}};
        QVERIFY(view.applyDelta(moved));
        QCOMPARE(view.lastUpdateStats().full_rebuilds, std::uint64_t(0));
        QCOMPARE(view.lastUpdateStats().cell_blocks, std::uint64_t(1));
        const auto installed = view.installedVersion();
        QVERIFY(installed);
        QCOMPARE(installed->revision, moved.revision);
        auto invalid = moved;
        invalid.base_revision = moved.revision;
        invalid.revision = moved.revision + 1;
        invalid.base_view_revision = moved.view_revision;
        invalid.view_revision = moved.view_revision + 1;
        invalid.points[0].point.position_mm = {100, 100, 100};
        invalid.visibility = {{qcae::RenderPrimitive::cell, 0, qcae::EntityId("area-back"), true}};
        const auto prior_arrays = pointArrays(widget);
        QVERIFY(!view.applyDelta(invalid));
        QCOMPARE(pointArrays(widget), prior_arrays);
        QCOMPARE(view.installedVersion()->revision, installed->revision);
        packet.cells[0].points = {0, 1, 1, 3};
        view.setPacket(packet);
        QCOMPARE(pointArrays(widget), prior_arrays);
        QCOMPARE(view.installedVersion()->revision, installed->revision);
    }
    void frozenPointAndLinePickingMatrix_data() {
        QTest::addColumn<QJsonObject>("fixture");
        QTest::addColumn<QJsonObject>("pick_case");
        QFile source(QFINDTESTDATA("fixtures/vtk-pick-matrix-v1.json"));
        QVERIFY(source.open(QIODevice::ReadOnly));
        const auto bytes = source.readAll();
        const auto digest = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
        QCOMPARE(digest,
                 QByteArray("82568cc06c30dbdec9ec418ef86eee1e6afaf349bee4b4173e130b8af84f7400"));
        const auto fixture = QJsonDocument::fromJson(bytes).object();
        const auto cases = fixture.value("cases").toArray();
        QCOMPARE(cases.size(), 8);
        for (const auto& value : cases) {
            const auto pick_case = value.toObject();
            QTest::newRow(qPrintable(pick_case.value("id").toString())) << fixture << pick_case;
        }
        qInfo().noquote() << "PICK fixture SHA-256:" << digest;
        QFile triangles(QFINDTESTDATA("fixtures/vtk-tri3-pick-matrix-v1.json"));
        QVERIFY(triangles.open(QIODevice::ReadOnly));
        const auto triangle_bytes = triangles.readAll();
        const auto triangle_digest =
            QCryptographicHash::hash(triangle_bytes, QCryptographicHash::Sha256).toHex();
        QCOMPARE(triangle_digest,
                 QByteArray("3cf184545acdf140d28718f5b1cc8fd525c93a136a7137105cc3032695d27ac3"));
        const auto triangle_fixture = QJsonDocument::fromJson(triangle_bytes).object();
        const auto triangle_cases = triangle_fixture.value("cases").toArray();
        QCOMPARE(triangle_cases.size(), 4);
        for (const auto& value : triangle_cases) {
            const auto pick_case = value.toObject();
            QTest::newRow(qPrintable(pick_case.value("id").toString()))
                << triangle_fixture << pick_case;
        }
        qInfo().noquote() << "Tri3 PICK fixture SHA-256:" << triangle_digest;
    }

    void frozenPointAndLinePickingMatrix() {
        QFETCH(QJsonObject, fixture);
        QFETCH(QJsonObject, pick_case);
        const auto coordinates = [](const QJsonValue& value) {
            const auto array = value.toArray();
            return std::array<double, 3>{
                array[0].toDouble(), array[1].toDouble(), array[2].toDouble()};
        };
        qcae::RenderPacket packet;
        packet.document = {qcae::DocumentId("pick-matrix"), qcae::DocumentEpoch("epoch")};
        packet.revision = 1;
        packet.view_session_id = "pick-view";
        packet.view_revision = 1;
        if (pick_case.value("topology") == "point") {
            for (const auto& value : fixture.value("points").toArray()) {
                const auto row = value.toObject();
                packet.points.push_back({qcae::EntityId(row.value("id").toString().toStdString()),
                                         coordinates(row.value("position")),
                                         row.value("visible").toBool()});
            }
        } else if (pick_case.value("topology") == "line") {
            for (const auto& value : fixture.value("lines").toArray()) {
                const auto row = value.toObject();
                packet.geometry_lines.push_back(
                    {qcae::EntityId(row.value("id").toString().toStdString()),
                     coordinates(row.value("start")),
                     coordinates(row.value("end")),
                     row.value("visible").toBool()});
            }
        } else {
            for (const auto& value : fixture.value("triangles").toArray()) {
                const auto row = value.toObject();
                const auto id = row.value("id").toString();
                const auto base = packet.points.size();
                for (const auto& point : row.value("positions").toArray()) {
                    packet.points.push_back(
                        {qcae::EntityId(
                             (id + "-node-" + QString::number(packet.points.size())).toStdString()),
                         coordinates(point),
                         false});
                }
                packet.cells.push_back({qcae::EntityId(id.toStdString()),
                                        qcae::RenderCellKind::polygon,
                                        {base, base + 1, base + 2},
                                        row.value("visible").toBool()});
            }
        }
        qcae::VtkView view;
        const auto viewport = fixture.value("viewport_logical_pixels").toArray();
        view.resize(viewport[0].toInt(), viewport[1].toInt());
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        view.setPacket(packet);
        auto* widget = view.findChild<QVTKOpenGLNativeWidget*>();
        QVERIFY(widget);
        QCOMPARE(widget->size(), QSize(viewport[0].toInt(), viewport[1].toInt()));
        auto* camera =
            widget->renderWindow()->GetRenderers()->GetFirstRenderer()->GetActiveCamera();
        const auto camera_fixture = fixture.value("camera").toObject();
        camera->SetPosition(coordinates(camera_fixture.value("position")).data());
        camera->SetFocalPoint(coordinates(camera_fixture.value("focal_point")).data());
        camera->SetViewUp(coordinates(camera_fixture.value("view_up")).data());
        camera->SetParallelProjection(true);
        camera->SetParallelScale(camera_fixture.value("parallel_scale").toDouble());
        const auto clipping = camera_fixture.value("clipping_range").toArray();
        camera->SetClippingRange(clipping[0].toDouble(), clipping[1].toDouble());
        const auto matrix = camera_fixture.value("view_matrix").toArray();
        for (int row = 0; row < 4; ++row)
            for (int column = 0; column < 4; ++column)
                QCOMPARE(camera->GetViewTransformMatrix()->GetElement(row, column),
                         matrix[row * 4 + column].toDouble());
        widget->renderWindow()->Render();
        if (fixture.contains("device_pixel_ratio")) {
            QCOMPARE(widget->devicePixelRatioF(), fixture.value("device_pixel_ratio").toDouble());
            const auto pixels = fixture.value("framebuffer_pixels").toArray();
            QCOMPARE(widget->renderWindow()->GetSize()[0], pixels[0].toInt());
            QCOMPARE(widget->renderWindow()->GetSize()[1], pixels[1].toInt());
            const auto projection = camera_fixture.value("projection_matrix").toArray();
            auto* actual_projection = camera->GetProjectionTransformMatrix(
                static_cast<double>(widget->width()) / widget->height(), -1, 1);
            for (int row = 0; row < 4; ++row)
                for (int column = 0; column < 4; ++column)
                    QVERIFY(std::abs(actual_projection->GetElement(row, column) -
                                     projection[row * 4 + column].toDouble()) < 1e-12);
        }
        view.setThroughSelection(pick_case.value("through").toBool());
        view.setBoxMode(pick_case.value("box_mode") == "contained"
                            ? qcae::VtkView::BoxMode::contained
                            : qcae::VtkView::BoxMode::intersecting);
        const auto box = pick_case.value("box").toArray();
        QSignalSpy picked(&view, &qcae::VtkView::picked);
        dragBox(widget, {box[0].toInt(), box[1].toInt()}, {box[2].toInt(), box[3].toInt()});
        QCOMPARE(picked.size(), 1);
        const auto reply = picked.takeFirst();
        QCOMPARE(reply[1].toBool(), pick_case.value("through").toBool());
        auto actual = reply[0].toStringList();
        QStringList expected;
        for (const auto& id : pick_case.value("golden").toArray())
            expected.append(id.toString());
        actual.sort();
        expected.sort();
        QCOMPARE(actual, expected);
        QVERIFY(saveEvidence(widget, pick_case.value("id").toString() + ".png"));
        if (pick_case.value("topology") == "tri3") {
            auto stale = deltaFor(packet);
            stale.base_revision = 0;
            stale.visibility = {{qcae::RenderPrimitive::cell, 0, packet.cells[0].entity, false}};
            QVERIFY(!view.applyDelta(stale));
            stale = deltaFor(packet);
            stale.base_view_revision = 0;
            QVERIFY(!view.applyDelta(stale));
            stale = deltaFor(packet);
            stale.document.epoch = qcae::DocumentEpoch("expired");
            QVERIFY(!view.applyDelta(stale));
            QCOMPARE(view.installedVersion()->revision, packet.revision);
            QCOMPARE(view.installedVersion()->view_revision, packet.view_revision);
        }
        qInfo().noquote() << pick_case.value("id").toString()
                          << "device_pixel_ratio=" << widget->devicePixelRatioF()
                          << "qt_platform=" << QGuiApplication::platformName()
                          << "vtk_window=" << widget->renderWindow()->GetClassName()
                          << "actual_ids=" << actual.join(',') << "missed=0 extra=0";
    }

    void asynchronousRepliesRemainCorrelated() {
        QTemporaryDir temp("/private/tmp/qcae-ipc-XXXXXX");
        QVERIFY(temp.isValid());
        const auto endpoint = temp.filePath("engine.sock");
        QLocalServer server;
        QVERIFY(server.listen(endpoint));
        qcae::DesktopClient client({endpoint, temp.path(), {}, false, 300});
        QLocalSocket* peer = nullptr;
        QByteArray requests;
        QStringList ids;
        connect(&server, &QLocalServer::newConnection, &server, [&] {
            peer = server.nextPendingConnection();
            connect(peer, &QLocalSocket::readyRead, &server, [&] {
                requests += peer->readAll();
                while (true) {
                    const auto newline = requests.indexOf('\n');
                    if (newline < 0)
                        break;
                    const auto line = requests.left(newline);
                    requests.remove(0, newline + 1);
                    const auto request = QJsonDocument::fromJson(line).object();
                    const auto id = request.value("request_id").toString();
                    if (request.value("operation") == "runtime.handshake") {
                        peer->write(QJsonDocument(
                                        QJsonObject{{"request_id", id},
                                                    {"status", "success"},
                                                    {"data", QJsonObject{{"api_version", "1.1"}}}})
                                        .toJson(QJsonDocument::Compact) +
                                    '\n');
                    } else if (request.value("operation") == "followup") {
                        peer->write(
                            QJsonDocument(QJsonObject{{"request_id", id},
                                                      {"status", "success"},
                                                      {"data", QJsonObject{{"value", "ok"}}}})
                                .toJson(QJsonDocument::Compact) +
                            '\n');
                    } else {
                        ids.append(id);
                    }
                }
            });
        });
        client.start();
        QTRY_VERIFY(client.ready());
        QStringList delivered;
        (void)client.request("first", {}, {}, [&](const QJsonObject& reply) {
            delivered.append(reply.value("data").toObject().value("value").toString());
        });
        (void)client.request("second", {}, {}, [&](const QJsonObject& reply) {
            delivered.append(reply.value("data").toObject().value("value").toString());
        });
        QTRY_COMPARE(ids.size(), 2);
        for (int index : {1, 0}) {
            peer->write(
                QJsonDocument(QJsonObject{{"request_id", ids[index]},
                                          {"status", "success"},
                                          {"data", QJsonObject{{"value", QString::number(index)}}}})
                    .toJson(QJsonDocument::Compact) +
                '\n');
        }
        QTRY_COMPARE(delivered, QStringList({"1", "0"}));
        QString timeout_code;
        QString followup_value;
        (void)client.request("timeout", {}, {}, [&](const QJsonObject& reply) {
            timeout_code = reply.value("error").toObject().value("code").toString();
            (void)client.request("followup", {}, {}, [&](const QJsonObject& next) {
                followup_value = next.value("data").toObject().value("value").toString();
            });
        });
        QTRY_COMPARE(timeout_code, QString("TIMEOUT"));
        QTRY_COMPARE(followup_value, QString("ok"));
    }

    void clientCanBeDestroyedInsideReply() {
        QTemporaryDir temp("/private/tmp/qcae-delete-XXXXXX");
        QVERIFY(temp.isValid());
        QLocalServer server;
        QVERIFY(server.listen(temp.filePath("engine.sock")));
        connect(&server, &QLocalServer::newConnection, &server, [&] {
            auto* peer = server.nextPendingConnection();
            connect(
                peer, &QLocalSocket::readyRead, &server, [peer, buffer = QByteArray{}]() mutable {
                    buffer += peer->readAll();
                    while (true) {
                        const auto newline = buffer.indexOf('\n');
                        if (newline < 0)
                            return;
                        const auto request = QJsonDocument::fromJson(buffer.left(newline)).object();
                        buffer.remove(0, newline + 1);
                        const auto id = request.value("request_id").toString();
                        const auto data = request.value("operation") == "runtime.handshake"
                                              ? QJsonObject{{"api_version", "1.1"}}
                                              : QJsonObject{{"value", "done"}};
                        peer->write(QJsonDocument(QJsonObject{{"request_id", id},
                                                              {"status", "success"},
                                                              {"data", data}})
                                        .toJson(QJsonDocument::Compact) +
                                    '\n');
                    }
                });
        });
        QPointer<qcae::DesktopClient> client =
            new qcae::DesktopClient({temp.filePath("engine.sock"), temp.path(), {}, false});
        client->start();
        QTRY_VERIFY(client && client->ready());
        bool called = false;
        (void)client->request("delete", {}, {}, [&](const QJsonObject& response) {
            called = response.value("data").toObject().value("value") == "done";
            delete client.data();
        });
        QTRY_VERIFY(called);
        QVERIFY(client.isNull());
    }

    void vtkRendersRealPacket() {
        QTemporaryDir temp("/private/tmp/qcae-view-XXXXXX");
        QVERIFY(temp.isValid());
        qcae::VtkView view;
        view.resize(640, 420);
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        QTest::qWait(100);
        qcae::RenderPacket packet;
        packet.document = {qcae::DocumentId{"doc"}, qcae::DocumentEpoch{"epoch"}};
        packet.revision = 1;
        packet.view_session_id = "view";
        packet.view_revision = 1;
        packet.points = {{qcae::EntityId{"node-a"}, {0, 0, 0}, true},
                         {qcae::EntityId{"node-b"}, {100, 0, 0}, true}};
        packet.beams = {{qcae::EntityId{"beam"}, {0, 1}}};
        view.setPacket(packet);
        view.setSelectedIds({"beam"});
        const auto initial_camera = view.cameraFingerprint();
        view.standardView(qcae::VtkView::StandardView::front);
        const auto front_camera = view.cameraFingerprint();
        QVERIFY(front_camera != initial_camera);
        view.standardView(qcae::VtkView::StandardView::top);
        QVERIFY(view.cameraFingerprint() != front_camera);
        view.standardView(qcae::VtkView::StandardView::front);
        view.fit();
        QVERIFY(view.hasPacket());
        auto* widget = view.findChild<QVTKOpenGLNativeWidget*>();
        QVERIFY(widget);
        QTest::qWait(100);
        QSignalSpy picked(&view, &qcae::VtkView::picked);
        const auto drag_box = [&] {
            QTest::mousePress(widget, Qt::LeftButton, Qt::ShiftModifier, {5, 5});
            QTest::mouseMove(widget, {widget->width() - 5, widget->height() - 5});
            QTest::mouseRelease(widget,
                                Qt::LeftButton,
                                Qt::ShiftModifier,
                                {widget->width() - 5, widget->height() - 5});
        };
        view.setThroughSelection(true);
        drag_box();
        QCOMPARE(picked.size(), 1);
        const auto through_ids = picked.takeFirst().at(0).toStringList();
        QVERIFY(through_ids.contains("node-a"));
        QVERIFY(through_ids.contains("node-b"));
        QVERIFY(through_ids.contains("beam"));

        packet.points[0].visible = false;
        view.setPacket(packet);
        view.setThroughSelection(false);
        drag_box();
        QCOMPARE(picked.size(), 1);
        const auto visible_ids = picked.takeFirst().at(0).toStringList();
        QVERIFY(!visible_ids.contains("node-a"));
        QVERIFY(visible_ids.contains("beam"));

        const auto screenshot = temp.filePath("viewport.png");
        QVERIFY(view.grab().save(screenshot));
        QVERIFY(QFile(screenshot).size() > 0);
        const auto image = widget->grab().toImage();
        std::set<QRgb> colors;
        int highlighted_pixels = 0;
        for (int y = 0; y < image.height(); y += 8)
            for (int x = 0; x < image.width(); x += 8)
                colors.insert(image.pixel(x, y));
        QVERIFY(colors.size() > 1);
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x) {
                const auto color = image.pixelColor(x, y);
                if (color.red() > 170 && color.green() > 80 && color.blue() < 100)
                    ++highlighted_pixels;
            }
        QVERIFY(highlighted_pixels > 5);
    }

    void geometryLinesKeepStablePickIdentity() {
        qcae::VtkView view;
        view.resize(640, 420);
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        qcae::RenderPacket packet;
        packet.document = {qcae::DocumentId{"doc"}, qcae::DocumentEpoch{"epoch"}};
        packet.revision = 7;
        packet.view_session_id = "geometry-view";
        packet.view_revision = 3;
        packet.geometry_lines = {{qcae::EntityId{"line-behind"}, {0, 20, 0}, {100, 20, 0}},
                                 {qcae::EntityId{"line-front"}, {0, -20, 0}, {100, -20, 0}},
                                 {qcae::EntityId{"line-to-hide"}, {0, 0, 35}, {100, 0, 35}}};
        view.setPacket(packet);
        view.standardView(qcae::VtkView::StandardView::front);
        view.fit();
        auto* widget = view.findChild<QVTKOpenGLNativeWidget*>();
        QVERIFY(widget);
        QTest::qWait(100);
        QSignalSpy picked(&view, &qcae::VtkView::picked);
        const auto center = viewportPosition(widget, {50, -20, 0});

        view.setThroughSelection(true);
        QTest::mouseClick(widget, Qt::LeftButton, Qt::NoModifier, center);
        QCOMPARE(picked.size(), 1);
        QCOMPARE(picked.first().at(1).toBool(), true);
        QCOMPARE(picked.takeFirst().at(0).toStringList(),
                 QStringList({"line-behind", "line-front"}));

        view.setThroughSelection(false);
        QTest::mouseClick(widget, Qt::LeftButton, Qt::NoModifier, center);
        QCOMPARE(picked.size(), 1);
        QCOMPARE(picked.first().at(1).toBool(), false);
        QCOMPARE(picked.takeFirst().at(0).toStringList(), QStringList({"line-front"}));

        const auto left = viewportPosition(widget, {40, -20, 0}) - QPoint(0, 10);
        const auto right = viewportPosition(widget, {60, -20, 0}) + QPoint(0, 10);
        view.setThroughSelection(true);
        view.setBoxMode(qcae::VtkView::BoxMode::intersecting);
        dragBox(widget, left, right);
        QCOMPARE(picked.size(), 1);
        QCOMPARE(picked.takeFirst().at(0).toStringList(),
                 QStringList({"line-behind", "line-front"}));

        view.setBoxMode(qcae::VtkView::BoxMode::contained);
        dragBox(widget, left, right);
        QCOMPARE(picked.size(), 1);
        QVERIFY(picked.takeFirst().at(0).toStringList().isEmpty());
        view.setThroughSelection(false);
        dragBox(widget, left, right);
        QCOMPARE(picked.size(), 1);
        QVERIFY(picked.takeFirst().at(0).toStringList().isEmpty());

        dragBox(widget, {5, 5}, {widget->width() - 5, widget->height() - 5});
        QCOMPARE(picked.size(), 1);
        QCOMPARE(picked.takeFirst().at(0).toStringList(),
                 QStringList({"line-front", "line-to-hide"}));
        // Clicks still hit a long line after choosing fully-contained box selection.
        QTest::mouseClick(widget, Qt::LeftButton, Qt::NoModifier, center);
        QCOMPARE(picked.size(), 1);
        QCOMPARE(picked.takeFirst().at(0).toStringList(), QStringList({"line-front"}));

        std::swap(packet.geometry_lines[0], packet.geometry_lines[1]);
        ++packet.view_revision;
        view.setPacket(packet);
        QTest::mouseClick(widget, Qt::LeftButton, Qt::NoModifier, center);
        QCOMPARE(picked.size(), 1);
        QCOMPARE(picked.takeFirst().at(0).toStringList(), QStringList({"line-front"}));
        const auto hidden_position = viewportPosition(widget, {50, 0, 35});
        packet.geometry_lines.pop_back();
        ++packet.view_revision;
        view.setPacket(packet);
        for (const bool through : {false, true}) {
            view.setThroughSelection(through);
            QTest::mouseClick(widget, Qt::LeftButton, Qt::NoModifier, hidden_position);
            QCOMPARE(picked.size(), 1);
            QVERIFY(picked.takeFirst().at(0).toStringList().isEmpty());
        }

        view.setSelectedIds({"line-front"});
        QVERIFY(highlightedPixels(widget) > 20);
        QVERIFY(saveEvidence(widget, "geometry-highlight.png"));
    }

    void geometryPreviewHasNoSelectionIdentity() {
        unsigned renders{};
        vtkNew<vtkCallbackCommand> render_observer;
        render_observer->SetClientData(&renders);
        render_observer->SetCallback([](vtkObject*, unsigned long, void* state, void*) {
            ++*static_cast<unsigned*>(state);
        });
        qcae::VtkView view;
        view.resize(640, 420);
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        qcae::RenderPacket packet;
        packet.document = {qcae::DocumentId{"doc"}, qcae::DocumentEpoch{"epoch"}};
        packet.revision = 9;
        packet.view_session_id = "preview-view";
        packet.view_revision = 4;
        packet.geometry_lines = {{qcae::EntityId{"committed-line"}, {0, 0, 0}, {100, 0, 0}}};
        view.setPacket(packet);
        view.setSelectedIds({"committed-line"});
        auto* widget = view.findChild<QVTKOpenGLNativeWidget*>();
        QVERIFY(widget);
        widget->renderWindow()->AddObserver(vtkCommand::StartEvent, render_observer);
        view.clearPreview();
        view.setPreview({});
        QCOMPARE(renders, 0U);
        view.setPreview({{{0, -10, 30}, {100, -10, 30}}, {{0, 1}}});
        QCOMPARE(renders, 1U);
        view.standardView(qcae::VtkView::StandardView::front);
        view.fit();
        QTest::qWait(100);
        QVERIFY(view.hasPacket());
        QVERIFY(highlightedPixels(widget) > 20);
        QVERIFY(previewPixels(widget) > 20);
        QVERIFY(saveEvidence(widget, "geometry-preview.png"));
        const auto camera = view.cameraFingerprint();
        QSignalSpy picked(&view, &qcae::VtkView::picked);
        const auto committed_position = viewportPosition(widget, {50, 0, 0});
        const auto preview_position = viewportPosition(widget, {50, -10, 30});
        for (const bool through : {false, true}) {
            view.setThroughSelection(through);
            QTest::mouseClick(widget, Qt::LeftButton, Qt::NoModifier, preview_position);
            QCOMPARE(picked.size(), 1);
            QVERIFY(picked.takeFirst().at(0).toStringList().isEmpty());
            QTest::mouseClick(widget, Qt::LeftButton, Qt::NoModifier, committed_position);
            QCOMPARE(picked.size(), 1);
            QCOMPARE(picked.takeFirst().at(0).toStringList(), QStringList({"committed-line"}));
            dragBox(widget, {5, 5}, {widget->width() - 5, widget->height() - 5});
            QCOMPARE(picked.size(), 1);
            QCOMPARE(picked.takeFirst().at(0).toStringList(), QStringList({"committed-line"}));
        }

        // A preview in front of the committed line cannot replace its pick identity.
        const auto before_replace = renders;
        view.setPreview({{{0, -10, 0}, {100, -10, 0}}, {{0, 1}}});
        QCOMPARE(renders, before_replace + 1);
        QCOMPARE(view.cameraFingerprint(), camera);
        view.setThroughSelection(false);
        QTest::mouseClick(widget, Qt::LeftButton, Qt::NoModifier, committed_position);
        QCOMPARE(picked.size(), 1);
        QCOMPARE(picked.takeFirst().at(0).toStringList(), QStringList({"committed-line"}));
        const auto before_clear = renders;
        view.clearPreview();
        QCOMPARE(renders, before_clear + 1);
        view.clearPreview();
        view.setPreview({});
        QCOMPARE(renders, before_clear + 1);
        QCOMPARE(view.cameraFingerprint(), camera);
        QCOMPARE(previewPixels(widget), 0);
        QVERIFY(highlightedPixels(widget) > 20);
        QVERIFY(saveEvidence(widget, "geometry-preview-cleared.png"));

        // Previewing an otherwise empty document does not fabricate selectable entities.
        packet.geometry_lines.clear();
        ++packet.revision;
        view.setPacket(packet);
        view.setPreview({{{0, -10, 0}, {100, -10, 0}}, {{0, 1}}});
        for (const bool through : {false, true}) {
            view.setThroughSelection(through);
            dragBox(widget, {5, 5}, {widget->width() - 5, widget->height() - 5});
            QCOMPARE(picked.size(), 1);
            QVERIFY(picked.takeFirst().at(0).toStringList().isEmpty());
        }
        view.clearPreview();
        QCOMPARE(previewPixels(widget), 0);
    }

    void deltaValidationIsAtomic() {
        qcae::VtkView view;
        view.resize(640, 420);
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        auto packet = chainPacket(3);
        packet.geometry_lines = {{qcae::EntityId("line"), {0, 0, 1}, {2, 0, 1}}};
        view.setPacket(packet);
        auto* widget = view.findChild<QVTKOpenGLNativeWidget*>();
        QVERIFY(widget);
        const auto before = pointArrays(widget);
        auto valid = deltaFor(packet);
        valid.points = {{1, {qcae::EntityId("node-1"), {1, 0, .5}, true}}};
        valid.geometry_lines = {{0, {qcae::EntityId("line"), {0, 0, 1.5}, {2, 0, 1.5}}}};
        for (int fault = 0; fault < 12; ++fault) {
            auto invalid = valid;
            if (fault == 0)
                invalid.document.id = qcae::DocumentId("other");
            if (fault == 1)
                invalid.document.epoch = qcae::DocumentEpoch("other");
            if (fault == 2)
                ++invalid.base_revision;
            if (fault == 3)
                invalid.view_session_id = "other";
            if (fault == 4)
                ++invalid.base_view_revision;
            if (fault == 5)
                invalid.points[0].index = 100;
            if (fault == 6)
                invalid.points[0].point.entity = qcae::EntityId("wrong");
            if (fault == 7)
                invalid.points[0].point.visible = false;
            if (fault == 8)
                invalid.points.push_back(invalid.points.front());
            if (fault == 9)
                invalid.geometry_lines[0].line.entity = qcae::EntityId("wrong");
            if (fault == 10)
                invalid.geometry_lines[0].line.start_mm[0] =
                    std::numeric_limits<double>::infinity();
            if (fault == 11)
                invalid.revision = invalid.base_revision;
            QVERIFY(!view.applyDelta(invalid));
            QVERIFY(pointArrays(widget) == before);
            QCOMPARE(view.lastUpdateStats().coordinate_bytes_copied, std::uint64_t(0));
        }
        QVERIFY(view.applyDelta(valid));
        QCOMPARE(view.lastUpdateStats().node_blocks, std::uint64_t(1));
        QCOMPARE(view.lastUpdateStats().beam_blocks, std::uint64_t(1));
        QCOMPARE(view.lastUpdateStats().geometry_blocks, std::uint64_t(1));
        QVERIFY(!view.applyDelta(valid));
        const auto updated = pointArrays(widget);
        auto metadata = valid;
        metadata.base_revision = valid.revision;
        metadata.revision = valid.revision + 1;
        metadata.base_view_revision = valid.view_revision;
        metadata.view_revision = valid.view_revision + 1;
        metadata.points.clear();
        metadata.geometry_lines.clear();
        QVERIFY(view.applyDelta(metadata));
        QVERIFY(pointArrays(widget) == updated);
        QCOMPARE(view.lastUpdateStats().dirty_coordinate_array_bytes, std::uint64_t(0));
    }

    void visibilityDeltaRetainsCoordinatesAndRejectsInvalidUpdates() {
        qcae::VtkView view;
        view.resize(640, 420);
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        auto packet = chainPacket(2049);
        packet.geometry_lines = {{qcae::EntityId("line"), {0, 0, 20}, {2, 0, 20}}};
        view.setPacket(packet);
        view.standardView(qcae::VtkView::StandardView::front);
        view.fit();
        auto* widget = view.findChild<QVTKOpenGLNativeWidget*>();
        QVERIFY(widget);
        const auto before = pointArrays(widget);
        auto hidden = deltaFor(packet);
        hidden.revision = packet.revision;
        hidden.visibility = {
            {qcae::RenderPrimitive::point, 1024, qcae::EntityId("node-1024"), false},
            {qcae::RenderPrimitive::beam, 1024, qcae::EntityId("beam-1024"), false},
            {qcae::RenderPrimitive::geometry_line, 0, qcae::EntityId("line"), false}};
        for (int fault = 0; fault < 6; ++fault) {
            auto invalid = hidden;
            if (fault == 0)
                invalid.visibility[1].index = 0;
            if (fault == 1)
                invalid.visibility[1].entity = qcae::EntityId("node-1024");
            if (fault == 2)
                invalid.visibility.push_back(invalid.visibility.front());
            if (fault == 3)
                invalid.visibility[1].primitive = static_cast<qcae::RenderPrimitive>(99);
            if (fault == 4)
                invalid.view_revision = invalid.base_view_revision;
            if (fault == 5)
                invalid.geometry_lines = {{0,
                                           {qcae::EntityId("line"),
                                            {0, 0, std::numeric_limits<double>::infinity()},
                                            {2, 0, 20}}}};
            QVERIFY(!view.applyDelta(invalid));
            QVERIFY(pointArrays(widget) == before);
        }
        QVERIFY(view.applyDelta(hidden));
        QVERIFY(pointArrays(widget) == before);
        const auto stats = view.lastUpdateStats();
        QCOMPARE(stats.full_rebuilds, std::uint64_t(0));
        QCOMPARE(stats.node_blocks, std::uint64_t(1));
        QCOMPARE(stats.beam_blocks, std::uint64_t(1));
        QCOMPARE(stats.geometry_blocks, std::uint64_t(1));
        QCOMPARE(stats.coordinate_bytes_copied, std::uint64_t(0));
        QCOMPARE(stats.dirty_coordinate_array_bytes, std::uint64_t(0));
        // Move the hidden node while its visible neighbour beam still uses its coordinates.
        auto moved = hidden;
        moved.base_view_revision = hidden.view_revision;
        moved.visibility.clear();
        moved.revision = moved.base_revision + 1;
        moved.points = {{1024, {qcae::EntityId("node-1024"), {1024, 0, 2}, false}}};
        QVERIFY(view.applyDelta(moved));
        auto shown = moved;
        shown.base_revision = moved.revision;
        shown.base_view_revision = moved.view_revision;
        ++shown.view_revision;
        shown.points.clear();
        shown.visibility = hidden.visibility;
        for (auto& update : shown.visibility)
            update.visible = true;
        QVERIFY(view.applyDelta(shown));
        QVERIFY(!view.applyDelta(hidden));
    }

    void hiddenDeltaExcludesEveryPrimitiveFromVisibleAndThroughPicking() {
        qcae::VtkView view;
        view.resize(640, 420);
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        auto packet = chainPacket(3);
        packet.geometry_lines = {{qcae::EntityId("line"), {0, 0, 1}, {2, 0, 1}}};
        view.setPacket(packet);
        view.standardView(qcae::VtkView::StandardView::front);
        view.fit();
        auto* widget = view.findChild<QVTKOpenGLNativeWidget*>();
        QVERIFY(widget);
        const auto before = pointArrays(widget);
        auto hidden = deltaFor(packet);
        hidden.revision = packet.revision;
        for (std::size_t i = 0; i < packet.points.size(); ++i)
            hidden.visibility.push_back(
                {qcae::RenderPrimitive::point, i, packet.points[i].entity, false});
        for (std::size_t i = 0; i < packet.beams.size(); ++i)
            hidden.visibility.push_back(
                {qcae::RenderPrimitive::beam, i, packet.beams[i].entity, false});
        hidden.visibility.push_back(
            {qcae::RenderPrimitive::geometry_line, 0, qcae::EntityId("line"), false});
        QVERIFY(view.applyDelta(hidden));
        QSignalSpy picked(&view, &qcae::VtkView::picked);
        for (const bool through : {false, true}) {
            view.setThroughSelection(through);
            dragBox(widget, {1, 1}, {widget->width() - 2, widget->height() - 2});
            QCOMPARE(picked.size(), 1);
            QVERIFY(picked.takeFirst()[0].toStringList().isEmpty());
        }
        auto shown = hidden;
        shown.base_view_revision = hidden.view_revision;
        ++shown.view_revision;
        for (auto& update : shown.visibility)
            update.visible = true;
        QVERIFY(view.applyDelta(shown));
        QVERIFY(pointArrays(widget) == before);
        view.setThroughSelection(true);
        dragBox(widget, {1, 1}, {widget->width() - 2, widget->height() - 2});
        auto actual = picked.takeFirst()[0].toStringList();
        actual.sort();
        QCOMPARE(actual, QStringList({"beam-0", "beam-1", "line", "node-0", "node-1", "node-2"}));
    }

    void localDeltaAndSelectionTouchOnlyBoundedBlocks() {
        for (const std::size_t count : {2049, 4097, 16385}) {
            qcae::VtkView view;
            view.resize(640, 420);
            view.show();
            QVERIFY(QTest::qWaitForWindowExposed(&view));
            auto packet = chainPacket(count);
            view.setPacket(packet);
            auto* widget = view.findChild<QVTKOpenGLNativeWidget*>();
            QVERIFY(widget);
            const auto before_selection = pointArrays(widget);
            view.setSelectedIds({"node-1024", "beam-1023", "beam-1024"});
            QVERIFY(pointArrays(widget) == before_selection);
            const auto selection = view.lastUpdateStats();
            QCOMPARE(selection.full_rebuilds, std::uint64_t(0));
            QCOMPARE(selection.node_blocks + selection.beam_blocks + selection.geometry_blocks,
                     std::uint64_t(0));
            QCOMPARE(selection.highlight_blocks, std::uint64_t(3));
            QCOMPARE(selection.highlight_cells_written, std::uint64_t(3));
            QCOMPARE(selection.coordinate_bytes_copied, std::uint64_t(0));
            for (int edit = 0; edit < 20; ++edit) {
                const auto before = pointArrays(widget);
                auto delta = deltaFor(packet);
                delta.points = {
                    {1024, {qcae::EntityId("node-1024"), {1024, 0, double(edit + 1)}, true}}};
                QVERIFY(view.applyDelta(delta));
                const auto after = pointArrays(widget);
                QCOMPARE(after.size(), before.size());
                std::uint64_t changed_arrays{}, dirty_bytes{};
                for (const auto& [points, modified] : before) {
                    QVERIFY(after.contains(points));
                    if (after.at(points) != modified) {
                        ++changed_arrays;
                        dirty_bytes +=
                            points->GetData()->GetDataSize() * points->GetData()->GetDataTypeSize();
                    }
                }
                QCOMPARE(changed_arrays, std::uint64_t(3));
                const auto stats = view.lastUpdateStats();
                QCOMPARE(stats.full_rebuilds, std::uint64_t(0));
                QCOMPARE(stats.node_blocks, std::uint64_t(1));
                QCOMPARE(stats.beam_blocks, std::uint64_t(2));
                QCOMPARE(stats.geometry_blocks, std::uint64_t(0));
                QCOMPARE(stats.highlight_blocks, std::uint64_t(0));
                QCOMPARE(stats.coordinate_bytes_copied, std::uint64_t(96));
                QCOMPARE(stats.dirty_coordinate_array_bytes, dirty_bytes);
                QCOMPARE(dirty_bytes, std::uint64_t(122880));
                packet.revision = delta.revision;
                packet.view_revision = delta.view_revision;
            }
        }
    }

    void geometryDeltaPreservesPickingAndPreview() {
        qcae::VtkView view;
        view.resize(640, 420);
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        auto packet = chainPacket(3);
        packet.geometry_lines = {{qcae::EntityId("moving-line"), {0, 0, 1}, {2, 0, 1}}};
        view.setPacket(packet);
        view.setSelectedIds({"moving-line"});
        view.setPreview({{{0, -.1, 2}, {2, -.1, 2}}, {{0, 1}}});
        view.standardView(qcae::VtkView::StandardView::front);
        view.fit();
        auto* widget = view.findChild<QVTKOpenGLNativeWidget*>();
        QVERIFY(widget);
        const auto camera = view.cameraFingerprint();
        const auto preview_count = previewPixels(widget);
        QVERIFY(preview_count > 20);
        const auto old_position = viewportPosition(widget, {1, 0, 1});
        auto delta = deltaFor(packet);
        delta.geometry_lines = {{0, {qcae::EntityId("moving-line"), {0, 0, 1.4}, {2, 0, 1.4}}}};
        QVERIFY(view.applyDelta(delta));
        QCOMPARE(view.lastUpdateStats().geometry_blocks, std::uint64_t(1));
        QCOMPARE(view.lastUpdateStats().coordinate_bytes_copied, std::uint64_t(96));
        QCOMPARE(view.lastUpdateStats().dirty_coordinate_array_bytes, std::uint64_t(48));
        QCOMPARE(view.cameraFingerprint(), camera);
        QCOMPARE(previewPixels(widget), preview_count);
        QVERIFY(highlightedPixels(widget) > 20);
        QSignalSpy picked(&view, &qcae::VtkView::picked);
        for (const bool through : {false, true}) {
            view.setThroughSelection(through);
            QTest::mouseClick(widget, Qt::LeftButton, Qt::NoModifier, old_position);
            QCOMPARE(picked.size(), 1);
            QVERIFY(picked.takeFirst().at(0).toStringList().isEmpty());
            QTest::mouseClick(
                widget, Qt::LeftButton, Qt::NoModifier, viewportPosition(widget, {1, 0, 1.4}));
            QCOMPARE(picked.size(), 1);
            QCOMPARE(picked.takeFirst().at(0).toStringList(), QStringList({"moving-line"}));
            QTest::mouseClick(
                widget, Qt::LeftButton, Qt::NoModifier, viewportPosition(widget, {1, -.1, 2}));
            QCOMPARE(picked.size(), 1);
            QVERIFY(picked.takeFirst().at(0).toStringList().isEmpty());
        }
    }
};
} // namespace

int main(int argc, char** argv) {
    QSurfaceFormat::setDefaultFormat(QVTKOpenGLNativeWidget::defaultFormat());
    QApplication app(argc, argv);
    DesktopTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "desktop_tests.moc"
