#pragma once
#include "qcae/solver_result.hpp"
#include "qcae/solver_run_record.hpp"

namespace qcae::ipc::detail {
void validate_solver_result_reader(const SolverResultReaderConfiguration&,
                                   const SolverRunConfiguration&);
void validate_solver_result_intent(const LocalArtifactIntent&,
                                   const SolverRunRecord&,
                                   const SolverResultReaderConfiguration&);
// The coordinator must resolve the original run/task/source artifact through their
// existing trusted cross-row validators before calling these private format helpers.
[[nodiscard]] Result<std::vector<TextResource>>
read_verified_solver_outputs(const SolverRunRecord&);
[[nodiscard]] Result<SolverParsedResult>
prepare_solver_result(const SolverOwnedRun&,
                      const SolverResultReaderConfiguration&,
                      std::span<const TextResource> verified_outputs);
[[nodiscard]] LocalArtifactIntent solver_result_artifact(const SolverParsedResult&,
                                                         const std::filesystem::path& directory);
[[nodiscard]] std::vector<TextResource>
solver_result_resources(const SolverParsedResult&, std::span<const TextResource> verified_outputs);
// Origin is the exited, pre-parse snapshot (sequence >= 3). The current durable parsed
// intent advances it by at least one; a publication fact advances it by at least two.
// Higher sequences permit explicit reconciliation without replacing original provenance.
void validate_solver_result_link(const SolverParsedResult&, const SolverOwnedRun&);
[[nodiscard]] std::shared_ptr<const OwnedRowImage> solver_result_row(const StoredSolverResult&);
[[nodiscard]] StoredSolverResult decode_solver_result_row(const OwnedRowImage&);
[[nodiscard]] OwnedRowHandler solver_result_row_handler();
} // namespace qcae::ipc::detail
