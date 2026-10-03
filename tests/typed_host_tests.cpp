#include "qcae/ipc_api.hpp"
#include "qcae/typed_host.hpp"
#include "qcae/operation_inputs.hpp"
#include "../adapters/engine_api/src/typed_json.hpp"
#include "runtime_test_support.hpp"
#include <QJsonArray>
#include <QJsonDocument>
#include <chrono>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

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
struct DiagnosticReportInput {};
template <> struct InputTraits<DiagnosticReportInput> {
    static constexpr std::string_view schema_id = "qcae.operation.test.diagnostic_report.v1";
    static OperationDefinition definition() {
        return {"test.diagnostic_report",
                1,
                std::string(schema_id),
                OperationEffect::read_only,
                {},
                {}};
    }
    static Result<DiagnosticReportInput> from_value(const Value& value) {
        const auto fields =
            wire::object_fields(value, std::span<const std::string_view>{}, "input");
        if (!fields.ok())
            return {fields.status, {}, fields.error};
        return {Status::success, DiagnosticReportInput{}, {}};
    }
};
} // namespace qcae::operations

namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
// The fake persists actual application rows and project payloads; it does not prove SQLite I/O.
struct OpenContractStore final : qcae::IWorkspaceStore, qcae::IRecordStore {
    runtime_test::Store records;
    std::map<std::string, qcae::StoredProject> projects;
    unsigned commits{}, publishes{}, project_reads{};
    std::optional<qcae::StoredWorkspace> load() override {
        return {};
    }
    std::uint64_t commit(std::uint64_t, const std::string&) override {
        throw qcae::StorageError("The record application must use commit_rows");
    }
    qcae::LoadedRows load_rows() override {
        return records.load_rows();
    }
    qcae::BatchReceipt commit_rows(const qcae::StoreBatch& batch) override {
        auto receipt = records.commit_rows(batch);
        ++commits;
        return receipt;
    }
    std::string acquire_project(const std::string& path) override {
        return path;
    }
    void release_projects_except(const std::string&) noexcept override {}
    qcae::StoredProject read_project(const std::string& path) override {
        ++project_reads;
        const auto found = projects.find(path);
        if (found == projects.end())
            throw qcae::StorageError("project_not_found");
        return found->second;
    }
    void publish_project(const std::string& path, const qcae::StoredProject& project) override {
        projects[path] = project;
        ++publishes;
    }
};
struct OpenContractState {
    std::uint64_t generation{};
    std::map<qcae::StoreKey, std::string> rows;
    std::map<std::string, std::pair<std::string, std::string>> projects;
    QJsonObject document, history;
    bool recovery_available{};
    unsigned commits{}, publishes{}, project_reads{};
    bool operator==(const OpenContractState&) const = default;
};
OpenContractState open_contract_state(qcae::MemoryApplication& app,
                                      const OpenContractStore& store,
                                      const qcae::Caller& caller) {
    OpenContractState state;
    state.generation = store.records.generation;
    for (const auto& [key, value] : store.records.rows)
        state.rows.emplace(key, *value);
    for (const auto& [path, project] : store.projects)
        state.projects.emplace(path, std::pair{project.save_token, project.payload});
    const auto current = app.current_document();
    if (current.ok()) {
        state.document = qcae::ipc::info_json(*current.value);
        const QJsonObject request{
            {"api_version", "1.1"},
            {"request_id", "observe-history"},
            {"operation", "history.list"},
            {"parameters", QJsonObject{}},
            {"document_id", QString::fromStdString(current.value->document.id.value)},
            {"document_epoch", QString::fromStdString(current.value->document.epoch.value)}};
        state.history = qcae::ipc::dispatch(app, request, caller);
        require(state.history.value("status") == "success", "observe full history");
    }
    state.recovery_available = app.recovery_available();
    state.commits = store.commits;
    state.publishes = store.publishes;
    state.project_reads = store.project_reads;
    return state;
}
QJsonObject open_request(QJsonObject parameters, const char* key) {
    return {{"api_version", "1.1"},
            {"request_id", "open-contract"},
            {"operation", "project.open"},
            {"parameters", parameters},
            {"idempotency_key", key}};
}
void rejected_open_inputs_are_atomic(qcae::MemoryApplication& app,
                                     OpenContractStore& store,
                                     qcae::ipc::TypedHost& host,
                                     const qcae::Caller& caller,
                                     const char* key) {
    const auto before = open_contract_state(app, store, caller);
    auto reject = [&](QJsonObject request, const char* code, const char* field) {
        // A rejected request must not reserve its host-level key, even after a retry.
        for (int retry = 0; retry < 2; ++retry) {
            const auto wire = QJsonDocument::fromJson(QJsonDocument(request).toJson()).object();
            const auto response =
                qcae::ipc::dispatch(app, wire, caller, nullptr, nullptr, nullptr, &host);
            const auto error = response.value("error").toObject();
            require(response.value("status") == "failed" && error.value("code") == code &&
                        error.value("field") == field,
                    "project.open rejects the exact parameter/version contract");
            require(open_contract_state(app, store, caller) == before,
                    "rejected open leaves every persisted row, payload, generation, history and "
                    "lifecycle unchanged");
        }
    };
    const QJsonObject normal{{"mode", "normal"}, {"path", "/open-contract.qcae"}};
    for (const auto& version : {QJsonValue(0),
                                QJsonValue(-1),
                                QJsonValue(1.5),
                                QJsonValue(4294967296.0),
                                QJsonValue(true),
                                QJsonValue("1"),
                                QJsonValue(QJsonValue::Null)}) {
        auto request = open_request(normal, key);
        request.insert("requested_version", version);
        reject(request, "INVALID_INPUT", "requested_version");
    }
    auto future = open_request(normal, key);
    future.insert("requested_version", 2);
    reject(future, "SCHEMA_UNSUPPORTED", "requested_version");
    for (const auto& [input, field] : std::vector<std::pair<QJsonObject, const char*>>{
             {{}, "input.mode"},
             {{{"mode", 1}}, "input.mode"},
             {{{"mode", ""}}, "input.mode"},
             {{{"mode", "other"}}, "input.mode"},
             {{{"mode", "normal"}}, "input.path"},
             {{{"mode", "normal"}, {"path", 1}}, "input.path"},
             {{{"mode", "normal"}, {"path", ""}}, "input.path"},
             {{{"mode", "recover"}, {"path", "/open-contract.qcae"}}, "input.path"},
             {{{"mode", "recover"}, {"extra", "unknown"}}, "input.extra"}})
        reject(open_request(input, key), "INVALID_INPUT", field);
    auto legacy = open_request({}, key);
    legacy.insert("operation", "operations.get");
    legacy.insert("parameters",
                  QJsonObject{{"lookup_scope", "host"},
                              {"original_operation", "project.create"},
                              {"idempotency_key", "not-recorded"}});
    legacy.insert("requested_version", 1);
    const auto legacy_failure = qcae::ipc::dispatch(app, legacy, caller);
    require(legacy_failure.value("error").toObject().value("code") == "INVALID_INPUT" &&
                open_contract_state(app, store, caller) == before,
            "the excluded compatibility lookup keeps its version-field rejection");
    auto profile = open_request(normal, key);
    profile.insert("expected_profile", QJsonObject{});
    require(qcae::ipc::dispatch(app, profile, caller).value("error").toObject().value("code") ==
                    "INVALID_INPUT" &&
                open_contract_state(app, store, caller) == before,
            "project.open keeps its existing expected_profile rejection");
}
void project_open_contract_and_lifecycle() {
    using namespace qcae;
    using namespace operations;
    auto store = std::make_shared<OpenContractStore>();
    const Caller caller{"open-contract-test"};
    DocumentInfo before_recovery;
    QJsonObject recovery_history;
    {
        MemoryApplication app({}, store);
        ipc::TypedHost host(app.record_application(), [](const auto&) { return true; });
        const auto created =
            runtime_test::good(app.create_document(caller, "Open contract", "create"));
        const auto preview = runtime_test::good(
            app.preview(caller, runtime_test::at(created), CreateMaterial{"Steel", {210, "GPa"}}));
        runtime_test::good(app.commit(caller, runtime_test::at(created), preview.id, "steel"));
        const auto saved = runtime_test::good(
            app.save_document(caller,
                              runtime_test::at(runtime_test::good(app.current_document())),
                              "/open-contract.qcae",
                              false,
                              "save"));
        runtime_test::good(
            app.close_document(caller, runtime_test::at(saved), ClosePolicy::discard, "discard"));
        const auto catalog = ipc::dispatch(app,
                                           {{"api_version", "1.1"},
                                            {"request_id", "catalog"},
                                            {"operation", "capabilities.list"},
                                            {"parameters", QJsonObject{}}},
                                           caller,
                                           nullptr,
                                           nullptr,
                                           nullptr,
                                           &host);
        QJsonObject descriptor;
        unsigned count = 0;
        for (const auto& entry : catalog.value("data").toObject().value("operations").toArray()) {
            if (entry.toObject().value("name") == "project.open") {
                descriptor = entry.toObject();
                ++count;
            }
        }
        const auto definition = InputTraits<ProjectOpenInput>::definition();
        require(count == 1 && descriptor.value("available") == true &&
                    descriptor.value("version").toInt() == static_cast<int>(definition.version) &&
                    descriptor.value("schema_id") == QString::fromStdString(definition.schema_id) &&
                    descriptor.value("wire_input_type") == "ProjectOpenInput" &&
                    descriptor.value("wire_output_type") == "DocumentInfo" &&
                    descriptor.value("requested_version_field") == "requested_version" &&
                    descriptor.value("omitted_version_policy") == "installed_version",
                "one discoverable host route advertises its installed generated wire contract");
        const auto fields = descriptor.value("fields").toArray();
        require(fields.size() == static_cast<qsizetype>(definition.fields.size()),
                "all generated fields discovered");
        for (qsizetype index = 0; index < fields.size(); ++index) {
            const auto field = fields[index].toObject();
            const auto& expected = definition.fields[static_cast<std::size_t>(index)];
            require(field.value("name") == QString::fromStdString(expected.name) &&
                        field.value("wire_type") == QString::fromStdString(expected.wire_type) &&
                        field.value("required").toBool() == expected.required,
                    "discovery uses generated input field names/types/requirements");
        }
        const auto schema = descriptor.value("parameters_schema").toObject();
        require(schema.value("additionalProperties") == false &&
                    schema.value("oneOf").toArray().size() == 2 &&
                    schema.value("required").toArray() == QJsonArray{"mode"},
                "discovery distinguishes normal-with-path and recover-without-path");
        const auto modes = schema.value("oneOf").toArray();
        require(modes[0].toObject().value("required").toArray() == QJsonArray{"mode", "path"} &&
                    modes[0].toObject()
                            .value("properties")
                            .toObject()
                            .value("mode")
                            .toObject()
                            .value("enum")
                            .toArray() == QJsonArray{"normal"} &&
                    modes[0].toObject().value("additionalProperties") == false &&
                    modes[0].toObject().value("properties").toObject().contains("path") &&
                    modes[1].toObject().value("required").toArray() == QJsonArray{"mode"} &&
                    modes[1].toObject().value("additionalProperties") == false &&
                    !modes[1].toObject().value("properties").toObject().contains("path") &&
                    modes[1].toObject()
                            .value("properties")
                            .toObject()
                            .value("mode")
                            .toObject()
                            .value("enum")
                            .toArray() == QJsonArray{"recover"},
                "discovered mode conditions match the actual accepted and rejected requests");
        require(InputTraits<ProjectOpenInput>::from_value(
                    Value(Value::Object{{"mode", Value("recover")}}))
                    .ok(),
                "generated decoder accepts recover without path");
        rejected_open_inputs_are_atomic(app, *store, host, caller, "open");
        auto request = open_request({{"mode", "normal"}, {"path", "/open-contract.qcae"}}, "open");
        request.insert("requested_version", 1);
        const auto opened = ipc::dispatch(app, request, caller, nullptr, nullptr, nullptr, &host);
        const auto info = runtime_test::good(app.current_document());
        require(opened.value("status") == "success" &&
                    opened.value("data").toObject() == ipc::info_json(info) &&
                    info.document.id != saved.document.id &&
                    info.document.epoch != saved.document.epoch &&
                    info.project_id == saved.project_id && info.revision == 0 &&
                    runtime_test::good(app.snapshot(info.document)).materials.size() == 1 &&
                    runtime_test::good(app.history(info.document)).items.empty(),
                "version-one normal open preserves DocumentInfo/model and creates its documented "
                "lifecycle");
        rejected_open_inputs_are_atomic(app, *store, host, caller, "active-open-rejections");
        const auto after_open = open_contract_state(app, *store, caller);
        request.remove("requested_version");
        require(
            ipc::dispatch(app, request, caller, nullptr, nullptr, nullptr, &host).value("data") ==
                    opened.value("data") &&
                open_contract_state(app, *store, caller) == after_open,
            "omitted installed version replays normal open without another persistent write");
        const auto change = runtime_test::good(
            app.preview(caller, runtime_test::at(info), CreateMaterial{"Aluminium", {70, "GPa"}}));
        runtime_test::good(app.commit(caller, runtime_test::at(info), change.id, "aluminium"));
        before_recovery = runtime_test::good(app.current_document());
        recovery_history = open_contract_state(app, *store, caller).history;
        runtime_test::good(app.close_document(
            caller, runtime_test::at(before_recovery), ClosePolicy::keep_recovery, "keep"));
    }
    MemoryApplication recovered_app({}, store);
    ipc::TypedHost recovered_host(recovered_app.record_application(),
                                  [](const auto&) { return true; });
    require(recovered_app.recovery_available() && !recovered_app.current_document().ok(),
            "recovery is explicit on a new host");
    rejected_open_inputs_are_atomic(recovered_app, *store, recovered_host, caller, "recover");
    auto request = open_request({{"mode", "recover"}}, "recover");
    const auto recovered =
        ipc::dispatch(recovered_app, request, caller, nullptr, nullptr, nullptr, &recovered_host);
    const auto info = runtime_test::good(recovered_app.current_document());
    const auto state = open_contract_state(recovered_app, *store, caller);
    require(recovered.value("status") == "success" &&
                recovered.value("data").toObject() == ipc::info_json(info) &&
                info.document.id == before_recovery.document.id &&
                info.document.epoch != before_recovery.document.epoch &&
                info.project_id == before_recovery.project_id &&
                info.revision == before_recovery.revision &&
                runtime_test::good(recovered_app.snapshot(info.document)).materials.size() == 2 &&
                state.history == recovery_history,
            "omitted-version recovery preserves document, model, revision and history while "
            "replacing epoch");
    request.insert("requested_version", 1);
    require(
        ipc::dispatch(recovered_app, request, caller, nullptr, nullptr, nullptr, &recovered_host)
                    .value("data") == recovered.value("data") &&
            open_contract_state(recovered_app, *store, caller) == state,
        "explicit installed version replays recovery without another write");
}
QJsonObject create_request(QJsonObject parameters, const char* key) {
    auto request = open_request(std::move(parameters), key);
    request.insert("operation", "project.create");
    request.insert("request_id", "create-contract");
    return request;
}
void rejected_create_inputs_are_atomic(qcae::MemoryApplication& app,
                                       OpenContractStore& store,
                                       qcae::ipc::TypedHost& host,
                                       const qcae::Caller& caller,
                                       const char* key) {
    const auto before = open_contract_state(app, store, caller);
    auto reject = [&](QJsonObject request,
                      const char* code,
                      const char* field,
                      const char* status = "failed") {
        const auto wire = QJsonDocument::fromJson(QJsonDocument(request).toJson()).object();
        const auto response =
            qcae::ipc::dispatch(app, wire, caller, nullptr, nullptr, nullptr, &host);
        const auto error = response.value("error").toObject();
        require(response.value("status") == status && error.value("code") == code &&
                    error.value("field") == field,
                "project.create rejects its exact input/version contract");
        require(
            open_contract_state(app, store, caller) == before,
            "rejected create preserves all rows, payloads, counters, RAM, history and recovery");
    };
    const QJsonObject valid{{"name", "Create contract"}};
    for (const auto& version : {QJsonValue(0),
                                QJsonValue(-1),
                                QJsonValue(1.5),
                                QJsonValue(4294967296.0),
                                QJsonValue(true),
                                QJsonValue("1"),
                                QJsonValue(QJsonValue::Null)}) {
        auto request = create_request(valid, key);
        request.insert("requested_version", version);
        reject(request, "INVALID_INPUT", "requested_version");
    }
    for (const auto& version : {QJsonValue(2), QJsonValue(4294967295.0)}) {
        auto request = create_request(valid, key);
        request.insert("requested_version", version);
        reject(request, "SCHEMA_UNSUPPORTED", "requested_version");
    }
    for (const auto& [input, field] : std::vector<std::pair<QJsonObject, const char*>>{
             {{}, "input.name"},
             {{{"name", ""}}, "input.name"},
             {{{"name", 1}}, "input.name"},
             {{{"name", "Create contract"}, {"extra", "unknown"}}, "input.extra"}})
        reject(create_request(input, key), "INVALID_INPUT", field);
    reject(
        create_request({{"name", QString(1025, QLatin1Char('n'))}}, key), "RESOURCE_LIMIT", "name");
    // The existing application quota is UTF-8 bytes, not QString characters.
    reject(create_request({{"name", QString(512, QChar{u'\u03a9'}) + QLatin1Char('a')}}, key),
           "RESOURCE_LIMIT",
           "name");
    reject(create_request({{"name", " \t"}}, key), "MISSING_INPUT", "", "needs_input");
    auto profile = create_request(valid, key);
    profile.insert("expected_profile", QJsonObject{});
    require(qcae::ipc::dispatch(app, profile, caller).value("error").toObject().value("code") ==
                    "INVALID_INPUT" &&
                open_contract_state(app, store, caller) == before,
            "project.create retains its expected_profile rejection");
}
void project_create_contract_and_replay() {
    using namespace qcae;
    using namespace operations;
    auto store = std::make_shared<OpenContractStore>();
    const Caller caller{"create-contract-test"};
    QJsonObject created;
    {
        MemoryApplication app({}, store);
        ipc::TypedHost host(app.record_application(), [](const auto&) { return true; });
        const auto catalog = ipc::dispatch(app,
                                           {{"api_version", "1.1"},
                                            {"request_id", "create-catalog"},
                                            {"operation", "capabilities.list"},
                                            {"parameters", QJsonObject{}}},
                                           caller,
                                           nullptr,
                                           nullptr,
                                           nullptr,
                                           &host);
        QJsonObject descriptor;
        unsigned count{};
        for (const auto& entry : catalog.value("data").toObject().value("operations").toArray())
            if (entry.toObject().value("name") == "project.create") {
                descriptor = entry.toObject();
                ++count;
            }
        const auto definition = InputTraits<ProjectCreateInput>::definition();
        require(count == 1 && descriptor.value("available") == true &&
                    descriptor.value("version").toInt() == static_cast<int>(definition.version) &&
                    descriptor.value("schema_id") == QString::fromStdString(definition.schema_id) &&
                    descriptor.value("wire_input_type") == "ProjectCreateInput" &&
                    descriptor.value("wire_output_type") == "DocumentInfo" &&
                    descriptor.value("requested_version_field") == "requested_version" &&
                    descriptor.value("omitted_version_policy") == "installed_version",
                "create discovery identifies the installed generated input adapter");
        const auto fields = descriptor.value("fields").toArray();
        require(fields.size() == 1 && definition.fields.size() == 1,
                "create has one generated input field");
        const auto& name = definition.fields.front();
        require(fields[0].toObject().value("name") == QString::fromStdString(name.name) &&
                    fields[0].toObject().value("wire_type") ==
                        QString::fromStdString(name.wire_type) &&
                    fields[0].toObject().value("required").toBool() == name.required,
                "create discovery uses the generated name field");
        const auto schema = descriptor.value("parameters_schema").toObject();
        require(schema.value("additionalProperties") == false &&
                    schema.value("required").toArray() == QJsonArray{"name"} &&
                    schema.value("properties").toObject() ==
                        QJsonObject{{"name", QJsonObject{{"type", "string"}, {"minLength", 1}}}},
                "create input discovery is strict and requires a nonempty name");
        rejected_create_inputs_are_atomic(app, *store, host, caller, "create");
        const QString boundary_name(1024, QLatin1Char('n'));
        auto request = create_request({{"name", boundary_name}}, "create");
        created = ipc::dispatch(app, request, caller, nullptr, nullptr, nullptr, &host);
        auto info = runtime_test::good(app.current_document());
        require(created.value("status") == "success" &&
                    created.value("data").toObject() == ipc::info_json(info) &&
                    info.name.size() == 1024 && info.name == boundary_name.toStdString() &&
                    info.revision == 0 &&
                    runtime_test::good(app.history(info.document)).items.empty(),
                "omitted version creates the exact 1024-byte name without prior rejected key use");
        rejected_create_inputs_are_atomic(app, *store, host, caller, "active-create-rejections");
        const auto change = runtime_test::good(
            app.preview(caller, runtime_test::at(info), CreateMaterial{"Steel", {210, "GPa"}}));
        runtime_test::good(app.commit(caller, runtime_test::at(info), change.id, "steel"));
        const auto after_edit = open_contract_state(app, *store, caller);
        request.insert("requested_version", 1);
        require(
            ipc::dispatch(app, request, caller, nullptr, nullptr, nullptr, &host).value("data") ==
                    created.value("data") &&
                open_contract_state(app, *store, caller) == after_edit,
            "explicit installed version replays the original create after edits without writes");
        auto conflict = create_request({{"name", "Another name"}}, "create");
        conflict.insert("requested_version", 1);
        require(ipc::dispatch(app, conflict, caller, nullptr, nullptr, nullptr, &host)
                            .value("error")
                            .toObject()
                            .value("code") == "IDEMPOTENCY_KEY_CONFLICT" &&
                    open_contract_state(app, *store, caller) == after_edit,
                "same create key with a different name cannot replace original facts or model");
        info = runtime_test::good(app.current_document());
        runtime_test::good(
            app.close_document(caller, runtime_test::at(info), ClosePolicy::keep_recovery, "keep"));
    }
    MemoryApplication app({}, store);
    ipc::TypedHost host(app.record_application(), [](const auto&) { return true; });
    require(app.recovery_available() && !app.current_document().ok(),
            "retained create is not active");
    rejected_create_inputs_are_atomic(app, *store, host, caller, "recovery-create-rejections");
    const auto before = open_contract_state(app, *store, caller);
    auto replay = create_request({{"name", QString(1024, QLatin1Char('n'))}}, "create");
    require(ipc::dispatch(app, replay, caller, nullptr, nullptr, nullptr, &host).value("data") ==
                    created.value("data") &&
                open_contract_state(app, *store, caller) == before,
            "omitted version replays retained create without activating or altering recovery");
    const QJsonObject lookup{{"api_version", "1.1"},
                             {"request_id", "create-lookup"},
                             {"operation", "operations.get"},
                             {"parameters",
                              QJsonObject{{"lookup_scope", "host"},
                                          {"original_operation", "project.create"},
                                          {"idempotency_key", "create"}}}};
    const auto fact = ipc::dispatch(app, lookup, caller);
    require(fact.value("status") == "success" && fact.value("data") == created.value("data") &&
                open_contract_state(app, *store, caller) == before,
            "existing host outcome lookup returns create facts without a document context");
}
QJsonObject write_lifecycle_request(const char* operation,
                                    QJsonObject parameters,
                                    const qcae::DocumentInfo& info,
                                    const char* key) {
    auto request = open_request(std::move(parameters), key);
    request.insert("operation", operation);
    request.insert("document_id", QString::fromStdString(info.document.id.value));
    request.insert("document_epoch", QString::fromStdString(info.document.epoch.value));
    request.insert("expected_revision", QString::number(info.revision));
    return request;
}
void reject_write_lifecycle_request(qcae::MemoryApplication& app,
                                    OpenContractStore& store,
                                    qcae::ipc::TypedHost& host,
                                    const qcae::Caller& caller,
                                    const QJsonObject& request,
                                    const char* code,
                                    const char* field = "",
                                    const char* status = "failed") {
    const auto before = open_contract_state(app, store, caller);
    const auto wire = QJsonDocument::fromJson(QJsonDocument(request).toJson()).object();
    const auto response = qcae::ipc::dispatch(app, wire, caller, nullptr, nullptr, nullptr, &host);
    const auto error = response.value("error").toObject();
    require(response.value("status") == status && error.value("code") == code &&
                error.value("field").toString() == field,
            "save/close rejects the expected input/version/context");
    require(open_contract_state(app, store, caller) == before,
            "rejected save/close preserves rows, project payloads, RAM, history, recovery and "
            "counters");
}
void rejected_write_lifecycle_inputs(qcae::MemoryApplication& app,
                                     OpenContractStore& store,
                                     qcae::ipc::TypedHost& host,
                                     const qcae::Caller& caller,
                                     const qcae::DocumentInfo& info) {
    for (const auto* operation : {"project.save", "project.save_as", "project.close"}) {
        const bool close = std::string_view(operation) == "project.close";
        const QJsonObject valid = close ? QJsonObject{{"policy", "keep_recovery"}} : QJsonObject{};
        auto reject = [&](const QJsonObject& request, const char* code, const char* field = "") {
            reject_write_lifecycle_request(app, store, host, caller, request, code, field);
        };
        for (const auto& version : {QJsonValue(0),
                                    QJsonValue(-1),
                                    QJsonValue(1.5),
                                    QJsonValue(4294967296.0),
                                    QJsonValue(true),
                                    QJsonValue("1"),
                                    QJsonValue(QJsonValue::Null)}) {
            auto request = write_lifecycle_request(operation, valid, info, "contract-key");
            request.insert("requested_version", version);
            reject(request, "INVALID_INPUT", "requested_version");
        }
        for (const auto& version : {QJsonValue(2), QJsonValue(4294967295.0)}) {
            auto request = write_lifecycle_request(operation, valid, info, "contract-key");
            request.insert("requested_version", version);
            reject(request, "SCHEMA_UNSUPPORTED", "requested_version");
        }
        auto context_free = write_lifecycle_request(operation, valid, info, "contract-key");
        context_free.insert("requested_version", 2);
        for (const auto* field :
             {"document_id", "document_epoch", "expected_revision", "idempotency_key"})
            context_free.remove(field);
        reject(context_free, "SCHEMA_UNSUPPORTED", "requested_version");
        const char* name = close ? "policy" : "path";
        for (const auto& value : {QJsonValue(true), QJsonValue(1), QJsonValue(QJsonValue::Null)})
            reject(write_lifecycle_request(operation, {{name, value}}, info, "contract-key"),
                   "INVALID_INPUT",
                   close ? "input.policy" : "input.path");
        auto extra = valid;
        extra.insert("extra", "unknown");
        reject(write_lifecycle_request(operation, extra, info, "contract-key"),
               "INVALID_INPUT",
               "input.extra");
        auto profile = write_lifecycle_request(operation, valid, info, "contract-key");
        profile.insert("expected_profile", QJsonObject{});
        reject(profile, "INVALID_INPUT");
        if (close) {
            reject(write_lifecycle_request(operation, {}, info, "contract-key"),
                   "INVALID_INPUT",
                   "input.policy");
            reject(write_lifecycle_request(operation, {{"policy", ""}}, info, "contract-key"),
                   "INVALID_INPUT",
                   "input.policy");
            reject(write_lifecycle_request(operation, {{"policy", "wrong"}}, info, "contract-key"),
                   "INVALID_INPUT",
                   "input.policy");
        }
    }
}
void project_save_close_contract_and_replay() {
    using namespace qcae;
    using namespace operations;
    auto store = std::make_shared<OpenContractStore>();
    const Caller caller{"save-close-contract-test"};
    QJsonObject saved_request, close_request, saved, copied, closed;
    DocumentInfo closed_info;
    {
        MemoryApplication app({}, store);
        ipc::TypedHost host(app.record_application(), [](const auto&) { return true; });
        auto dispatch = [&](const QJsonObject& request) {
            return ipc::dispatch(app, request, caller, nullptr, nullptr, nullptr, &host);
        };
        auto info =
            runtime_test::good(app.create_document(caller, "Save close contract", "create"));
        const auto change = runtime_test::good(
            app.preview(caller, runtime_test::at(info), CreateMaterial{"Steel", {210, "GPa"}}));
        runtime_test::good(app.commit(caller, runtime_test::at(info), change.id, "steel"));
        info = runtime_test::good(app.current_document());
        const auto catalog = dispatch({{"api_version", "1.1"},
                                       {"request_id", "lifecycle-catalog"},
                                       {"operation", "capabilities.list"},
                                       {"parameters", QJsonObject{}}});
        for (const auto& [definition, type] :
             std::vector<std::pair<OperationDefinition, const char*>>{
                 {InputTraits<ProjectSaveInput>::definition(), "ProjectSaveInput"},
                 {InputTraits<ProjectSaveAsInput>::definition(), "ProjectSaveAsInput"},
                 {InputTraits<ProjectCloseInput>::definition(), "ProjectCloseInput"}}) {
            QJsonObject descriptor;
            unsigned count{};
            for (const auto& entry : catalog.value("data").toObject().value("operations").toArray())
                if (entry.toObject().value("name") ==
                    QString::fromStdString(definition.operation_id)) {
                    descriptor = entry.toObject();
                    ++count;
                }
            require(count == 1 && descriptor.value("available") == true &&
                        descriptor.value("version").toInt() == 1 &&
                        descriptor.value("schema_id") ==
                            QString::fromStdString(definition.schema_id) &&
                        descriptor.value("wire_input_type") == type &&
                        descriptor.value("wire_output_type") == "DocumentInfo" &&
                        descriptor.value("requested_version_field") == "requested_version" &&
                        descriptor.value("omitted_version_policy") == "installed_version" &&
                        descriptor.value("requires_document") == true &&
                        descriptor.value("requires_epoch") == true &&
                        descriptor.value("requires_revision") == true &&
                        descriptor.value("requires_idempotency_key") == true,
                    "save/close discovery retains context and names its generated input/output");
            const auto fields = descriptor.value("fields").toArray();
            require(fields.size() == 1 && definition.fields.size() == 1,
                    "save/close has one generated field");
            const auto& field = definition.fields.front();
            require(fields[0].toObject().value("name") == QString::fromStdString(field.name) &&
                        fields[0].toObject().value("required").toBool() == field.required &&
                        fields[0].toObject().value("allow_empty").toBool() == field.allow_empty,
                    "save/close discovery reflects generated optional and empty flags");
            const bool close = definition.operation_id == "project.close";
            const auto schema = descriptor.value("parameters_schema").toObject();
            require(
                schema.value("additionalProperties") == false &&
                    schema.value("required").toArray() ==
                        (close ? QJsonArray{"policy"} : QJsonArray{}) &&
                    schema.value("properties").toObject() ==
                        (close
                             ? QJsonObject{{"policy",
                                            QJsonObject{
                                                {"type", "string"},
                                                {"minLength", 1},
                                                {"enum", QJsonArray{"discard", "keep_recovery"}}}}}
                             : QJsonObject{{"path", QJsonObject{{"type", "string"}}}}),
                "save paths allow omission/empty; close advertises exactly its two policies");
        }
        rejected_write_lifecycle_inputs(app, *store, host, caller, info);
        for (const auto* operation : {"project.save", "project.save_as"})
            for (const auto& params : {QJsonObject{}, QJsonObject{{"path", ""}}})
                reject_write_lifecycle_request(
                    app,
                    *store,
                    host,
                    caller,
                    write_lifecycle_request(operation, params, info, "contract-key"),
                    "MISSING_INPUT",
                    "",
                    "needs_input");
        saved_request = write_lifecycle_request(
            "project.save", {{"path", "/save-contract.qcae"}}, info, "contract-key");
        saved_request.insert("requested_version", 1);
        saved = dispatch(saved_request);
        require(saved.value("status") == "success" &&
                    store->projects.contains("/save-contract.qcae"),
                "version 1 save succeeds after rejected requests without consuming its key");
        info = runtime_test::good(app.current_document());
        require(!info.dirty && info.saved_path == "/save-contract.qcae" &&
                    runtime_test::good(app.history(info.document)).items.size() == 1,
                "save retains the model history and marks the original content saved");
        auto omitted = write_lifecycle_request("project.save", {}, info, "save-default");
        const auto default_save = dispatch(omitted);
        require(default_save.value("status") == "success", "omitted save path selects saved_path");
        auto before = open_contract_state(app, *store, caller);
        omitted.insert("parameters", QJsonObject{{"path", ""}});
        omitted.insert("requested_version", 1);
        require(
            dispatch(omitted).value("data") == default_save.value("data") &&
                open_contract_state(app, *store, caller) == before,
            "explicit empty save path replays the same raw empty signature without publication");
        omitted.insert("parameters", QJsonObject{{"path", "/save-contract.qcae"}});
        reject_write_lifecycle_request(
            app, *store, host, caller, omitted, "IDEMPOTENCY_KEY_CONFLICT", "", "conflict");
        // A missing saved file lets save-as reuse the saved_path with its original empty-path
        // rules.
        store->projects.erase("/save-contract.qcae");
        auto save_as = write_lifecycle_request("project.save_as", {}, info, "save-as-default");
        copied = dispatch(save_as);
        const auto copied_info = runtime_test::good(app.current_document());
        require(copied.value("status") == "success" &&
                    copied_info.document.id == info.document.id &&
                    copied_info.document.epoch == info.document.epoch &&
                    copied_info.saved_path == info.saved_path &&
                    copied_info.project_id != info.project_id,
                "omitted save-as path preserves the document and creates a new project snapshot");
        before = open_contract_state(app, *store, caller);
        save_as.insert("parameters", QJsonObject{{"path", ""}});
        save_as.insert("requested_version", 1);
        require(dispatch(save_as).value("data") == copied.value("data") &&
                    open_contract_state(app, *store, caller) == before,
                "empty save-as path replays omitted path without another publication");
        save_as.insert("operation", "project.save");
        reject_write_lifecycle_request(
            app, *store, host, caller, save_as, "IDEMPOTENCY_KEY_CONFLICT", "", "conflict");
        info = runtime_test::good(app.current_document());
        close_request = write_lifecycle_request(
            "project.close", {{"policy", "keep_recovery"}}, info, "contract-key");
        auto stale = close_request;
        stale.insert("document_epoch", "stale-epoch");
        reject_write_lifecycle_request(app,
                                       *store,
                                       host,
                                       caller,
                                       stale,
                                       "DOCUMENT_EPOCH_EXPIRED",
                                       "document_epoch",
                                       "conflict");
        stale = close_request;
        stale.insert("expected_revision", QString::number(info.revision - 1));
        reject_write_lifecycle_request(
            app, *store, host, caller, stale, "REVISION_CONFLICT", "expected_revision", "conflict");
        close_request.insert("requested_version", 1);
        closed = dispatch(close_request);
        closed_info = info;
        require(closed.value("status") == "success" && !app.current_document().ok() &&
                    app.recovery_available(),
                "valid close retains recovery after rejected key use");
        before = open_contract_state(app, *store, caller);
        close_request.remove("requested_version");
        require(dispatch(close_request).value("data") == closed.value("data") &&
                    open_contract_state(app, *store, caller) == before,
                "omitted version replays keep_recovery close with no active document");
        for (const auto& [field, value] : std::vector<std::pair<const char*, QJsonValue>>{
                 {"parameters", QJsonObject{{"policy", "discard"}}},
                 {"document_epoch", "stale-epoch"},
                 {"expected_revision", QString::number(info.revision + 1)}}) {
            auto conflict = close_request;
            conflict.insert(field, value);
            reject_write_lifecycle_request(
                app, *store, host, caller, conflict, "IDEMPOTENCY_KEY_CONFLICT", "", "conflict");
        }
    }
    MemoryApplication app({}, store);
    ipc::TypedHost host(app.record_application(), [](const auto&) { return true; });
    require(app.recovery_available() && !app.current_document().ok(),
            "restart retains closed recovery");
    rejected_write_lifecycle_inputs(app, *store, host, caller, closed_info);
    auto before = open_contract_state(app, *store, caller);
    require(ipc::dispatch(app, saved_request, caller).value("data") == saved.value("data") &&
                ipc::dispatch(app, close_request, caller).value("data") == closed.value("data") &&
                open_contract_state(app, *store, caller) == before,
            "original save/close facts replay after restart without activating recovery");
    for (const auto& [operation, key, expected] :
         std::vector<std::tuple<const char*, const char*, QJsonObject>>{
             {"project.save", "contract-key", saved},
             {"project.save_as", "save-as-default", copied},
             {"project.close", "contract-key", closed}}) {
        const QJsonObject lookup{{"api_version", "1.1"},
                                 {"request_id", "host-lookup"},
                                 {"operation", "operations.get"},
                                 {"parameters",
                                  QJsonObject{{"lookup_scope", "host"},
                                              {"original_operation", operation},
                                              {"idempotency_key", key}}}};
        require(ipc::dispatch(app, lookup, caller).value("data") == expected.value("data") &&
                    open_contract_state(app, *store, caller) == before,
                "original host lookup returns save/close facts without an active document");
    }
    reject_write_lifecycle_request(
        app,
        *store,
        host,
        caller,
        write_lifecycle_request("project.close", {{"policy", "discard"}}, closed_info, "discard"),
        "DOCUMENT_NOT_FOUND",
        "document_id");
    const auto recovered =
        ipc::dispatch(app, open_request({{"mode", "recover"}}, "recover"), caller);
    require(recovered.value("status") == "success",
            "recovery remains available after replay/rejection");
    const auto info = runtime_test::good(app.current_document());
    auto discard =
        write_lifecycle_request("project.close", {{"policy", "discard"}}, info, "discard");
    auto stale = discard;
    stale.insert("document_epoch", QString::fromStdString(closed_info.document.epoch.value));
    reject_write_lifecycle_request(
        app, *store, host, caller, stale, "DOCUMENT_EPOCH_EXPIRED", "document_epoch", "conflict");
    const auto discarded = ipc::dispatch(app, discard, caller);
    require(discarded.value("status") == "success" && !app.current_document().ok() &&
                !app.recovery_available(),
            "discard clears the active document and recovery");
    before = open_contract_state(app, *store, caller);
    discard.insert("requested_version", 1);
    require(ipc::dispatch(app, discard, caller).value("data") == discarded.value("data") &&
                open_contract_state(app, *store, caller) == before,
            "version 1 discard retry returns its original fact without recreating a document");
}
void changes_commit_contract_and_replay() {
    using namespace qcae;
    using namespace operations;
    for (const bool explicit_version : {false, true}) {
        auto store = std::make_shared<OpenContractStore>();
        MemoryApplication app({}, store);
        ipc::TypedHost host(app.record_application(), [](const auto&) { return true; });
        const Caller caller{"commit-contract-test"};
        auto info = runtime_test::good(app.create_document(caller, "Commit contract", "create"));
        const auto seed = runtime_test::good(
            app.preview(caller, runtime_test::at(info), CreateMaterial{"Existing", {200, "GPa"}}));
        runtime_test::good(app.commit(caller, runtime_test::at(info), seed.id, "seed"));
        info = runtime_test::good(app.current_document());
        const auto preview = runtime_test::good(
            app.preview(caller, runtime_test::at(info), CreateMaterial{"Steel", {210, "GPa"}}));
        const QJsonObject parameters{{"preview_id", QString::fromStdString(preview.id.value)}};
        auto request = write_lifecycle_request("changes.commit", parameters, info, "commit-key");
        request.insert("request_id", "commit-contract");
        if (explicit_version)
            request.insert("requested_version", 1);

        const auto catalog = ipc::dispatch(app,
                                           {{"api_version", "1.1"},
                                            {"request_id", "commit-catalog"},
                                            {"operation", "capabilities.list"},
                                            {"parameters", QJsonObject{}}},
                                           caller,
                                           nullptr,
                                           nullptr,
                                           nullptr,
                                           &host);
        QJsonObject descriptor;
        unsigned count{};
        for (const auto& entry : catalog.value("data").toObject().value("operations").toArray())
            if (entry.toObject().value("name") == "changes.commit") {
                descriptor = entry.toObject();
                ++count;
            }
        const auto definition = InputTraits<ChangesCommitInput>::definition();
        require(count == 1 && descriptor.value("available") == true &&
                    descriptor.value("version").toInt() == static_cast<int>(definition.version) &&
                    descriptor.value("schema_id") == QString::fromStdString(definition.schema_id) &&
                    descriptor.value("wire_input_type") == "ChangesCommitInput" &&
                    descriptor.value("wire_output_type") == "ChangeReceipt" &&
                    descriptor.value("requires_document").toBool() == definition.context.document &&
                    descriptor.value("requires_epoch").toBool() == definition.context.epoch &&
                    descriptor.value("requires_revision").toBool() ==
                        definition.context.expected_revision &&
                    descriptor.value("requires_idempotency_key").toBool() ==
                        definition.context.idempotency_key &&
                    descriptor.value("requested_version_field") == "requested_version" &&
                    descriptor.value("omitted_version_policy") == "installed_version",
                "commit discovery identifies its generated input and existing receipt output");
        const auto fields = descriptor.value("fields").toArray();
        require(fields.size() == 1 && definition.fields.size() == 1,
                "commit has exactly one generated preview field");
        const auto& field = definition.fields.front();
        require(
            field.name == "preview_id" && field.wire_type == "string" && field.required &&
                !field.allow_empty &&
                fields[0].toObject().value("name") == QString::fromStdString(field.name) &&
                fields[0].toObject().value("wire_type") ==
                    QString::fromStdString(field.wire_type) &&
                fields[0].toObject().value("required").toBool() == field.required &&
                fields[0].toObject().value("allow_empty").toBool() == field.allow_empty &&
                descriptor.value("parameters_schema").toObject() ==
                    QJsonObject{{"type", "object"},
                                {"properties",
                                 QJsonObject{{"preview_id",
                                              QJsonObject{{"type", "string"}, {"minLength", 1}}}}},
                                {"required", QJsonArray{"preview_id"}},
                                {"additionalProperties", false}},
            "commit discovery describes the generated required nonempty string");

        const auto reject = [&](const QJsonObject& invalid,
                                const char* code,
                                const char* error_field,
                                const char* status = "failed") {
            const auto before = open_contract_state(app, *store, caller);
            const auto before_view =
                runtime_test::good(app.record_application().snapshot(info.document)).records;
            for (int retry = 0; retry < 2; ++retry) {
                const auto wire = QJsonDocument::fromJson(QJsonDocument(invalid).toJson()).object();
                const auto response =
                    ipc::dispatch(app, wire, caller, nullptr, nullptr, nullptr, &host);
                const auto error = response.value("error").toObject();
                const auto after =
                    runtime_test::good(app.record_application().snapshot(info.document)).records;
                require(response.value("status") == status && error.value("code") == code &&
                            error.value("field").toString() == error_field,
                        "commit refuses the expected input/version/context");
                require(open_contract_state(app, *store, caller) == before &&
                            after.matches_version(before_view.version()) &&
                            diff_record_views(before_view, after).empty(),
                        "rejected commit preserves every record, persistent row, generation and "
                        "history on both attempts");
            }
        };
        for (const auto& version : {QJsonValue(0),
                                    QJsonValue(-1),
                                    QJsonValue(1.5),
                                    QJsonValue(4294967296.0),
                                    QJsonValue(true),
                                    QJsonValue("1"),
                                    QJsonValue(QJsonValue::Null)}) {
            auto invalid = request;
            invalid.insert("requested_version", version);
            reject(invalid, "INVALID_INPUT", "requested_version");
        }
        for (const auto& version : {QJsonValue(2), QJsonValue(4294967295.0)}) {
            auto invalid = request;
            invalid.insert("requested_version", version);
            reject(invalid, "SCHEMA_UNSUPPORTED", "requested_version");
        }
        for (const auto& input : {QJsonObject{},
                                  QJsonObject{{"preview_id", ""}},
                                  QJsonObject{{"preview_id", 1}},
                                  QJsonObject{{"preview_id", true}},
                                  QJsonObject{{"preview_id", QJsonValue(QJsonValue::Null)}},
                                  QJsonObject{{"preview_id", QJsonArray{}}},
                                  QJsonObject{{"preview_id", QJsonObject{}}}}) {
            auto invalid = request;
            invalid.insert("parameters", input);
            reject(invalid, "INVALID_INPUT", "input.preview_id");
        }
        auto invalid = request;
        auto extra = parameters;
        extra.insert("unexpected", "value");
        invalid.insert("parameters", extra);
        reject(invalid, "INVALID_INPUT", "input.unexpected");
        for (const auto& shape : {QJsonValue(QJsonArray{}),
                                  QJsonValue("preview"),
                                  QJsonValue(1),
                                  QJsonValue(QJsonValue::Null)}) {
            invalid = request;
            invalid.insert("parameters", shape);
            reject(invalid, "INVALID_INPUT", "");
        }
        invalid = request;
        invalid.insert("expected_profile", QJsonObject{});
        reject(invalid, "INVALID_INPUT", "");

        const auto before = open_contract_state(app, *store, caller);
        const auto before_view =
            runtime_test::good(app.record_application().snapshot(info.document)).records;
        const auto committed =
            ipc::dispatch(app, request, caller, nullptr, nullptr, nullptr, &host);
        const auto original = committed.value("data").toObject();
        const auto committed_info = runtime_test::good(app.current_document());
        require(committed.value("status") == "success" && !original.value("replayed").toBool() &&
                    !original.value("transaction_id").toString().isEmpty() &&
                    original.value("entity_id") ==
                        QString::fromStdString(preview.affected_entity.value) &&
                    original.value("committed_revision") == QString::number(info.revision + 1) &&
                    committed_info.revision == info.revision + 1 &&
                    store->records.generation == before.generation + 1 &&
                    store->commits == before.commits + 1 &&
                    runtime_test::good(app.history(info.document)).items.size() == 2 &&
                    runtime_test::good(app.snapshot(info.document)).materials.size() == 2,
                "omitted and v1 commit each consume the preserved preview/key in one transaction");
        const auto replay = [&] {
            const auto unchanged = open_contract_state(app, *store, caller);
            const auto snapshot =
                runtime_test::good(app.record_application().snapshot(info.document));
            auto expected = original;
            expected.insert("replayed", true);
            expected.insert("current_revision", QString::number(snapshot.info.revision));
            expected.insert("current_content_state",
                            QString::fromStdString(snapshot.info.content_state));
            const auto response =
                ipc::dispatch(app, request, caller, nullptr, nullptr, nullptr, &host);
            const auto after =
                runtime_test::good(app.record_application().snapshot(info.document)).records;
            require(response.value("status") == "success" && response.value("data") == expected &&
                        open_contract_state(app, *store, caller) == unchanged &&
                        after.matches_version(snapshot.records.version()) &&
                        diff_record_views(snapshot.records, after).empty(),
                    "retry returns the original receipt with current state and no second write");
        };
        if (explicit_version)
            request.remove("requested_version");
        else
            request.insert("requested_version", 1);
        replay();
        const auto changed_preview =
            runtime_test::good(app.preview(caller,
                                           runtime_test::at(committed_info),
                                           SetYoungModulus{preview.affected_entity, {220, "GPa"}}));
        auto conflict = write_lifecycle_request(
            "changes.commit",
            {{"preview_id", QString::fromStdString(changed_preview.id.value)}},
            committed_info,
            "commit-key");
        conflict.insert("requested_version", 1);
        reject(conflict, "IDEMPOTENCY_KEY_CONFLICT", "idempotency_key", "conflict");
        auto stale = request;
        stale.insert("idempotency_key", "stale-revision");
        reject(stale, "REVISION_CONFLICT", "expected_revision", "conflict");
        stale.insert("idempotency_key", "stale-epoch");
        stale.insert("document_epoch", "old-epoch");
        reject(stale, "DOCUMENT_EPOCH_EXPIRED", "document_epoch", "conflict");
        runtime_test::good(app.undo(caller, runtime_test::at(committed_info), "undo"));
        require(diff_record_views(
                    before_view,
                    runtime_test::good(app.record_application().snapshot(info.document)).records)
                    .empty(),
                "undo restores the complete precommit record view");
        replay();
    }
}
void host_read_and_history_contracts() {
    using namespace qcae;
    using namespace operations;
    struct Contract {
        const char* operation;
        const char* wire_input;
        const char* wire_output;
        const char* catalog_input;
        const char* catalog_output;
        bool document;
        bool write;
        OperationDefinition definition;
    };
    const std::vector<Contract> contracts{{"capabilities.list",
                                           "CapabilitiesListInput",
                                           "CapabilityCatalog",
                                           "CapabilityFilter",
                                           "CapabilityCatalog",
                                           false,
                                           false,
                                           InputTraits<CapabilitiesListInput>::definition()},
                                          {"project.current",
                                           "ProjectCurrentInput",
                                           "DocumentInfo",
                                           "CurrentProject",
                                           "DocumentContext",
                                           false,
                                           false,
                                           InputTraits<ProjectCurrentInput>::definition()},
                                          {"project.status",
                                           "ProjectStatusInput",
                                           "ProjectStatus",
                                           "ProjectStatusQuery",
                                           "ProjectStatus",
                                           true,
                                           false,
                                           InputTraits<ProjectStatusInput>::definition()},
                                          {"model.summary",
                                           "ModelSummaryInput",
                                           "ModelSummary",
                                           "SummaryQuery",
                                           "ModelSummary",
                                           true,
                                           false,
                                           InputTraits<ModelSummaryInput>::definition()},
                                          {"history.list",
                                           "HistoryListInput",
                                           "HistorySnapshot",
                                           "HistoryQuery",
                                           "HistorySummary",
                                           true,
                                           false,
                                           InputTraits<HistoryListInput>::definition()},
                                          {"history.undo",
                                           "HistoryUndoInput",
                                           "ChangeReceipt",
                                           "HistoryMove",
                                           "ChangeReceipt",
                                           true,
                                           true,
                                           InputTraits<HistoryUndoInput>::definition()},
                                          {"history.redo",
                                           "HistoryRedoInput",
                                           "ChangeReceipt",
                                           "HistoryMove",
                                           "ChangeReceipt",
                                           true,
                                           true,
                                           InputTraits<HistoryRedoInput>::definition()}};
    auto store = std::make_shared<OpenContractStore>();
    MemoryApplication app({}, store);
    ipc::TypedHost host(app.record_application(), [](const auto&) { return true; });
    const Caller caller{"host-controls-test"};
    const Caller other{"another-host-controls-caller"};
    const auto dispatch = [&](const QJsonObject& request, const Caller& actor) {
        const auto wire = QJsonDocument::fromJson(QJsonDocument(request).toJson()).object();
        return ipc::dispatch(app, wire, actor, nullptr, nullptr, nullptr, &host);
    };
    const auto read_request = [](const char* operation) {
        return QJsonObject{{"api_version", "1.1"},
                           {"request_id", "host-controls"},
                           {"operation", operation},
                           {"parameters", QJsonObject{}}};
    };
    const auto inactive = open_contract_state(app, *store, caller);
    auto catalog_request = read_request("capabilities.list");
    const auto catalog = dispatch(catalog_request, caller);
    catalog_request.insert("requested_version", 1);
    require(catalog.value("status") == "success" && dispatch(catalog_request, caller) == catalog &&
                open_contract_state(app, *store, caller) == inactive,
            "the global catalog accepts omitted/v1 without an active document or persistence");
    const auto catalog_data = catalog.value("data").toObject();
    require(catalog_data.value("storage_mode") == "sqlite" &&
                catalog_data.value("durable") == true &&
                catalog_data.value("recovery_available") == false &&
                catalog_data.value("max_name_bytes") == 1024 &&
                catalog_data.value("configured_solver_profiles").toArray().isEmpty() &&
                catalog_data.value("declared_solver_profiles").toArray().isEmpty() &&
                catalog_data.value("supported_pressure_units").toArray() ==
                    QJsonArray{"Pa", "kPa", "MPa", "GPa"},
            "the catalog preserves storage, recovery, profile and unit facts");
    const QJsonObject empty_schema{{"type", "object"},
                                   {"properties", QJsonObject{}},
                                   {"required", QJsonArray{}},
                                   {"additionalProperties", false}};
    for (const auto& contract : contracts) {
        const OperationDefinition expected{
            contract.operation,
            1,
            "qcae.operation." + std::string(contract.operation) + ".v1",
            contract.write ? OperationEffect::document_write : OperationEffect::read_only,
            {contract.document, contract.document, contract.write, contract.write, false},
            {}};
        const char* target_context = contract.write ? "from_history"
                                     : std::string_view(contract.operation) == "capabilities.list"
                                         ? "optional_profile"
                                         : "none";
        require(contract.definition == expected,
                "all seven generated definitions have the exact empty v1 context and effect");
        QJsonObject descriptor;
        unsigned count{};
        for (const auto& entry : catalog_data.value("operations").toArray())
            if (entry.toObject().value("name") == contract.operation) {
                descriptor = entry.toObject();
                ++count;
            }
        require(count == 1 && descriptor.value("available") == true &&
                    descriptor.value("version") == 1 &&
                    descriptor.value("schema_id") == QString::fromStdString(expected.schema_id) &&
                    descriptor.value("wire_input_type") == contract.wire_input &&
                    descriptor.value("wire_output_type") == contract.wire_output &&
                    descriptor.value("input_type") == contract.catalog_input &&
                    descriptor.value("output_type") == contract.catalog_output &&
                    descriptor.value("effect") == (contract.write ? "model_write" : "query") &&
                    descriptor.value("target_context") == target_context &&
                    descriptor.value("requires_document") == contract.document &&
                    descriptor.value("requires_epoch") == contract.document &&
                    descriptor.value("requires_revision") == contract.write &&
                    descriptor.value("requires_idempotency_key") == contract.write &&
                    descriptor.value("requires_profile_match") == false &&
                    descriptor.value("requested_version_field") == "requested_version" &&
                    descriptor.value("omitted_version_policy") == "installed_version" &&
                    !descriptor.contains("requested_version") &&
                    descriptor.value("fields") == QJsonArray{} &&
                    descriptor.value("parameters_schema") == empty_schema,
                "each available host route advertises its exact generated input and existing "
                "result labels with a closed empty schema");
        if (std::string_view(contract.operation) == "capabilities.list")
            require(descriptor.value("supported_scope") == "global_catalog_only",
                    "catalog discovery retains its supported scope");
    }
    QJsonObject lookup_descriptor;
    for (const auto& entry : catalog_data.value("operations").toArray())
        if (entry.toObject().value("name") == "operations.get")
            lookup_descriptor = entry.toObject();
    require(lookup_descriptor.value("available") == true &&
                lookup_descriptor.value("supported_scope") ==
                    "host_lifecycle_and_document_change_outcomes" &&
                lookup_descriptor.value("requires_document") == false &&
                lookup_descriptor.value("requires_epoch") == false &&
                lookup_descriptor.value("requires_revision") == false &&
                lookup_descriptor.value("requires_idempotency_key") == false &&
                !lookup_descriptor.contains("version") &&
                !lookup_descriptor.contains("schema_id") &&
                !lookup_descriptor.contains("wire_input_type") &&
                !lookup_descriptor.contains("parameters_schema"),
            "conditional outcome lookup retains its existing descriptor outside this batch");
    auto current_request = read_request("project.current");
    const auto no_document = dispatch(current_request, caller);
    current_request.insert("requested_version", 1);
    require(no_document.value("status") == "failed" &&
                no_document.value("error").toObject().value("code") == "DOCUMENT_NOT_FOUND" &&
                dispatch(current_request, caller) == no_document &&
                open_contract_state(app, *store, caller) == inactive,
            "current preserves its no-active-document result for omitted and installed versions");

    const auto created = runtime_test::good(app.create_document(caller, "Host controls", "create"));
    auto info = created;
    const auto empty_view =
        runtime_test::good(app.record_application().snapshot(info.document)).records;
    const auto preview = runtime_test::good(
        app.preview(caller, runtime_test::at(info), CreateMaterial{"Steel", {210, "GPa"}}));
    const auto committed =
        dispatch(write_lifecycle_request("changes.commit",
                                         {{"preview_id", QString::fromStdString(preview.id.value)}},
                                         info,
                                         "history-C"),
                 caller);
    require(committed.value("status") == "success", "seed one actual committed history item");
    info = runtime_test::good(app.current_document());
    info = runtime_test::good(
        app.save_document(caller, runtime_test::at(info), "/host-controls.qcae", false, "save"));
    const auto saved_view =
        runtime_test::good(app.record_application().snapshot(info.document)).records;
    const auto saved_history = runtime_test::good(app.history(info.document));
    require(!info.dirty && info.material_count == 1 && saved_history.items.size() == 1 &&
                saved_history.cursor == 1 && store->projects.contains("/host-controls.qcae"),
            "shared scenario starts with one saved model and a real application history");
    const auto unchanged = [&](const OpenContractState& before, const DocumentView& before_view) {
        const auto after =
            runtime_test::good(app.record_application().snapshot(info.document)).records;
        require(open_contract_state(app, *store, caller) == before &&
                    after.matches_version(before_view.version()) &&
                    diff_record_views(before_view, after).empty(),
                "host reads/refusals/replays preserve the complete record view, saved/dirty "
                "metadata, rows, projects, generations, history and store counters");
    };
    const auto reject = [&](const QJsonObject& invalid,
                            const char* code,
                            const char* field,
                            const char* status = "failed",
                            const Caller& actor = Caller{"host-controls-test"}) {
        const auto before = open_contract_state(app, *store, caller);
        const auto before_view =
            runtime_test::good(app.record_application().snapshot(info.document)).records;
        for (int retry = 0; retry < 2; ++retry) {
            const auto response = dispatch(invalid, actor);
            const auto error = response.value("error").toObject();
            require(response.value("status") == status && error.value("code") == code &&
                        error.value("field").toString() == field,
                    "host boundary refuses the exact input/version/context without reserving a "
                    "mutation key");
            unchanged(before, before_view);
        }
    };
    const auto document_request = [&](const char* operation) {
        auto request = read_request(operation);
        request.insert("document_id", QString::fromStdString(info.document.id.value));
        request.insert("document_epoch", QString::fromStdString(info.document.epoch.value));
        return request;
    };
    for (const auto& contract : contracts) {
        auto request = contract.document ? document_request(contract.operation)
                                         : read_request(contract.operation);
        if (contract.write) {
            request.insert("expected_revision", QString::number(info.revision));
            request.insert("idempotency_key",
                           std::string_view(contract.operation) == "history.undo" ? "history-U"
                                                                                  : "history-R");
        }
        for (const auto& version : {QJsonValue(0),
                                    QJsonValue(-1),
                                    QJsonValue(1.5),
                                    QJsonValue(4294967296.0),
                                    QJsonValue(true),
                                    QJsonValue("1"),
                                    QJsonValue(QJsonValue::Null),
                                    QJsonValue(QJsonArray{}),
                                    QJsonValue(QJsonObject{})}) {
            auto invalid = request;
            invalid.insert("requested_version", version);
            reject(invalid, "INVALID_INPUT", "requested_version");
        }
        for (const auto& version : {QJsonValue(2), QJsonValue(4294967295.0)}) {
            auto invalid = request;
            invalid.insert("requested_version", version);
            reject(invalid, "SCHEMA_UNSUPPORTED", "requested_version");
        }
        for (const auto& extra :
             {QJsonValue("unexpected"), QJsonValue(1), QJsonValue(QJsonValue::Null)}) {
            auto invalid = request;
            invalid.insert("parameters", QJsonObject{{"extra", extra}});
            reject(invalid, "INVALID_INPUT", "input.extra");
        }
        for (const auto& shape : {QJsonValue(QJsonArray{}),
                                  QJsonValue("input"),
                                  QJsonValue(1),
                                  QJsonValue(QJsonValue::Null)}) {
            auto invalid = request;
            invalid.insert("parameters", shape);
            reject(invalid, "INVALID_INPUT", "");
        }
        auto invalid = request;
        invalid.insert("expected_profile", QJsonObject{});
        reject(invalid, "INVALID_INPUT", "");
        // Input/version diagnostics precede context extraction and any application call.
        for (const auto* field : {"document_id", "document_epoch", "expected_revision"})
            invalid.remove(field);
        invalid.remove("expected_profile");
        invalid.insert("requested_version", 2);
        reject(invalid, "SCHEMA_UNSUPPORTED", "requested_version");
        invalid.remove("requested_version");
        invalid.insert("parameters", QJsonObject{{"extra", "unexpected"}});
        reject(invalid, "INVALID_INPUT", "input.extra");
    }

    auto status_data = ipc::info_json(info);
    status_data.insert("geometry_count", 0);
    status_data.insert("mesh_count", 0);
    auto summary_data = status_data;
    summary_data.insert(
        "materials",
        QJsonArray{QJsonObject{{"entity_id", QString::fromStdString(preview.affected_entity.value)},
                               {"name", "Steel"},
                               {"young_modulus_mpa", 210000}}});
    for (const auto* count : {"node_count",
                              "beam_count",
                              "section_count",
                              "part_count",
                              "assembly_count",
                              "set_count",
                              "include_count",
                              "analysis_count"})
        summary_data.insert(count, 0);
    const QJsonObject history_data{
        {"items",
         QJsonArray{QJsonObject{
             {"transaction_id", QString::fromStdString(saved_history.items[0].transaction.value)},
             {"label", QString::fromStdString(saved_history.items[0].label)},
             {"applied", true}}}},
        {"cursor", 1},
        {"revision", QString::number(info.revision)}};
    const std::map<std::string, QJsonObject> expected_reads{
        {"capabilities.list",
         dispatch(read_request("capabilities.list"), caller).value("data").toObject()},
        {"project.current", ipc::info_json(info)},
        {"project.status", status_data},
        {"model.summary", summary_data},
        {"history.list", history_data}};
    for (const auto& contract : contracts) {
        if (contract.write)
            continue;
        auto request = contract.document ? document_request(contract.operation)
                                         : read_request(contract.operation);
        require(!request.contains("expected_revision") && !request.contains("idempotency_key"),
                "all five reads require no write revision or mutation key");
        const auto before = open_contract_state(app, *store, caller);
        const auto before_view =
            runtime_test::good(app.record_application().snapshot(info.document)).records;
        const auto response = dispatch(request, caller);
        require(response.value("status") == "success" &&
                    response.value("data") == expected_reads.at(contract.operation) &&
                    (std::string_view(contract.operation) == "capabilities.list"
                         ? !response.contains("revision")
                         : response.value("revision") == QString::number(info.revision)),
                "omitted-version reads preserve each full existing result and revision projection");
        unchanged(before, before_view);
        request.insert("requested_version", 1);
        require(dispatch(request, caller) == response,
                "installed-version reads return the identical complete wire response");
        unchanged(before, before_view);
        if (contract.document) {
            for (const auto* field : {"document_id", "document_epoch"}) {
                auto invalid = request;
                invalid.remove(field);
                reject(invalid, "INVALID_INPUT", "");
            }
            auto invalid = request;
            invalid.insert("document_epoch", "old-epoch");
            reject(invalid, "DOCUMENT_EPOCH_EXPIRED", "document_epoch", "conflict");
            invalid.insert("document_id", "another-document");
            reject(invalid, "DOCUMENT_NOT_FOUND", "document_id");
        }
    }

    const auto move = [&](const QJsonObject& request,
                          std::size_t cursor,
                          bool dirty,
                          const DocumentView& expected_view) {
        const auto before = open_contract_state(app, *store, caller);
        const auto revision = info.revision;
        const auto response = dispatch(request, caller);
        info = runtime_test::good(app.current_document());
        const auto history = runtime_test::good(app.history(info.document));
        const auto view =
            runtime_test::good(app.record_application().snapshot(info.document)).records;
        const auto data = response.value("data").toObject();
        const QJsonObject expected{
            {"transaction_id", data.value("transaction_id")},
            {"committed_revision", QString::number(revision + 1)},
            {"current_revision", QString::number(revision + 1)},
            {"current_content_state", QString::fromStdString(info.content_state)},
            {"replayed", false},
            {"entity_id", ""}};
        require(response.value("status") == "success" && data == expected &&
                    !data.value("transaction_id").toString().isEmpty() &&
                    response.value("revision") == QString::number(revision + 1) &&
                    info.revision == revision + 1 && info.dirty == dirty &&
                    info.saved_path == "/host-controls.qcae" &&
                    info.saved_content_state ==
                        before.document.value("saved_content_state").toString().toStdString() &&
                    store->records.generation == before.generation + 1 &&
                    store->commits == before.commits + 1 && store->publishes == before.publishes &&
                    store->project_reads == before.project_reads &&
                    history.revision == info.revision && history.items.size() == 1 &&
                    history.cursor == cursor &&
                    history.items[0].transaction == saved_history.items[0].transaction &&
                    history.items[0].label == saved_history.items[0].label &&
                    history.items[0].applied == (cursor == 1) &&
                    diff_record_views(expected_view, view).empty(),
                "undo/redo each perform one persistent revision and cursor move, preserve saved "
                "metadata/history facts and return the unchanged complete receipt shape");
        return data;
    };
    const auto replay = [&](const QJsonObject& request, const QJsonObject& original) {
        const auto before = open_contract_state(app, *store, caller);
        const auto before_view =
            runtime_test::good(app.record_application().snapshot(info.document)).records;
        auto expected = original;
        expected.insert("replayed", true);
        expected.insert("current_revision", QString::number(info.revision));
        expected.insert("current_content_state", QString::fromStdString(info.content_state));
        const auto response = dispatch(request, caller);
        require(response.value("status") == "success" && response.value("data") == expected &&
                    response.value("revision") == QString::number(info.revision),
                "retry/lookup returns the original transaction with current state facts");
        unchanged(before, before_view);
    };
    // Both keys survived every malformed input/version; an inapplicable redo also consumes none.
    reject(write_lifecycle_request("history.redo", {}, info, "history-R"),
           "NOTHING_TO_REDO",
           "",
           "conflict");
    auto undo_request = write_lifecycle_request("history.undo", {}, info, "history-U");
    const auto undone = move(undo_request, 0, true, empty_view);
    undo_request.insert("requested_version", 1);
    replay(undo_request, undone);
    reject(write_lifecycle_request("history.undo", {}, info, "unavailable-undo"),
           "NOTHING_TO_UNDO",
           "",
           "conflict");
    auto redo_request = write_lifecycle_request("history.redo", {}, info, "history-R");
    redo_request.insert("requested_version", 1);
    const auto redone = move(redo_request, 1, false, saved_view);
    redo_request.remove("requested_version");
    replay(redo_request, redone);
    replay(undo_request, undone);
    require(undone.value("transaction_id") != redone.value("transaction_id") &&
                runtime_test::good(app.history(info.document)).cursor == 1 &&
                info.material_count == 1,
            "undo U, redo R, retry original U retains the fully redone model and cursor");
    reject(write_lifecycle_request("history.redo", {}, info, "unavailable-redo"),
           "NOTHING_TO_REDO",
           "",
           "conflict");
    for (const auto& request : {undo_request, redo_request}) {
        auto invalid = request;
        invalid.insert("idempotency_key", "unrecorded-key");
        reject(invalid, "REVISION_CONFLICT", "expected_revision", "conflict");
        reject(request, "REVISION_CONFLICT", "expected_revision", "conflict", other);
        invalid = request;
        invalid.insert("expected_revision", QString::number(info.revision));
        reject(invalid, "IDEMPOTENCY_KEY_CONFLICT", "idempotency_key", "conflict");
        invalid = request;
        invalid.insert("document_epoch", "old-epoch");
        reject(invalid, "DOCUMENT_EPOCH_EXPIRED", "document_epoch", "conflict");
        invalid.insert("document_id", "another-document");
        reject(invalid, "DOCUMENT_NOT_FOUND", "document_id");
    }
    for (const auto& [operation, key, original] :
         std::vector<std::tuple<const char*, const char*, QJsonObject>>{
             {"changes.commit", "history-C", committed.value("data").toObject()},
             {"history.undo", "history-U", undone},
             {"history.redo", "history-R", redone}}) {
        auto lookup = document_request("operations.get");
        lookup.insert("parameters",
                      QJsonObject{{"lookup_scope", "document"},
                                  {"original_operation", operation},
                                  {"original_mode", 7},
                                  {"idempotency_key", key}});
        replay(lookup, original);
        reject(lookup, "ENTITY_NOT_FOUND", "idempotency_key", "failed", other);
        auto parameters = lookup.value("parameters").toObject();
        parameters.insert("idempotency_key", "unrecorded-key");
        lookup.insert("parameters", parameters);
        reject(lookup, "ENTITY_NOT_FOUND", "idempotency_key");
        lookup.insert("requested_version", 1);
        reject(lookup, "INVALID_INPUT", "");
    }
    auto host_lookup = read_request("operations.get");
    host_lookup.insert("parameters",
                       QJsonObject{{"lookup_scope", "host"},
                                   {"original_operation", "project.create"},
                                   {"original_mode", QJsonValue(QJsonValue::Null)},
                                   {"idempotency_key", "create"}});
    const auto before_lookup = open_contract_state(app, *store, caller);
    const auto before_lookup_view =
        runtime_test::good(app.record_application().snapshot(info.document)).records;
    const auto found = dispatch(host_lookup, caller);
    require(found.value("status") == "success" && found.value("data") == ipc::info_json(created),
            "host lookup still needs no document and ignores irrelevant original_mode values");
    unchanged(before_lookup, before_lookup_view);
}
void diagnostic_reports_survive_transport() {
    using namespace qcae;
    using namespace operations;
    MemoryApplication application;
    const Caller caller{"report-reader"};
    const auto info = runtime_test::good(application.create_document(caller, "Report", "create"));
    Status outcome = Status::needs_input;
    ipc::TypedHost host(
        application.record_application(),
        [](const auto&) { return true; },
        [&](OperationRegistry& registry, RecordApplication&, std::function<TaskService&()>) {
            return registry.register_typed<DiagnosticReportInput>(
                InputTraits<DiagnosticReportInput>::definition(),
                [&](const OperationContext&, const DiagnosticReportInput&) -> Result<Value> {
                    return {outcome,
                            Value(Value::Object{{"check_id", Value("check-1")},
                                                {"issues",
                                                 Value(Value::Array{Value(Value::Object{
                                                     {"rule_id", Value("missing-load")}})})}}),
                            Diagnostic{outcome == Status::needs_input ? ErrorCode::missing_input
                                                                      : ErrorCode::invalid_input,
                                       "The scenario has unresolved physical facts.",
                                       "forces"}};
                });
        });
    const QJsonObject request{{"api_version", "1.1"},
                              {"request_id", "report-request"},
                              {"operation", "test.diagnostic_report"},
                              {"parameters", QJsonObject{}}};
    for (const auto status : {Status::needs_input, Status::failed}) {
        outcome = status;
        const auto response =
            ipc::dispatch(application, request, caller, nullptr, nullptr, nullptr, &host);
        const auto data = response.value("data").toObject();
        require(response.value("status") == QString::fromUtf8(status_name(status)) &&
                    response.value("error").toObject().value("field") == "forces" &&
                    data.value("check_id") == "check-1" &&
                    data.value("issues").toArray().size() == 1,
                "JSON transport preserves non-success status, diagnostic and report facts");
    }
    require(runtime_test::good(application.current_document()).revision == info.revision &&
                runtime_test::good(application.history(info.document)).items.empty(),
            "reading a diagnostic response adds no model change or history");
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
        {
            const qcae::operations::Value::Array history{
                qcae::operations::Value(qcae::operations::Value::Object{
                    {"id", qcae::operations::Value(std::string(121, '\1'))},
                    {"ordinal",
                     qcae::operations::Value(std::numeric_limits<std::int64_t>::max())}})};
            const auto expected = QJsonDocument(qcae::ipc::detail::typed_json_array(history))
                                      .toJson(QJsonDocument::Compact);
            auto measurement = std::make_shared<qcae::ledger::OperationLedger>(
                qcae::ledger::Identity{"quota-scratch-encoding", {}, {}, 0});
            qcae::ledger::activate(measurement);
            const auto bytes = qcae::ipc::detail::typed_json_array_bytes(history);
            qcae::ledger::activate({});
            const auto observed = measurement->snapshot();
            const auto& metrics =
                observed.values[static_cast<std::size_t>(qcae::ledger::Stage::socket_send)];
            require(bytes == static_cast<std::size_t>(expected.size()) &&
                        metrics[static_cast<std::size_t>(qcae::ledger::Metric::encoded_bytes)] ==
                            bytes &&
                        observed.frames.empty(),
                    "Quota scratch JSON encoding must count actual escaped bytes without a socket "
                    "frame");
            const auto parsed = QJsonDocument::fromJson(expected).array().at(0).toObject();
            require(parsed.value("id").toString() == QString(121, QChar(1)) &&
                        parsed.value("ordinal").toString() == "9223372036854775807",
                    "Quota measurement must use the actual control-string/int64 response codec");
        }
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
        diagnostic_reports_survive_transport();
        project_open_contract_and_lifecycle();
        project_create_contract_and_replay();
        project_save_close_contract_and_replay();
        changes_commit_contract_and_replay();
        host_read_and_history_contracts();
        std::cout << "PASS typed JSON transport, mesh idempotency and task fault recovery\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
