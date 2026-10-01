#pragma once
#include "qcae/operation_inputs.hpp"
#include "qcae/record_application.hpp"
#include "qcae/records.hpp"

namespace qcae::features::analysis {
struct OperationPlan {
    std::string signature;
    RecordPrepare prepare;
};
[[nodiscard]] Result<OperationPlan> prepare_create_force(const operations::ForceCreateInput&);
[[nodiscard]] Result<OperationPlan>
prepare_set_force_vector(const operations::ForceSetVectorInput&);
[[nodiscard]] Result<OperationPlan>
prepare_create_constraint(const operations::ConstraintCreateInput&);
[[nodiscard]] Result<OperationPlan>
prepare_create_load_case(const operations::LoadCaseCreateInput&);
[[nodiscard]] Result<OperationPlan>
prepare_set_references(const operations::LoadCaseSetReferencesInput&);
[[nodiscard]] Result<OperationPlan> prepare_create_analysis(const operations::AnalysisCreateInput&,
                                                            const ProfileRef&);

struct RuleDefinition {
    std::string_view id;
    std::uint32_t version{};
    std::string_view description;
    Status outcome{Status::failed};
};
struct Issue {
    std::string rule_id;
    std::uint32_t rule_version{};
    std::string severity;
    RecordKey entity;
    RecordVersion input;
    std::string field, message, actual, expected;
};
struct CheckReport {
    std::string id, principal, signature;
    RecordVersion input;
    EntityId analysis;
    TargetBinding target;
    std::string catalog_version;
    std::vector<Issue> issues;
    // The operation can produce a persisted report even when this conclusion is
    // needs_input or failed. Success establishes only the installed scenario checks.
    Status outcome{Status::success};
};
enum class IssueState { current, stale };
// Scenario rules consume a frozen view; they cannot edit document authority.
class RuleCatalog {
  public:
    static constexpr std::string_view legacy_version = "qcae.analysis.rules.v1";
    static constexpr std::string_view version = "qcae.analysis.rules.v2";
    [[nodiscard]] std::span<const RuleDefinition> definitions() const noexcept;
    [[nodiscard]] Result<CheckReport> check(const DocumentView&, const EntityId&) const;
};
[[nodiscard]] std::string_view check_outcome_name(Status) noexcept;
[[nodiscard]] IssueState issue_state(const CheckReport&, const DocumentView&) noexcept;
[[nodiscard]] std::string encode_check_report(const CheckReport&);
[[nodiscard]] CheckReport decode_check_report(std::string_view);
[[nodiscard]] OwnedRowHandler check_row_handler();

// Diagnostic reports are persisted through the application's side-row port.
// Their input revision remains fixed; querying derives current/stale from a view.
class CheckService {
  public:
    explicit CheckService(RecordApplication&);
    [[nodiscard]] Result<CheckReport>
    run(const Caller&, const WriteContext&, const EntityId&, const std::string& idempotency_key);
    [[nodiscard]] Result<CheckReport>
    get(const Caller&, const DocumentRef&, const std::string& check_id) const;

  private:
    RecordApplication& app_;
    RuleCatalog catalog_;
};
[[nodiscard]] operations::Value check_report_value(const CheckReport&, const DocumentView&);
[[nodiscard]] Result<bool> register_handlers(operations::OperationRegistry&, RecordApplication&);
} // namespace qcae::features::analysis
