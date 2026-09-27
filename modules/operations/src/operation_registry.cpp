#include "qcae/operation_registry.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace qcae::operations {
namespace {
template <class T>
Result<T>
failure(ErrorCode code, std::string message, std::string field, Status status = Status::failed) {
    return {status, std::nullopt, Diagnostic{code, std::move(message), std::move(field)}};
}

template <class T> Result<T> success(T value) {
    return {Status::success, std::move(value), std::nullopt};
}

std::string child_field(std::string_view parent, std::string_view name) {
    return parent.empty() ? std::string(name) : std::string(parent) + "." + std::string(name);
}

Result<bool> valid_context(const ContextRequirements& required, const OperationContext& context) {
    const auto missing = [](std::string field) {
        return failure<bool>(ErrorCode::missing_input,
                             "Required operation context is missing.",
                             std::move(field),
                             Status::needs_input);
    };
    if (context.caller.principal.empty()) {
        return missing("caller");
    }
    if (required.document && (!context.document || context.document->id.value.empty())) {
        return missing("document");
    }
    if (required.epoch && (!context.document || context.document->epoch.value.empty())) {
        return missing("epoch");
    }
    if (required.expected_revision && !context.expected_revision) {
        return missing("expected_revision");
    }
    if (required.idempotency_key && context.idempotency_key.empty()) {
        return missing("idempotency_key");
    }
    if (required.expected_profile &&
        (!context.expected_profile || context.expected_profile->profile_id.empty() ||
         context.expected_profile->profile_version.empty() ||
         context.expected_profile->definition_digest.empty())) {
        return missing("expected_profile");
    }
    return success(true);
}

Result<bool> valid_definition(const OperationDefinition& definition) {
    if (definition.operation_id.empty() || definition.schema_id.empty() ||
        definition.version == 0 || definition.fields.empty()) {
        return failure<bool>(ErrorCode::invalid_input,
                             "Operation metadata requires identity, version and input fields.",
                             "definition");
    }
    switch (definition.effect) {
    case OperationEffect::read_only:
    case OperationEffect::document_write:
    case OperationEffect::preview:
    case OperationEffect::background_task:
        break;
    default:
        return failure<bool>(ErrorCode::invalid_input, "Unknown operation effect.", "effect");
    }
    if ((definition.context.epoch || definition.context.expected_revision) &&
        !definition.context.document) {
        return failure<bool>(ErrorCode::invalid_input,
                             "Epoch/revision requirements need a document context.",
                             "context");
    }
    std::set<std::uint32_t> ids;
    std::set<std::string> names;
    for (const auto& field : definition.fields) {
        if (field.field_id == 0 || field.name.empty() || field.wire_type.empty() ||
            !ids.insert(field.field_id).second || !names.insert(field.name).second) {
            return failure<bool>(ErrorCode::invalid_input,
                                 "Input field identity/name/type is missing or duplicated.",
                                 "fields");
        }
        std::set<std::string> units;
        for (const auto& unit : field.units) {
            if (unit.empty() || !units.insert(unit).second) {
                return failure<bool>(ErrorCode::invalid_input,
                                     "Input unit choices are empty or duplicated.",
                                     "fields");
            }
        }
    }
    return success(true);
}
} // namespace

namespace wire {
Result<const Value::Object*> object_fields(const Value& value,
                                           std::span<const std::string_view> allowed,
                                           std::string_view field) {
    return object_fields(value, allowed, allowed, field);
}

Result<const Value::Object*> object_fields(const Value& value,
                                           std::span<const std::string_view> allowed,
                                           std::span<const std::string_view> required,
                                           std::string_view field) {
    const auto* object = std::get_if<Value::Object>(&value.data);
    if (!object) {
        return failure<const Value::Object*>(
            ErrorCode::invalid_input, "Expected an object.", std::string(field));
    }
    for (const auto& [key, unused] : *object) {
        static_cast<void>(unused);
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) {
            return failure<const Value::Object*>(
                ErrorCode::invalid_input, "Unknown input field.", child_field(field, key));
        }
    }
    for (const auto key : required) {
        if (!object->contains(key)) {
            return failure<const Value::Object*>(ErrorCode::missing_input,
                                                 "Required input field is missing.",
                                                 child_field(field, key),
                                                 Status::needs_input);
        }
    }
    return success(object);
}

Result<std::string> string_value(const Value& value, std::string_view field) {
    const auto* string = std::get_if<std::string>(&value.data);
    if (!string || string->empty()) {
        return failure<std::string>(
            ErrorCode::invalid_input, "Expected a nonempty string.", std::string(field));
    }
    return success(*string);
}

