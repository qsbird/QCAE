#pragma once
#include "qcae/artifacts_local.hpp"
#include "qcae/solver_runner.hpp"
#include <functional>
#include <memory>

namespace qcae {
// All hooks are mandatory, trusted host dependencies. They must not re-enter the runner.
// persist is an atomic compare-and-swap against the exact expected fact, including absence.
// Success means durable commit, not a queued write. An uncertain commit must return failure;
// the runner then stops admission and never blindly executes the process again.
struct LocalSolverHost {
    std::function<Result<LocalArtifactIntent>(const SolverRunRequest&)> input_artifact;
    // Checks frozen analysis/export/map/profile facts and actual configuration compatibility.
    // LocalArtifactStore verifies bytes; that alone cannot certify these semantic relations.
    std::function<Result<bool>(const SolverRunRequest&, const LocalArtifactIntent&)> validate_start;
    std::function<Result<std::optional<SolverRunRecord>>(std::string_view run_id)> load;
    std::function<Result<bool>(const SolverRunRecord&,
                               const std::optional<SolverRunRecord>& expected)>
        persist;
};
struct LocalSolverLimits {
    std::size_t max_active_processes{2};
};
// A POSIX process adapter, independent of Qt and result parsers. Keep it alive until its jobs
// finish. Destruction neither reports cancellation nor kills unverified/recovered processes.
class LocalSolverRunner final : public ISolverRunner {
  public:
    explicit LocalSolverRunner(LocalSolverHost, LocalSolverLimits = {});
    ~LocalSolverRunner() override;
    LocalSolverRunner(const LocalSolverRunner&) = delete;
    LocalSolverRunner& operator=(const LocalSolverRunner&) = delete;
    Result<SolverRunRecord> start(const SolverRunRequest&) override;
    Result<SolverRunRecord> query(std::string_view run_id) override;
    Result<SolverCancelResult> cancel(std::string_view run_id) override;

  private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace qcae
