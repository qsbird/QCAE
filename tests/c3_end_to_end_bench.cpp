#include "qcae/desktop.hpp"
#include "qcae/operation_ledger.hpp"
#include "qcae/json_ledger.hpp"
#include "qcae/vtk_view.hpp"
#include "c3_qt_source_bridge.hpp"
#include "c3_process_observation.hpp"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMainWindow>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QSettings>
#include <QSurfaceFormat>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QVTKOpenGLNativeWidget.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>
#include <vtkRenderWindow.h>

namespace {
using namespace qcae;
constexpr std::array<std::size_t, 3> sizes{1000, 10000, 100000};
constexpr std::uint64_t node_limit = 589824;
constexpr std::uint64_t material_limit = 69632;
constexpr std::uint64_t metadata_limit = 65536;

void require(bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error(message);
}
void wait_until(const std::function<bool()>& ready, const char* operation, int limit = 60000) {
    QElapsedTimer timer;
    timer.start();
    while (!ready() && timer.elapsed() < limit) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    require(ready(), std::string("Timed out waiting for ") + operation);
}
QJsonObject call(DesktopClient& client,
                 const QString& operation,
                 const QJsonObject& parameters = {},
                 const QJsonObject& context = {},
                 bool end_measurement = false,
                 int wait_ms = 60000) {
    auto response = std::make_shared<std::optional<QJsonObject>>();
    (void)client.request(operation, parameters, context, [response](const QJsonObject& value) {
        *response = value;
    });
    if (end_measurement)
        ledger::activate({});
    wait_until([&] { return response->has_value(); }, qPrintable(operation), wait_ms);
    require((*response)->value("status") == "success",
            operation.toStdString() + ": " +
                QJsonDocument(**response).toJson(QJsonDocument::Compact).toStdString());
    return **response;
}
QJsonObject data(const QJsonObject& response) {
    return response.value("data").toObject();
}
QJsonObject sdk_snapshot_json(const SdkCopySnapshot& snapshot) {
    QJsonArray components;
    for (const auto& component : snapshot.components) {
        components.append(QJsonObject{
            {"component", QString::fromStdString(component.component)},
            {"calls", QString::number(component.calls)},
            {"copy_bytes",
             component.copy_bytes ? QJsonValue(QString::number(*component.copy_bytes))
                                  : QJsonValue()},
            {"unsupported_reason", QString::fromStdString(component.unsupported_reason)}});
    }
    return {{"run_id", QString::fromStdString(snapshot.run_id)}, {"components", components}};
}
QJsonObject context(const QJsonObject& info, const QString& key = {}) {
    QJsonObject result{{"document_id", info.value("document_id")},
                       {"document_epoch", info.value("document_epoch")},
                       {"expected_revision", info.value("revision")}};
    transport::json_ledger::ObjectCopies copies;
    copies.insert(QLatin1StringView("document_id"), result.value("document_id"), false);
    copies.insert(QLatin1StringView("document_epoch"), result.value("document_epoch"), false);
    copies.insert(QLatin1StringView("expected_revision"), result.value("expected_revision"), false);
    if (!key.isEmpty()) {
        result.insert("idempotency_key", key);
        copies.insert(QLatin1StringView("idempotency_key"), key);
    }
    return result;
}
QString fixed_id(const char* kind, std::size_t index) {
    const auto label = transport::json_ledger::from_utf8(kind);
    const auto number = transport::json_ledger::number(index).rightJustified(12, '0');
    ledger::add(
        ledger::Stage::socket_send, ledger::Metric::json_object_copy_bytes, number.size() * 2);
    const auto result = label + '-' + number;
    ledger::add(
        ledger::Stage::socket_send, ledger::Metric::json_object_copy_bytes, result.size() * 2);
    return result;
}
QJsonObject snapshot_json(const ledger::Snapshot& snapshot) {
    QJsonArray frames;
    for (const auto& frame : snapshot.frames)
        frames.append(
            QJsonObject{{"stage",
                         QString::fromLatin1(
                             ledger::stage_names[static_cast<std::size_t>(frame.stage)].data(),
                             ledger::stage_names[static_cast<std::size_t>(frame.stage)].size())},
                        {"bytes", QString::number(frame.bytes)},
                        {"kind", QString::fromStdString(frame.kind)},
                        {"operation", QString::fromStdString(frame.operation)},
                        {"request_id", QString::fromStdString(frame.request_id)},
                        {"event", QString::fromStdString(frame.event)}});
    QJsonObject stages;
    for (std::size_t stage = 0; stage < snapshot.covered.size(); ++stage) {
        QJsonObject metrics;
        for (std::size_t metric = 0; metric < snapshot.values[stage].size(); ++metric) {
            const auto& value = snapshot.values[stage][metric];
            metrics.insert(
                QString::fromLatin1(ledger::metric_names[metric].data(),
                                    static_cast<qsizetype>(ledger::metric_names[metric].size())),
                value ? QJsonValue(QString::number(*value)) : QJsonValue::Null);
        }
        stages.insert(
            QString::fromLatin1(ledger::stage_names[stage].data(),
                                static_cast<qsizetype>(ledger::stage_names[stage].size())),
            QJsonObject{{"covered", snapshot.covered[stage]}, {"metrics", metrics}});
    }
    return {{"run_id", QString::fromStdString(snapshot.identity.run_id)},
            {"frames", frames},
            {"frame_trace_complete", snapshot.frame_trace_complete},
            {"document_id", QString::fromStdString(snapshot.identity.document_id)},
            {"document_epoch", QString::fromStdString(snapshot.identity.document_epoch)},
            {"base_revision", QString::number(snapshot.identity.base_revision)},
            {"stages", stages}};
}
QJsonObject normalize_server(QJsonObject server) {
#ifdef QCAE_C3_QT_SDK_MANIFEST
    const auto source = server.take("qt_source_observation_json");
    const auto parsed = QJsonDocument::fromJson(source.toString().toUtf8());
    require(parsed.isObject(), "The engine source observer fragment is malformed");
    server.insert("qt_source_observation", parsed.object());
#ifdef QCAE_C3_HARFBUZZ_SDK_MANIFEST
    const auto hb = QJsonDocument::fromJson(
        server.take("harfbuzz_source_observation_json").toString().toUtf8());
    require(hb.isObject(), "The engine HarfBuzz observer fragment is malformed");
    server.insert("harfbuzz_source_observation", hb.object());
#endif
#endif
    auto stages = server.value("stages").toObject();
    for (auto stage = stages.begin(); stage != stages.end(); ++stage) {
        auto detail = stage.value().toObject();
        auto metrics = detail.value("metrics").toObject();
        for (auto metric = metrics.begin(); metric != metrics.end(); ++metric)
            if (metric.value() == "unmeasured")
                metric.value() = QJsonValue::Null;
        detail.insert("metrics", metrics);
        stage.value() = detail;
    }
    server.insert("stages", stages);
    return server;
}
std::optional<std::uint64_t>
metric(const QJsonObject& fragment, const char* stage, const char* key) {
    const auto value = fragment.value("stages")
                           .toObject()
                           .value(stage)
                           .toObject()
                           .value("metrics")
                           .toObject()
                           .value(key);
    if (!value.isString())
        return {};
    bool valid = false;
    const auto result = value.toString().toULongLong(&valid);
    return valid ? std::optional<std::uint64_t>(result) : std::nullopt;
}
std::uint64_t known_sum(const QJsonObject& fragment, const char* key) {
    std::uint64_t result = 0;
    for (const auto stage : ledger::stage_names)
        if (const auto value = metric(fragment, std::string(stage).c_str(), key))
            result += *value;
    return result;
}
bool covered(const QJsonObject& fragment, const QString& stage) {
    return fragment.value("stages").toObject().value(stage).toObject().value("covered").toBool();
}
void wait_for_render(QMainWindow& window, const QJsonObject& info) {
    auto* viewport = window.findChild<VtkView*>();
    require(viewport, "Actual desktop has no VTK viewport");
    auto* ui_client = window.findChild<DesktopClient*>();
    require(ui_client, "Actual desktop has no IPC client");
    try {
        wait_until(
            [&] {
                const auto version = viewport->installedVersion();
                return version &&
                       version->document.id.value ==
                           info.value("document_id").toString().toStdString() &&
                       version->document.epoch.value ==
                           info.value("document_epoch").toString().toStdString() &&
                       version->revision == info.value("revision").toString().toULongLong() &&
                       window.property("treeDocumentId").toString() ==
                           info.value("document_id").toString() &&
                       window.property("treeDocumentEpoch").toString() ==
                           info.value("document_epoch").toString() &&
                       window.property("treeRevision").toString() ==
                           info.value("revision").toString() &&
                       ui_client->pendingRequests() == 0 && desktop_pipeline_idle(window);
            },
            "desktop resource decode, VTK installation and rendering",
            120000);
    } catch (const std::exception&) {
        const auto version = viewport->installedVersion();
        const auto actual = version ? std::to_string(version->revision) : "none";
        throw std::runtime_error(
            "Desktop completion stalled: target revision=" +
            info.value("revision").toString().toStdString() + "; VTK revision=" + actual +
            "; tree revision=" + window.property("treeRevision").toString().toStdString() +
            "; tree document=" + window.property("treeDocumentId").toString().toStdString() +
            "; tree epoch=" + window.property("treeDocumentEpoch").toString().toStdString() +
            "; pending requests=" + std::to_string(ui_client->pendingRequests()) +
            "; pipeline idle=" + (desktop_pipeline_idle(window) ? "true" : "false") +
            "; pending camera=" + (viewport->pendingCameraUpdate() ? "true" : "false"));
    }
    // Drain queued reply callbacks after all socket requests have completed.
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}

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
QJsonObject sample(DesktopClient& observer,
                   QMainWindow& window,
                   std::size_t size,
                   std::size_t index,
                   bool node_edit,
                   qint64 engine_process_id) {
    const auto before = data(call(observer, "project.current"));
    wait_for_render(window, before);
    const auto run_id =
        QString("e2e-%1-%2-%3").arg(size).arg(node_edit ? "node" : "material").arg(index);
    std::cerr << run_id.toStdString() << " begin\n" << std::flush;
    call(
        observer,
        "test.ledger.begin",
        {{"run_id", run_id}, {"operation", node_edit ? "node.move" : "material.set_young_modulus"}},
        context(before));
    // Background work while the test-only arm request was in flight remains
    // outside the sample. Wait for the real display before its triggering edit.
    wait_for_render(window, before);
    const auto client = std::make_shared<ledger::OperationLedger>(
        ledger::Identity{run_id.toStdString(),
                         before.value("document_id").toString().toStdString(),
                         before.value("document_epoch").toString().toStdString(),
                         before.value("revision").toString().toULongLong()});
#ifdef QCAE_C3_QT_SDK_MANIFEST
    c3_qt_source::bridge().begin(run_id.toStdString());
#ifdef QCAE_C3_HARFBUZZ_SDK_MANIFEST
    c3_qt_source::harfbuzz_bridge().begin(run_id.toStdString());
#endif
#endif
    ledger::activate(client);
    QJsonObject parameters;
    QString operation;
    if (node_edit) {
        operation = transport::json_ledger::from_utf8("node.move");
        parameters = {
            {"entity_id", fixed_id("node", size / 2)},
            {"position_mm",
             QJsonArray{static_cast<double>(size / 2), static_cast<double>(index + 1), 0.0}}};
    } else {
        operation = transport::json_ledger::from_utf8("material.set_young_modulus");
        parameters = {{"entity_id", transport::json_ledger::from_utf8("material-locality-0001")},
                      {"young_modulus",
                       QJsonObject{{"value", 211000.0 + index * 1000.0},
                                   {"unit", transport::json_ledger::from_utf8("MPa")}}}};
    }
    if (node_edit) {
        transport::json_ledger::ObjectCopies values;
        for (const auto& value : parameters.value("position_mm").toArray())
            values.append(value);
        transport::json_ledger::object(parameters, {"entity_id", "position_mm"});
    } else {
        transport::json_ledger::object(parameters.value("young_modulus").toObject(),
                                       {"value", "unit"});
        transport::json_ledger::object(parameters, {"entity_id", "young_modulus"});
    }
    QElapsedTimer edit_timer;
    edit_timer.start();
    const auto receipt = data(call(observer, operation, parameters, context(before, run_id)));
    const auto commit_ack_ms = static_cast<double>(edit_timer.nsecsElapsed()) / 1000000.0;
    auto after = before;
    after.insert("revision", receipt.value("current_revision"));
    require(after.value("revision").toString().toULongLong() ==
                before.value("revision").toString().toULongLong() + 1,
            "Measured operation did not commit exactly one revision");
    wait_for_render(window, after);
    const auto derived_sync_ms = static_cast<double>(edit_timer.nsecsElapsed()) / 1000000.0;
    require(std::isfinite(commit_ack_ms) && std::isfinite(derived_sync_ms) && commit_ack_ms >= 0 &&
                derived_sync_ms >= commit_ack_ms,
            "Edit response timing is not a valid monotonic interval");
    const auto server =
        normalize_server(data(call(observer, "test.ledger.end", {}, context(after), true)));
    const auto local = snapshot_json(client->snapshot());
    const auto ui_sdk = sdk_snapshot_json(desktop_sdk_copy_observation(window));
    const auto vtk_sdk = sdk_snapshot_json(window.findChild<VtkView*>()->sdkCopyObservation());
#ifdef QCAE_C3_QT_SDK_MANIFEST
    const auto qt_source = c3_qt_source::bridge().snapshot();
    require(qt_source.value("run_id") == run_id && qt_source.value("collector_complete").toBool(),
            "The SDK source collector lost its operation identity or audited sites");
    const auto server_source = server.value("qt_source_observation").toObject();
    require(server_source.value("run_id") == run_id &&
                server_source.value("collector_complete").toBool() &&
                server_source.value("observer_manifest") == qt_source.value("observer_manifest"),
            "The engine and desktop SDK source fragments have different identities or manifests");
#ifdef QCAE_C3_HARFBUZZ_SDK_MANIFEST
    const auto hb_source = c3_qt_source::harfbuzz_bridge().snapshot();
    const auto server_hb_source = server.value("harfbuzz_source_observation").toObject();
    require(hb_source.value("run_id") == run_id && hb_source.value("collector_complete").toBool() &&
                server_hb_source.value("run_id") == run_id &&
                server_hb_source.value("collector_complete").toBool() &&
                server_hb_source.value("observer_manifest") == hb_source.value("observer_manifest"),
            "The HarfBuzz fragments have different identities, manifests or missing sites");
#endif
#endif
    const auto memory = c3_process_observation::peak_rss();
    const auto engine_memory = server.value("process_memory").toObject();
    require(memory.bytes && *memory.bytes > 0 &&
                engine_memory.value("process_id").toString() ==
                    QString::number(engine_process_id) &&
                engine_memory.value("peak_rss_bytes").toString().toULongLong() > 0,
            "Peak memory must be observed independently in the actual GUI and engine processes");
    const QJsonObject desktop_memory{{"process_id", QString::number(memory.process_id)},
                                     {"peak_rss_bytes", QString::number(*memory.bytes)},
                                     {"source", "getrusage(RUSAGE_SELF)"},
                                     {"scope", "process_lifetime_high_water"}};
    const auto vtk_widget = window.findChild<QVTKOpenGLNativeWidget*>();
    require(vtk_widget && vtk_widget->renderWindow(), "Actual framebuffer is missing");
    const auto framebuffer = vtk_widget->renderWindow()->GetSize();
    const QJsonObject experience{
        {"commit_ack_ms", commit_ack_ms},
        {"derived_sync_ms", derived_sync_ms},
        {"timing_boundary",
         "client edit dispatch through reply and actual desktop render completion"},
        {"derived_sync_includes_commit_ack", true},
        {"memory", QJsonObject{{"desktop", desktop_memory}, {"engine", engine_memory}}},
        {"framebuffer_pixels", QJsonArray{framebuffer[0], framebuffer[1]}},
        {"device_pixel_ratio", vtk_widget->devicePixelRatioF()}};
    for (const auto* key : {"run_id", "document_id", "document_epoch", "base_revision"})
        require(server.value(key) == local.value(key),
                "Ledger fragments have unrelated run identities");

    QJsonArray violations, missing;
    const auto assert_bound = [&](std::uint64_t actual, std::uint64_t limit, const char* name) {
        if (actual > limit)
            violations.append(QString::fromLatin1(name));
    };
    const auto limit = node_edit ? node_limit : material_limit;
    const auto payload_copy_bytes =
        known_sum(server, "model_copy_bytes") + known_sum(local, "model_copy_bytes");
    // The instrumented SQLite buffer count includes SQLITE_TRANSIENT binds.
    // Preserve the separately audited bind count as a subset, not another copy.
    const auto library_copy_bytes = known_sum(server, "library_internal_copy_bytes") +
                                    known_sum(local, "library_internal_copy_bytes") +
                                    known_sum(server, "json_object_copy_bytes") +
                                    known_sum(local, "json_object_copy_bytes");
    const auto driver_bind_bytes = known_sum(server, "driver_bind_copy_bytes");
    const auto driver_internal_bytes = known_sum(server, "driver_internal_copy_bytes");
    const bool internal_includes_bind = server.value("sqlite_internal_includes_bind").toBool();
    const auto driver_copy_bytes =
        driver_internal_bytes + (internal_includes_bind ? 0 : driver_bind_bytes);
    const auto copy_bytes = payload_copy_bytes + library_copy_bytes + driver_copy_bytes;
    const auto metadata_bytes =
        known_sum(server, "metadata_copy_bytes") + known_sum(local, "metadata_copy_bytes");
    const auto reference_bytes = known_sum(server, "reference_descriptor_copy_bytes") +
                                 known_sum(local, "reference_descriptor_copy_bytes");
    const auto encoded_bytes =
        known_sum(server, "encoded_bytes") + known_sum(local, "encoded_bytes");
    const auto sent = known_sum(server, "socket_bytes") + known_sum(local, "socket_bytes");
    const auto server_tx = metric(server, "socket_send", "socket_bytes");
    const auto server_rx = metric(server, "socket_receive", "socket_bytes");
    const auto client_tx = metric(local, "socket_send", "socket_bytes");
    const auto client_rx = metric(local, "socket_receive", "socket_bytes");
    const auto frame_bytes = [](const QJsonObject& fragment, const char* stage) {
        std::uint64_t bytes{};
        for (const auto& item : fragment.value("frames").toArray()) {
            const auto frame = item.toObject();
            if (frame.value("stage") == QLatin1StringView(stage))
                bytes += frame.value("bytes").toString().toULongLong();
        }
        return bytes;
    };
    if (!server.value("frame_trace_complete").toBool() ||
        !local.value("frame_trace_complete").toBool())
        missing.append("complete actual frame attribution");
    if (frame_bytes(server, "socket_send") != server_tx.value_or(UINT64_MAX) ||
        frame_bytes(server, "socket_receive") != server_rx.value_or(UINT64_MAX) ||
        frame_bytes(local, "socket_send") != client_tx.value_or(UINT64_MAX) ||
        frame_bytes(local, "socket_receive") != client_rx.value_or(UINT64_MAX))
        violations.append("decoded/sent frame lengths equal actual stream bytes");
    if (!server_tx || !server_rx || !client_tx || !client_rx || *server_tx != *client_rx ||
        *server_rx != *client_tx)
        violations.append("all actual socket frames accounted in both processes");
    assert_bound(copy_bytes + metadata_bytes, limit, "total logical copy byte cap");
    assert_bound(metadata_bytes, metadata_limit, "transaction metadata byte cap");
    assert_bound(encoded_bytes, limit, "all encoder output byte cap");
    assert_bound(metric(server, "sqlite", "batch_payload_bytes").value_or(UINT64_MAX),
                 limit,
                 "complete StoreBatch payload including keys and deletes");
    // Count each actual socket payload once; the receiver verifies the same bytes.
    assert_bound(server_tx.value_or(UINT64_MAX) + client_tx.value_or(UINT64_MAX),
                 limit,
                 "actual IPC payload byte cap");
    if (metric(server, "application", "changed_records") != std::optional<std::uint64_t>(1))
        violations.append("exactly one changed model record");
    if (known_sum(server, "full_model_serializations") ||
        known_sum(server, "full_model_materializations"))
        violations.append("no whole-model encoding or materialization");
    for (const auto stage : ledger::stage_names) {
        const auto name = QString::fromLatin1(stage.data(), static_cast<qsizetype>(stage.size()));
        if (!covered(server, name) && !covered(local, name))
            missing.append(name);
    }
    const auto require_metric =
        [&](const QJsonObject& fragment, const char* process, const char* stage, const char* key) {
            if (!metric(fragment, stage, key))
                missing.append(QString("%1.%2.%3").arg(process, stage, key));
        };
    for (const auto* stage :
         {"application", "records", "projection", "render_encode", "resource_publish"}) {
        require_metric(server, "engine", stage, "model_copy_bytes");
        require_metric(server, "engine", stage, "metadata_copy_bytes");
    }
    for (const auto* stage : {"socket_send", "socket_receive"})
        for (const auto* key : {"model_copy_bytes", "library_internal_copy_bytes"}) {
            require_metric(server, "engine", stage, key);
            require_metric(local, "desktop", stage, key);
        }
    for (const auto* stage : {"resource_decode", "render_decode", "vtk_apply"})
        for (const auto* key : {"model_copy_bytes", "metadata_copy_bytes"})
            require_metric(local, "desktop", stage, key);
    require_metric(server, "engine", "resource_publish", "library_internal_copy_bytes");
    require_metric(local, "desktop", "resource_decode", "library_internal_copy_bytes");
    require_metric(server, "engine", "socket_send", "json_object_copy_bytes");
    require_metric(local, "desktop", "socket_send", "json_object_copy_bytes");
    require_metric(local, "desktop", "vtk_apply", "library_internal_copy_bytes");
    require_metric(local, "desktop", "vtk_apply", "gpu_upload_bytes");
    const auto gpu_upload_bytes = metric(local, "vtk_apply", "gpu_upload_bytes");
    if (gpu_upload_bytes && (node_edit ? *gpu_upload_bytes == 0 : *gpu_upload_bytes != 0))
        violations.append(node_edit ? "changed coordinates have an observed VBO upload"
                                    : "empty material delta performs no VBO upload");
    require_metric(server, "engine", "sqlite", "encoded_bytes");
    for (const auto* key : {"batch_payload_bytes",
                            "batch_key_bytes",
                            "batch_delete_count",
                            "driver_bind_copy_bytes",
                            "driver_internal_copy_bytes",
                            "sql_fullscan_steps",
                            "sql_vm_steps",
                            "physical_wal_write_bytes",
                            "physical_database_write_bytes"})
        require_metric(server, "engine", "sqlite", key);
    for (const auto* key : {"full_model_serializations", "full_model_materializations"})
        require_metric(server, "engine", "records", key);
    const bool complete = missing.isEmpty();
    std::cerr << run_id.toStdString() << " collected; copy=" << copy_bytes
              << "; metadata=" << metadata_bytes << "; missing=" << missing.size()
              << "; violations=" << violations.size() << '\n'
              << std::flush;
    return {{"run_id", run_id},
            {"model_size", static_cast<qint64>(size)},
            {"sample", static_cast<qint64>(index)},
            {"kind", node_edit ? "node" : "material"},
            {"base_revision", before.value("revision")},
            {"revision", after.value("revision")},
            {"server", server},
            {"desktop", local},
            {"editing_experience", experience},
#ifdef QCAE_C3_QT_SDK_MANIFEST
            {"qt_source_observation", qt_source},
#ifdef QCAE_C3_HARFBUZZ_SDK_MANIFEST
            {"harfbuzz_source_observation", hb_source},
#endif
#endif
            {"sdk_observation",
             QJsonObject{{"desktop", ui_sdk},
                         {"vtk", vtk_sdk},
                         {"same_run",
                          ui_sdk.value("run_id") == run_id && vtk_sdk.value("run_id") == run_id}}},
            {"measured_copy_bytes", QString::number(copy_bytes)},
            {"adapter_payload_copy_bytes", QString::number(payload_copy_bytes)},
            {"library_internal_copy_bytes", QString::number(library_copy_bytes)},
            {"sqlite_driver_copy_bytes", QString::number(driver_copy_bytes)},
            {"sqlite_bind_copy_subset_bytes", QString::number(driver_bind_bytes)},
            {"sqlite_internal_includes_bind", internal_includes_bind},
            {"measured_metadata_bytes", QString::number(metadata_bytes)},
            {"reference_descriptor_copy_bytes", QString::number(reference_bytes)},
            {"raw_conservative_copy_bytes",
             QString::number(copy_bytes + metadata_bytes + reference_bytes)},
            {"contract_copy_bytes", QString::number(copy_bytes + metadata_bytes)},
            {"measured_encoded_bytes", QString::number(encoded_bytes)},
            {"vtk_gpu_upload_argument_bytes",
             gpu_upload_bytes ? QJsonValue(QString::number(*gpu_upload_bytes)) : QJsonValue()},
            {"socket_endpoint_bytes", QString::number(sent)},
            {"observed_bounds_passed", violations.isEmpty()},
            {"violations", violations},
            {"missing_coverage", missing},
            {"complete_coverage", complete},
            {"passed", complete && violations.isEmpty()}};
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 3 && argc != 4)
        return 2;
    const auto engine_path = QString::fromLocal8Bit(argv[1]);
    const auto output = QString::fromLocal8Bit(argv[2]);
    std::vector<std::size_t> selected_sizes(sizes.begin(), sizes.end());
    if (argc == 4) {
        bool valid = false;
        const auto size = QString::fromLocal8Bit(argv[3]).toULongLong(&valid);
        if (!valid || std::find(sizes.begin(), sizes.end(), size) == sizes.end())
            return 2;
        selected_sizes = {static_cast<std::size_t>(size)};
    }
    QTemporaryDir settings(QDir::temp().filePath("qcae-ledger-settings-XXXXXX"));
    if (!settings.isValid())
        return 3;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    QSurfaceFormat::setDefaultFormat(QVTKOpenGLNativeWidget::defaultFormat());
    QApplication app(argc, argv);
    QJsonArray samples;
    QString error;
    try {
#ifdef QCAE_C3_QT_SDK_MANIFEST
        (void)c3_qt_source::bridge();
#ifdef QCAE_C3_HARFBUZZ_SDK_MANIFEST
        (void)c3_qt_source::harfbuzz_bridge();
#endif
#endif
        for (const auto size : selected_sizes) {
            std::cerr << "Preparing actual desktop/SQLite fixture N=" << size << '\n' << std::flush;
            QTemporaryDir temp(QDir::temp().filePath("qcae-ledger-e2e-XXXXXX"));
            require(temp.isValid(), "Cannot create independent SQLite fixture directory");
            const auto endpoint = temp.filePath("engine.sock");
            const auto workspace = temp.filePath("work.sqlite");
            Engine engine;
            auto environment = QProcessEnvironment::systemEnvironment();
            environment.insert("QCAE_LEDGER_VFS", "1");
            engine.process.setProcessEnvironment(environment);
            engine.process.setProgram(engine_path);
            engine.process.setArguments({"--socket", endpoint, "--workspace", workspace});
            engine.process.start();
            require(engine.process.waitForStarted(5000), "Cannot start trusted test engine");
            DesktopClient observer({endpoint, workspace, {}, false, 30000});
            observer.start();
            wait_until([&] { return observer.ready(); }, "actual engine handshake");
            call(observer,
                 "project.create",
                 {{"name", "C3 end-to-end ledger"}},
                 {{"idempotency_key", "create"}});
            auto info = data(call(observer, "project.current"));
            {
                DesktopClient setup({endpoint, workspace, {}, false, 180000});
                setup.start();
                wait_until([&] { return setup.ready(); }, "trusted fixture setup connection");
                call(setup,
                     "test.locality.seed",
                     {{"node_count", static_cast<int>(size)}},
                     context(info, "seed"),
                     false,
                     180000);
            }
            info = data(call(observer, "project.current"));
            std::unique_ptr<QMainWindow> window(
                create_desktop_window({endpoint, workspace, {}, false, 30000}));
            window->resize(1400, 850);
            window->show();
            require(QTest::qWaitForWindowExposed(window.get()),
                    "Actual desktop window is not exposed");
            wait_for_render(*window, info);
            for (const bool node_edit : {true, false})
                for (std::size_t index = 0; index < 10; ++index) {
                    samples.append(sample(
                        observer, *window, size, index, node_edit, engine.process.processId()));
                    const auto edited = data(call(observer, "project.current"));
                    call(observer,
                         "history.undo",
                         {},
                         context(edited, QString("restore-%1-%2").arg(node_edit).arg(index)));
                    const auto restored = data(call(observer, "project.current"));
                    wait_for_render(*window, restored);
                }
            window.reset();
        }
    } catch (const std::exception& failure) {
        ledger::activate({});
        error = QString::fromUtf8(failure.what());
    }
    const QJsonObject report{
        {"schema_version", "c3-same-run-ledger-v1"},
        {"measurement_boundary",
         "actual IPC command through SQLite and desktop resource decode to VTK render completion"},
        {"control_boundary",
         "begin request/response excluded; end request included; end response excluded"},
        {"node_limit_bytes", static_cast<qint64>(node_limit)},
        {"material_limit_bytes", static_cast<qint64>(material_limit)},
        {"metadata_limit_bytes", static_cast<qint64>(metadata_limit)},
        {"samples", samples},
        {"sample_count", samples.size()},
        {"error", error},
        {"complete_sk12_passed", false},
        {"c4_start_requires_complete_sdk_copy_coverage", false},
        {"editing_experience_definition",
         "Actual monotonic dispatch-to-commit and dispatch-to-render durations; getrusage peak RSS "
         "is each process lifetime high water, not an isolated edit allocation or a current RSS. "
         "The engine is fresh per model size; the desktop process spans the three model sizes. "
         "No dedicated edit/RSS acceptance threshold is inferred from viewport-input/task ACK "
         "limits."},
        {"counter_coverage_definition",
         "Per-sample complete_coverage checks necessary counter presence/unknown; it does not "
         "declare complete SDK source audit"},
        {"whole_pipeline_owned_copy_coverage", "unknown"},
        {"owned_copy_audit_gaps",
         QJsonArray{"Qt widget/text document owned buffers",
                    "VTK mapper CPU packed vertex/index payload",
                    "remaining library-owned temporary buffers pending source audit"}},
        {"sqlite_physical_definition",
         "successful SQLite VFS xWrite requested bytes; storage-device write amplification not "
         "measured"}};
    QSaveFile file(output);
    if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(report).toJson()) < 0 ||
        !file.commit())
        return 4;
    if (!error.isEmpty()) {
        std::cerr << error.toStdString() << '\n';
        return 1;
    }
    std::cout << samples.size()
              << " same-run samples recorded; complete SK-12 coverage remains unverified\n";
    return samples.size() == static_cast<qsizetype>(selected_sizes.size() * 20) ? 0 : 1;
}
