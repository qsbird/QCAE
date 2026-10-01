#pragma once
#include "qcae/solver_result.hpp"
#include "qcae/static_validation.hpp"

namespace qcae::ipc::detail {
inline constexpr std::string_view solver_validation_owner = "qcae.solver.validation";
inline constexpr std::size_t solver_validation_history_limit = 16;
struct SolverValidationFact {
    std::string principal, idempotency_key, signature;
    std::uint64_t ordinal{};
    std::string run_id, result_id, task_id;
    // Bind the original canonical facts without duplicating their complete payloads.
    std::string origin_sha256, parsed_sha256, manifest_sha256, frozen_input_sha256;
    ProfileRef registered_profile;
    SolverResultReaderConfiguration reader;
    bool test_only{};
    features::results::StaticComparisonReport report;
};
struct SolverValidationIndex {
    std::string run_id, principal;
    std::vector<std::string> report_rows;
};
[[nodiscard]] std::string solver_validation_prefix(std::string_view run_id);
[[nodiscard]] std::string
solver_validation_identity(std::string_view run_id, const Caller&, std::string_view key);
[[nodiscard]] std::shared_ptr<const OwnedRowImage>
solver_validation_index_row(const SolverValidationIndex&);
[[nodiscard]] SolverValidationIndex decode_solver_validation_index(const OwnedRowImage&);
// These functions need the coordinator's already validated original task/run/source
// chain. File verification remains a separate admission step in the coordinator.
[[nodiscard]] Result<SolverValidationFact>
prepare_solver_validation(const StoredSolverResult&, const ProfileRef& registered_profile);
[[nodiscard]] std::shared_ptr<const OwnedRowImage>
solver_validation_row(const SolverValidationFact&);
[[nodiscard]] SolverValidationFact decode_solver_validation_row(const OwnedRowImage&);
// Recomputes all six components through the one reference helper and compares the
// canonical report and source digests. Never trust a decoded report alone.
void validate_solver_validation_link(const SolverValidationFact&,
                                     const StoredSolverResult&,
                                     const ProfileRef& registered_profile);
[[nodiscard]] OwnedRowHandler solver_validation_row_handler();
[[nodiscard]] std::string solver_validation_numerical_stage(const SolverValidationFact&);
} // namespace qcae::ipc::detail
