#include "qcae/typed_host.hpp"
#include "qcae/json_ledger.hpp"
#include "typed_values.hpp"
#include "typed_json.hpp"
#include "qcae/ipc_api.hpp"
#include "qcae/operation_inputs.hpp"
#include "qcae/operations.hpp"
#include <algorithm>
#include <QJsonDocument>
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
using namespace detail;
constexpr std::array<std::string_view, 4> intrinsic_operations{
    "task.status", "task.cancel", "task.reconcile", "entity.fields"};
bool intrinsic(std::string_view operation) {
    return std::find(intrinsic_operations.begin(), intrinsic_operations.end(), operation) !=
           intrinsic_operations.end();
}
QString qs(std::string_view value) {
    return transport::json_ledger::from_utf8(value);
}
std::string string(const QJsonObject& object, const char* key) {
    const auto value = object.value(QLatin1String(key));
    if (!value.isString())
        throw std::invalid_argument(std::string("Expected nonempty string: ") + key);
    if (transport::json_ledger::trimmed_empty(value.toStringView()))
        throw std::invalid_argument(std::string("Expected nonempty string: ") + key);
    return transport::json_ledger::utf8(value.toStringView());
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
        return Value(transport::json_ledger::utf8(value.toStringView()));
    if (value.isArray()) {
        Value::Array result;
        for (const auto& item : value.toArray())
            result.push_back(decode(item));
        return Value(std::move(result));
    }
    if (value.isObject()) {
        Value::Object result;
        const auto object = value.toObject();
        for (auto it = object.begin(); it != object.end(); ++it) {
            result.emplace(transport::json_ledger::utf8(it.keyView()), decode(it.value()));
        }
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
                return transport::json_ledger::number(item);
            else if constexpr (std::is_same_v<T, Value::Array>) {
                return detail::typed_json_array(item);
            } else if constexpr (std::is_same_v<T, Value::Object>) {
                QJsonObject result;
                transport::json_ledger::ObjectCopies copies;
                for (const auto& [key, child] : item) {
                    const auto value = encode(child);
                    const auto name = qs(key);
                    result.insert(name, value);
                    copies.insert(name, value, true, false);
                }
                return result;
            } else
                return item;
        },
        value.data);
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
        if (result.value) {
            const auto data = encode(*result.value).toObject();
            output.insert("data", data);
            transport::json_ledger::ObjectCopies copies;
            copies.insert(QLatin1StringView("data"), data);
        }
        return output;
    }
    const auto data = encode(*result.value).toObject();
    QJsonObject output{{"request_id", id}, {"status", "success"}, {"data", data}};
    transport::json_ledger::ObjectCopies copies;
    copies.insert(QLatin1StringView("request_id"), id);
    copies.insert(QLatin1StringView("status"), QStringLiteral("success"));
    copies.insert(QLatin1StringView("data"), data);
    if (data.contains("current_revision")) {
        output.insert("revision", data.value("current_revision"));
        copies.insert(QLatin1StringView("revision"), data.value("current_revision"), false);
    } else if (data.contains("revision")) {
        output.insert("revision", data.value("revision"));
        copies.insert(QLatin1StringView("revision"), data.value("revision"), false);
    }
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
    if (request.contains("requested_version")) {
        const auto version =
            wire::positive_uint32(decode(request.value("requested_version")), "requested_version");
        if (!version.ok())
            throw RecordError(version.error->code, version.error->message, version.error->field);
        result.requested_version = *version.value;
    }
    if (request.contains("expected_profile")) {
        static constexpr std::array<std::string_view, 3> members{
            "profile_id", "profile_version", "definition_digest"};
        const auto value = decode(request.value("expected_profile"));
        const auto profile = wire::object_fields(value, members, "expected_profile");
        if (!profile.ok())
            throw RecordError(profile.error->code, profile.error->message, profile.error->field);
        ProfileRef expected;
        for (const auto& [key, target] :
             {std::pair{"profile_id", &expected.profile_id},
              std::pair{"profile_version", &expected.profile_version},
              std::pair{"definition_digest", &expected.definition_digest}}) {
            const auto text = wire::string_value((**profile.value).at(key),
                                                 std::string("expected_profile.") + key);
            if (!text.ok())
                throw RecordError(text.error->code, text.error->message, text.error->field);
            *target = *text.value;
        }
        result.expected_profile = std::move(expected);
    }
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

