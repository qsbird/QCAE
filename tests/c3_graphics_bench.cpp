#include "qcae/vtk_view.hpp"
#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMouseEvent>
#include <QSaveFile>
#include <QSurfaceFormat>
#include <QSysInfo>
#include <QTest>
#include <QThread>
#include <QTimer>
#include <QVTKOpenGLNativeWidget.h>
#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <string_view>
#include <vector>
#include <vtkCamera.h>
#include <vtkRenderWindow.h>
#include <vtkRenderer.h>
#include <vtkRendererCollection.h>
#include <vtkSMPTools.h>
#include <vtkVersion.h>
#ifdef Q_OS_MACOS
#include <sys/sysctl.h>
#endif

namespace {
constexpr int warmup_frames = 60;
constexpr int sample_frames = 300;
constexpr int framebuffer_width = 1280;
constexpr int framebuffer_height = 720;
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
qcae::RenderPacket fixture() {
    qcae::RenderPacket packet;
    packet.document = {qcae::DocumentId("graphics-chain"), qcae::DocumentEpoch("graphics-epoch")};
    packet.revision = 1;
    packet.view_session_id = "graphics-view";
    packet.view_revision = 1;
    packet.points.reserve(10000);
    packet.beams.reserve(9999);
    for (std::size_t index = 0; index < 10000; ++index) {
        packet.points.push_back({qcae::EntityId("node-" + std::to_string(index)),
                                 {static_cast<double>(index), 0, 0},
                                 true});
        if (index)
            packet.beams.push_back(
                {qcae::EntityId("beam-" + std::to_string(index)), {index - 1, index}});
    }
    return packet;
}
struct Series {
    std::vector<double> milliseconds;
    double p95{};
    QJsonObject json(double limit = 33) const {
        QJsonArray samples;
        for (const auto value : milliseconds)
            samples.append(value);
        return {{"samples_ms", samples},
                {"sample_count", static_cast<int>(milliseconds.size())},
                {"p95_ms", p95},
                {"limit_ms", limit},
                {"passed", p95 <= limit}};
    }
};
Series trajectory(vtkRenderWindow& window, vtkRenderer& renderer, bool zoom) {
    auto* camera = renderer.GetActiveCamera();
    camera->SetParallelProjection(false);
    camera->SetViewUp(0, 0, 1);
    const std::array<double, 3> focal{4999.5, 0, 0};
    constexpr double radius = 16000;
    camera->SetFocalPoint(focal.data());
    camera->SetPosition(focal[0] + radius, radius, radius * .55);
    renderer.ResetCameraClippingRange();
    window.Render();
    window.MakeCurrent();
    window.WaitForCompletion();
    Series result;
    result.milliseconds.reserve(sample_frames);
    QEventLoop loop;
    QElapsedTimer elapsed;
    elapsed.start();
    qint64 previous_completion{};
    int frame = 0;
    std::function<void()> advance;
    advance = [&] {
        const bool warming = frame < warmup_frames;
        const auto count = warming ? warmup_frames : sample_frames;
        const auto index = warming ? frame : frame - warmup_frames;
        const double phase = static_cast<double>(index + 1) / count;
        if (zoom) {
            // Frozen triangle: base radius -> half radius -> base radius.
            const double scale = 1 - .5 * (1 - std::abs(2 * phase - 1));
            camera->SetPosition(focal[0] + radius * scale, radius * scale, radius * .55 * scale);
        } else {
            // Frozen orbit: 360 degrees about Z, with constant elevation.
            const double angle = 2 * std::numbers::pi * phase;
            camera->SetPosition(
                focal[0] + radius * std::cos(angle), radius * std::sin(angle), radius * .55);
        }
        renderer.ResetCameraClippingRange();
        window.Render();
        window.MakeCurrent();
        window.WaitForCompletion(); // vtkOpenGLRenderWindow implements this with glFinish.
        const auto completed = elapsed.nsecsElapsed();
        if (!warming)
            result.milliseconds.push_back(static_cast<double>(completed - previous_completion) /
                                          1e6);
        previous_completion = completed;
        if (++frame == warmup_frames + sample_frames)
            loop.quit();
        else
            QTimer::singleShot(0, &loop, advance);
    };
    // Each frame is queued through Qt. The completion interval includes that queue,
    // camera/CPU updates, the render and GPU wait; no slow sample is discarded.
    QTimer::singleShot(0, &loop, advance);
    loop.exec();
    require(result.milliseconds.size() == sample_frames, "Incorrect frame sample count");
    auto ordered = result.milliseconds;
    std::sort(ordered.begin(), ordered.end());
    const auto rank = static_cast<std::size_t>(std::ceil(.95 * ordered.size()));
    result.p95 = ordered.at(rank - 1);
    return result;
}
Series inputFeedback(qcae::VtkView& view,
                     QVTKOpenGLNativeWidget& widget,
                     vtkRenderWindow& window,
                     vtkRenderer& renderer) {
    view.standardView(qcae::VtkView::StandardView::front);
    view.setThroughSelection(false);
    window.MakeCurrent();
    window.WaitForCompletion();
    const auto screen = [&](double x) {
        renderer.SetWorldPoint(x, 0, 0, 1);
        renderer.WorldToDisplay();
        const auto* display = renderer.GetDisplayPoint();
        return QPoint(qRound(display[0] / widget.devicePixelRatioF()),
                      qRound(widget.height() - display[1] / widget.devicePixelRatioF()));
    };
    const std::array<QPoint, 2> clicks{screen(0), screen(9999)};
    require(widget.rect().contains(clicks[0]) && widget.rect().contains(clicks[1]),
            "Frozen feedback click endpoints are outside the viewport");
    Series result;
    result.milliseconds.reserve(50);
    QEventLoop loop;
    QElapsedTimer elapsed;
    elapsed.start();
    qint64 queued{};
    int input = 0;
    bool failed = false;
    QStringList previous;
    QTimer timeout;
    timeout.setSingleShot(true);
    timeout.setInterval(10000);
    QObject::connect(&timeout, &QTimer::timeout, &loop, [&] {
        failed = true;
        loop.quit();
    });
    std::function<void()> queueInput;
    queueInput = [&] {
        const auto local = clicks[input % 2];
        const auto global = widget.mapToGlobal(local);
        queued = elapsed.nsecsElapsed();
        QCoreApplication::postEvent(&widget,
                                    new QMouseEvent(QEvent::MouseButtonPress,
                                                    local,
                                                    global,
                                                    Qt::LeftButton,
                                                    Qt::LeftButton,
                                                    Qt::NoModifier));
        QCoreApplication::postEvent(&widget,
                                    new QMouseEvent(QEvent::MouseButtonRelease,
                                                    local,
                                                    global,
                                                    Qt::LeftButton,
                                                    Qt::NoButton,
                                                    Qt::NoModifier));
        timeout.start();
    };
    QObject::connect(
        &view, &qcae::VtkView::picked, &loop, [&](const QStringList& ids, bool through) {
            timeout.stop();
            if (ids.isEmpty() || ids == previous || through) {
                failed = true;
                loop.quit();
                return;
            }
            previous = ids;
            view.setSelectedIds(ids);
            window.MakeCurrent();
            window.WaitForCompletion();
            const auto completed = elapsed.nsecsElapsed();
            if (input >= 5)
                result.milliseconds.push_back(static_cast<double>(completed - queued) / 1e6);
            if (++input == 55)
                loop.quit();
            else
                QTimer::singleShot(0, &loop, queueInput);
        });
    QTimer::singleShot(0, &loop, queueInput);
    loop.exec();
    require(!failed && result.milliseconds.size() == 50,
            "Actual visible-pick feedback did not complete all 5 warmup + 50 samples");
    auto ordered = result.milliseconds;
    std::sort(ordered.begin(), ordered.end());
    result.p95 = ordered.at(static_cast<std::size_t>(std::ceil(.95 * ordered.size())) - 1);
    return result;
}
QJsonObject hardware() {
    QJsonObject result{{"os", QSysInfo::prettyProductName()},
                       {"kernel", QSysInfo::kernelVersion()},
                       {"cpu_architecture", QSysInfo::currentCpuArchitecture()},
                       {"qt_version", QT_VERSION_STR},
                       {"vtk_version", vtkVersion::GetVTKVersion()},
                       {"qt_thread_count", QThread::idealThreadCount()},
                       {"vtk_estimated_threads", vtkSMPTools::GetEstimatedNumberOfThreads()},
                       {"graphics_backend", "VTK OpenGL2 / QVTKOpenGLNativeWidget"}};
#ifdef Q_OS_MACOS
    char cpu[256]{};
    auto cpu_size = sizeof(cpu);
    if (!sysctlbyname("machdep.cpu.brand_string", cpu, &cpu_size, nullptr, 0))
        result.insert("cpu", QString::fromUtf8(cpu));
    std::uint64_t memory{};
    auto memory_size = sizeof(memory);
    if (!sysctlbyname("hw.memsize", &memory, &memory_size, nullptr, 0))
        result.insert("physical_memory_bytes", QString::number(memory));
#endif
#ifdef __clang__
    result.insert("compiler", "Clang " __clang_version__);
#elif defined(__GNUC__)
    result.insert("compiler", "GCC " __VERSION__);
#endif
#ifdef NDEBUG
    result.insert("build_config", "Release");
#else
    result.insert("build_config", "Debug");
#endif
    return result;
}
void save_report(const char* path, const QJsonObject& report) {
    QSaveFile output(QString::fromLocal8Bit(path));
    require(output.open(QIODevice::WriteOnly), "Cannot open graphics report");
    const auto bytes = QJsonDocument(report).toJson(QJsonDocument::Indented);
    require(output.write(bytes) == bytes.size() && output.commit(),
            "Cannot publish graphics report");
}
} // namespace
int main(int argc, char** argv) {
    auto format = QVTKOpenGLNativeWidget::defaultFormat();
    format.setSwapInterval(0);
    QSurfaceFormat::setDefaultFormat(format);
    QApplication application(argc, argv);
    try {
#ifndef NDEBUG
        throw std::runtime_error("SK-12 graphics evidence requires a Release build");
#endif
        const bool environment_only =
            argc == 3 && std::string_view(argv[2]) == "--environment-only";
        require(argc == 2 || environment_only,
                "Usage: qcae_c3_graphics_bench output.json [--environment-only]");
        qcae::VtkView view;
        view.resize(framebuffer_width, framebuffer_height);
        view.show();
        require(QTest::qWaitForWindowExposed(&view), "Actual graphics window was not exposed");
        auto* widget = view.findChild<QVTKOpenGLNativeWidget*>();
        require(widget, "VTK widget unavailable");
        const auto ratio = widget->devicePixelRatioF();
        view.setFixedSize(qRound(framebuffer_width / ratio), qRound(framebuffer_height / ratio));
        QCoreApplication::processEvents();
        view.setPacket(fixture());
        auto* window = widget->renderWindow();
        auto* renderer = window->GetRenderers()->GetFirstRenderer();
        require(renderer, "VTK renderer unavailable");
        renderer->SetUseShadows(false);
        window->Render();
        window->MakeCurrent();
        window->WaitForCompletion();
        const auto* size = window->GetSize();
        require(size[0] == framebuffer_width && size[1] == framebuffer_height,
                "Actual VTK framebuffer must be exactly 1280x720");
        auto environment = hardware();
        environment.insert("device_pixel_ratio", ratio);
        environment.insert("framebuffer", QJsonArray{size[0], size[1]});
        environment.insert("logical_widget", QJsonArray{widget->width(), widget->height()});
        environment.insert("opengl_capabilities", QString::fromUtf8(window->ReportCapabilities()));
        if (environment_only) {
            save_report(argv[1],
                        QJsonObject{{"kind", "actual_graphics_environment_probe"},
                                    {"environment", environment},
                                    {"performance_samples_collected", 0}});
            std::cout << "Actual OpenGL environment captured; framebuffer 1280x720\n";
            return 0;
        }
        const auto rotation = trajectory(*window, *renderer, false);
        const auto zoom = trajectory(*window, *renderer, true);
        const auto feedback = inputFeedback(view, *widget, *window, *renderer);
        const auto screenshot = widget->grab().toImage();
        int highlighted = 0;
        for (int y = 0; y < screenshot.height(); ++y)
            for (int x = 0; x < screenshot.width(); ++x) {
                const auto color = screenshot.pixelColor(x, y);
                if (color.red() > 170 && color.green() > 80 && color.blue() < 100)
                    ++highlighted;
            }
        require(highlighted > 0,
                "Completed input feedback did not produce visible highlight pixels");
        const auto screenshot_path = QString::fromLocal8Bit(argv[1]) + ".png";
        require(screenshot.save(screenshot_path), "Cannot publish graphics screenshot");
        QJsonObject report{
            {"protocol", "SK-12 C3 graphics v1"},
            {"environment", environment},
            {"fixture",
             QJsonObject{{"nodes", 10000},
                         {"beams", 9999},
                         {"coordinates", "node[i]=(i,0,0) mm; beam[i]=(i,i+1)"},
                         {"labels", false},
                         {"shadows", false},
                         {"points", true},
                         {"lines", true}}},
            {"timing",
             QJsonObject{
                 {"warmup_frames_per_trajectory", warmup_frames},
                 {"sample_frames_per_trajectory", sample_frames},
                 {"queued_input_interval_ms", 0},
                 {"synchronization", "VTK WaitForCompletion/glFinish after each Render"},
                 {"definition",
                  "wall time between successive GPU completion points, including Qt queue and "
                  "camera update"},
                 {"p95", "nearest rank ceil(0.95*N); no interpolation or dropped samples"}}},
            {"rotation", rotation.json()},
            {"zoom", zoom.json()},
            {"camera_protocol",
             QJsonObject{
                 {"focal_point_mm", QJsonArray{4999.5, 0, 0}},
                 {"base_radius_mm", 16000},
                 {"elevation_radius_factor", .55},
                 {"rotation", "360 degrees about Z; one next camera state per queued frame"},
                 {"zoom", "triangle radius factor 1 -> 0.5 -> 1; fixed direction (1,1,0.55)"}}},
            {"input_feedback", feedback.json(100)},
            {"input_feedback_path",
             "posted alternating endpoint mouse clicks -> VtkView visible hardware selection -> "
             "highlight -> GPU completion; no engine round trip"},
            {"input_feedback_warmup_count", 5},
            {"feedback_highlight_pixels", highlighted},
            {"screenshot", screenshot_path},
            {"passed", rotation.p95 <= 33 && zoom.p95 <= 33 && feedback.p95 <= 100}};
        save_report(argv[1], report);
        std::cout << "rotation P95 " << rotation.p95 << " ms; zoom P95 " << zoom.p95 << " ms\n";
        std::cout << "visible-pick highlight input P95 " << feedback.p95 << " ms\n";
        return rotation.p95 <= 33 && zoom.p95 <= 33 && feedback.p95 <= 100 ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
