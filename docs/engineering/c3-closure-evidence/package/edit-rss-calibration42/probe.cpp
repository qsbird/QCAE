#include "c3_process_observation.hpp"
#include <iostream>
#include <memory>

int main() {
  constexpr std::size_t allocation = 32 * 1024 * 1024;
  rusage before{}, after{};
  if (getrusage(RUSAGE_SELF, &before) != 0)
    return 1;
  auto pages = std::make_unique<unsigned char[]>(allocation);
  volatile unsigned char *touched = pages.get();
  for (std::size_t i = 0; i < allocation; i += 4096)
    touched[i] = 1;
  touched[allocation - 1] = 1;
  if (getrusage(RUSAGE_SELF, &after) != 0)
    return 2;
  const auto observed = c3_process_observation::peak_rss();
  std::cout << "{\"allocation_bytes\":" << allocation
            << ",\"raw_before\":" << before.ru_maxrss
            << ",\"raw_after\":" << after.ru_maxrss
            << ",\"helper_after_bytes\":" << observed.bytes.value_or(0)
            << ",\"pid\":" << observed.process_id << "}\n";
  if (!observed.bytes ||
      observed.process_id != static_cast<std::uint64_t>(getpid()))
    return 3;
#if defined(__APPLE__)
  if (*observed.bytes != static_cast<std::uint64_t>(after.ru_maxrss) ||
      after.ru_maxrss - before.ru_maxrss < 16 * 1024 * 1024)
    return 4;
#else
  return 5; // This calibration is expressly for the actual macOS host.
#endif
  return 0;
}