operations::Value detail::typed_value(const QJsonValue& value) {
    return decode(value);
}
QJsonObject detail::typed_input_failure(const QString& id, Status status, const Diagnostic& error) {
    auto output =
        failure(id, qs(error_name(error.code)), qs(error.message), qs(status_name(status)));
    auto diagnostic = output.value("error").toObject();
    diagnostic.insert("field", qs(error.field));
    output.insert("error", diagnostic);
    return output;
}
QJsonArray detail::typed_json_array(const operations::Value::Array& values) {
    QJsonArray array;
    transport::json_ledger::ObjectCopies copies;
    for (const auto& child : values) {
        const auto value = encode(child);
        array.append(value);
        copies.append(value);
    }
    return array;
}
std::size_t detail::typed_json_array_bytes(const operations::Value::Array& values) {
    const auto array = typed_json_array(values);
    const auto bytes = QJsonDocument(array).toJson(QJsonDocument::Compact);
    transport::json_ledger::encoding(bytes, array.size());
    ledger::add(ledger::Stage::socket_send, ledger::Metric::encoded_bytes, bytes.size());
    ledger::add(ledger::Stage::socket_send, ledger::Metric::metadata_copy_bytes, bytes.size());
    return static_cast<std::size_t>(bytes.size());
}

namespace {
// Host-owned reads and task controls. They decode generated inputs like contributed operations;
// contributions cannot claim these names.
Result<Value> entity_fields(RecordApplication& app,
                            const OperationContext& context,
                            const operations::EntityFieldsInput& input) {
    const auto snapshot = app.snapshot(*context.document);
    if (!snapshot.ok())
        return {snapshot.status, {}, snapshot.error};
    const auto fields = query_fields(snapshot.value->records, input.entity_id);
    if (!fields.ok())
        return {fields.status, {}, fields.error};
    const auto record = snapshot.value->records.find_identity(input.entity_id.value);
    Value::Object output;
    for (const auto& field : fields.value->fields)
        for (const auto& descriptor : record->descriptor().fields)
            if (descriptor.id == field.id)
                output.emplace(descriptor.name, field_value(field));
    return {Status::success,
            Value(Value::Object{{"entity_id", Value(input.entity_id.value)},
                                {"kind", Value(record->descriptor().query_kind)},
                                {"revision", Value(std::to_string(snapshot.value->info.revision))},
                                {"fields", Value(std::move(output))}}),
            {}};
}
Result<Value> task_control(RecordApplication& app,
                           TaskService& tasks,
                           const OperationContext& context,
                           const std::string& task_id,
                           bool cancel) {
    const auto snapshot = app.snapshot(*context.document);
    if (!snapshot.ok())
        return {snapshot.status, {}, snapshot.error};
    const auto current = tasks.query(context.caller, task_id);
    if (!current.ok())
        return converted(current, task_value);
    if (current.value->input.document.id != context.document->id)
        return {
            Status::failed,
            {},
            Diagnostic{ErrorCode::entity_not_found, "Task belongs to another document", "task_id"}};
    if (!cancel)
        return converted(current, task_value);
    return converted(tasks.cancel(context.caller, task_id), [](const TaskCancelResult& value) {
        auto output = std::get<Value::Object>(task_value(value.task).data);
        output.emplace("accepted", Value(value.accepted));
        return Value(std::move(output));
    });
}
Result<Value>
task_reconcile(RecordApplication& app, TaskService& tasks, const OperationContext& context) {
    const auto snapshot = app.snapshot(*context.document);
    if (!snapshot.ok())
        return {snapshot.status, {}, snapshot.error};
    return converted(tasks.reconcile(), [](bool reconciled) {
        return Value(Value::Object{{"reconciled", Value(reconciled)}});
    });
}
} // namespace

TypedHost::TypedHost(RecordApplication& app, std::function<bool(const ProfileRef&)> supported)
    : TypedHost(app, std::move(supported), default_operations()) {}
TypedHost::TypedHost(RecordApplication& app,
                     std::function<bool(const ProfileRef&)> supported,
                     OperationContributor contributor)
    : TypedHost(app, std::move(supported), std::move(contributor), {}) {}
