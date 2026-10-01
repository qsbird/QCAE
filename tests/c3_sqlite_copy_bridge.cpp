#include "c3_sqlite_copy_bridge.h"
#include "qcae/operation_ledger.hpp"

#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <map>
#include <mutex>
#if defined(__APPLE__)
#include <malloc/malloc.h>
#elif defined(__linux__)
#include <malloc.h>
#endif

namespace {
struct CopyProfile {
    std::size_t bytes{}, calls{};
};
std::mutex profile_mutex;
std::map<std::pair<const char*, bool>, CopyProfile> profile;
void record(std::size_t bytes, const char* function, bool aggregate) {
    const auto active = qcae::ledger::current();
    if (!active)
        return;
    active->add(
        qcae::ledger::Stage::sqlite, qcae::ledger::Metric::driver_internal_copy_bytes, bytes);
    active->add(qcae::ledger::Stage::sqlite,
                aggregate ? qcae::ledger::Metric::driver_aggregate_copy_bytes
                          : qcae::ledger::Metric::driver_explicit_copy_bytes,
                bytes);
    try {
        std::lock_guard lock(profile_mutex);
        auto& observed = profile[{function, aggregate}];
        observed.bytes += bytes;
        ++observed.calls;
    } catch (...) {
        // Observation must not change SQLite allocation/failure behavior.
        // A lost attribution makes completeness unknown, never falsely zero.
        active->unknown(qcae::ledger::Stage::sqlite,
                        qcae::ledger::Metric::driver_internal_copy_bytes);
    }
}
} // namespace

extern "C" int sqlite3_libversion_number(void);
extern "C" int qcae_c3_sqlite_copy_instrumentation_version(void);

extern "C" void* qcae_c3_sqlite_memcpy(void* destination, const void* source, size_t bytes) {
    record(bytes, "bridge.explicit", false);
    return std::memcpy(destination, source, bytes);
}
extern "C" void* qcae_c3_sqlite_memmove(void* destination, const void* source, size_t bytes) {
    record(bytes, "bridge.explicit", false);
    return std::memmove(destination, source, bytes);
}
extern "C" void* qcae_c3_sqlite_memcpy_tagged(void* destination,
                                              const void* source,
                                              size_t bytes,
                                              const char* function) {
    record(bytes, function, false);
    return std::memcpy(destination, source, bytes);
}
extern "C" void* qcae_c3_sqlite_memmove_tagged(void* destination,
                                               const void* source,
                                               size_t bytes,
                                               const char* function) {
    record(bytes, function, false);
    return std::memmove(destination, source, bytes);
}
extern "C" void* qcae_c3_sqlite_memcpy_aggregate(void* destination,
                                                 const void* source,
                                                 size_t bytes,
                                                 const char* function) {
    record(bytes, function, true);
    return std::memcpy(destination, source, bytes);
}
extern "C" void* qcae_c3_sqlite_realloc(void* allocation, size_t bytes, const char* function) {
    const auto previous_address = reinterpret_cast<std::uintptr_t>(allocation);
    std::size_t previous_size{};
#if defined(__APPLE__)
    if (allocation)
        previous_size = malloc_size(allocation);
#elif defined(__linux__)
    if (allocation)
        previous_size = malloc_usable_size(allocation);
#else
    qcae::ledger::unknown(qcae::ledger::Stage::sqlite,
                          qcae::ledger::Metric::driver_internal_copy_bytes);
#endif
    auto* result = std::realloc(allocation, bytes);
    if (result && previous_address &&
        reinterpret_cast<std::uintptr_t>(result) != previous_address) {
        // A successful relocation copies at most the old usable allocation,
        // capped by the new request. Includes allocator padding conservatively;
        // it is never treated as evidence of actual owned-payload size.
        const auto copied = std::min(previous_size, bytes);
        record(copied, function, false);
        qcae::ledger::add(
            qcae::ledger::Stage::sqlite, qcae::ledger::Metric::driver_allocator_copy_bytes, copied);
    }
    return result;
}
extern "C" void qcae_c3_sqlite_visit_copy_profile(void* context,
                                                  qcae_c3_sqlite_profile_visitor visitor) {
    std::lock_guard lock(profile_mutex);
    for (const auto& [key, value] : profile)
        visitor(context, key.first, value.bytes, value.calls, key.second);
}
extern "C" void qcae_c3_sqlite_encoded(size_t bytes) {
    qcae::ledger::add(qcae::ledger::Stage::sqlite, qcae::ledger::Metric::encoded_bytes, bytes);
}
extern "C" int qcae_c3_sqlite_observer_begin(void) {
    if (sqlite3_libversion_number() != 3051000 ||
        qcae_c3_sqlite_copy_instrumentation_version() != 3051000)
        return 0;
    {
        std::lock_guard lock(profile_mutex);
        profile.clear();
    }
    qcae::ledger::add(
        qcae::ledger::Stage::sqlite, qcae::ledger::Metric::driver_internal_copy_bytes, 0);
    qcae::ledger::add(qcae::ledger::Stage::sqlite, qcae::ledger::Metric::encoded_bytes, 0);
    qcae::ledger::add(
        qcae::ledger::Stage::sqlite, qcae::ledger::Metric::driver_explicit_copy_bytes, 0);
    qcae::ledger::add(
        qcae::ledger::Stage::sqlite, qcae::ledger::Metric::driver_aggregate_copy_bytes, 0);
    qcae::ledger::add(
        qcae::ledger::Stage::sqlite, qcae::ledger::Metric::driver_allocator_copy_bytes, 0);
    return 1;
}
