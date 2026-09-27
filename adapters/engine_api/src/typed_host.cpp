#include "qcae/typed_host.hpp"
#include "qcae/geometry_features.hpp"
#include "qcae/ipc_api.hpp"
#include "qcae/line_mesh_task.hpp"
#include "qcae/material_operations.hpp"
#include "qcae/mesh_editing_operations.hpp"
#include "qcae/query.hpp"
#include <charconv>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace qcae::ipc {
struct TypedHost::State {
    operations::OperationRegistry registry;
};
namespace {
using namespace operations;
QString qs(std::string_view value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}
std::string string(const QJsonObject& object, const char* key) {
    const auto value = object.value(QLatin1String(key));
    if (!value.isString() || value.toString().trimmed().isEmpty())
        throw std::invalid_argument(std::string("Expected nonempty string: ") + key);
    return value.toString().toStdString();
}
Value decode(const QJsonValue& value) {
    if (value.isBool())
        return Value(value.toBool());
    if (value.isDouble()) {
        const auto number = value.toDouble();
        if (std::isfinite(number) && std::trunc(number) == number &&
            number >= static_cast<double>(std::numeric_limits<std::int64_t>::min()) &&
            number < static_cast<double>(std::numeric_limits<std::int64_t>::max()))
            return Value(static_cast<std::int64_t>(number));
        return Value(number);
    }
    if (value.isString())
        return Value(value.toString().toStdString());
    if (value.isArray()) {
        Value::Array result;
        for (const auto& item : value.toArray())
            result.push_back(decode(item));
        return Value(std::move(result));
    }
    if (value.isObject()) {
        Value::Object result;
        const auto object = value.toObject();
        for (auto it = object.begin(); it != object.end(); ++it)
            result.emplace(it.key().toStdString(), decode(it.value()));
        return Value(std::move(result));
    }
    throw std::invalid_argument("Null parameters are unsupported; omit optional fields");
}
QJsonValue encode(const Value& value) {
    return std::visit(
        [](const auto& item) -> QJsonValue {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, std::string>)
                return qs(item);
            else if constexpr (std::is_same_v<T, std::int64_t>)
                return QString::number(item);
            else if constexpr (std::is_same_v<T, Value::Array>) {
                QJsonArray result;
                for (const auto& child : item)
                    result.append(encode(child));
                return result;
            } else if constexpr (std::is_same_v<T, Value::Object>) {
                QJsonObject result;
                for (const auto& [key, child] : item)
                    result.insert(qs(key), encode(child));
                return result;
            } else
                return item;
        },
        value.data);
}
Value task_value(const TaskRecord& task) {
    Value::Array events;
    for (const auto& event : task.events)
        events.emplace_back(Value::Object{{"sequence", Value(std::to_string(event.sequence))},
                                          {"state", Value(task_state_name(event.state))},
                                          {"progress", Value(event.progress)}});
    Value::Object object{{"task_id", Value(task.id)},
                         {"state", Value(task_state_name(task.state))},
                         {"progress", Value(task.progress)},
                         {"input_revision", Value(std::to_string(task.input.revision))},
                         {"events", Value(std::move(events))}};
    if (task.receipt)
        object.emplace("receipt", change_receipt_value(*task.receipt));
    if (task.diagnostic)
        object.emplace("diagnostic",
                       Value(Value::Object{{"code", Value(error_name(task.diagnostic->code))},
                                           {"message", Value(task.diagnostic->message)},
                                           {"field", Value(task.diagnostic->field)}}));
    return Value(std::move(object));
}
template <class T, class Convert>
Result<Value> converted(const Result<T>& result, Convert convert) {
    if (!result.ok())
        return {result.status, {}, result.error};
    return {Status::success, convert(*result.value), {}};
}
QJsonObject response(const QString& id, const Result<Value>& result) {
    if (!result.ok()) {
        auto output = failure(id,
                              qs(error_name(result.error->code)),
                              qs(result.error->message),
                              qs(status_name(result.status)));
        auto error = output.value("error").toObject();
        error.insert("field", qs(result.error->field));
        output.insert("error", error);
        return output;
    }
    const auto data = encode(*result.value).toObject();
    QJsonObject output{{"request_id", id}, {"status", "success"}, {"data", data}};
    if (data.contains("current_revision"))
        output.insert("revision", data.value("current_revision"));
    else if (data.contains("revision"))
        output.insert("revision", data.value("revision"));
    return output;
}
OperationContext context(const QJsonObject& request, const Caller& caller) {
    OperationContext result;
    result.caller = caller;
    result.request_id = string(request, "request_id");
    if (request.contains("document_id") || request.contains("document_epoch"))
        result.document = DocumentRef{DocumentId(string(request, "document_id")),
                                      DocumentEpoch(string(request, "document_epoch"))};
    if (request.contains("expected_revision")) {
        const auto text = string(request, "expected_revision");
        Revision revision{};
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), revision);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
            throw std::invalid_argument("expected_revision must be an unsigned integer string");
        result.expected_revision = revision;
    }
    if (request.contains("idempotency_key"))
        result.idempotency_key = string(request, "idempotency_key");
    return result;
}
Value profile_value(const ProfileRef& profile) {
    return Value(Value::Object{{"profile_id", Value(profile.profile_id)},
                               {"profile_version", Value(profile.profile_version)},
                               {"definition_digest", Value(profile.definition_digest)}});
}
Value field_value(const RecordFieldInput& field) {
    switch (field.kind) {
    case RecordFieldKind::text:
    case RecordFieldKind::reference:
        return Value(record_wire::read_text(field.payload));
    case RecordFieldKind::real:
        return Value(record_wire::read_real(field.payload));
    case RecordFieldKind::unsigned_integer:
        return Value(std::to_string(record_wire::read_number(field.payload)));
    case RecordFieldKind::boolean:
        return Value(record_wire::read_boolean(field.payload));
    case RecordFieldKind::vector3:
        return wire::to_value(record_wire::read_vector3(field.payload));
    case RecordFieldKind::references: {
        Value::Array result;
        for (const auto& identity : record_wire::read_strings(field.payload))
            result.emplace_back(identity);
        return Value(std::move(result));
    }
    case RecordFieldKind::profile:
        return profile_value(record_wire::read_profile(field.payload));
    case RecordFieldKind::target: {
        const auto target = record_wire::read_target(field.payload);
        return Value(Value::Object{{"profile", profile_value(target.profile)},
                                   {"analysis_kind", Value(target.analysis_kind)}});
    }
    }
    throw std::invalid_argument("Unsupported record field type");
}
void checked(const Result<bool>& result) {
    if (!result.ok())
        throw std::runtime_error(result.error->message);
}
} // namespace

