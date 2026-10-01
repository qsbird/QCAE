#include "qcae/result_service.hpp"
#include "qcae/operation_registry.hpp"
#include "qcae/records.hpp"

namespace qcae::features::results {
namespace {
constexpr const char* owner = "qcae.results.fixture";
template <class T> Result<T> success(T value) {
    return {Status::success, std::move(value), {}};
}
template <class T> Result<T> failed(ErrorCode code, std::string message, std::string field = {}) {
    return {code == ErrorCode::missing_input ? Status::needs_input
            : code == ErrorCode::revision_conflict || code == ErrorCode::idempotency_key_conflict
                ? Status::conflict
                : Status::failed,
            {},
            Diagnostic{code, std::move(message), std::move(field)}};
}
std::shared_ptr<const OwnedRowImage> row(const StoredFixtureResult& result) {
    const std::array fields{std::string("QCAE-FIXTURE-RESULT-ROW-1"),
                            result.id,
                            result.principal,
                            result.signature,
                            result.artifact_id,
                            encode_result_bundle(result.bundle)};
    return std::make_shared<const OwnedRowImage>(
        OwnedRowImage{{StoreSpace::artifact_record, result.id},
                      owner,
                      1,
                      std::make_shared<const std::string>(record_wire::strings(fields)),
                      {}});
}
StoredFixtureResult read_row(const OwnedRowImage& image) {
    if (image.key.space != StoreSpace::artifact_record || image.owner != owner ||
        image.schema_version != 1 || !image.payload)
        throw RecordError(ErrorCode::schema_unsupported, "Unsupported fixture result row");
    const auto fields = record_wire::read_strings(*image.payload);
    if (fields.size() != 6 || fields[0] != "QCAE-FIXTURE-RESULT-ROW-1" ||
        fields[1] != image.key.identity || fields[2].empty() || fields[3].empty() ||
        fields[4].empty())
        throw RecordError(ErrorCode::schema_unsupported, "Malformed fixture result row");
    return {fields[1], fields[2], fields[3], fields[4], decode_result_bundle(fields[5])};
}
std::string
result_identity(const Caller& caller, const DocumentRef& document, std::string_view key) {
    using operations::Value;
    const auto value =
        operations::canonical_value(Value(Value::Object{{"caller", Value(caller.principal)},
                                                        {"document", Value(document.id.value)},
                                                        {"key", Value(key)}}));
    if (!value.ok())
        throw RecordError(value.error->code, value.error->message, value.error->field);
    return "qcae:fixture-result:" + *value.value;
}
std::string fixture_signature(const WriteContext& context,
                              std::string_view artifact_id,
                              const ResultFixture& fixture) {
    std::vector<std::string> fields{context.document.id.value,
                                    context.document.epoch.value,
                                    std::to_string(context.expected_revision),
                                    std::string(artifact_id),
                                    fixture.input_fingerprint,
                                    fixture.quantity,
                                    fixture.unit,
                                    record_wire::strings(fixture.components),
                                    fixture.location,
                                    fixture.coordinate_basis,
                                    fixture.case_label,
                                    fixture.source_kind,
                                    fixture.frame ? std::to_string(*fixture.frame) : "absent"};
    for (const auto& identity : fixture.identities)
        fields.push_back(record_wire::strings(std::array{
            identity.entity.value, identity.name_space, std::to_string(identity.number)}));
    fields.push_back("values");
    for (const auto& value : fixture.values)
        fields.push_back(record_wire::strings(
            std::array{std::to_string(value.solver_number), record_wire::vector3(value.value)}));
    return record_wire::strings(fields);
}
} // namespace
OwnedRowHandler result_row_handler() {
    return {StoreSpace::artifact_record,
            owner,
            [](const OwnedRowImage& image) { (void)read_row(image); },
            [](const OwnedRowImage& image) -> std::shared_ptr<const OwnedRowImage> {
                (void)read_row(image);
                return {};
            },
            [](const OwnedRowImage&) { return false; }};
}
FixtureResultService::FixtureResultService(RecordApplication& app, FrozenInputResolver resolve)
    : app_(app), resolve_(std::move(resolve)) {
    if (!resolve_)
        throw std::invalid_argument("Fixture result service requires a frozen artifact resolver");
}
Result<StoredFixtureResult> FixtureResultService::get(const Caller& caller,
                                                      const DocumentRef& document,
                                                      std::string_view identity) const {
    if (caller.principal.empty())
        return failed<StoredFixtureResult>(
            ErrorCode::missing_input, "Caller is required", "caller");
    const auto rows = app_.owned_rows(document, StoreSpace::artifact_record, owner);
    if (!rows.ok())
        return {rows.status, {}, rows.error};
    try {
        for (const auto& image : *rows.value) {
            if (image->key.identity != identity)
                continue;
            auto result = read_row(*image);
            if (result.principal == caller.principal)
                return success(std::move(result));
        }
    } catch (const RecordError& error) {
        return failed<StoredFixtureResult>(error.code(), error.what(), error.field());
    }
    return failed<StoredFixtureResult>(
        ErrorCode::entity_not_found, "Fixture result is not recorded for this caller", "result_id");
}
Result<StoredFixtureResult> FixtureResultService::read(const Caller& caller,
                                                       const WriteContext& context,
                                                       std::string_view artifact_id,
                                                       const ResultFixture& fixture,
                                                       const std::string& key) {
    if (caller.principal.empty() || key.empty() || artifact_id.empty())
        return failed<StoredFixtureResult>(ErrorCode::missing_input,
                                           "Caller, artifact ID and idempotency key are required");
    const auto snapshot = app_.snapshot(context.document);
    if (!snapshot.ok())
        return {snapshot.status, {}, snapshot.error};
    try {
        const auto identity = result_identity(caller, context.document, key);
        const auto signature = fixture_signature(context, artifact_id, fixture);
        const auto retained = get(caller, context.document, identity);
        if (retained.ok()) {
            if (retained.value->signature != signature)
                return failed<StoredFixtureResult>(ErrorCode::idempotency_key_conflict,
                                                   "Result key was used with different input",
                                                   "idempotency_key");
            return retained;
        }
        if (retained.error->code != ErrorCode::entity_not_found)
            return retained;
        if (snapshot.value->info.revision != context.expected_revision)
            return failed<StoredFixtureResult>(ErrorCode::revision_conflict,
                                               "Fixture request input revision is stale",
                                               "expected_revision");
        const auto input = resolve_(caller, context.document, artifact_id);
        if (!input.ok())
            return {input.status, {}, input.error};
        const auto bundle = FixtureResultReader{}.read(*input.value, fixture);
        if (!bundle.ok())
            return {bundle.status, {}, bundle.error};
        StoredFixtureResult result{
            identity, caller.principal, signature, std::string(artifact_id), *bundle.value};
        const auto image = row(result);
        const OwnedRowUpdate update{image->key, {}, image};
        const auto persisted =
            app_.update_owned_rows(caller, context.document, std::span(&update, 1));
        if (!persisted.ok()) {
            if (persisted.error->code == ErrorCode::revision_conflict) {
                const auto concurrent = get(caller, context.document, identity);
                if (concurrent.ok() && concurrent.value->signature == signature)
                    return concurrent;
                if (concurrent.ok())
                    return failed<StoredFixtureResult>(ErrorCode::idempotency_key_conflict,
                                                       "Result key was used with different input",
                                                       "idempotency_key");
            }
            return {persisted.status, {}, persisted.error};
        }
        return success(std::move(result));
    } catch (const RecordError& error) {
        return failed<StoredFixtureResult>(error.code(), error.what(), error.field());
    }
}
} // namespace qcae::features::results
