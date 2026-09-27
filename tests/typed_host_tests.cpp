#include "qcae/ipc_api.hpp"
#include "qcae/typed_host.hpp"
#include "runtime_test_support.hpp"
#include <QJsonArray>
#include <QJsonDocument>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
struct FaultFixture {
    std::shared_ptr<runtime_test::Store> store = std::make_shared<runtime_test::Store>();
    qcae::RecordApplication app{runtime_test::options(store)};
    qcae::ipc::TypedHost host{app, [](const auto&) { return true; }};
    qcae::Caller caller{"fault-transport"};
    QJsonObject envelope;
    QString geometry;
    FaultFixture() {
        const auto info = runtime_test::good(app.create_document(caller, "Faults", "create"));
        set_context(info);
        const auto line =
            invoke("geometry.create_line",
                   {{"start_mm", QJsonArray{0, 0, 0}}, {"end_mm", QJsonArray{1000, 0, 0}}},
                   "line");
        require(line.value("status") == "success", "fault fixture line");
        geometry = line.value("data").toObject().value("entity_id").toString();
        set_context(runtime_test::good(app.current_document()));
    }
    void set_context(const qcae::DocumentInfo& info) {
        envelope = {{"request_id", "fault-test"},
                    {"document_id", QString::fromStdString(info.document.id.value)},
                    {"document_epoch", QString::fromStdString(info.document.epoch.value)},
                    {"expected_revision", QString::number(info.revision)}};
    }
    QJsonObject invoke(const char* operation, QJsonObject params, const char* key) {
        auto request = envelope;
        request.insert("operation", operation);
        request.insert("parameters", params);
        request.insert("idempotency_key", key);
        return host.dispatch(request, caller);
    }
    QJsonObject start(const char* key) {
        return invoke("mesh.generate_line", {{"geometry_id", geometry}, {"segments", 10}}, key);
    }
    QJsonObject wait(const QJsonValue& task, const char* state) {
        QJsonObject value;
        for (unsigned attempt = 0; attempt < 1000; ++attempt) {
            value = invoke("task.status", {{"task_id", task}}, "status").value("data").toObject();
            if (value.value("state") == QLatin1String(state))
                return value;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        throw std::runtime_error(std::string("Task did not reach ") + state);
    }
};
void definite_task_failure_reconciles() {
    FaultFixture fixture;
    fixture.store->task_write_failure.store(2);
    const auto started = fixture.start("state-write-failure");
    require(started.value("status") == "success", "durable task admission");
    const auto task = started.value("data").toObject().value("task_id");
    fixture.wait(task, "interrupted");
    const auto initial = runtime_test::good(fixture.app.current_document());
    require(!fixture.app
                 .close_document(fixture.caller,
                                 runtime_test::at(initial),
                                 qcae::ClosePolicy::keep_recovery,
                                 "blocked-close")
                 .ok(),
            "unfinished persisted task prevents close until repair");
    const auto epoch = fixture.envelope.value("document_epoch");
    fixture.envelope.insert("document_epoch", "stale-epoch");
    require(fixture.invoke("task.reconcile", {}, "stale").value("error").toObject().value("code") ==
                "DOCUMENT_EPOCH_EXPIRED",
            "reconcile must check the active session");
    fixture.envelope.insert("document_epoch", epoch);
    fixture.store->task_write_failure.store(1);
    require(fixture.invoke("task.reconcile", {}, "repair-failure")
                    .value("error")
                    .toObject()
                    .value("code") == "STORAGE_FAILURE",
            "failed reconciliation must remain visible and retryable");
    require(fixture.invoke("task.reconcile", {}, "repair").value("status") == "success",
            "explicit repair");
    require(runtime_test::good(fixture.app.current_document()).revision == initial.revision,
            "reconcile must not change model revision");
    const auto resumed = fixture.start("after-repair");
    require(resumed.value("status") == "success", "task admission resumes after repair");
    fixture.wait(resumed.value("data").toObject().value("task_id"), "succeeded");
}
void poisoned_lazy_tasks_require_recovery() {
    FaultFixture fixture;
    fixture.store->fault.store(runtime_test::Store::Fault::after_model);
    const auto uncertain = fixture.invoke(
        "material.create",
        {{"name", "Steel"}, {"young_modulus", QJsonObject{{"value", 210000}, {"unit", "MPa"}}}},
        "uncertain-material");
    require(uncertain.value("error").toObject().value("code") == "STORAGE_UNCERTAIN",
            "uncertain write");
    require(fixture.start("before-recovery").value("error").toObject().value("code") ==
                "STORAGE_UNCERTAIN",
            "lazy service initialization must report recovery requirement without an exception");
    require(fixture.invoke("task.reconcile", {}, "unsafe-repair")
                    .value("error")
                    .toObject()
                    .value("code") == "STORAGE_UNCERTAIN",
            "task reconcile must never bypass application recovery");
    fixture.set_context(
        runtime_test::good(fixture.app.recover_document(fixture.caller, "recover")));
    require(fixture.invoke("task.reconcile", {}, "after-recovery").value("status") == "success",
            "recovered service initializes");
    const auto started = fixture.start("recovered-start");
    require(started.value("status") == "success", "admit after application recovery");
    fixture.wait(started.value("data").toObject().value("task_id"), "succeeded");
}
} // namespace