TypedHost::TypedHost(RecordApplication& app, std::function<bool(const ProfileRef&)> supported)
    : app_(app), profile_supported_(std::move(supported)), state_(std::make_unique<State>()) {
    checked(features::materials::register_handlers(state_->registry, app_));
    checked(features::mesh_editing::register_handlers(state_->registry, app_));
    checked(state_->registry.register_typed<GeometryCreateLineInput>(
        InputTraits<GeometryCreateLineInput>::definition(),
        [this](const OperationContext& ctx, const GeometryCreateLineInput& input) {
            const LineGeometryInput line{input.start_mm, input.end_mm};
            return converted(app_.execute(ctx.caller,
                                          {*ctx.document, *ctx.expected_revision},
                                          "geometry.create_line",
                                          line_geometry_signature(line),
                                          create_line_handler(line),
                                          ctx.idempotency_key),
                             change_receipt_value);
        }));
    checked(state_->registry.register_typed<MeshGenerateLineInput>(
        InputTraits<MeshGenerateLineInput>::definition(),
        [this](const OperationContext& ctx, const MeshGenerateLineInput& input) -> Result<Value> {
            const auto snapshot = app_.snapshot(*ctx.document);
            if (!snapshot.ok())
                return {snapshot.status, {}, snapshot.error};
            try {
                auto request = line_mesh_task(
                    *snapshot.value,
                    ctx.caller,
                    {},
                    {records::GeometryId(input.geometry_id.value), input.segments, {}},
                    ctx.idempotency_key);
                // Deduplication compares the original submitted context before freshness checks.
                request.input.revision = *ctx.expected_revision;
                return converted(tasks().start(std::move(request)), task_value);
            } catch (const RecordError& error) {
                return {Status::failed, {}, Diagnostic{error.code(), error.what(), error.field()}};
            }
        }));
}
TypedHost::~TypedHost() = default;
TaskService& TypedHost::tasks() {
    if (!tasks_) {
        if (app_.recovery_available())
            throw RecordError(ErrorCode::storage_uncertain,
                              "Explicit application recovery is required before task access");
        tasks_ = std::make_unique<TaskService>(record_task_publisher(app_, profile_supported_));
    }
    return *tasks_;
}
Result<bool> TypedHost::reconcile() {
    return tasks_ ? tasks_->reconcile() : Result<bool>{Status::success, true, {}};
}
bool TypedHost::supports(std::string_view operation) const {
    if (operation == "task.status" || operation == "task.cancel" || operation == "task.reconcile" ||
        operation == "entity.fields")
        return true;
    for (const auto& descriptor : state_->registry.descriptors())
        if (descriptor.definition.operation_id == operation)
            return true;
    return false;
}
QJsonArray TypedHost::capabilities() const {
    QJsonArray result;
    for (const auto& descriptor : state_->registry.descriptors()) {
        const auto& definition = descriptor.definition;
        QJsonArray fields;
        for (const auto& field : definition.fields) {
            QJsonArray units;
            for (const auto& unit : field.units)
                units.append(qs(unit));
            fields.append(QJsonObject{{"name", qs(field.name)},
                                      {"wire_type", qs(field.wire_type)},
                                      {"required", field.required},
                                      {"units", units}});
        }
        const auto effect =
            definition.effect == OperationEffect::background_task  ? "background_task"
            : definition.effect == OperationEffect::document_write ? "document_write"
            : definition.effect == OperationEffect::preview        ? "preview"
                                                                   : "read_only";
        result.append(QJsonObject{
            {"name", qs(definition.operation_id)},
            {"effect", effect},
            {"version", static_cast<int>(definition.version)},
            {"schema_id", qs(definition.schema_id)},
            {"available", descriptor.available},
            {"implementation_status", descriptor.available ? "implemented" : "unavailable"},
            {"requires_document", definition.context.document},
            {"requires_epoch", definition.context.epoch},
            {"requires_revision", definition.context.expected_revision},
            {"requires_idempotency_key", definition.context.idempotency_key},
            {"fields", fields}});
    }
    for (const auto* name : {"task.status", "task.cancel", "task.reconcile", "entity.fields"})
        result.append(QJsonObject{{"name", name},
                                  {"available", true},
                                  {"implementation_status", "implemented"},
                                  {"requires_document", true},
                                  {"requires_epoch", true}});
    return result;
}
QJsonObject TypedHost::dispatch(const QJsonObject& request, const Caller& caller) {
    const auto id = request.value("request_id").toString();
    try {
        const auto operation = string(request, "operation");
        const auto ctx = context(request, caller);
        if (caller.principal.empty())
            throw std::invalid_argument("Trusted caller identity is required");
        if (operation == "task.reconcile") {
            if (!ctx.document || !request.value("parameters").toObject().isEmpty())
                throw std::invalid_argument(
                    "Task reconciliation requires document context and empty parameters");
            const auto snapshot = app_.snapshot(*ctx.document);
            if (!snapshot.ok())
                return response(id, {snapshot.status, {}, snapshot.error});
            return response(id, converted(tasks().reconcile(), [](bool reconciled) {
                                return Value(Value::Object{{"reconciled", Value(reconciled)}});
                            }));
        }
        if (operation == "entity.fields") {
            const auto params = request.value("parameters").toObject();
            if (params.size() != 1 || !params.contains("entity_id") || !ctx.document)
                throw std::invalid_argument(
                    "Entity fields requires document context and only entity_id");
            const auto snapshot = app_.snapshot(*ctx.document);
            if (!snapshot.ok())
                return response(id, {snapshot.status, {}, snapshot.error});
            const EntityId identity(string(params, "entity_id"));
            const auto fields = query_fields(snapshot.value->records, identity);
            if (!fields.ok())
                return response(id, {fields.status, {}, fields.error});
            const auto record = snapshot.value->records.find_identity(identity.value);
            Value::Object output;
            for (const auto& field : fields.value->fields) {
                for (const auto& descriptor : record->descriptor().fields)
                    if (descriptor.id == field.id)
                        output.emplace(descriptor.name, field_value(field));
            }
            return response(id,
                            {Status::success,
                             Value(Value::Object{
                                 {"entity_id", Value(identity.value)},
                                 {"kind", Value(record->descriptor().query_kind)},
                                 {"revision", Value(std::to_string(snapshot.value->info.revision))},
                                 {"fields", Value(std::move(output))}}),
                             {}});
        }
        if (operation == "task.status" || operation == "task.cancel") {
            const auto params = request.value("parameters").toObject();
            if (params.size() != 1 || !params.contains("task_id") || !ctx.document)
                throw std::invalid_argument(
                    "Task requests require document context and only task_id");
            const auto snapshot = app_.snapshot(*ctx.document);
            if (!snapshot.ok())
                return response(id, {snapshot.status, {}, snapshot.error});
            const auto task_id = string(params, "task_id");
            const auto current = tasks().query(caller, task_id);
            if (!current.ok())
                return response(id, converted(current, task_value));
            if (current.value->input.document.id != ctx.document->id)
                return failure(id, "ENTITY_NOT_FOUND", "Task belongs to another document");
            if (operation == "task.status")
                return response(id, converted(current, task_value));
            return response(
                id, converted(tasks().cancel(caller, task_id), [](const TaskCancelResult& value) {
                    auto output = std::get<Value::Object>(task_value(value.task).data);
                    output.emplace("accepted", Value(value.accepted));
                    return Value(std::move(output));
                }));
        }
        return response(
            id, state_->registry.invoke(operation, ctx, decode(request.value("parameters"))));
    } catch (const std::invalid_argument& error) {
        return failure(id, "INVALID_INPUT", qs(error.what()));
    } catch (const RecordError& error) {
        return failure(id, qs(error_name(error.code())), qs(error.what()));
    }
}
} // namespace qcae::ipc
