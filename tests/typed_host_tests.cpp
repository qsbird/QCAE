#include "qcae/ipc_api.hpp"
#include "qcae/typed_host.hpp"
#include "runtime_test_support.hpp"
#include <QJsonArray>
#include <QJsonDocument>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace qcae::operations {
// Test contribution only; no synthetic profile operation is registered by production.
struct ProfileWriteInput {};
template <> struct InputTraits<ProfileWriteInput> {
    static constexpr std::string_view schema_id = "qcae.operation.test.profile_write.v1";
    static OperationDefinition definition() {
        return {"test.profile_write",
                1,
                std::string(schema_id),
                OperationEffect::document_write,
                {true, true, true, true, true},
                {}};
    }
    static Result<ProfileWriteInput> from_value(const Value& value) {
        const auto fields =
            wire::object_fields(value, std::span<const std::string_view>{}, "input");
        if (!fields.ok())
            return {fields.status, {}, fields.error};
        return {Status::success, ProfileWriteInput{}, {}};
    }
};
} // namespace qcae::operations

namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
void retained_outcome_without_handler(qcae::MemoryApplication& application,
                                      const qcae::Caller& caller,
                                      QJsonObject request,
                                      const QJsonObject& original_receipt) {
    using namespace qcae;
    ipc::TypedHost reduced(
        application.record_application(),
        [](const auto&) { return true; },
        [](operations::OperationRegistry&, RecordApplication&, std::function<TaskService&()>) {
            return Result<bool>{Status::success, true, {}};
        });
    require(!reduced.supports("test.profile_write"), "removed contributor is not executable");
    for (const auto& capability : reduced.capabilities())
        require(capability.toObject().value("name") != "test.profile_write",
                "removed handler must not advertise availability");
    const auto before = runtime_test::good(application.current_document());
    const auto rejected =
        ipc::dispatch(application, request, caller, nullptr, nullptr, nullptr, &reduced);
    require(rejected.value("error").toObject().value("code") == "UNSUPPORTED_CAPABILITY",
            "removed handler rejects a new invocation");
    request.remove("requested_version");
    request.remove("expected_profile");
    request.insert("operation", "operations.get");
    QJsonObject parameters{{"lookup_scope", "document"},
                           {"original_operation", "test.profile_write"},
                           {"idempotency_key", "profile-write"}};
    request.insert("parameters", parameters);
    auto lookup = [&](const Caller& reader) {
        return ipc::dispatch(application, request, reader, nullptr, nullptr, nullptr, &reduced);
    };
    const auto fact = lookup(caller);
    const auto receipt = fact.value("data").toObject();
    require(fact.value("status") == "success" && receipt.value("replayed") == true &&
                receipt.value("transaction_id") == original_receipt.value("transaction_id") &&
                receipt.value("entity_id") == original_receipt.value("entity_id"),
            "removed handler cannot hide retained action facts");
    require(lookup(Caller{"another-caller"}).value("error").toObject().value("code") ==
                "ENTITY_NOT_FOUND",
            "retained action lookup is caller scoped");
    parameters.insert("original_operation", "unknown.action");
    request.insert("parameters", parameters);
    require(lookup(caller).value("error").toObject().value("code") == "ENTITY_NOT_FOUND",
            "unrecorded operation returns not found independently of current capabilities");
    const auto after = runtime_test::good(application.current_document());
    require(after.revision == before.revision && after.material_count == before.material_count,
            "retained lookup and unavailable invocation have no document effects");
}
void version_and_profile_contract() {
    using namespace qcae;
    using namespace qcae::operations;
    MemoryApplication application;
    const Caller caller{"profile-contract"};
    auto installed = runtime_test::profile;
    unsigned calls{};
    ipc::TypedHost host(
        application.record_application(),
        [](const auto&) { return true; },
        [&](OperationRegistry& registry, RecordApplication& app, std::function<TaskService&()>) {
            return registry.register_typed<ProfileWriteInput>(
                InputTraits<ProfileWriteInput>::definition(),
                [&](const OperationContext& ctx, const ProfileWriteInput&) -> Result<Value> {
                    ++calls;
                    require(ctx.requested_version == 1, "requested version reaches contributor");
                    const auto profile = *ctx.expected_profile;
                    const auto signature = runtime_test::good(canonical_value(Value(
                        Value::Object{{"profile_id", Value(profile.profile_id)},
                                      {"profile_version", Value(profile.profile_version)},
                                      {"definition_digest", Value(profile.definition_digest)}})));
                    const auto result = app.execute(
                        ctx.caller,
                        {*ctx.document, *ctx.expected_revision},
                        "test.profile_write",
                        signature,
                        [&, profile, signature](const DocumentView& view,
                                                const RecordIdentityAllocator& allocate)
                            -> Result<RecordPreparedOperation> {
                            // Applicability is checked only for a new candidate: retained replay
                            // facts are resolved by execute before this callback is entered.
                            if (profile != installed)
                                return {Status::failed,
                                        {},
                                        Diagnostic{ErrorCode::schema_unsupported,
                                                   "Expected profile does not match the target.",
                                                   "expected_profile"}};
                            const auto identity = allocate();
                            EditSession edit(view);
                            edit.put(records::Material{identity, "Profile probe", 42, {}});
                            return {
                                Status::success,
                                RecordPreparedOperation{
                                    edit.prepare(), "Profile probe", identity, signature, 42, true},
                                {}};
                        },
                        ctx.idempotency_key);
                    if (!result.ok())
                        return {result.status, {}, result.error};
                    return {Status::success, change_receipt_value(*result.value), {}};
                });
        });
    require(host.supports("test.profile_write") && !host.supports("material.create"),
            "explicit contributor replaces default registrations");
    const auto descriptor = host.capabilities().first().toObject();
    require(descriptor.value("available") == true &&
                descriptor.value("requires_expected_profile") == true &&
                descriptor.value("version") == 1 && descriptor.value("fields").toArray().isEmpty(),
            "profile/parameterless capability derives from actual handler");
    const auto info = runtime_test::good(application.create_document(caller, "Profile", "create"));
    QJsonObject request{{"api_version", "1.1"},
                        {"request_id", "contract"},
                        {"operation", "test.profile_write"},
                        {"parameters", QJsonObject{}},
                        {"document_id", QString::fromStdString(info.document.id.value)},
                        {"document_epoch", QString::fromStdString(info.document.epoch.value)},
                        {"expected_revision", "0"},
                        {"idempotency_key", "profile-write"},
                        {"requested_version", 1}};
    auto invoke = [&] {
        const auto parsed = QJsonDocument::fromJson(QJsonDocument(request).toJson()).object();
        return ipc::dispatch(application, parsed, caller, nullptr, nullptr, nullptr, &host);
    };
    auto unchanged = [&] {
        const auto current = runtime_test::good(application.current_document());
        require(current.revision == 0 && current.material_count == 0,
                "contract rejection has no revision or document effects");
        require(runtime_test::good(application.history(info.document)).items.empty(),
                "contract rejection has no history effects");
    };
    const auto missing = invoke();
    require(missing.value("status") == "needs_input" &&
                missing.value("error").toObject().value("field") == "expected_profile",
            "missing profile is a discoverable context requirement");
    unchanged();
    QJsonObject expected{
        {"profile_id", QString::fromStdString(installed.profile_id)},
        {"profile_version", QString::fromStdString(installed.profile_version)},
        {"definition_digest", QString::fromStdString(installed.definition_digest)}};
    request.insert("expected_profile", expected);
    for (const QJsonValue& version : {QJsonValue(2),
                                      QJsonValue(0),
                                      QJsonValue(-1),
                                      QJsonValue(1.5),
                                      QJsonValue(4294967296.0),
                                      QJsonValue("1"),
                                      QJsonValue(true)}) {
        request.insert("requested_version", version);
        const auto rejected = invoke();
        require(rejected.value("status") == "failed" &&
                    rejected.value("error").toObject().value("code") ==
                        (version == QJsonValue(2) ? "SCHEMA_UNSUPPORTED" : "INVALID_INPUT"),
                "wrong or malformed requested version rejected");
        unchanged();
    }
    require(calls == 0, "missing context/version never reaches typed handler");
    request.insert("requested_version", 1);
    for (const char* field : {"profile_id", "profile_version", "definition_digest"}) {
        auto wrong = expected;
        wrong.insert(field, "different");
        request.insert("expected_profile", wrong);
        const auto rejected = invoke();
        require(rejected.value("error").toObject().value("code") == "SCHEMA_UNSUPPORTED" &&
                    rejected.value("error").toObject().value("field") == "expected_profile",
                "all immutable profile identity fields are checked");
        unchanged();
    }
    auto extra = expected;
    extra.insert("unexpected", 1);
    request.insert("expected_profile", extra);
    require(invoke().value("error").toObject().value("code") == "INVALID_INPUT",
            "unknown profile member rejected");
    unchanged();
    auto partial = expected;
    partial.remove("definition_digest");
    request.insert("expected_profile", partial);
    require(invoke().value("status") == "needs_input", "incomplete profile needs input");
    unchanged();
    request.insert("expected_profile", expected);
    const auto committed = invoke();
    require(committed.value("status") == "success", "matching profile commits");
    const auto receipt = committed.value("data").toObject();
    require(receipt.value("committed_revision") == "1", "single profile write");
    installed.definition_digest = "new-profile-definition";
    request.remove("requested_version");
    const auto retry = invoke();
    const auto replay = retry.value("data").toObject();
    require(retry.value("status") == "success" && replay.value("replayed") == true &&
                replay.value("entity_id") == receipt.value("entity_id") &&
                replay.value("transaction_id") == receipt.value("transaction_id"),
            "legacy omitted version and old profile retry return retained committed fact");
    request.insert("idempotency_key", "new-request");
    request.insert("expected_revision", "1");
    require(invoke().value("error").toObject().value("code") == "SCHEMA_UNSUPPORTED",
            "new request cannot use old profile after definition changes");
    expected.insert("definition_digest", QString::fromStdString(installed.definition_digest));
    request.insert("expected_profile", expected);
    request.insert("expected_revision", "0");
    request.insert("idempotency_key", "profile-write");
    require(invoke().value("error").toObject().value("code") == "IDEMPOTENCY_KEY_CONFLICT",
            "same key with a different profile cannot reuse old outcome");
    const auto current = runtime_test::good(application.current_document());
    require(current.revision == 1 && current.material_count == 1,
            "profile errors/retries never duplicate or revise committed work");
    retained_outcome_without_handler(application, caller, request, receipt);
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
        envelope.insert("requested_version", 2);
        require(invoke("task.reconcile", {}, "unsupported-control-version")
                        .value("error")
                        .toObject()
                        .value("code") == "SCHEMA_UNSUPPORTED",
                "task control also checks its discovered operation version");
        require(invoke("geometry.create_line",
                       {{"start_mm", QJsonArray{0, 0, 0}}, {"end_mm", QJsonArray{1000, 0, 0}}},
                       "unsupported-line-version")
                        .value("error")
                        .toObject()
                        .value("code") == "SCHEMA_UNSUPPORTED",
                "default feature handler rejects an unsupported contract version");
        require(runtime_test::good(application.current_document()).revision == 0,
                "version rejection leaves default application revision unchanged");
        envelope.remove("requested_version");
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
        version_and_profile_contract();
        std::cout << "PASS typed JSON transport, mesh idempotency and task fault recovery\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
