#include "qcae/desktop_client.hpp"
#include "qcae/vtk_view.hpp"

#include <QFile>
#include <QApplication>
#include <QDir>
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

class DesktopTests : public QObject {
    Q_OBJECT
  private slots:
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
        view.setPreview({{{0, -10, 30}, {100, -10, 30}}, {{0, 1}}});
        view.standardView(qcae::VtkView::StandardView::front);
        view.fit();
        auto* widget = view.findChild<QVTKOpenGLNativeWidget*>();
        QVERIFY(widget);
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
        view.setPreview({{{0, -10, 0}, {100, -10, 0}}, {{0, 1}}});
        QCOMPARE(view.cameraFingerprint(), camera);
        view.setThroughSelection(false);
        QTest::mouseClick(widget, Qt::LeftButton, Qt::NoModifier, committed_position);
        QCOMPARE(picked.size(), 1);
        QCOMPARE(picked.takeFirst().at(0).toStringList(), QStringList({"committed-line"}));
        view.clearPreview();
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
};
} // namespace

int main(int argc, char** argv) {
    QSurfaceFormat::setDefaultFormat(QVTKOpenGLNativeWidget::defaultFormat());
    QApplication app(argc, argv);
    DesktopTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "desktop_tests.moc"
