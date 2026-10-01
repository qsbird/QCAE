#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace qcae {

// Diagnostic data only. Calls count the named entry point; bytes can be a
// source-proven upper bound rather than an exact count of memcpy instructions.
struct SdkCopyObservation {
    std::string component;
    std::uint64_t calls{};
    std::optional<std::uint64_t> copy_bytes;
    std::string unsupported_reason;
};

struct SdkCopySnapshot {
    std::string run_id;
    std::vector<SdkCopyObservation> components;
};

} // namespace qcae
