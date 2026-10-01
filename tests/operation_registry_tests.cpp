#include "qcae/operation_inputs.hpp"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace qcae::operations {
struct DuplicateSchemaInput {};
template <> struct InputTraits<DuplicateSchemaInput> {
    static constexpr std::string_view schema_id = InputTraits<MaterialCreateInput>::schema_id;
    static OperationDefinition definition() {
        auto definition = InputTraits<MaterialCreateInput>::definition();
        definition.operation_id = "test.duplicate_type";
        return definition;
    }
    static Result<DuplicateSchemaInput> from_value(const Value&) {
        return {Status::success, DuplicateSchemaInput{}, std::nullopt};
    }
};
struct EmptyInput {};
template <> struct InputTraits<EmptyInput> {
    static constexpr std::string_view schema_id = "qcae.operation.test.empty.v1";
    static OperationDefinition definition() {
        return {"test.empty", 1, std::string(schema_id), OperationEffect::read_only, {}, {}};
    }
    static Result<EmptyInput> from_value(const Value& value) {
        const auto fields =
            wire::object_fields(value, std::span<const std::string_view>{}, "input");
        if (!fields.ok())
            return {fields.status, {}, fields.error};
        return {Status::success, EmptyInput{}, {}};
    }
};
} // namespace qcae::operations

using namespace qcae;
using namespace qcae::operations;

