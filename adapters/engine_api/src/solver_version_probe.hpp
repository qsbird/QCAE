#pragma once
#include "qcae/solver_version.hpp"

namespace qcae::ipc::detail {
struct SolverVersionProbeLimits {
    std::uint64_t wall_time_ms{2000}, max_output_bytes{65536}, max_executable_bytes{134217728};
};
[[nodiscard]] Result<SolverExecutableIdentity>
solver_executable_identity(const std::filesystem::path&, std::uint64_t max_bytes = 134217728);
// Fixed argv [help], fixed C locale, private cwd, no inherited environment or shell.
[[nodiscard]] Result<SolverVersionEvidence>
probe_solver_version(const std::filesystem::path& executable,
                     const std::filesystem::path& private_root,
                     std::string_view expected_version,
                     const SolverVersionProbeLimits& = {});
[[nodiscard]] bool solver_version_allows_execution(const SolverVersionEvidence&,
                                                   std::string_view expected_version);
} // namespace qcae::ipc::detail
