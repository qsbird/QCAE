#pragma once
#include "qcae/engine_contributions.hpp"
#include "qcae/solver_local.hpp"
#include "qcae/analysis_input.hpp"
#include "qcae/solver_version.hpp"
#include "qcae/solver_result_reader.hpp"

namespace qcae::ipc {
struct LocalSolverConfiguration {
    SolverRunConfiguration run;
    std::filesystem::path run_root;
    bool test_only{};
    // Set only by a trusted host after actual version verification, never read from a JSON flag.
    bool version_validated{};
    std::string declaration_digest;
    std::optional<SolverVersionEvidence> version_probe;
    std::string version_probe_failure;
    std::optional<SolverResultReaderConfiguration> result_reader;
};
// Trusted local configuration file only. No inherited environment, credentials or shell.
// Parsing a declaration does not certify an installed solver version.
[[nodiscard]] Result<LocalSolverConfiguration>
load_local_solver_configuration(const std::filesystem::path&);
using SolverArtifactResolver =
    std::function<Result<std::pair<features::analysis::FrozenAnalysisInput, LocalArtifactIntent>>(
        const Caller&, const DocumentRef&, std::string_view artifact_id)>;
class SolverCoordinator {
  public:
    SolverCoordinator(std::optional<LocalSolverConfiguration>, SolverArtifactResolver);
    ~SolverCoordinator();
    [[nodiscard]] TaskPublisher wrap_publisher(RecordApplication&, TaskPublisher);
    [[nodiscard]] Result<bool> register_operations(operations::OperationRegistry&,
                                                   RecordApplication&,
                                                   std::function<TaskService&()>);
    [[nodiscard]] static OwnedRowHandler row_handler();

  private:
    struct State;
    std::shared_ptr<State> state_;
};
// Operations and owned rows only; the composition host wraps the existing single publisher.
[[nodiscard]] EngineContribution
solver_engine_contribution(const std::shared_ptr<SolverCoordinator>&);
} // namespace qcae::ipc