TypedHost::TypedHost(RecordApplication& app,
                     std::function<bool(const ProfileRef&)> supported,
                     OperationContributor contributor,
                     TaskPublisherFactory publisher_factory)
    : app_(app), profile_supported_(std::move(supported)),
      publisher_factory_(std::move(publisher_factory)), state_(std::make_unique<State>()) {
    if (!contributor)
        throw std::invalid_argument("Operation contributor is empty");
    checked(contributor(state_->registry, app_, [this]() -> TaskService& { return tasks(); }));
    for (const auto& descriptor : state_->registry.descriptors()) {
        const auto& id = descriptor.definition.operation_id;
        if (intrinsic(id) || compatibility_operation_supported(id) || id == "runtime.handshake")
            throw std::invalid_argument("Operation contribution conflicts with host operation: " +
                                        id);
    }
    auto& registry = state_->registry;
    checked(registry.register_typed<operations::EntityFieldsInput>(
        operations::InputTraits<operations::EntityFieldsInput>::definition(),
        [this](const OperationContext& context, const operations::EntityFieldsInput& input) {
            return entity_fields(app_, context, input);
        }));
    checked(registry.register_typed<operations::TaskStatusInput>(
        operations::InputTraits<operations::TaskStatusInput>::definition(),
        [this](const OperationContext& context, const operations::TaskStatusInput& input) {
            return task_control(app_, tasks(), context, input.task_id, false);
        }));
    checked(registry.register_typed<operations::TaskCancelInput>(
        operations::InputTraits<operations::TaskCancelInput>::definition(),
        [this](const OperationContext& context, const operations::TaskCancelInput& input) {
            return task_control(app_, tasks(), context, input.task_id, true);
        }));
    checked(registry.register_typed<operations::TaskReconcileInput>(
        operations::InputTraits<operations::TaskReconcileInput>::definition(),
        [this](const OperationContext& context, const operations::TaskReconcileInput&) {
            return task_reconcile(app_, tasks(), context);
        }));
}
TypedHost::~TypedHost() = default;
TaskService& TypedHost::tasks() {
    if (!tasks_) {
        if (app_.recovery_available())
            throw RecordError(ErrorCode::storage_uncertain,
                              "Explicit application recovery is required before task access");
        tasks_ = std::make_unique<TaskService>(
            publisher_factory_ ? publisher_factory_(app_, profile_supported_)
                               : record_task_publisher(app_, profile_supported_));
    }
    return *tasks_;
}
Result<bool> TypedHost::reconcile() {
    return tasks_ ? tasks_->reconcile() : Result<bool>{Status::success, true, {}};
}
bool TypedHost::supports(std::string_view operation) const {
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
            QJsonObject metadata{{"name", qs(field.name)},
                                 {"wire_type", qs(field.wire_type)},
                                 {"required", field.required},
                                 {"allow_empty", field.allow_empty},
                                 {"units", units}};
            if (!field.values.empty()) {
                QJsonArray values;
                for (const auto& value : field.values)
                    values.append(qs(value));
                metadata.insert("values", values);
            }
            fields.append(metadata);
        }
        const auto effect =
            definition.effect == OperationEffect::background_task   ? "background_task"
            : definition.effect == OperationEffect::document_write  ? "document_write"
            : definition.effect == OperationEffect::preview         ? "preview"
            : definition.effect == OperationEffect::auxiliary_write ? "auxiliary_write"
                                                                    : "read_only";
        result.append(QJsonObject{
            {"name", qs(definition.operation_id)},
            {"effect", effect},
            {"version", static_cast<qint64>(definition.version)},
            {"schema_id", qs(definition.schema_id)},
            {"available", descriptor.available},
            {"implementation_status", descriptor.available ? "implemented" : "unavailable"},
            {"requires_document", definition.context.document},
            {"requires_epoch", definition.context.epoch},
            {"requires_revision", definition.context.expected_revision},
            {"requires_idempotency_key", definition.context.idempotency_key},
            {"requires_expected_profile", definition.context.expected_profile},
            {"requested_version_field", "requested_version"},
            {"omitted_version_policy", "installed_version"},
            {"fields", fields}});
    }
    return result;
}
QJsonObject TypedHost::dispatch(const QJsonObject& request, const Caller& caller) {
    const auto id = request.value("request_id").toString();
    try {
        const auto operation = string(request, "operation");
        const auto ctx = context(request, caller);
        if (caller.principal.empty())
            throw std::invalid_argument("Trusted caller identity is required");
        return response(
            id, state_->registry.invoke(operation, ctx, decode(request.value("parameters"))));
    } catch (const std::invalid_argument& error) {
        return failure(id, "INVALID_INPUT", qs(error.what()));
    } catch (const RecordError& error) {
        return response(
            id,
            {error.code() == ErrorCode::missing_input ? Status::needs_input : Status::failed,
             {},
             Diagnostic{error.code(), error.what(), error.field()}});
    }
}
} // namespace qcae::ipc