namespace {
void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <class T> const T& good(const Result<T>& result, const std::string& message) {
    check(result.ok(), message + (result.error ? ": " + result.error->message : ""));
    return *result.value;
}

template <class T> void bad(const Result<T>& result, ErrorCode code, const std::string& message) {
    const auto status = code == ErrorCode::missing_input ? Status::needs_input : Status::failed;
    check(!result.ok() && !result.value && result.status == status && result.error &&
              result.error->code == code,
          message);
}

OperationContext test_context() {
    return {Caller{"test-host"},
            DocumentRef{DocumentId{"doc-1"}, DocumentEpoch{"epoch-1"}},
            Revision{7},
            "idempotency-1",
            ProfileRef{"nastran", "1", "digest-1"},
            "request-1"};
}

Result<Value> output(std::string label) {
    return {
        Status::success, Value(Value::Object{{"result", Value(std::move(label))}}), std::nullopt};
}

Value material_input(Value modulus = wire::to_value(Quantity{210, "GPa"})) {
    return Value(Value::Object{{"name", Value("Steel")}, {"young_modulus", std::move(modulus)}});
}

void registration_and_availability() {
    OperationRegistry registry;
    const auto context = test_context();
    check(registry.descriptors().empty(), "no global supported-operation list");
    std::function<Result<Value>(const OperationContext&, const MaterialCreateInput&)> empty;
    bad(registry.register_typed<MaterialCreateInput>(InputTraits<MaterialCreateInput>::definition(),
                                                     empty),
        ErrorCode::invalid_input,
        "empty typed handler cannot advertise availability");
    check(registry.descriptors().empty(), "invalid registration publishes no metadata");
    auto mismatched = InputTraits<MaterialCreateInput>::definition();
    mismatched.fields[1].units = {"N"};
    bad(registry.register_typed<MaterialCreateInput>(
            mismatched, [](const OperationContext&, const auto&) { return output("bad"); }),
        ErrorCode::invalid_input,
        "metadata and typed decoding cannot diverge");

    int calls = 0;
    good(registry.register_typed<MaterialCreateInput>(
             InputTraits<MaterialCreateInput>::definition(),
             [&calls](const OperationContext& call_context, const MaterialCreateInput& input) {
                 check(call_context.request_id == "request-1",
                       "per-call context reaches typed handler");
                 check(call_context.requested_version == 1,
                       "resolved operation version reaches typed handler");
                 ++calls;
                 check(input.name == "Steel" && input.young_modulus.value == 210 &&
                           input.young_modulus.unit == "GPa",
                       "handler receives generated typed parameters");
                 return output("created");
             }),
         "register actual typed material handler");
    const auto descriptors = registry.descriptors();
    check(descriptors.size() == 1 && descriptors[0].available &&
              descriptors[0].definition == InputTraits<MaterialCreateInput>::definition() &&
              descriptors[0].unavailable_reason.empty(),
          "availability and metadata derive from actual handler entry");
    check(descriptors[0].definition.context.document && descriptors[0].definition.context.epoch &&
              descriptors[0].definition.context.expected_revision &&
              descriptors[0].definition.context.idempotency_key &&
              descriptors[0].definition.effect == OperationEffect::document_write,
          "effect/context requirements declared in generated metadata");
    good(registry.invoke("material.create", context, material_input()), "typed invocation");
    check(calls == 1, "actual callable invoked once");
    bad(registry.invoke("material.create", 2, context, material_input()),
        ErrorCode::schema_unsupported,
        "unknown future operation version rejected before handler");
    check(calls == 1, "version rejection does not invoke handler");
    auto future_context = context;
    future_context.requested_version = 2;
    const auto future_result = registry.invoke("material.create", future_context, material_input());
    bad(future_result, ErrorCode::schema_unsupported, "contextual requested version is respected");
    check(future_result.error->field == "requested_version" && calls == 1,
          "version error identifies wire field before effects");
    bad(registry.invoke("material.create", 1, future_context, material_input()),
        ErrorCode::invalid_input,
        "contradictory explicit/contextual versions cannot silently override each other");
    bad(registry.register_typed<MaterialCreateInput>(
            InputTraits<MaterialCreateInput>::definition(),
            [](const OperationContext&, const auto&) { return output("duplicate"); }),
        ErrorCode::invalid_input,
        "duplicate operation rejected");
    bad(registry.register_typed<DuplicateSchemaInput>(
            InputTraits<DuplicateSchemaInput>::definition(),
            [](const OperationContext&, const auto&) { return output("duplicate"); }),
        ErrorCode::invalid_input,
        "duplicate generated input schema/type token rejected");
    check(registry.descriptors().size() == 1, "duplicate attempts preserve registry");

    auto future = InputTraits<GeometryCreateLineInput>::definition();
    future.operation_id = "geometry.future_boolean";
    future.schema_id = "qcae.operation.geometry.future_boolean.v1";
    good(registry.declare_unavailable(future, "No real Boolean backend handler is installed."),
         "declare future operation explicitly unavailable");
    const auto known = registry.descriptors();
    check(known.size() == 2 && !known.front().available && known.back().available,
          "metadata order is stable and future declaration is disabled");
    check(!known.front().unavailable_reason.empty(), "disabled capability has a reason");
    bad(registry.invoke("geometry.future_boolean", context, Value{}),
        ErrorCode::unsupported_capability,
        "future operation cannot invoke an absent handler");
    bad(registry.invoke("unknown.operation", context, Value{}),
        ErrorCode::unsupported_capability,
        "unknown operation rejected");
    bad(registry.declare_unavailable(future, "duplicate"),
        ErrorCode::invalid_input,
        "duplicate declaration rejected");
    check(calls == 1, "unavailable/unknown capabilities do not invoke another handler");
}

void mechanical_input_validation() {
    OperationRegistry registry;
    const auto context = test_context();
    int calls = 0;
    good(registry.register_typed<MaterialCreateInput>(
             InputTraits<MaterialCreateInput>::definition(),
             [&calls](const OperationContext& call_context, const auto&) {
                 check(call_context.request_id == "request-1",
                       "per-call context reaches typed handler");
                 ++calls;
                 return output("ok");
             }),
         "validation handler");
    bad(registry.invoke("material.create", context, Value(true)),
        ErrorCode::invalid_input,
        "input must be object");
    bad(registry.invoke("material.create", context, Value{}),
        ErrorCode::missing_input,
        "required fields checked");
    auto extra = std::get<Value::Object>(material_input().data);
    extra.emplace("secret", Value(1));
    bad(registry.invoke("material.create", context, Value(extra)),
        ErrorCode::invalid_input,
        "unknown field rejected");
    auto wrong_name = std::get<Value::Object>(material_input().data);
    wrong_name.at("name") = Value(false);
    bad(registry.invoke("material.create", context, Value(wrong_name)),
        ErrorCode::invalid_input,
        "field type checked");
    wrong_name.at("name") = Value("");
    bad(registry.invoke("material.create", context, Value(wrong_name)),
        ErrorCode::invalid_input,
        "empty name rejected");
    bad(registry.invoke("material.create",
                        context,
                        material_input(Value(Value::Object{{"value", Value(210)}}))),
        ErrorCode::missing_input,
        "missing quantity unit needs input");
    bad(registry.invoke("material.create", context, material_input(wire::to_value({210, ""}))),
        ErrorCode::missing_input,
        "empty quantity unit needs input");
    for (const auto unit : {"N", "unknown", "mpa"}) {
        bad(registry.invoke(
                "material.create", context, material_input(wire::to_value({210, unit}))),
            ErrorCode::invalid_unit,
            "unknown/dimension-mismatched unit rejected");
    }
    auto extra_quantity =
        Value::Object{{"value", Value(210)}, {"unit", Value("MPa")}, {"ignored", Value(1)}};
    bad(registry.invoke("material.create", context, material_input(Value(extra_quantity))),
        ErrorCode::invalid_input,
        "unknown quantity member rejected");
    bad(registry.invoke(
            "material.create",
            context,
            material_input(Value(Value::Object{{"value", Value(true)}, {"unit", Value("MPa")}}))),
        ErrorCode::invalid_input,
        "bool cannot silently become numeric quantity");
    for (const auto number : {std::numeric_limits<double>::infinity(),
                              std::numeric_limits<double>::quiet_NaN(),
                              std::numeric_limits<double>::denorm_min()}) {
        bad(registry.invoke(
                "material.create", context, material_input(wire::to_value({number, "MPa"}))),
            ErrorCode::invalid_input,
            "nonfinite/subnormal numeric input rejected");
    }
    bad(registry.invoke("material.create",
                        context,
                        material_input(Value(Value::Object{
                            {"value", Value(std::numeric_limits<std::int64_t>::max())},
                            {"unit", Value("MPa")}}))),
        ErrorCode::invalid_input,
        "integers outside exact double range are not silently rounded");
    check(calls == 0, "all malformed requests reject before typed handler");
    good(registry.invoke("material.create", context, material_input()),
         "valid request after rejection");
    check(calls == 1, "registry remains usable after malformed inputs");
}

void all_adapters_and_handlers() {
    const MaterialCreateInput material{"Steel", {210, "GPa"}};
    const auto decoded_material = good(InputTraits<MaterialCreateInput>::from_value(
                                           InputTraits<MaterialCreateInput>::to_value(material)),
                                       "material adapter round trip");
    check(decoded_material.name == material.name && decoded_material.young_modulus.unit == "GPa" &&
              decoded_material.young_modulus.value == 210,
          "material typed DTO preserved");
    OperationRegistry registry;
    const auto context = test_context();
    good(registry.register_typed<MaterialSetYoungModulusInput>(
             InputTraits<MaterialSetYoungModulusInput>::definition(),
             [](const OperationContext& call_context, const MaterialSetYoungModulusInput& input) {
                 check(call_context.caller.principal == "test-host", "context forwarded to setter");
                 check(input.entity_id == EntityId("mat") && input.young_modulus.value == 200000,
                       "set modulus typed payload");
                 return output("updated");
             }),
         "register setter");
    good(registry.invoke("material.set_young_modulus",
                         context,
                         InputTraits<MaterialSetYoungModulusInput>::to_value(
                             {EntityId("mat"), {200000, "MPa"}})),
         "invoke setter");
    good(registry.register_typed<NodeMoveInput>(
             InputTraits<NodeMoveInput>::definition(),
             [](const OperationContext& call_context, const NodeMoveInput& input) {
                 check(call_context.request_id == "request-1", "context forwarded to node handler");
                 check(input.entity_id == EntityId("node") &&
                           input.position_mm == std::array<double, 3>{1, 2, 3},
                       "move typed vector");
                 return output("moved");
             }),
         "register node move");
    good(registry.invoke("node.move",
                         context,
                         InputTraits<NodeMoveInput>::to_value({EntityId("node"), {1, 2, 3}})),
         "invoke node move");
    bad(InputTraits<NodeMoveInput>::from_value(
            Value(Value::Object{{"entity_id", Value("node")},
                                {"position_mm", Value(Value::Array{Value(1), Value(2)})}})),
        ErrorCode::invalid_input,
        "vector length must be three");
    bad(InputTraits<NodeMoveInput>::from_value(
            Value(Value::Object{{"entity_id", Value("")},
                                {"position_mm", wire::to_value(std::array<double, 3>{1, 2, 3})}})),
        ErrorCode::invalid_input,
        "entity ID must be nonempty");
    good(registry.register_typed<GeometryCreateLineInput>(
             InputTraits<GeometryCreateLineInput>::definition(),
             [](const OperationContext& call_context, const GeometryCreateLineInput& input) {
                 check(call_context.request_id == "request-1",
                       "context forwarded to geometry handler");
                 check(input.start_mm == std::array<double, 3>{0, 0, 0} &&
                           input.end_mm == std::array<double, 3>{1000, 0, 0},
                       "line typed endpoints");
                 return output("line");
             }),
         "register line create");
    good(registry.invoke("geometry.create_line",
                         context,
                         InputTraits<GeometryCreateLineInput>::to_value({{0, 0, 0}, {1000, 0, 0}})),
         "invoke line create");
    good(registry.register_typed<MeshGenerateLineInput>(
             InputTraits<MeshGenerateLineInput>::definition(),
             [](const OperationContext& call_context, const MeshGenerateLineInput& input) {
                 check(call_context.request_id == "request-1", "context forwarded to mesher");
                 check(input.geometry_id == EntityId("line") && input.segments == 10,
                       "mesh typed positive count");
                 return output("queued");
             }),
         "register mesh handler");
    good(registry.invoke("mesh.generate_line",
                         context,
                         InputTraits<MeshGenerateLineInput>::to_value({EntityId("line"), 10})),
         "invoke meshing");
    for (const auto& count :
         {Value(0), Value(-1), Value(10.0), Value(true), Value(std::int64_t{1} << 32)}) {
        bad(InputTraits<MeshGenerateLineInput>::from_value(
                Value(Value::Object{{"geometry_id", Value("line")}, {"segments", count}})),
            ErrorCode::invalid_input,
            "segments must be a positive uint32 wire integer");
    }
    check(registry.descriptors().size() == 4,
          "each supported adapter uses an actual handler registration");
    check(InputTraits<MaterialCreateInput>::definition().fields[0].field_id == 1 &&
              InputTraits<MaterialCreateInput>::definition().fields[1].field_id == 2,
          "explicit schema field IDs preserved");
    check(operation_schema_digest.size() == 64, "generated schema fingerprint present");
}

void context_validation_and_isolation() {
    OperationRegistry registry;
    std::vector<std::string> observed_requests;
    good(registry.register_typed<MaterialCreateInput>(
             InputTraits<MaterialCreateInput>::definition(),
             [&observed_requests](const OperationContext& context, const MaterialCreateInput&) {
                 observed_requests.push_back(context.caller.principal + ":" + context.request_id);
                 return output("context received");
             }),
         "register explicit-context handler");
    const auto baseline = test_context();
    for (const auto field :
         {"caller", "document", "epoch", "expected_revision", "idempotency_key"}) {
        auto context = baseline;
        if (std::string_view(field) == "caller") {
            context.caller.principal.clear();
        } else if (std::string_view(field) == "document") {
            context.document.reset();
        } else if (std::string_view(field) == "epoch") {
            context.document->epoch.value.clear();
        } else if (std::string_view(field) == "expected_revision") {
            context.expected_revision.reset();
        } else {
            context.idempotency_key.clear();
        }
        const auto result = registry.invoke("material.create", 1, context, material_input());
        bad(result, ErrorCode::missing_input, "required context rejected before handler");
        check(result.error->field == field, "missing context field is specific");
    }
    check(observed_requests.empty(), "invalid context never reaches handler");
    good(registry.invoke("material.create", 1, baseline, material_input()), "first client context");
    auto other = baseline;
    other.caller = Caller{"another-host"};
    other.document = DocumentRef{DocumentId{"doc-2"}, DocumentEpoch{"epoch-2"}};
    other.expected_revision = 9;
    other.idempotency_key = "idempotency-2";
    other.request_id = "request-2";
    good(registry.invoke("material.create", 1, other, material_input()), "second client context");
    check(observed_requests ==
              std::vector<std::string>{"test-host:request-1", "another-host:request-2"},
          "independent call context is forwarded without a shared mutable request capture");

    auto future = InputTraits<GeometryCreateLineInput>::definition();
    future.operation_id = "test.future_readonly";
    future.schema_id = "qcae.operation.test.future_readonly.v1";
    future.effect = OperationEffect::read_only;
    future.context = {};
    good(registry.declare_unavailable(future, "Backend not installed."),
         "readonly future metadata");
    OperationContext reader{
        Caller{"read-host"}, std::nullopt, std::nullopt, "", std::nullopt, "read-request"};
    bad(registry.invoke("test.future_readonly", 1, reader, Value{}),
        ErrorCode::unsupported_capability,
        "readonly future does not require undeclared write context");
    reader.caller.principal.clear();
    bad(registry.invoke("test.future_readonly", 1, reader, Value{}),
        ErrorCode::missing_input,
        "even readonly/unavailable operations require explicit caller");

    future.operation_id = "test.future_profile";
    future.schema_id = "qcae.operation.test.future_profile.v1";
    future.context.expected_profile = true;
    good(registry.declare_unavailable(future, "Profile handler not installed."),
         "future profile flag");
    reader.caller = Caller{"read-host"};
    bad(registry.invoke("test.future_profile", 1, reader, Value{}),
        ErrorCode::missing_input,
        "declared profile requirement is checked");
    reader.expected_profile = ProfileRef{"nastran", "1", "digest-1"};
    bad(registry.invoke("test.future_profile", 1, reader, Value{}),
        ErrorCode::unsupported_capability,
        "valid profile presence does not invent availability");
}

void output_contract() {
    const auto context = test_context();
    OperationRegistry malformed;
    good(malformed.register_typed<MaterialCreateInput>(
             InputTraits<MaterialCreateInput>::definition(),
             [](const OperationContext&, const auto&) -> Result<Value> {
                 return {Status::success, Value(42), std::nullopt};
             }),
         "install test nonobject handler");
    bad(malformed.invoke("material.create", context, material_input()),
        ErrorCode::invalid_input,
        "successful output must be an object");
    OperationRegistry conflict;
    good(conflict.register_typed<MaterialCreateInput>(
             InputTraits<MaterialCreateInput>::definition(),
             [](const OperationContext&, const auto&) -> Result<Value> {
                 return {Status::conflict,
                         std::nullopt,
                         Diagnostic{
                             ErrorCode::revision_conflict, "Stale revision.", "expected_revision"}};
             }),
         "install test service failure");
    const auto result = conflict.invoke("material.create", context, material_input());
    check(result.status == Status::conflict && result.error &&
              result.error->code == ErrorCode::revision_conflict && !result.value,
          "registry preserves structured service conflict without model authority");
    OperationRegistry reports;
    Status report_status = Status::needs_input;
    good(reports.register_typed<MaterialCreateInput>(
             InputTraits<MaterialCreateInput>::definition(),
             [&](const OperationContext&, const auto&) -> Result<Value> {
                 return {report_status,
                         Value(Value::Object{{"check_id", Value("retained-report")}}),
                         Diagnostic{ErrorCode::missing_input, "Missing physical facts.", "forces"}};
             }),
         "install diagnostic report handler");
    for (const auto status : {Status::needs_input, Status::failed, Status::conflict}) {
        report_status = status;
        const auto report = reports.invoke("material.create", context, material_input());
        check(report.status == status && report.error && report.value &&
                  std::get<std::string>(
                      std::get<Value::Object>(report.value->data).at("check_id").data) ==
                      "retained-report",
              "non-success envelope retains its immutable diagnostic report");
    }
    OperationRegistry malformed_report;
    good(malformed_report.register_typed<MaterialCreateInput>(
             InputTraits<MaterialCreateInput>::definition(),
             [](const OperationContext&, const auto&) -> Result<Value> {
                 return {Status::failed,
                         Value(42),
                         Diagnostic{ErrorCode::missing_input, "Missing physical facts.", "forces"}};
             }),
         "install malformed diagnostic report handler");
    bad(malformed_report.invoke("material.create", context, material_input()),
        ErrorCode::invalid_input,
        "non-success report must still be an object");
}
void receipt_projection() {
    ChangeReceipt receipt{TransactionId("transaction-1"),
                          std::numeric_limits<Revision>::max(),
                          Revision{9007199254740993ULL},
                          "content-state",
                          true,
                          EntityId("stable-entity")};
    const auto output = std::get<Value::Object>(change_receipt_value(receipt).data);
    check(std::get<std::string>(output.at("transaction_id").data) == "transaction-1" &&
              !output.contains("transaction"),
          "receipt exposes one public transaction field name");
    check(std::get<std::string>(output.at("committed_revision").data) == "18446744073709551615" &&
              std::get<std::string>(output.at("current_revision").data) == "9007199254740993",
          "receipt revisions remain exact beyond signed int64 and JSON double precision");
    check(std::get<bool>(output.at("replayed").data) &&
              std::get<std::string>(output.at("entity_id").data) == "stable-entity",
          "receipt projection preserves replay and stable entity identity");
    receipt.primary_entity = EntityId{};
    check(!std::get<Value::Object>(change_receipt_value(receipt).data).contains("entity_id"),
          "receipt without a primary entity does not fabricate an identity");
}
void owned_value_copy_ledger() {
    Value::Object object;
    object.emplace("label", Value(std::string(80, 'x')));
    object.emplace("number", Value(1.25));
    const Value source(std::move(object));
    Value assigned;
    const auto operation = std::make_shared<ledger::OperationLedger>(
        ledger::Identity{"owned-value-copies", {}, {}, 0});
    {
        ledger::Scope scope(operation);
        auto copied = source;
        assigned = source;
        const auto& same = assigned;
        assigned = same;
        auto moved = std::move(copied);
        check(moved == source && assigned == source, "Value copy/move preserves the exact tree");
    }
    const auto measured = operation->snapshot();
    const auto& values = measured.values[static_cast<std::size_t>(ledger::Stage::application)];
    check(values[static_cast<std::size_t>(ledger::Metric::model_copy_bytes)] ==
              2 * (80 + sizeof(double)),
          "Each deep copy clones one owned string and numeric leaf; container move reuses nodes");
    check(values[static_cast<std::size_t>(ledger::Metric::metadata_copy_bytes)] ==
              2 * (2 * sizeof(std::string) + 5 + 6),
          "Each map clone owns new keys, without a second recursive payload sum");
    auto& copied_string =
        std::get<std::string>(std::get<Value::Object>(assigned.data).at("label").data);
    copied_string.front() = 'z';
    check(std::get<std::string>(std::get<Value::Object>(source.data).at("label").data).front() ==
              'x',
          "Metered Value copies still own independent string storage");

    ChangeReceipt receipt{
        TransactionId("transaction"), 17, 18, "content", false, EntityId("entity")};
    const auto receipt_operation = std::make_shared<ledger::OperationLedger>(
        ledger::Identity{"receipt-value-scope", {}, {}, 0});
    {
        ledger::Scope scope(receipt_operation);
        const auto output = change_receipt_value(receipt);
        const Value numeric(1.25);
        check(std::get<Value::Object>(output.data).size() == 5 &&
                  std::get<double>(numeric.data) == 1.25,
              "Receipt projection and next physical Value retain their original data");
    }
    const auto receipt_values =
        receipt_operation->snapshot().values[static_cast<std::size_t>(ledger::Stage::application)];
    check(
        receipt_values[static_cast<std::size_t>(ledger::Metric::model_copy_bytes)] ==
                sizeof(double) &&
            receipt_values[static_cast<std::size_t>(ledger::Metric::metadata_copy_bytes)].value_or(
                0) > 0,
        "Receipt source scope records transaction H and restores physical Value classification");
}
void parameterless_contract() {
    OperationRegistry registry;
    int calls{};
    good(registry.register_typed<EmptyInput>(
             InputTraits<EmptyInput>::definition(),
             [&calls](const OperationContext& context, const EmptyInput&) {
                 check(context.requested_version == 1, "empty handler version forwarded");
                 ++calls;
                 return output("empty");
             }),
         "register parameterless input");
    good(registry.invoke("test.empty", test_context(), Value{}), "empty object accepted");
    bad(registry.invoke("test.empty", test_context(), Value(true)),
        ErrorCode::invalid_input,
        "parameterless input still requires object");
    bad(registry.invoke("test.empty", test_context(), Value(Value::Object{{"extra", Value(1)}})),
        ErrorCode::invalid_input,
        "parameterless input rejects extra parameters");
    check(calls == 1, "invalid empty inputs do not execute handler");
}
} // namespace

int main() {
    try {
        registration_and_availability();
        mechanical_input_validation();
        all_adapters_and_handlers();
        context_validation_and_isolation();
        output_contract();
        receipt_projection();
        owned_value_copy_ledger();
        parameterless_contract();
        std::cout
            << "PASS: typed operation registry, availability and mechanical wire validation\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