Result<double> finite_number(const Value& value, std::string_view field) {
    double number{};
    if (const auto* real = std::get_if<double>(&value.data)) {
        number = *real;
    } else if (const auto* integer = std::get_if<std::int64_t>(&value.data)) {
        constexpr std::int64_t exact_integer_limit = std::int64_t{1} << 53;
        if (*integer < -exact_integer_limit || *integer > exact_integer_limit) {
            return failure<double>(ErrorCode::invalid_input,
                                   "Integer exceeds the exact double input range.",
                                   std::string(field));
        }
        number = static_cast<double>(*integer);
    } else {
        return failure<double>(ErrorCode::invalid_input, "Expected a number.", std::string(field));
    }
    if (!std::isfinite(number) ||
        (number != 0 && std::abs(number) < std::numeric_limits<double>::min())) {
        return failure<double>(ErrorCode::invalid_input,
                               "Number must be finite and within the normal double range.",
                               std::string(field));
    }
    return success(number);
}

Result<EntityId> entity_id(const Value& value, std::string_view field) {
    const auto string = string_value(value, field);
    if (!string.ok()) {
        return {string.status, std::nullopt, string.error};
    }
    return success(EntityId{*string.value});
}

Result<std::vector<EntityId>> entity_id_array(const Value& value, std::string_view field) {
    const auto* array = std::get_if<Value::Array>(&value.data);
    if (!array || array->empty()) {
        return failure<std::vector<EntityId>>(
            ErrorCode::invalid_input, "Expected a nonempty entity ID array.", std::string(field));
    }
    std::vector<EntityId> identities;
    identities.reserve(array->size());
    std::set<std::string> unique;
    for (std::size_t index = 0; index < array->size(); ++index) {
        const auto identity =
            entity_id((*array)[index], std::string(field) + "[" + std::to_string(index) + "]");
        if (!identity.ok()) {
            return {identity.status, std::nullopt, identity.error};
        }
        if (!unique.insert(identity.value->value).second) {
            return failure<std::vector<EntityId>>(ErrorCode::invalid_input,
                                                  "Entity ID array contains duplicates.",
                                                  std::string(field));
        }
        identities.push_back(*identity.value);
    }
    return success(std::move(identities));
}

Result<Quantity> quantity(const Value& value,
                          std::span<const std::string_view> allowed_units,
                          std::string_view field) {
    static constexpr std::array<std::string_view, 2> members{"value", "unit"};
    const auto parsed = object_fields(value, members, field);
    if (!parsed.ok()) {
        return {parsed.status, std::nullopt, parsed.error};
    }
    const auto& object = **parsed.value;
    const auto* unit = std::get_if<std::string>(&object.at("unit").data);
    if (!unit) {
        return failure<Quantity>(ErrorCode::invalid_input,
                                 "Quantity unit must be a string.",
                                 child_field(field, "unit"));
    }
    if (unit->empty()) {
        return failure<Quantity>(ErrorCode::missing_input,
                                 "Quantity unit must be explicit.",
                                 child_field(field, "unit"),
                                 Status::needs_input);
    }
    if (std::find(allowed_units.begin(), allowed_units.end(), *unit) == allowed_units.end()) {
        return failure<Quantity>(ErrorCode::invalid_unit,
                                 "Unknown or incompatible quantity unit.",
                                 child_field(field, "unit"));
    }
    const auto number = finite_number(object.at("value"), child_field(field, "value"));
    if (!number.ok()) {
        return {number.status, std::nullopt, number.error};
    }
    return success(Quantity{*number.value, *unit});
}

Result<std::array<double, 3>> vector3(const Value& value, std::string_view field) {
    const auto* array = std::get_if<Value::Array>(&value.data);
    if (!array || array->size() != 3) {
        return failure<std::array<double, 3>>(ErrorCode::invalid_input,
                                              "Expected exactly three millimeter coordinates.",
                                              std::string(field));
    }
    std::array<double, 3> coordinates{};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const auto number =
            finite_number((*array)[axis], std::string(field) + "[" + std::to_string(axis) + "]");
        if (!number.ok()) {
            return {number.status, std::nullopt, number.error};
        }
        coordinates[axis] = *number.value;
    }
    return success(coordinates);
}

Result<std::uint32_t> positive_uint32(const Value& value, std::string_view field) {
    const auto* integer = std::get_if<std::int64_t>(&value.data);
    if (!integer || *integer <= 0 ||
        static_cast<std::uint64_t>(*integer) > std::numeric_limits<std::uint32_t>::max()) {
        return failure<std::uint32_t>(
            ErrorCode::invalid_input, "Expected a positive uint32 integer.", std::string(field));
    }
    return success(static_cast<std::uint32_t>(*integer));
}

