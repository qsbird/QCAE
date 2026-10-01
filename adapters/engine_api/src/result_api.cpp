#include "qcae/result_api.hpp"
#include "qcae/operation_inputs.hpp"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <charconv>
#include <cmath>

namespace qcae::ipc {
namespace {
using namespace features::results;
using operations::Value;
[[noreturn]] void invalid(const char* message, std::string field) {
    throw RecordError(ErrorCode::invalid_input, message, std::move(field));
}
void fields(const QJsonObject& object,
            std::initializer_list<const char*> required,
            std::string_view field) {
    if (object.size() != static_cast<qsizetype>(required.size()))
        invalid("Fixture JSON object has missing or extra fields", std::string(field));
    for (const char* key : required)
        if (!object.contains(QLatin1String(key)))
            invalid("Fixture JSON field is required", std::string(field) + "." + key);
}
std::string text(const QJsonObject& object, const char* key) {
    const auto value = object.value(QLatin1String(key));
    if (!value.isString() || value.toString().isEmpty())
        invalid("Fixture field must be a nonempty string", key);
    return value.toString().toStdString();
}
std::uint64_t number(const QJsonValue& value, std::string field) {
    if (value.isString()) {
        const auto text = value.toString().toStdString();
        std::uint64_t result{};
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
        if (parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size())
            return result;
    } else if (value.isDouble()) {
        const auto result = value.toDouble();
        if (std::isfinite(result) && result >= 0 && result <= 9007199254740991.0 &&
            result == std::trunc(result))
            return static_cast<std::uint64_t>(result);
    }
    invalid("Fixture field must be an exact nonnegative integer", std::move(field));
}
QJsonArray array(const QJsonObject& object, const char* key) {
    const auto value = object.value(QLatin1String(key));
    if (!value.isArray())
        invalid("Fixture field must be an array", key);
    return value.toArray();
}
ResultFixture parse_fixture(std::string_view bytes) {
    if (bytes.size() > 65536)
        throw RecordError(ErrorCode::resource_limit,
                          "Fixture JSON exceeds its small-data byte quota",
                          "fixture_json");
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(
        QByteArray(bytes.data(), static_cast<qsizetype>(bytes.size())), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        invalid("Fixture JSON must be one valid object", "fixture_json");
    const auto object = document.object();
    fields(object,
           {"input_fingerprint",
            "identities",
            "quantity",
            "unit",
            "components",
            "location",
            "coordinate_basis",
            "case",
            "frame",
            "source_kind",
            "values"},
           "fixture");
    ResultFixture fixture;
    fixture.input_fingerprint = text(object, "input_fingerprint");
    fixture.quantity = text(object, "quantity");
    fixture.unit = text(object, "unit");
    fixture.location = text(object, "location");
    fixture.coordinate_basis = text(object, "coordinate_basis");
    fixture.case_label = text(object, "case");
    fixture.source_kind = text(object, "source_kind");
    fixture.frame = number(object.value("frame"), "frame");
    for (const auto& component : array(object, "components")) {
        if (!component.isString())
            invalid("Fixture component must be a string", "components");
        fixture.components.push_back(component.toString().toStdString());
    }
    for (const auto& item : array(object, "identities")) {
        if (!item.isObject())
            invalid("Fixture identity must be an object", "identities");
        const auto identity = item.toObject();
        fields(identity, {"entity_id", "namespace", "number"}, "identities");
        fixture.identities.push_back({EntityId(text(identity, "entity_id")),
                                      text(identity, "namespace"),
                                      number(identity.value("number"), "number")});
    }
    for (const auto& item : array(object, "values")) {
        if (!item.isObject())
            invalid("Fixture value must be an object", "values");
        const auto value = item.toObject();
        fields(value, {"solver_number", "value"}, "values");
        FixtureNodeValue sample;
        sample.solver_number = number(value.value("solver_number"), "solver_number");
        const auto vector = array(value, "value");
        if (vector.size() != 3)
            invalid("Fixture node value must have three components", "value");
        for (qsizetype index = 0; index < 3; ++index) {
            if (!vector[index].isDouble() || !std::isfinite(vector[index].toDouble()))
                invalid("Fixture node component must be finite", "value");
            sample.value[static_cast<std::size_t>(index)] = vector[index].toDouble();
        }
        fixture.values.push_back(sample);
    }
    return fixture;
}
Value version(const RecordVersion& input) {
    return Value(Value::Object{{"document_id", Value(input.document.id.value)},
                               {"document_epoch", Value(input.document.epoch.value)},
                               {"revision", Value(std::to_string(input.revision))}});
}
Value bundle_value(const StoredFixtureResult& result, const DocumentView& current) {
    const auto& bundle = result.bundle;
    Value::Array components, samples;
    for (const auto& component : bundle.field.components)
        components.emplace_back(component);
    for (const auto& sample : bundle.field.values)
        samples.emplace_back(Value::Object{{"entity_id", Value(sample.entity.value)},
                                           {"value", operations::wire::to_value(sample.value)}});
    return Value(Value::Object{
        {"result_id", Value(result.id)},
        {"artifact_id", Value(result.artifact_id)},
        {"analysis_id", Value(bundle.input.analysis.value)},
        {"input_version", version(bundle.input.version)},
        {"current_version", version(current.version())},
        {"input_fingerprint", Value(features::analysis::physical_signature_hex(bundle.input))},
        {"source_kind", Value(bundle.source_kind)},
        {"reader_version", Value(bundle.reader_version)},
        {"state",
         Value(result_state(bundle, current) == ResultState::current ? "current" : "stale")},
        {"field",
         Value(Value::Object{{"quantity", Value(bundle.field.quantity)},
                             {"unit", Value(bundle.field.unit)},
                             {"components", Value(std::move(components))},
                             {"location", Value(bundle.field.location)},
                             {"coordinate_basis", Value(bundle.field.coordinate_basis)},
                             {"case", Value(bundle.field.case_label)},
                             {"frame", Value(std::to_string(bundle.field.frame))},
                             {"values", Value(std::move(samples))}})}});
}
} // namespace
Result<bool> register_fixture_result_handlers(operations::OperationRegistry& registry,
                                              RecordApplication& app,
                                              FrozenInputResolver resolver) {
    using namespace operations;
    const auto service = std::make_shared<FixtureResultService>(app, std::move(resolver));
    auto response = [&app](const OperationContext& context,
                           const Result<StoredFixtureResult>& result) -> Result<Value> {
        if (!result.ok())
            return {result.status, {}, result.error};
        const auto snapshot = app.snapshot(*context.document);
        if (!snapshot.ok())
            return {snapshot.status, {}, snapshot.error};
        return {Status::success, bundle_value(*result.value, snapshot.value->records), {}};
    };
    auto result = registry.register_typed<ResultsReadFixtureInput>(
        InputTraits<ResultsReadFixtureInput>::definition(),
        [service, response](const OperationContext& context,
                            const ResultsReadFixtureInput& input) -> Result<Value> {
            try {
                return response(context,
                                service->read(context.caller,
                                              {*context.document, *context.expected_revision},
                                              input.artifact_id,
                                              parse_fixture(input.fixture_json),
                                              context.idempotency_key));
            } catch (const RecordError& error) {
                return {Status::failed, {}, Diagnostic{error.code(), error.what(), error.field()}};
            }
        });
    if (!result.ok())
        return result;
    return registry.register_typed<ResultsGetInput>(
        InputTraits<ResultsGetInput>::definition(),
        [service, response](const OperationContext& context, const ResultsGetInput& input) {
            return response(context,
                            service->get(context.caller, *context.document, input.result_id));
        });
}
} // namespace qcae::ipc
