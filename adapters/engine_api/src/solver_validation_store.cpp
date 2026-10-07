#include "solver_validation_store.hpp"
#include "solver_result_store.hpp"
#include "solver_version_probe.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <set>
#include <tuple>

namespace qcae::ipc::detail {
namespace {
namespace results = features::results;
void require(bool value, const char* message) {
    if (!value)
        throw RecordError(ErrorCode::schema_unsupported, message, "solver_validation");
}
bool bounded(std::string_view value, std::size_t limit = 128) {
    return !value.empty() && value.size() <= limit && value.find('\0') == value.npos;
}
bool digest(std::string_view value) {
    return value.size() == 64 && value.find_first_not_of("0123456789abcdef") == value.npos;
}
std::uint64_t number(std::string_view text) {
    std::uint64_t value{};
    const auto read = std::from_chars(text.data(), text.data() + text.size(), value);
    require(read.ec == std::errc{} && read.ptr == text.data() + text.size() &&
                std::to_string(value) == text,
            "Validation has a noncanonical integer");
    return value;
}
std::string report_bytes(const results::StaticComparisonReport& report) {
    std::vector<std::string> fields{
        report.reference_id, report.reference_input, record_wire::boolean(report.matched)};
    for (const auto& item : report.components)
        fields.push_back(
            record_wire::strings(std::array<std::string, 11>{item.entity.value,
                                                             std::to_string(item.solver_number),
                                                             item.quantity,
                                                             item.component,
                                                             item.unit,
                                                             record_wire::real(item.actual),
                                                             record_wire::real(item.expected),
                                                             record_wire::real(item.absolute_error),
                                                             record_wire::real(item.tolerance),
                                                             record_wire::boolean(item.matched),
                                                             "six-component-v1"}));
    return record_wire::strings(fields);
}
void validate_report(const results::StaticComparisonReport& report) {
    require(report.reference_id == "qcae.cantilever.numeric-reference.v1" &&
                report.reference_input == "nastran-real-benchmark-v3" &&
                report.components.size() == 132,
            "Validation does not identify the bounded preregistered comparison");
    std::set<std::tuple<std::string, std::string, std::string>> keys;
    bool matched = true;
    for (const auto& item : report.components) {
        require(bounded(item.entity.value) && item.solver_number > 0 &&
                    (item.quantity == "displacement" || item.quantity == "spc_reaction") &&
                    (item.component == "T1" || item.component == "T2" || item.component == "T3" ||
                     item.component == "R1" || item.component == "R2" || item.component == "R3") &&
                    keys.emplace(item.entity.value, item.quantity, item.component).second,
                "Validation contains an invalid or repeated component key");
        const bool rotation = item.component.front() == 'R';
        const std::string unit =
            item.quantity == "displacement" ? (rotation ? "rad" : "mm") : (rotation ? "N*mm" : "N");
        require(item.unit == unit && std::isfinite(item.actual) && std::isfinite(item.expected) &&
                    std::isfinite(item.absolute_error) && std::isfinite(item.tolerance) &&
                    item.tolerance >= 0 &&
                    item.absolute_error == std::abs(item.actual - item.expected) &&
                    item.matched == (item.absolute_error <= item.tolerance),
                "Validation component units, error or comparison are inconsistent");
        matched = matched && item.matched;
    }
    require(report.matched == matched, "Validation aggregate differs from its components");
}
results::StaticComparisonReport read_report(std::string_view bytes) {
    const auto fields = record_wire::read_strings(bytes);
    require(fields.size() == 135, "Validation report component count is invalid");
    results::StaticComparisonReport report;
    report.reference_id = fields[0];
    report.reference_input = fields[1];
    report.matched = record_wire::read_boolean(fields[2]);
    for (std::size_t index = 3; index < fields.size(); ++index) {
        const auto row = record_wire::read_strings(fields[index]);
        require(row.size() == 11 && row[10] == "six-component-v1",
                "Validation component codec is unsupported");
        report.components.push_back({EntityId(row[0]),
                                     number(row[1]),
                                     row[2],
                                     row[3],
                                     row[4],
                                     record_wire::read_real(row[5]),
                                     record_wire::read_real(row[6]),
                                     record_wire::read_real(row[7]),
                                     record_wire::read_real(row[8]),
                                     record_wire::read_boolean(row[9])});
    }
    validate_report(report);
    require(report_bytes(report) == bytes, "Validation report bytes are not canonical");
    return report;
}
void validate_fact(const SolverValidationFact& fact) {
    require(bounded(fact.principal, 4096) && bounded(fact.idempotency_key, 4096) &&
                fact.signature.size() <= 16384 && fact.ordinal > 0 &&
                fact.ordinal <= solver_validation_history_limit && bounded(fact.run_id) &&
                bounded(fact.result_id) && bounded(fact.task_id) && digest(fact.origin_sha256) &&
                digest(fact.parsed_sha256) && digest(fact.manifest_sha256) &&
                digest(fact.frozen_input_sha256) && bounded(fact.registered_profile.profile_id) &&
                bounded(fact.registered_profile.profile_version) &&
                bounded(fact.registered_profile.definition_digest),
            "Validation request or source binding is malformed");
    const auto signature = record_wire::read_strings(fact.signature);
    require(signature.size() == 8 && bounded(signature[0]) && bounded(signature[1]) &&
                signature[3] == fact.run_id && signature[4] == fact.principal &&
                signature[5] == fact.registered_profile.profile_id &&
                signature[6] == fact.registered_profile.profile_version &&
                signature[7] == fact.registered_profile.definition_digest,
            "Validation signature differs from its request/profile");
    (void)number(signature[2]);
    require((fact.reader.reader_version == "qcae.nastran.static-f06.v1" ||
             fact.reader.reader_version == "qcae.mystran.static-f06.v1") &&
                bounded(fact.reader.resource, 4096) && fact.reader.subcase == 1 &&
                fact.reader.unit_system == "mm-N-MPa" && fact.reader.coordinate_basis == "basic",
            "Validation reader contract is unsupported");
    validate_report(fact.report);
}
std::string encode(const SolverValidationFact& fact) {
    validate_fact(fact);
    const auto bytes = record_wire::strings(std::array<std::string, 16>{
        "QCAE-SOLVER-VALIDATION-1",
        fact.principal,
        fact.idempotency_key,
        fact.signature,
        std::to_string(fact.ordinal),
        fact.run_id,
        fact.result_id,
        fact.task_id,
        fact.origin_sha256,
        fact.parsed_sha256,
        fact.manifest_sha256,
        fact.frozen_input_sha256,
        record_wire::strings(std::array<std::string, 3>{fact.registered_profile.profile_id,
                                                        fact.registered_profile.profile_version,
                                                        fact.registered_profile.definition_digest}),
        record_wire::strings(std::array<std::string, 5>{fact.reader.reader_version,
                                                        fact.reader.resource,
                                                        std::to_string(fact.reader.subcase),
                                                        fact.reader.unit_system,
                                                        fact.reader.coordinate_basis}),
        record_wire::boolean(fact.test_only),
        report_bytes(fact.report)});
    require(bytes.size() <= 65536, "Validation owned fact exceeds 64KiB");
    return bytes;
}
} // namespace
std::string solver_validation_prefix(std::string_view run_id) {
    require(bounded(run_id), "Validation run id is not bounded");
    return "validation-" + artifact_sha256(run_id).substr(0, 32) + "-";
}
std::string
solver_validation_identity(std::string_view run_id, const Caller& caller, std::string_view key) {
    return solver_validation_prefix(run_id) +
           artifact_sha256(record_wire::strings(
               std::array<std::string, 2>{caller.principal, std::string(key)}));
}
std::shared_ptr<const OwnedRowImage>
solver_validation_index_row(const SolverValidationIndex& index) {
    const auto prefix = solver_validation_prefix(index.run_id);
    require(bounded(index.principal, 4096) && !index.report_rows.empty() &&
                index.report_rows.size() <= solver_validation_history_limit,
            "Validation history index exceeds its bounds");
    std::vector<std::string> fields{
        "QCAE-SOLVER-VALIDATION-INDEX-1", index.run_id, index.principal};
    std::set<std::string> unique;
    for (const auto& identity : index.report_rows) {
        require(identity.size() == prefix.size() + 64 && identity.starts_with(prefix) &&
                    digest(std::string_view(identity).substr(prefix.size())) &&
                    unique.insert(identity).second,
                "Validation history index has an invalid/repeated row");
        fields.push_back(identity);
    }
    return std::make_shared<const OwnedRowImage>(
        OwnedRowImage{{StoreSpace::artifact_record, prefix + "head"},
                      std::string(solver_validation_owner),
                      2,
                      std::make_shared<const std::string>(record_wire::strings(fields)),
                      {}});
}
SolverValidationIndex decode_solver_validation_index(const OwnedRowImage& image) {
    require(image.owner == solver_validation_owner && image.schema_version == 2 &&
                image.key.space == StoreSpace::artifact_record && image.payload &&
                image.payload->size() <= 8192,
            "Validation index owner/schema/quota is invalid");
    const auto fields = record_wire::read_strings(*image.payload);
    require(fields.size() >= 4 && fields.size() <= solver_validation_history_limit + 3 &&
                fields[0] == "QCAE-SOLVER-VALIDATION-INDEX-1",
            "Validation index codec is unsupported");
    SolverValidationIndex index{fields[1], fields[2], {fields.begin() + 3, fields.end()}};
    const auto canonical = solver_validation_index_row(index);
    require(canonical->key == image.key && *canonical->payload == *image.payload,
            "Validation index key/canonical bytes differ");
    return index;
}
Result<SolverValidationFact> prepare_solver_validation(const StoredSolverResult& stored,
                                                       const ProfileRef& registered_profile) {
    try {
        (void)solver_result_row(stored);
        require(stored.published, "Validation requires a published parsed result");
        const auto& parsed = stored.parsed;
        const auto& origin = parsed.origin;
        if (!origin.test_only)
            require(origin.version_probe && !origin.version_probe->synthetic &&
                        solver_version_allows_execution(
                            *origin.version_probe, origin.run.request.configuration.solver_version),
                    "External validation lacks its original trusted version observation");
        results::StaticFields fields;
        fields.subcase = parsed.fields.subcase;
        fields.coordinate_basis = parsed.fields.coordinate_basis;
        fields.displacement_units = parsed.fields.displacement_units;
        fields.reaction_units = parsed.fields.reaction_units;
        for (const auto& row : parsed.fields.displacements)
            fields.displacements.push_back({row.entity, row.solver_number, row.components});
        for (const auto& row : parsed.fields.spc_reactions)
            fields.spc_reactions.push_back({row.entity, row.solver_number, row.components});
        const auto comparison = results::compare_preregistered_cantilever(
            origin.source_input, fields, registered_profile);
        if (!comparison.ok())
            return {comparison.status, {}, comparison.error};
        SolverValidationFact fact;
        fact.principal = origin.principal;
        fact.run_id = origin.run.request.run_id;
        fact.result_id = stored.artifact.artifact_id;
        fact.task_id = origin.run.request.task_id;
        fact.origin_sha256 = artifact_sha256(encode_solver_owned_run(origin));
        fact.parsed_sha256 = artifact_sha256(encode_solver_parsed_result(parsed));
        fact.manifest_sha256 = artifact_sha256(stored.artifact.manifest);
        fact.frozen_input_sha256 =
            artifact_sha256(features::analysis::encode_frozen_analysis_input(origin.source_input));
        fact.registered_profile = registered_profile;
        fact.reader = parsed.reader;
        fact.test_only = origin.test_only;
        fact.report = *comparison.value;
        return {Status::success, std::move(fact), {}};
    } catch (const RecordError& error) {
        return {Status::failed, {}, Diagnostic{error.code(), error.what(), "solver_validation"}};
    }
}
std::shared_ptr<const OwnedRowImage> solver_validation_row(const SolverValidationFact& fact) {
    return std::make_shared<const OwnedRowImage>(OwnedRowImage{
        {StoreSpace::artifact_record,
         solver_validation_identity(fact.run_id, {fact.principal}, fact.idempotency_key)},
        std::string(solver_validation_owner),
        1,
        std::make_shared<const std::string>(encode(fact)),
        {}});
}
SolverValidationFact decode_solver_validation_row(const OwnedRowImage& image) {
    require(image.owner == solver_validation_owner && image.schema_version == 1 &&
                image.key.space == StoreSpace::artifact_record && image.payload &&
                image.payload->size() <= 65536,
            "Validation row owner/schema/quota is invalid");
    const auto fields = record_wire::read_strings(*image.payload);
    require(fields.size() == 16 && fields[0] == "QCAE-SOLVER-VALIDATION-1",
            "Validation fact codec is unsupported");
    SolverValidationFact fact;
    fact.principal = fields[1];
    fact.idempotency_key = fields[2];
    fact.signature = fields[3];
    fact.ordinal = number(fields[4]);
    fact.run_id = fields[5];
    fact.result_id = fields[6];
    fact.task_id = fields[7];
    fact.origin_sha256 = fields[8];
    fact.parsed_sha256 = fields[9];
    fact.manifest_sha256 = fields[10];
    fact.frozen_input_sha256 = fields[11];
    const auto profile = record_wire::read_strings(fields[12]);
    const auto reader = record_wire::read_strings(fields[13]);
    require(profile.size() == 3 && reader.size() == 5, "Validation profile/reader is malformed");
    fact.registered_profile = {profile[0], profile[1], profile[2]};
    fact.reader = {reader[0], reader[1], number(reader[2]), reader[3], reader[4]};
    fact.test_only = record_wire::read_boolean(fields[14]);
    fact.report = read_report(fields[15]);
    require(encode(fact) == *image.payload &&
                image.key.identity ==
                    solver_validation_identity(fact.run_id, {fact.principal}, fact.idempotency_key),
            "Validation row key or canonical bytes differ");
    return fact;
}
void validate_solver_validation_link(const SolverValidationFact& fact,
                                     const StoredSolverResult& stored,
                                     const ProfileRef& registered_profile) {
    const auto expected = prepare_solver_validation(stored, registered_profile);
    require(expected.ok(), "Original parsed source cannot reproduce this validation");
    auto canonical = *expected.value;
    canonical.idempotency_key = fact.idempotency_key;
    canonical.signature = fact.signature;
    canonical.ordinal = fact.ordinal;
    require(encode(canonical) == encode(fact),
            "Validation differs from its original source or preregistered components");
}
OwnedRowHandler solver_validation_row_handler() {
    return {StoreSpace::artifact_record,
            std::string(solver_validation_owner),
            [](const OwnedRowImage& image) {
                // Structural validation runs under Core's lock. Trusted cross-row and
                // reference revalidation must occur at the coordinator boundary.
                if (image.schema_version == 2)
                    (void)decode_solver_validation_index(image);
                else
                    (void)decode_solver_validation_row(image);
            },
            {},
            {}};
}
std::string solver_validation_numerical_stage(const SolverValidationFact& fact) {
    return fact.test_only ? "not_run" : fact.report.matched ? "passed" : "failed";
}
} // namespace qcae::ipc::detail
