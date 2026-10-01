#include "qcae/analysis_features.hpp"
#include <charconv>
#include <set>

namespace qcae::features::analysis {
namespace {
[[noreturn]] void invalid() {
    throw RecordError(ErrorCode::schema_unsupported, "Malformed analysis check report");
}
std::uint64_t number(std::string_view text) {
    std::uint64_t value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        invalid();
    return value;
}
void validate(const CheckReport& report) {
    const auto& profile = report.target.profile;
    const bool legacy = report.catalog_version == RuleCatalog::legacy_version;
    const RuleCatalog catalog;
    const auto definitions = catalog.definitions();
    if (report.id.empty() || report.principal.empty() || report.signature.empty() ||
        report.input.document.id.value.empty() || report.input.document.epoch.value.empty() ||
        report.analysis.value.empty() || profile.profile_id.empty() ||
        profile.profile_version.empty() || profile.definition_digest.empty() ||
        report.target.analysis_kind != "linear_static" ||
        (!legacy && report.catalog_version != RuleCatalog::version) ||
        report.issues.size() > (legacy ? 2 : definitions.size()) ||
        (legacy && report.outcome != Status::success) || report.outcome == Status::conflict)
        invalid();
    std::set<std::string_view> ids;
    auto conclusion = Status::success;
    for (const auto& issue : report.issues) {
        const RuleDefinition* known = nullptr;
        for (const auto& rule : definitions)
            if (rule.id == issue.rule_id && rule.version == issue.rule_version &&
                rule.description == issue.message)
                known = &rule;
        if (!known || !ids.insert(issue.rule_id).second || issue.severity != "error" ||
            issue.entity.identity.empty() || !issue.entity.type.value ||
            !same_record_version(issue.input, report.input) || issue.field.empty() ||
            issue.field.size() > 256 || issue.actual.empty() || issue.expected.empty() ||
            issue.actual.size() > 4096 || issue.expected.size() > 4096)
            invalid();
        if (legacy) {
            if ((issue.rule_id != "missing-load" && issue.rule_id != "missing-constraint") ||
                issue.entity != RecordKey{RecordTraits<records::AnalysisDefinition>::type_id,
                                          report.analysis.value} ||
                issue.actual != "0" || issue.expected != ">=1")
                invalid();
            const auto expected = issue.rule_id == "missing-load" ? "forces" : "constraints";
            if (issue.field != expected && issue.field != "load_cases." + std::string(expected))
                invalid();
        } else if (known->outcome == Status::failed || conclusion == Status::success) {
            conclusion = known->outcome;
        }
    }
    if (!legacy && report.outcome != conclusion)
        invalid();
}
operations::Value version_value(const RecordVersion& version) {
    using operations::Value;
    return Value(Value::Object{{"document_id", Value(version.document.id.value)},
                               {"document_epoch", Value(version.document.epoch.value)},
                               {"revision", Value(std::to_string(version.revision))}});
}
} // namespace
std::string encode_check_report(const CheckReport& report) {
    validate(report);
    const bool legacy = report.catalog_version == RuleCatalog::legacy_version;
    std::vector<std::string> fields{legacy ? "QCAE-ANALYSIS-CHECK-1" : "QCAE-ANALYSIS-CHECK-2",
                                    report.id,
                                    report.principal,
                                    report.signature,
                                    report.input.document.id.value,
                                    report.input.document.epoch.value,
                                    std::to_string(report.input.revision),
                                    report.analysis.value,
                                    report.target.profile.profile_id,
                                    report.target.profile.profile_version,
                                    report.target.profile.definition_digest,
                                    report.target.analysis_kind,
                                    report.catalog_version};
    if (!legacy)
        fields.emplace_back(check_outcome_name(report.outcome));
    fields.push_back(std::to_string(report.issues.size()));
    for (const auto& issue : report.issues) {
        fields.insert(fields.end(),
                      {issue.rule_id,
                       std::to_string(issue.rule_version),
                       issue.severity,
                       std::to_string(issue.entity.type.value),
                       issue.entity.identity,
                       issue.field,
                       issue.message,
                       issue.actual,
                       issue.expected});
    }
    const auto bytes = record_wire::strings(fields);
    if (bytes.size() > 65536)
        throw RecordError(ErrorCode::resource_limit,
                          "Analysis check report exceeds the byte quota");
    return bytes;
}
CheckReport decode_check_report(std::string_view bytes) {
    if (bytes.size() > 65536)
        invalid();
    const auto fields = record_wire::read_strings(bytes);
    if (fields.size() < 14 ||
        (fields[0] != "QCAE-ANALYSIS-CHECK-1" && fields[0] != "QCAE-ANALYSIS-CHECK-2"))
        invalid();
    const bool legacy = fields[0] == "QCAE-ANALYSIS-CHECK-1";
    const std::size_t header = legacy ? 14 : 15;
    if (fields.size() < header)
        invalid();
    const auto count = number(fields[header - 1]);
    const RuleCatalog catalog;
    if (count > (legacy ? 2 : catalog.definitions().size()) || fields.size() != header + count * 9)
        invalid();
    CheckReport report;
    report.id = fields[1];
    report.principal = fields[2];
    report.signature = fields[3];
    report.input = {{DocumentId(fields[4]), DocumentEpoch(fields[5])}, number(fields[6])};
    report.analysis = EntityId(fields[7]);
    report.target = {{fields[8], fields[9], fields[10]}, fields[11]};
    report.catalog_version = fields[12];
    if (report.catalog_version != (legacy ? RuleCatalog::legacy_version : RuleCatalog::version))
        invalid();
    if (!legacy) {
        if (fields[13] == "needs_input")
            report.outcome = Status::needs_input;
        else if (fields[13] == "failed")
            report.outcome = Status::failed;
        else if (fields[13] != "success")
            invalid();
    }
    for (std::size_t index = header; index < fields.size(); index += 9) {
        const auto rule_version = number(fields[index + 1]);
        const auto type = number(fields[index + 3]);
        if (rule_version > UINT32_MAX || type > UINT32_MAX)
            invalid();
        report.issues.push_back(
            {fields[index],
             static_cast<std::uint32_t>(rule_version),
             fields[index + 2],
             {RecordTypeId{static_cast<std::uint32_t>(type)}, fields[index + 4]},
             report.input,
             fields[index + 5],
             fields[index + 6],
             fields[index + 7],
             fields[index + 8]});
    }
    validate(report);
    return report;
}
operations::Value check_report_value(const CheckReport& report, const DocumentView& current) {
    using operations::Value;
    const auto state = issue_state(report, current) == IssueState::current ? "current" : "stale";
    Value::Array issues;
    for (const auto& issue : report.issues) {
        issues.emplace_back(Value::Object{
            {"rule_id", Value(issue.rule_id)},
            {"rule_version", Value(static_cast<std::int64_t>(issue.rule_version))},
            {"severity", Value(issue.severity)},
            {"entity_id", Value(issue.entity.identity)},
            {"entity_type", Value(static_cast<std::int64_t>(issue.entity.type.value))},
            {"input_version", version_value(issue.input)},
            {"state", Value(state)},
            {"field", Value(issue.field)},
            {"message", Value(issue.message)},
            {"actual", Value(issue.actual)},
            {"expected", Value(issue.expected)}});
    }
    return Value(
        Value::Object{{"check_id", Value(report.id)},
                      {"analysis_id", Value(report.analysis.value)},
                      {"input_version", version_value(report.input)},
                      {"current_version", version_value(current.version())},
                      {"catalog_version", Value(report.catalog_version)},
                      {"check_execution", Value("completed")},
                      {"outcome", Value(check_outcome_name(report.outcome))},
                      {"scope",
                       Value(report.catalog_version == RuleCatalog::legacy_version
                                 ? "legacy-linear-static-completeness"
                                 : "cantilever-linear-static-necessary-conditions")},
                      {"profile",
                       Value(Value::Object{
                           {"profile_id", Value(report.target.profile.profile_id)},
                           {"profile_version", Value(report.target.profile.profile_version)},
                           {"definition_digest", Value(report.target.profile.definition_digest)}})},
                      {"analysis_kind", Value(report.target.analysis_kind)},
                      {"state", Value(state)},
                      {"issues", Value(std::move(issues))}});
}
} // namespace qcae::features::analysis
