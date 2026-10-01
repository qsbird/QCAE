#pragma once
#include "qcae/types.hpp"
#include <string_view>
#include <vector>

namespace qcae {
// Configuration is supplied by the trusted local host, never inferred from a profile or PATH.
// argv excludes argv[0]. The initial adapter uses a fixed C locale and no inherited environment.
struct SolverRunConfiguration {
    std::string id, executable;
    std::vector<std::string> argv;
    std::string solver_family, dialect, solver_version, version_evidence, configuration_digest;
    std::vector<std::string> expected_outputs;
    std::uint64_t max_wall_time_ms{60000}, cancel_grace_ms{200}, max_output_bytes{16777216};
    bool operator==(const SolverRunConfiguration&) const = default;
};
struct SolverInputProvenance {
    DocumentId document;
    DocumentEpoch epoch;
    Revision revision{};
    EntityId analysis;
    ProfileRef profile;
    std::string artifact_id, manifest_sha256, input_fingerprint, export_identity_digest;
    std::string export_rule_version, result_reader_version;
    // Exact frozen input encoding supplied and checked by the application, not a mutable model.
    std::string frozen_analysis_input;
    bool operator==(const SolverInputProvenance&) const = default;
};
struct SolverRunRequest {
    std::string run_id, task_id, run_directory;
    SolverInputProvenance input;
    SolverRunConfiguration configuration;
    bool operator==(const SolverRunRequest&) const = default;
};
enum class SolverExecutionState {
    startup_intent,
    running,
    cancel_requested,
    exited,
    cancelled,
    launch_failed,
    outcome_unknown
};
enum class SolverParsingState { not_run, parsed, failed };
enum class SolverNumericalState { not_run, passed, failed };
enum class SolverOutputState { not_collected, collected, incomplete };
struct SolverProcessIdentity {
    std::int64_t pid{};
    // OS process creation identity; neither a wall-clock observation nor PID alone.
    std::string start_identity;
    bool operator==(const SolverProcessIdentity&) const = default;
};
struct SolverOutputFile {
    std::string path;
    std::uint64_t byte_length{};
    std::string sha256;
};
struct SolverRunRecord {
    SolverRunRequest request;
    std::uint64_t sequence{1};
    SolverExecutionState execution{SolverExecutionState::startup_intent};
    SolverParsingState parsing{SolverParsingState::not_run};
    SolverNumericalState numerical_validation{SolverNumericalState::not_run};
    SolverOutputState output_state{SolverOutputState::not_collected};
    bool cancellation_requested{};
    std::optional<SolverProcessIdentity> process;
    std::optional<int> exit_code, termination_signal;
    std::vector<SolverOutputFile> outputs;
    std::string detail;
};
struct SolverCancelResult {
    SolverRunRecord run;
    bool accepted{};
};
class ISolverRunner {
  public:
    virtual ~ISolverRunner() = default;
    virtual Result<SolverRunRecord> start(const SolverRunRequest&) = 0;
    virtual Result<SolverRunRecord> query(std::string_view run_id) = 0;
    virtual Result<SolverCancelResult> cancel(std::string_view run_id) = 0;
};
} // namespace qcae
