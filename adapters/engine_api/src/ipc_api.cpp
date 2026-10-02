#include "qcae/ipc_api.hpp"
#include "qcae/json_ledger.hpp"
#include "qcae/ipc_model.hpp"
#include "qcae/ipc_selection.hpp"
#include "qcae/operations.hpp"
#include "qcae/operation_inputs.hpp"
#include "qcae/typed_host.hpp"
#include "qcae/records.hpp"

#include <QJsonArray>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace qcae::ipc {
namespace {
QString qs(std::string_view value) {
    return transport::json_ledger::from_utf8(value);
}
QString number(Revision revision) {
    return transport::json_ledger::number(revision);
}
struct InvalidRequest : std::runtime_error {
    std::string field;
    explicit InvalidRequest(const std::string& message, std::string input_field = {})
        : std::runtime_error(message), field(std::move(input_field)) {}
};

QString string_field(const QJsonObject& object, const char* key, bool allow_empty = false) {
    const auto value = object.value(QLatin1String(key));
    if (!value.isString() || (!allow_empty && value.toStringView().isEmpty()))
        throw InvalidRequest(std::string("Expected a non-empty string: ") + key);
    return transport::json_ledger::string(value);
}
void fields(const QJsonObject& object, std::initializer_list<const char*> allowed) {
    for (auto it = object.begin(); it != object.end(); ++it)
        if (std::none_of(allowed.begin(), allowed.end(), [&](const char* name) {
                return it.keyView() == QAnyStringView(QLatin1StringView(name));
            }))
            throw InvalidRequest("Unexpected field: " + it.key().toStdString());
}
// Explicit legacy read contracts use the same names and bounds as dispatch_model.
// Absence means the legacy contract has not yet been described, not an empty input.
std::optional<QJsonObject> read_parameters_schema(std::string_view operation) {
    QJsonObject properties;
    const QJsonObject identity{{"type", "string"}, {"minLength", 1}};
    QJsonArray required;
    if (operation == "entity.query") {
        properties = QJsonObject{
            {"kind",
             QJsonObject{{"type", "string"},
                         {"minLength", 1},
                         {"description", "Case-sensitive query kind, for example node or beam."}}},
            {"name_contains", QJsonObject{{"type", "string"}}},
            {"ids", QJsonObject{{"type", "array"}, {"items", identity}}},
            {"view", identity},
            {"owner_id", identity},
            {"offset", QJsonObject{{"type", "integer"}, {"minimum", 0}, {"maximum", 100000}}},
            {"limit", QJsonObject{{"type", "integer"}, {"minimum", 0}, {"maximum", 1000}}}};
    } else if (operation == "entity.references") {
        properties = QJsonObject{
            {"entity_id", identity},
            {"direction",
             QJsonObject{{"type", "string"}, {"enum", QJsonArray{"incoming", "outgoing"}}}}};
        required.append("entity_id");
    } else if (operation != "model.summary" && operation != "project.status") {
        return {};
    }
    return QJsonObject{{"type", "object"},
                       {"properties", properties},
                       {"required", required},
                       {"additionalProperties", false}};
}
// Host lifecycle DTOs contain strings; their application semantics stay explicit below.
template <class Input> Input lifecycle_input(const QJsonObject& parameters) {
    operations::Value::Object input;
    for (auto it = parameters.begin(); it != parameters.end(); ++it) {
        const auto name = transport::json_ledger::utf8(it.keyView());
        if (!it.value().isString())
            throw InvalidRequest("Expected a non-empty string: " + name, "input." + name);
        input.emplace(name,
                      operations::Value(transport::json_ledger::utf8(it.value().toStringView())));
    }
    auto decoded = operations::InputTraits<Input>::from_value(operations::Value(std::move(input)));
    if (!decoded.ok())
        throw InvalidRequest(decoded.error->message, decoded.error->field);
    return std::move(*decoded.value);
}
std::optional<QJsonObject> lifecycle_version_rejection(const QJsonObject& request,
                                                       const QString& id,
                                                       std::uint32_t installed_version) {
    if (!request.contains("requested_version"))
        return {};
    const auto value = request.value("requested_version");
    const auto version = value.toDouble();
    if (!value.isDouble() || !std::isfinite(version) || std::trunc(version) != version ||
        version <= 0 || version > std::numeric_limits<std::uint32_t>::max())
        throw InvalidRequest("Expected a positive uint32 integer.", "requested_version");
    if (static_cast<std::uint32_t>(version) == installed_version)
        return {};
    auto rejected =
        failure(id, "SCHEMA_UNSUPPORTED", "Requested operation version is not installed");
    auto error = rejected.value("error").toObject();
    error.insert("field", "requested_version");
    rejected.insert("error", error);
    return rejected;
}
void describe_lifecycle_input(QJsonObject& entry,
                              const operations::OperationDefinition& definition,
                              std::string_view input_type) {
    QJsonArray fields, required;
    QJsonObject properties;
    for (const auto& field : definition.fields) {
        fields.append(QJsonObject{{"name", qs(field.name)},
                                  {"wire_type", qs(field.wire_type)},
                                  {"required", field.required},
                                  {"allow_empty", field.allow_empty},
                                  {"units", QJsonArray{}}});
        QJsonObject property{{"type", "string"}};
        if (!field.allow_empty)
            property.insert("minLength", 1);
        properties.insert(qs(field.name), property);
        if (field.required)
            required.append(qs(field.name));
    }
    entry.insert("version", static_cast<qint64>(definition.version));
    entry.insert("schema_id", qs(definition.schema_id));
    entry.insert("requested_version_field", "requested_version");
    entry.insert("omitted_version_policy", "installed_version");
    entry.insert("wire_input_type", qs(input_type));
    entry.insert("wire_output_type", "DocumentInfo");
    entry.insert("fields", fields);
    entry.insert("parameters_schema",
                 QJsonObject{{"type", "object"},
                             {"properties", properties},
                             {"required", required},
                             {"additionalProperties", false}});
}
void describe_project_open(QJsonObject& entry) {
    const auto definition = operations::InputTraits<operations::ProjectOpenInput>::definition();
    describe_lifecycle_input(entry, definition, "ProjectOpenInput");
    auto schema = entry.value("parameters_schema").toObject();
    auto properties = schema.value("properties").toObject();
    properties.insert("mode",
                      QJsonObject{{"type", "string"}, {"enum", QJsonArray{"normal", "recover"}}});
    const QJsonArray modes{
        QJsonObject{
            {"type", "object"},
            {"properties",
             QJsonObject{{"mode", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"normal"}}}},
                         {"path", properties.value("path")}}},
            {"required", QJsonArray{"mode", "path"}},
            {"additionalProperties", false}},
        QJsonObject{
            {"type", "object"},
            {"properties",
             QJsonObject{
                 {"mode", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"recover"}}}}}},
            {"required", QJsonArray{"mode"}},
            {"additionalProperties", false}}};
    schema.insert("properties", properties);
    schema.insert("oneOf", modes);
    entry.insert("parameters_schema", schema);
}
void describe_project_close(QJsonObject& entry) {
    describe_lifecycle_input(entry,
                             operations::InputTraits<operations::ProjectCloseInput>::definition(),
                             "ProjectCloseInput");
    auto schema = entry.value("parameters_schema").toObject();
    auto properties = schema.value("properties").toObject();
    auto policy = properties.value("policy").toObject();
    policy.insert("enum", QJsonArray{"discard", "keep_recovery"});
    properties.insert("policy", policy);
    schema.insert("properties", properties);
    entry.insert("parameters_schema", schema);
}
DocumentRef document_ref(const QJsonObject& request) {
    return {DocumentId{transport::json_ledger::utf8(string_field(request, "document_id"))},
            DocumentEpoch{transport::json_ledger::utf8(string_field(request, "document_epoch"))}};
}
WriteContext context(const QJsonObject& request) {
    const auto text = transport::json_ledger::utf8(string_field(request, "expected_revision"));
    Revision revision{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), revision);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        throw InvalidRequest("expected_revision must be an unsigned integer string");
    return {document_ref(request), revision};
}
Quantity quantity(const QJsonObject& params) {
    if (!params.value("young_modulus").isObject())
        throw InvalidRequest("young_modulus must be an object");
    const auto object = params.value("young_modulus").toObject();
    fields(object, {"value", "unit"});
    if (!object.value("value").isDouble())
        throw InvalidRequest("young_modulus.value must be numeric");
    const auto unit =
        object.contains("unit") ? string_field(object, "unit", true).toStdString() : std::string{};
    return {object.value("value").toDouble(), unit};
}
} // namespace
QJsonObject info_json(const DocumentInfo& info) {
    const QJsonObject output{{"document_id", qs(info.document.id.value)},
                             {"document_epoch", qs(info.document.epoch.value)},
                             {"revision", number(info.revision)},
                             {"content_state", qs(info.content_state)},
                             {"name", qs(info.name)},
                             {"material_count", static_cast<qint64>(info.material_count)},
                             {"dirty", info.dirty},
                             {"durable", info.durable},
                             {"project_id", qs(info.project_id)},
                             {"saved_path", qs(info.saved_path)},
                             {"saved_content_state", qs(info.saved_content_state)}};
    transport::json_ledger::object(output,
                                   {"document_id",
                                    "document_epoch",
                                    "revision",
                                    "content_state",
                                    "name",
                                    "material_count",
                                    "dirty",
                                    "durable",
                                    "project_id",
                                    "saved_path",
                                    "saved_content_state"});
    return output;
}
namespace {
template <class T, class Convert>
QJsonObject result_json(const QString& id, const Result<T>& result, Convert convert) {
    QJsonObject response{{"request_id", id}, {"status", qs(status_name(result.status))}};
    transport::json_ledger::ObjectCopies copies;
    copies.insert(QLatin1StringView("request_id"), id);
    copies.insert(QLatin1StringView("status"), response.value("status"));
    if (result.ok()) {
        const auto data = convert(*result.value);
        response.insert("data", data);
        copies.insert(QLatin1StringView("data"), data);
    } else if (result.error)
        response.insert("error",
                        QJsonObject{{"code", QString::fromLatin1(error_name(result.error->code))},
                                    {"message", qs(result.error->message)},
                                    {"field", qs(result.error->field)}});
    else
        response.insert("error",
                        QJsonObject{{"code", "INTERNAL_ERROR"}, {"message", "Missing diagnostic"}});
    return response;
}
QJsonObject receipt_json(const ChangeReceipt& receipt) {
    return {{"transaction_id", qs(receipt.transaction.value)},
            {"committed_revision", number(receipt.committed_revision)},
            {"current_revision", number(receipt.current_revision)},
            {"current_content_state", qs(receipt.current_content_state)},
            {"replayed", receipt.replayed},
            {"entity_id", qs(receipt.primary_entity.value)}};
}
bool supported(std::string_view name) {
    return name == "project.current" || name == "project.open" || name == "project.close" ||
           name == "project.save" || name == "project.save_as" || name == "view.create" ||
           name == "view.update" || name == "view.render_data" || name == "selection.evaluate" ||
           name == "selection.combine" || name == "selection.get" || name == "entity.query" ||
           name == "entity.references" || name == "model.export_preview" ||
           name == "project.create" || name == "project.status" || name == "model.summary" ||
           name == "changes.preview" || name == "changes.commit" || name == "history.list" ||
           name == "history.undo" || name == "history.redo" || name == "operations.get" ||
           name == "capabilities.list";
}
} // namespace
bool compatibility_operation_supported(std::string_view name) {
    return supported(name);
}

