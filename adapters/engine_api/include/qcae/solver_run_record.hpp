#pragma once
#include "qcae/analysis_input.hpp"
#include "qcae/record_application.hpp"
#include "qcae/solver_runner.hpp"
#include "qcae/task_service.hpp"
#include "qcae/solver_version.hpp"
#include "qcae/artifacts_local.hpp"
#include "qcae/solver_result_reader.hpp"

namespace qcae::ipc {
// Owned execution facts, never a mutable engineering model or a result bundle.
struct SolverOwnedRun {
    std::string principal, idempotency_key, signature;
    features::analysis::FrozenAnalysisInput source_input;
    SolverRunRecord run;
    bool test_only{};
    // A queued snapshot is not a startup intent. Only the runner's durable admission sets this.
    bool startup_intent_persisted{};
    // v1 has no version observation. Only v2 binds a trusted startup probe.
    std::optional<SolverVersionEvidence> version_probe;
    std::string declaration_digest;
    // v3 freezes an explicit reader; v1/v2 retain unconfigured/not_run semantics.
    std::optional<SolverResultReaderConfiguration> result_reader;
    std::optional<LocalArtifactIntent> result_artifact;
    bool result_published{};
};
[[nodiscard]] std::string encode_solver_owned_run(const SolverOwnedRun&);
[[nodiscard]] std::string
solver_export_identity_digest(const features::analysis::FrozenAnalysisInput&);
[[nodiscard]] std::string solver_run_signature(const EntityId& analysis,
                                               std::string_view artifact_id,
                                               std::string_view configuration_id,
                                               std::string_view configuration_digest);
[[nodiscard]] SolverOwnedRun decode_solver_owned_run(const OwnedRowImage&);
[[nodiscard]] std::shared_ptr<const OwnedRowImage> solver_owned_row(const SolverOwnedRun&);
[[nodiscard]] OwnedRowHandler solver_run_row_handler();
void validate_solver_task(const SolverOwnedRun&, const TaskRecord&);
} // namespace qcae::ipc
