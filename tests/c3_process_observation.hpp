#pragma once
#include <cstdint>
#include <limits>
#include <optional>
#include <sys/resource.h>
#include <unistd.h>

namespace c3_process_observation {
struct PeakRss {
    std::uint64_t process_id{};
    std::optional<std::uint64_t> bytes;
};
inline PeakRss peak_rss() noexcept {
    PeakRss result{static_cast<std::uint64_t>(getpid()), std::nullopt};
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0 || usage.ru_maxrss < 0)
        return result;
    const auto native = static_cast<std::uint64_t>(usage.ru_maxrss);
#if defined(__APPLE__)
    result.bytes = native;
#elif defined(__linux__)
    if (native <= std::numeric_limits<std::uint64_t>::max() / 1024)
        result.bytes = native * 1024;
#endif
    return result;
}
} // namespace c3_process_observation