int main() {
    try {
        qcae::MemoryApplication application({}, {}, {}, {qcae::task_row_handler()});
        qcae::ipc::TypedHost host(application.record_application(),
                                  [](const auto&) { return true; });
        const qcae::Caller caller{"transport-test"};
        const auto created = application.create_document(caller, "Typed transport", "create");
        require(created.ok(), "create document");
        QJsonObject envelope{
            {"api_version", "1.1"},
            {"request_id", "typed"},
            {"document_id", QString::fromStdString(created.value->document.id.value)},
            {"document_epoch", QString::fromStdString(created.value->document.epoch.value)},
            {"expected_revision", "0"}};
        auto invoke = [&](const char* operation, QJsonObject parameters, const char* key) {
            auto request = envelope;
            request.insert("operation", operation);
            request.insert("parameters", parameters);
            request.insert("idempotency_key", key);
            // Exercise actual JSON number parsing, not only hand-created Qt values.
            request = QJsonDocument::fromJson(QJsonDocument(request).toJson()).object();
            return qcae::ipc::dispatch(
                application, request, caller, nullptr, nullptr, nullptr, &host);
        };
        const auto line =
            invoke("geometry.create_line",
                   {{"start_mm", QJsonArray{0, 0, 0}}, {"end_mm", QJsonArray{1000, 0, 0}}},
                   "line");
        require(line.value("status") == "success", "geometry dispatch");
        const auto identity = line.value("data").toObject().value("entity_id").toString();
        require(!identity.isEmpty(), "created entity identity");
        envelope.insert("expected_revision", "1");
        for (const auto segments : {10.5, 0.0, -1.0, 4294967296.0, 9223372036854775808.0}) {
            const auto rejected = invoke("mesh.generate_line",
                                         {{"geometry_id", identity}, {"segments", segments}},
                                         "invalid");
            require(rejected.value("status") == "failed", "invalid integer accepted");
            require(rejected.value("error").toObject().value("code") == "INVALID_INPUT",
                    "integer diagnostic");
        }
        const auto started =
            invoke("mesh.generate_line", {{"geometry_id", identity}, {"segments", 10}}, "mesh");
        require(started.value("status") == "success", "JSON integer segment count rejected");
        const auto task = started.value("data").toObject().value("task_id");
        QJsonObject complete;
        for (int index = 0; index < 1000; ++index) {
            complete =
                invoke("task.status", {{"task_id", task}}, "status").value("data").toObject();
            if (complete.value("state") == "succeeded")
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        require(complete.value("state") == "succeeded", "mesh task did not publish");
        const auto summary = invoke("model.summary", {}, "summary").value("data").toObject();
        require(summary.value("node_count").toInt() == 11 &&
                    summary.value("beam_count").toInt() == 10,
                "task must publish one complete 11-node/10-beam candidate");
        const auto retry =
            invoke("mesh.generate_line", {{"geometry_id", identity}, {"segments", 10}}, "mesh");
        require(retry.value("data").toObject().value("task_id") == task,
                "task retry duplicated work");
        envelope.insert("expected_revision", "2");
        require(invoke("history.undo", {}, "undo-mesh").value("status") == "success", "undo mesh");
        envelope.insert("expected_revision", "3");
        require(invoke("history.undo", {}, "undo-line").value("status") == "success",
                "undo geometry");
        envelope.insert("expected_revision", "1");
        const auto undone_retry =
            invoke("mesh.generate_line", {{"geometry_id", identity}, {"segments", 10}}, "mesh");
        require(
            undone_retry.value("status") == "success" &&
                undone_retry.value("data").toObject().value("task_id") == task,
            "accepted task retry must return its fact even after its source geometry is undone");
        const auto after_retry =
            invoke("model.summary", {}, "after-retry").value("data").toObject();
        require(after_retry.value("revision") == "4" &&
                    after_retry.value("node_count").toInt() == 0 &&
                    after_retry.value("geometry_count").toInt() == 0,
                "retry must not recreate undone entities");
        definite_task_failure_reconciles();
        poisoned_lazy_tasks_require_recovery();
        std::cout << "PASS typed JSON transport, mesh idempotency and task fault recovery\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
