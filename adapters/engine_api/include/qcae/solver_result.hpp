#pragma once
#include "qcae/analysis_input.hpp"
#include "qcae/artifacts_local.hpp"
#include "qcae/nastran_static_result.hpp"
#include "qcae/solver_version.hpp"
#include "qcae/solver_run_record.hpp"
#include "qcae/solver_result_reader.hpp"

namespace qcae::ipc {
// Immutable parsed fields and their original process/input provenance. Numerical
// validation is deliberately absent: parsed fields never imply a scenario passed.
struct SolverParsedResult {
    // Uses the existing complete owned-run codec rather than a second provenance validator.
    SolverOwnedRun origin;
    SolverResultReaderConfiguration reader;
    std::vector<ArtifactFileDigest> raw_outputs;
    NastranStaticResult fields;
};
struct StoredSolverResult {
    SolverParsedResult parsed;
    LocalArtifactIntent artifact;
    // Filesystem completion and database publication remain separate facts.
    bool published{};
};
[[nodiscard]] std::string encode_solver_parsed_result(const SolverParsedResult&);
[[nodiscard]] SolverParsedResult decode_solver_parsed_result(std::string_view);
} // namespace qcae::ipc
