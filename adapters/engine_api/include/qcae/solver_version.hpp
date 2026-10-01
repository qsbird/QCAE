#pragma once
#include "qcae/solver_runner.hpp"
#include <filesystem>

namespace qcae::ipc {
struct SolverExecutableIdentity {
    std::uint64_t device{}, inode{}, byte_length{};
    std::int64_t modified_seconds{}, changed_seconds{};
    std::uint64_t modified_nanoseconds{}, changed_nanoseconds{};
    std::string sha256;
    bool operator==(const SolverExecutableIdentity&) const = default;
};
// Actual local information-command observation. Never a numerical/result acceptance.
struct SolverVersionEvidence {
    std::string protocol{"msc.help.v1"}, reported_version;
    SolverExecutableIdentity executable;
    SolverProcessIdentity process;
    std::uint64_t stdout_bytes{}, stderr_bytes{};
    std::string stdout_sha256, stderr_sha256;
    int exit_code{};
    bool synthetic{};
    bool operator==(const SolverVersionEvidence&) const = default;
};
[[nodiscard]] std::string encode_solver_version_evidence(const SolverVersionEvidence&);
[[nodiscard]] SolverVersionEvidence decode_solver_version_evidence(std::string_view);
[[nodiscard]] std::string solver_version_configuration_digest(std::string_view declaration_digest,
                                                              const SolverVersionEvidence&);
} // namespace qcae::ipc
