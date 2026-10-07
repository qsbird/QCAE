#pragma once
#include "qcae/solver_runner.hpp"
#include <string_view>

namespace qcae::ipc::detail {
inline bool mystran_backend(const SolverRunConfiguration& config) {
    return config.solver_family == "MYSTRAN" && config.dialect == "MYSTRAN" &&
           config.solver_version == "19.0.0";
}
inline bool supported_solver_backend(const SolverRunConfiguration& config) {
    return mystran_backend(config) ||
           (config.solver_family == "Nastran" && config.dialect == "MSC" &&
            (config.solver_version == "2022.1" || config.solver_version == "2024.1"));
}
inline std::string_view solver_backend_version_protocol(const SolverRunConfiguration& config) {
    return mystran_backend(config) ? "mystran.version.v1" : "msc.help.v1";
}
} // namespace qcae::ipc::detail