Value to_value(const std::vector<EntityId>& identities) {
    Value::Array values;
    values.reserve(identities.size());
    for (const auto& identity : identities) {
        values.emplace_back(identity.value);
    }
    return Value(std::move(values));
}

Value to_value(const Quantity& quantity) {
    return Value(Value::Object{{"value", Value(quantity.value)}, {"unit", Value(quantity.unit)}});
}

Value to_value(const std::array<double, 3>& vector) {
    return Value(Value::Array{Value(vector[0]), Value(vector[1]), Value(vector[2])});
}
} // namespace wire

Result<bool> OperationRegistry::register_handler(OperationDefinition definition,
                                                 std::string schema_token,
                                                 Handler handler) {
    const auto valid = valid_definition(definition);
    if (!valid.ok()) {
        return valid;
    }
    if (schema_token != definition.schema_id || entries_.contains(definition.operation_id)) {
        return failure<bool>(ErrorCode::invalid_input,
                             "Duplicate operation or inconsistent schema token.",
                             "operation_id");
    }
    for (const auto& [id, entry] : entries_) {
        static_cast<void>(id);
        if (entry.definition.schema_id == schema_token) {
            return failure<bool>(
                ErrorCode::invalid_input, "Duplicate input schema/type token.", "schema_id");
        }
    }
    const auto id = definition.operation_id;
    entries_.emplace(id, Entry{std::move(definition), std::move(handler), {}});
    return success(true);
}

Result<bool> OperationRegistry::declare_unavailable(OperationDefinition definition,
                                                    std::string reason) {
    if (reason.empty()) {
        return failure<bool>(
            ErrorCode::invalid_input, "Unavailable operation needs a reason.", "reason");
    }
    const auto id = definition.operation_id;
    const auto token = definition.schema_id;
    const auto registered = register_handler(std::move(definition), token, {});
    if (!registered.ok()) {
        return registered;
    }
    entries_.at(id).unavailable_reason = std::move(reason);
    return success(true);
}

Result<Value> OperationRegistry::invoke(std::string_view operation_id,
                                        const OperationContext& context,
                                        const Value& input) const {
    const auto actor = valid_context({}, context);
    if (!actor.ok()) {
        return {actor.status, std::nullopt, actor.error};
    }
    const auto found = entries_.find(operation_id);
    if (found == entries_.end()) {
        return failure<Value>(
            ErrorCode::unsupported_capability, "Operation is not registered.", "operation_id");
    }
    return invoke(operation_id, found->second.definition.version, context, input);
}

Result<Value> OperationRegistry::invoke(std::string_view operation_id,
                                        std::uint32_t requested_version,
                                        const OperationContext& context,
                                        const Value& input) const {
    const auto actor = valid_context({}, context);
    if (!actor.ok()) {
        return {actor.status, std::nullopt, actor.error};
    }
    const auto found = entries_.find(operation_id);
    if (found == entries_.end()) {
        return failure<Value>(
            ErrorCode::unsupported_capability, "Operation is not registered.", "operation_id");
    }
    if (requested_version != found->second.definition.version) {
        return failure<Value>(ErrorCode::schema_unsupported,
                              "Requested operation contract version is not installed.",
                              "version");
    }
    const auto valid = valid_context(found->second.definition.context, context);
    if (!valid.ok()) {
        return {valid.status, std::nullopt, valid.error};
    }
    if (!found->second.handler) {
        return failure<Value>(
            ErrorCode::unsupported_capability, found->second.unavailable_reason, "operation_id");
    }
    const auto result = found->second.handler(context, input);
    if (!result.ok()) {
        if (result.error && result.status != Status::success) {
            return {result.status, std::nullopt, result.error};
        }
        return failure<Value>(ErrorCode::invalid_input,
                              "Handler returned an inconsistent result envelope.",
                              "output");
    }
    if (result.error || !std::holds_alternative<Value::Object>(result.value->data)) {
        return failure<Value>(ErrorCode::invalid_input,
                              "Successful handler output must be an object without an error.",
                              "output");
    }
    return result;
}

std::vector<OperationDescriptor> OperationRegistry::descriptors() const {
    std::vector<OperationDescriptor> result;
    result.reserve(entries_.size());
    for (const auto& [id, entry] : entries_) {
        static_cast<void>(id);
        result.push_back(
            {entry.definition, static_cast<bool>(entry.handler), entry.unavailable_reason});
    }
    return result;
}
} // namespace qcae::operations
