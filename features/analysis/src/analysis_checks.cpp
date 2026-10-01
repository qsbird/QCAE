#include "qcae/analysis_features.hpp"
#include "cantilever_checks.hpp"
#include <algorithm>
#include <array>

namespace qcae::features::analysis {
namespace {
constexpr const char* report_owner = "qcae.analysis.check";
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
Result<CheckReport> conclusion(CheckReport report) {
    const auto status = report.outcome;
    std::optional<Diagnostic> error;
    if (status != Status::success) {
        for (const auto& issue : report.issues) {
            const auto definitions = detail::cantilever_rules();
            const auto rule =
                std::find_if(definitions.begin(), definitions.end(), [&](const auto& item) {
                    return item.id == issue.rule_id && item.outcome == status;
                });
            if (rule == definitions.end())
                continue;
            error = Diagnostic{status == Status::needs_input ? ErrorCode::missing_input
                                                             : ErrorCode::invalid_input,
                               issue.message,
                               issue.field};
            break;
        }
    }
    return {status, std::move(report), std::move(error)};
}
std::shared_ptr<const OwnedRowImage> report_row(const CheckReport& report) {
    return std::make_shared<const OwnedRowImage>(
        OwnedRowImage{{StoreSpace::artifact_record, report.id},
                      report_owner,
                      report.catalog_version == RuleCatalog::legacy_version ? 1u : 2u,
                      std::make_shared<const std::string>(encode_check_report(report)),
                      {}});
}
CheckReport read_report(const OwnedRowImage& row) {
    if (row.key.space != StoreSpace::artifact_record || row.owner != report_owner ||
        (row.schema_version != 1 && row.schema_version != 2) || !row.payload)
        throw RecordError(ErrorCode::schema_unsupported, "Unsupported analysis check row");
    auto report = decode_check_report(*row.payload);
    const auto schema = report.catalog_version == RuleCatalog::legacy_version ? 1u : 2u;
    if (report.id != row.key.identity || row.schema_version != schema)
        throw RecordError(ErrorCode::schema_unsupported, "Check report identity mismatch");
    return report;
}
std::string
check_identity(const Caller& caller, const DocumentRef& document, std::string_view key) {
    // Exact length-delimited canonical data avoids a hash collision replacing a report.
    const auto identity = operations::canonical_value(operations::Value(
        operations::Value::Object{{"caller", operations::Value(caller.principal)},
                                  {"document", operations::Value(document.id.value)},
                                  {"key", operations::Value(key)}}));
    if (!identity.ok())
        throw RecordError(identity.error->code, identity.error->message, identity.error->field);
    return "qcae:analysis-check:" + *identity.value;
}
std::string signature(const WriteContext& context, const EntityId& analysis) {
    const std::array values{context.document.id.value,
                            context.document.epoch.value,
                            std::to_string(context.expected_revision),
                            analysis.value,
                            std::string(RuleCatalog::version)};
    return record_wire::strings(values);
}
} // namespace

std::span<const RuleDefinition> RuleCatalog::definitions() const noexcept {
    return detail::cantilever_rules();
}
Result<CheckReport> RuleCatalog::check(const DocumentView& view, const EntityId& identity) const {
    const auto record = view.find<records::AnalysisDefinition>(identity);
    if (!record)
        return failed<CheckReport>(
            ErrorCode::entity_not_found, "Analysis definition does not exist", "analysis_id");
    const auto& analysis = record->get<records::AnalysisDefinition>();
    if (analysis.target.analysis_kind != "linear_static")
        return failed<CheckReport>(
            ErrorCode::unsupported_capability,
            "Only the controlled linear static cantilever rules are installed",
            "analysis_kind");
    CheckReport report;
    report.input = view.version();
    report.analysis = identity;
    report.target = analysis.target;
    report.catalog_version = version;
    detail::check_cantilever(view, record, report);
    return conclusion(std::move(report));
}
std::string_view check_outcome_name(Status status) noexcept {
    switch (status) {
    case Status::success:
        return "success";
    case Status::needs_input:
        return "needs_input";
    case Status::failed:
        return "failed";
    case Status::conflict:
        return "conflict";
    }
    return "failed";
}
IssueState issue_state(const CheckReport& report, const DocumentView& current) noexcept {
    const auto record = current.find<records::AnalysisDefinition>(report.analysis);
    return same_record_version(report.input, current.version()) &&
                   report.catalog_version == RuleCatalog::version && record &&
                   record->get<records::AnalysisDefinition>().target == report.target
               ? IssueState::current
               : IssueState::stale;
}
OwnedRowHandler check_row_handler() {
    OwnedRowHandler handler;
    handler.space = StoreSpace::artifact_record;
    handler.owner = report_owner;
    handler.validate = [](const OwnedRowImage& row) { (void)read_report(row); };
    // Restart does not rewrite diagnostics or claim that old input is current.
    handler.recover = [](const OwnedRowImage& row) -> std::shared_ptr<const OwnedRowImage> {
        (void)read_report(row);
        return {};
    };
    handler.blocks_close = [](const OwnedRowImage&) { return false; };
    return handler;
}
CheckService::CheckService(RecordApplication& app) : app_(app) {}
Result<CheckReport> CheckService::get(const Caller& caller,
                                      const DocumentRef& document,
                                      const std::string& identity) const {
    if (caller.principal.empty())
        return failed<CheckReport>(ErrorCode::missing_input, "Caller is required", "caller");
    const auto rows = app_.owned_rows(document, StoreSpace::artifact_record, report_owner);
    if (!rows.ok())
        return {rows.status, {}, rows.error};
    try {
        for (const auto& row : *rows.value) {
            if (row->key.identity != identity)
                continue;
            auto report = read_report(*row);
            if (report.principal == caller.principal)
                return success(std::move(report));
        }
    } catch (const RecordError& error) {
        return failed<CheckReport>(error.code(), error.what(), error.field());
    }
    return failed<CheckReport>(ErrorCode::entity_not_found,
                               "Analysis check report does not exist for this caller",
                               "check_id");
}
Result<CheckReport> CheckService::run(const Caller& caller,
                                      const WriteContext& context,
                                      const EntityId& analysis,
                                      const std::string& key) {
    if (caller.principal.empty() || key.empty())
        return failed<CheckReport>(ErrorCode::missing_input,
                                   "Caller and idempotency key are required",
                                   key.empty() ? "idempotency_key" : "caller");
    const auto snapshot = app_.snapshot(context.document);
    if (!snapshot.ok())
        return {snapshot.status, {}, snapshot.error};
    try {
        const auto identity = check_identity(caller, context.document, key);
        const auto requested = signature(context, analysis);
        const auto retained = get(caller, context.document, identity);
        if (retained.ok()) {
            if (retained.value->signature != requested)
                return failed<CheckReport>(ErrorCode::idempotency_key_conflict,
                                           "Check key was used for different input",
                                           "idempotency_key");
            return conclusion(*retained.value);
        }
        if (retained.error->code != ErrorCode::entity_not_found)
            return retained;
        if (snapshot.value->info.revision != context.expected_revision)
            return failed<CheckReport>(ErrorCode::revision_conflict,
                                       "Analysis check input revision is stale",
                                       "expected_revision");
        auto report = catalog_.check(snapshot.value->records, analysis);
        if (!report.value)
            return report;
        report.value->id = identity;
        report.value->principal = caller.principal;
        report.value->signature = requested;
        const auto row = report_row(*report.value);
        const OwnedRowUpdate update{row->key, {}, row};
        const auto persisted =
            app_.update_owned_rows(caller, context.document, std::span(&update, 1));
        if (!persisted.ok()) {
            // Concurrent identical checks use one retained fact. Conflicting input
            // cannot overwrite it, regardless of which computation completed first.
            if (persisted.error->code == ErrorCode::revision_conflict) {
                const auto concurrent = get(caller, context.document, identity);
                if (concurrent.ok() && concurrent.value->signature == requested)
                    return conclusion(*concurrent.value);
                if (concurrent.ok())
                    return failed<CheckReport>(ErrorCode::idempotency_key_conflict,
                                               "Check key was used for different input",
                                               "idempotency_key");
            }
            return {persisted.status, {}, persisted.error};
        }
        return report;
    } catch (const RecordError& error) {
        return failed<CheckReport>(error.code(), error.what(), error.field());
    }
}
} // namespace qcae::features::analysis