QJsonObject
failure(const QString& id, const QString& code, const QString& message, const QString& status) {
    return {{"request_id", id},
            {"status", status},
            {"error", QJsonObject{{"code", code}, {"message", message}}}};
}

QJsonObject dispatch(MemoryApplication& app,
                     const QJsonObject& request,
                     const Caller& caller,
                     const IModelCodec* codec,
                     const ProfileDefinition* profile,
                     SelectionService* selections,
                     TypedHost* typed,
                     bool display_services) {
    const QString id = request.value("request_id").isString()
                           ? transport::json_ledger::string(request.value("request_id"))
                           : QString{};
    if (id.toUtf8().size() > 128)
        return failure({}, "INVALID_INPUT", "request_id exceeds 128 UTF-8 bytes");
    try {
        fields(request,
               {"api_version",
                "request_id",
                "operation",
                "parameters",
                "document_id",
                "document_epoch",
                "expected_revision",
                "idempotency_key",
                "requested_version",
                "expected_profile"});
        (void)string_field(request, "request_id");
        if (string_field(request, "api_version") != qs(api_version))
            return failure(
                id, "API_VERSION_UNSUPPORTED", "Expected API version " + qs(api_version));
        const auto op = transport::json_ledger::utf8(string_field(request, "operation"));
        if (op == "model.export_preview" && (!codec || !profile))
            return failure(id, "UNSUPPORTED_CAPABILITY", "No model codec package is enabled");
        if ((!find_operation(op) || !supported(op)) && !(typed && typed->supports(op)))
            return failure(id,
                           "UNSUPPORTED_CAPABILITY",
                           "Operation is not implemented in the current memory slice");
        if (!request.value("parameters").isObject())
            throw InvalidRequest("parameters must be an object");
        const auto params = request.value("parameters").toObject();
        if (typed && typed->supports(op))
            return typed->dispatch(request, caller);
        const bool lifecycle = op == "project.open" || op == "project.create" ||
                               op == "project.save" || op == "project.save_as" ||
                               op == "project.close";
        if ((request.contains("requested_version") && !lifecycle) ||
            request.contains("expected_profile"))
            throw InvalidRequest("requested_version/expected_profile require a typed operation");
        if (selections) {
            if (const auto selection_response =
                    dispatch_selection(app, *selections, request, caller))
                return *selection_response;
        }
        if (const auto model_response = dispatch_model(app, request, caller, codec, profile))
            return *model_response;
        QJsonObject response;
        if (op == "capabilities.list") {
            fields(params, {});
            QJsonArray catalog;
            for (const auto& descriptor : operation_catalog) {
                if (typed && typed->supports(descriptor.name))
                    continue;
                bool available = supported(descriptor.name) &&
                                 (descriptor.name != "model.export_preview" || (codec && profile));
                QJsonObject entry{{"name", qs(descriptor.name)},
                                  {"description", qs(descriptor.description)},
                                  {"effect", qs(descriptor.effect)},
                                  {"input_type", qs(descriptor.input_type)},
                                  {"output_type", qs(descriptor.output_type)},
                                  {"target_context", qs(descriptor.target_context)},
                                  {"requires_document", descriptor.requires_document},
                                  {"requires_epoch", descriptor.requires_epoch},
                                  {"requires_revision", descriptor.requires_revision},
                                  {"requires_idempotency_key", descriptor.requires_idempotency_key},
                                  {"requires_profile_match", descriptor.requires_profile_match},
                                  {"implementation_status", available ? "partial" : "planned"}};
                if (display_services && (descriptor.name == "view.render_resource" ||
                                         descriptor.name.starts_with("resources.") ||
                                         descriptor.name.starts_with("events."))) {
                    entry.insert("implementation_status", "implemented");
                    entry.insert("requested_version", 1);
                    available = true;
                }
                if (descriptor.name == "project.open")
                    describe_project_open(entry);
                else if (descriptor.name == "project.create")
                    describe_lifecycle_input(
                        entry,
                        operations::InputTraits<operations::ProjectCreateInput>::definition(),
                        "ProjectCreateInput");
                else if (descriptor.name == "project.save")
                    describe_lifecycle_input(
                        entry,
                        operations::InputTraits<operations::ProjectSaveInput>::definition(),
                        "ProjectSaveInput");
                else if (descriptor.name == "project.save_as")
                    describe_lifecycle_input(
                        entry,
                        operations::InputTraits<operations::ProjectSaveAsInput>::definition(),
                        "ProjectSaveAsInput");
                else if (descriptor.name == "project.close")
                    describe_project_close(entry);
                entry.insert("available", available);
                if (const auto schema = read_parameters_schema(descriptor.name))
                    entry.insert("parameters_schema", *schema);
                if (descriptor.name == "changes.preview") {
                    QJsonArray commands{"material.create",
                                        "material.set_young_modulus",
                                        "part.upsert",
                                        "assembly.upsert",
                                        "set.upsert",
                                        "node.move",
                                        "entity.delete"};
                    if (codec && profile)
                        commands.append("model.import");
                    entry.insert("supported_commands", commands);
                }
                if (descriptor.name == "operations.get")
                    entry.insert("supported_scope", "host_lifecycle_and_document_change_outcomes");
                if (descriptor.name == "capabilities.list")
                    entry.insert("supported_scope", "global_catalog_only");
                catalog.append(entry);
            }
            if (typed) {
                for (const auto& descriptor : typed->capabilities())
                    catalog.append(descriptor);
            }
            const QJsonObject data{
                {"operations", catalog},
                {"storage_mode", app.durable() ? "sqlite" : "memory"},
                {"durable", app.durable()},
                {"recovery_available", app.recovery_available()},
                {"implementation_scope",
                 "Shared record services, analysis checks, resource deltas; optional Nastran "
                 "artifact publication and fixture results; no solver execution or "
                 "AI bridge"},
                {"max_name_bytes", 1024},
                {"configured_solver_profiles", QJsonArray{}},
                {"declared_solver_profiles",
                 profile ? QJsonArray{profile_json(*profile)} : QJsonArray{}},
                {"supported_pressure_units", QJsonArray{"Pa", "kPa", "MPa", "GPa"}}};
            response = {{"request_id", id}, {"status", "success"}, {"data", data}};
        } else if (op == "project.current") {
            fields(params, {});
            response = result_json(id, app.current_document(), info_json);
        } else if (op == "project.open") {
            const auto definition =
                operations::InputTraits<operations::ProjectOpenInput>::definition();
            if (const auto rejected = lifecycle_version_rejection(request, id, definition.version))
                return *rejected;
            const auto input = lifecycle_input<operations::ProjectOpenInput>(params);
            const auto key = string_field(request, "idempotency_key").toStdString();
            if (input.mode == "normal") {
                if (!input.path)
                    throw InvalidRequest("Use mode normal with path, or recover without path",
                                         "input.path");
                response = result_json(id, app.open_document(caller, *input.path, key), info_json);
            } else if (input.mode == "recover") {
                if (input.path)
                    throw InvalidRequest("Use mode normal with path, or recover without path",
                                         "input.path");
                response = result_json(id, app.recover_document(caller, key), info_json);
            } else
                throw InvalidRequest("Use mode normal with path, or recover without path",
                                     "input.mode");
        } else if (op == "project.save" || op == "project.save_as") {
            const bool save_as = op == "project.save_as";
            const auto definition =
                save_as ? operations::InputTraits<operations::ProjectSaveAsInput>::definition()
                        : operations::InputTraits<operations::ProjectSaveInput>::definition();
            if (const auto rejected = lifecycle_version_rejection(request, id, definition.version))
                return *rejected;
            const auto path =
                (save_as ? lifecycle_input<operations::ProjectSaveAsInput>(params).path
                         : lifecycle_input<operations::ProjectSaveInput>(params).path)
                    .value_or(std::string{});
            response = result_json(
                id,
                app.save_document(caller,
                                  context(request),
                                  path,
                                  save_as,
                                  string_field(request, "idempotency_key").toStdString()),
                info_json);
        } else if (op == "project.close") {
            const auto definition =
                operations::InputTraits<operations::ProjectCloseInput>::definition();
            if (const auto rejected = lifecycle_version_rejection(request, id, definition.version))
                return *rejected;
            const auto input = lifecycle_input<operations::ProjectCloseInput>(params);
            const auto& policy = input.policy;
            if (policy != "discard" && policy != "keep_recovery")
                throw InvalidRequest("Unknown close policy", "input.policy");
            response = result_json(
                id,
                app.close_document(caller,
                                   context(request),
                                   policy == "discard" ? ClosePolicy::discard
                                                       : ClosePolicy::keep_recovery,
                                   string_field(request, "idempotency_key").toStdString()),
                info_json);
        } else if (op == "project.create") {
            const auto definition =
                operations::InputTraits<operations::ProjectCreateInput>::definition();
            if (const auto rejected = lifecycle_version_rejection(request, id, definition.version))
                return *rejected;
            const auto input = lifecycle_input<operations::ProjectCreateInput>(params);
            response = result_json(
                id,
                app.create_document(
                    caller, input.name, string_field(request, "idempotency_key").toStdString()),
                info_json);
        } else if (op == "model.summary" || op == "project.status") {
            fields(params, {});
            response = result_json(
                id, app.snapshot(document_ref(request)), [&](const ModelSnapshot& snapshot) {
                    QJsonObject data = info_json(snapshot.info);
                    if (op == "model.summary") {
                        QJsonArray materials;
                        for (const auto& material : snapshot.materials)
                            materials.append(
                                QJsonObject{{"entity_id", qs(material.id.value)},
                                            {"name", qs(material.name)},
                                            {"young_modulus_mpa", material.young_modulus_mpa}});
                        data.insert("materials", materials);
                        data.insert("node_count", static_cast<qint64>(snapshot.nodes.size()));
                        data.insert("beam_count", static_cast<qint64>(snapshot.beams.size()));
                        data.insert("section_count", static_cast<qint64>(snapshot.sections.size()));
                        data.insert("part_count", static_cast<qint64>(snapshot.parts.size()));
                        data.insert("assembly_count",
                                    static_cast<qint64>(snapshot.assemblies.size()));
                        data.insert("set_count", static_cast<qint64>(snapshot.sets.size()));
                        data.insert("include_count", static_cast<qint64>(snapshot.includes.size()));
                        data.insert("analysis_count",
                                    static_cast<qint64>(snapshot.analyses.size()));
                    }
                    const auto records = app.record_application().snapshot(document_ref(request));
                    if (records.ok()) {
                        data.insert("geometry_count",
                                    static_cast<qint64>(records.value->records.count(
                                        RecordTraits<records::GeometryLine>::type_id)));
                        data.insert("mesh_count",
                                    static_cast<qint64>(records.value->records.count(
                                        RecordTraits<records::Mesh>::type_id)));
                    }
                    return data;
                });
        } else if (op == "changes.preview") {
            const auto command = string_field(params, "command");
            MaterialCommand material_command;
            if (command == "material.create") {
                fields(params, {"command", "name", "young_modulus"});
                material_command =
                    CreateMaterial{string_field(params, "name").toStdString(), quantity(params)};
            } else if (command == "material.set_young_modulus") {
                fields(params, {"command", "entity_id", "young_modulus"});
                material_command = SetYoungModulus{
                    EntityId{string_field(params, "entity_id").toStdString()}, quantity(params)};
            } else
                return failure(id, "UNSUPPORTED_CAPABILITY", "Unsupported material command");
            response = result_json(
                id,
                app.preview(caller, context(request), material_command),
                [](const ChangePreview& preview) {
                    return QJsonObject{
                        {"preview_id", qs(preview.id.value)},
                        {"affected_entity_id", qs(preview.affected_entity.value)},
                        {"normalized_young_modulus_mpa", preview.normalized_modulus_mpa},
                        {"creates_entity", preview.creates_entity},
                        {"revision", number(preview.context.expected_revision)}};
                });
        } else if (op == "changes.commit") {
            fields(params, {"preview_id"});
            response =
                result_json(id,
                            app.commit(caller,
                                       context(request),
                                       PreviewId{string_field(params, "preview_id").toStdString()},
                                       string_field(request, "idempotency_key").toStdString()),
                            receipt_json);
        } else if (op == "history.undo" || op == "history.redo") {
            fields(params, {});
            const auto ctx = context(request);
            const auto key = string_field(request, "idempotency_key").toStdString();
            response = result_json(id,
                                   op == "history.undo" ? app.undo(caller, ctx, key)
                                                        : app.redo(caller, ctx, key),
                                   receipt_json);
        } else if (op == "history.list") {
            fields(params, {});
            response = result_json(
                id, app.history(document_ref(request)), [](const HistorySnapshot& history) {
                    QJsonArray items;
                    for (const auto& item : history.items)
                        items.append(QJsonObject{{"transaction_id", qs(item.transaction.value)},
                                                 {"label", qs(item.label)},
                                                 {"applied", item.applied}});
                    return QJsonObject{{"items", items},
                                       {"cursor", static_cast<qint64>(history.cursor)},
                                       {"revision", number(history.revision)}};
                });
        } else if (op == "operations.get") {
            fields(params,
                   {"lookup_scope", "original_operation", "original_mode", "idempotency_key"});
            const auto lookup_scope = string_field(params, "lookup_scope");
            if (lookup_scope == "host") {
                auto original = string_field(params, "original_operation");
                if (original == "project.create")
                    original = "create_document";
                else if (original == "project.open") {
                    const auto mode = params.contains("original_mode")
                                          ? string_field(params, "original_mode")
                                          : QStringLiteral("normal");
                    if (mode != "normal" && mode != "recover")
                        throw InvalidRequest("Invalid original_mode");
                    original = mode == "recover" ? "recover_document" : "open_document";
                } else if (original == "project.save" || original == "project.save_as")
                    original = "save_document";
                else if (original == "project.close")
                    original = "close_document";
                else
                    throw InvalidRequest("Unknown lifecycle operation");
                return result_json(
                    id,
                    app.host_operation(caller,
                                       original.toStdString(),
                                       string_field(params, "idempotency_key").toStdString()),
                    info_json);
            }
            if (lookup_scope != "document")
                throw InvalidRequest("Unknown lookup_scope");
            auto original = string_field(params, "original_operation").toStdString();
            if (original == "changes.commit")
                original = "commit";
            else if (original == "history.undo")
                original = "undo";
            else if (original == "history.redo")
                original = "redo";
            else
                return result_json(id,
                                   app.record_application().action_outcome(
                                       caller,
                                       document_ref(request),
                                       original,
                                       string_field(params, "idempotency_key").toStdString()),
                                   receipt_json);
            response =
                result_json(id,
                            app.operation(caller,
                                          document_ref(request),
                                          original,
                                          string_field(params, "idempotency_key").toStdString()),
                            receipt_json);
        }
        if (typed && response.value("status").toString() == "success" &&
            (op == "project.create" || op == "project.open" || op == "project.close")) {
            const auto reconciled = typed->reconcile();
            if (!reconciled.ok())
                return failure(id,
                               QString::fromLatin1(error_name(reconciled.error->code)),
                               qs(reconciled.error->message));
        }
        if (response.value("data").isObject()) {
            const auto data = response.value("data").toObject();
            if (data.contains("current_revision"))
                response.insert("revision", data.value("current_revision"));
            else if (data.contains("revision"))
                response.insert("revision", data.value("revision"));
        }
        return response;
    } catch (const InvalidRequest& error) {
        auto rejected = failure(id, "INVALID_INPUT", QString::fromUtf8(error.what()));
        if (!error.field.empty()) {
            auto diagnostic = rejected.value("error").toObject();
            diagnostic.insert("field", qs(error.field));
            rejected.insert("error", diagnostic);
        }
        return rejected;
    } catch (const std::exception&) {
        return failure(id,
                       "INTERNAL_ERROR",
                       "Request failed without a completed response; query before retrying");
    }
}
} // namespace qcae::ipc
