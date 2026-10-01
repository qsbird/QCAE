#pragma once
#include <cstdint>
#include <string>

namespace qcae::ipc {
struct SolverResultReaderConfiguration {
    std::string reader_version{"qcae.nastran.static-f06.v1"}, resource;
    std::uint64_t subcase{1};
    std::string unit_system{"mm-N-MPa"}, coordinate_basis{"basic"};
    bool operator==(const SolverResultReaderConfiguration&) const = default;
};
} // namespace qcae::ipc
