#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace qcae::ledger {

// Measurement is observational. No ledger counter changes document semantics.
enum class Stage : std::size_t {
    command,
    application,
    records,
    sqlite,
    projection,
    render_encode,
    resource_publish,
    socket_send,
    socket_receive,
    resource_decode,
    render_decode,
    vtk_apply,
    vtk_complete,
    count
};
enum class Metric : std::size_t {
    model_copy_bytes,
    metadata_copy_bytes,
    encoded_bytes,
    decoded_bytes,
    batch_payload_bytes,
    batch_key_bytes,
    batch_delete_count,
    driver_bind_copy_bytes,
    socket_bytes,
    changed_records,
    full_model_serializations,
    full_model_materializations,
    scanned_records,
    sql_fullscan_steps,
    sql_vm_steps,
    sqlite_cache_page_writes,
    physical_wal_write_bytes,
    physical_database_write_bytes,
    array_write_bytes,
    array_invalidated_bytes,
    library_internal_copy_bytes,
    driver_internal_copy_bytes,
    driver_explicit_copy_bytes,
    driver_aggregate_copy_bytes,
    driver_allocator_copy_bytes,
    json_object_copy_bytes,
    reference_descriptor_copy_bytes,
    gpu_upload_bytes,
    count
};
inline constexpr std::array<std::string_view, static_cast<std::size_t>(Stage::count)> stage_names{
    "command",
    "application",
    "records",
    "sqlite",
    "projection",
    "render_encode",
    "resource_publish",
    "socket_send",
    "socket_receive",
    "resource_decode",
    "render_decode",
    "vtk_apply",
    "vtk_complete"};
inline constexpr std::array<std::string_view, static_cast<std::size_t>(Metric::count)> metric_names{
    "model_copy_bytes",
    "metadata_copy_bytes",
    "encoded_bytes",
    "decoded_bytes",
    "batch_payload_bytes",
    "batch_key_bytes",
    "batch_delete_count",
    "driver_bind_copy_bytes",
    "socket_bytes",
    "changed_records",
    "full_model_serializations",
    "full_model_materializations",
    "scanned_records",
    "sql_fullscan_steps",
    "sql_vm_steps",
    "sqlite_cache_page_writes",
    "physical_wal_write_bytes",
    "physical_database_write_bytes",
    "array_write_bytes",
    "array_invalidated_bytes",
    "library_internal_copy_bytes",
    "driver_internal_copy_bytes",
    "driver_explicit_copy_bytes",
    "driver_aggregate_copy_bytes",
    "driver_allocator_copy_bytes",
    "json_object_copy_bytes",
    "reference_descriptor_copy_bytes",
    "gpu_upload_bytes"};

struct Identity {
    std::string run_id;
    std::string document_id;
    std::string document_epoch;
    std::uint64_t base_revision{};
};
struct FrameTrace {
    Stage stage{};
    std::uint64_t bytes{};
    std::string kind, operation, request_id, event;
};
struct Snapshot {
    Identity identity;
    std::array<bool, static_cast<std::size_t>(Stage::count)> covered{};
    std::array<std::array<std::optional<std::uint64_t>, static_cast<std::size_t>(Metric::count)>,
               static_cast<std::size_t>(Stage::count)>
        values{};
    std::vector<FrameTrace> frames;
    bool frame_trace_complete{true};
};

class OperationLedger {
  public:
    explicit OperationLedger(Identity identity, std::string trigger_operation = {})
        : identity_(std::move(identity)), trigger_operation_(std::move(trigger_operation)) {}
    const Identity& identity() const noexcept {
        return identity_;
    }
    std::string_view trigger_operation() const noexcept {
        return trigger_operation_;
    }
    void cover(Stage stage) noexcept {
        covered_[static_cast<std::size_t>(stage)].store(true, std::memory_order_relaxed);
    }
    void add(Stage stage, Metric metric, std::uint64_t amount) noexcept {
        auto& cell = cells_[static_cast<std::size_t>(stage)][static_cast<std::size_t>(metric)];
        cell.value.fetch_add(amount, std::memory_order_relaxed);
        // An explicitly counted zero differs from an unaudited channel.
        unsigned expected = 0;
        cell.state.compare_exchange_strong(expected, 1, std::memory_order_relaxed);
    }
    void unknown(Stage stage, Metric metric) noexcept {
        cells_[static_cast<std::size_t>(stage)][static_cast<std::size_t>(metric)].state.store(
            2, std::memory_order_relaxed);
    }
    void frame_trace_unknown() noexcept {
        frame_trace_complete_.store(false, std::memory_order_relaxed);
    }
    void frame(Stage stage,
               std::uint64_t bytes,
               std::string_view kind,
               std::string_view operation,
               std::string_view request_id,
               std::string_view event) noexcept {
        try {
            std::lock_guard lock(frame_mutex_);
            if (frames_.size() >= 4096 || requests_.size() >= 4096) {
                frame_trace_unknown();
                return;
            }
            if (!operation.empty() && !request_id.empty())
                requests_.insert_or_assign(std::string(request_id), std::string(operation));
            if (operation.empty() && !request_id.empty()) {
                const auto found = requests_.find(request_id);
                if (found != requests_.end())
                    operation = found->second;
            }
            frames_.push_back({stage,
                               bytes,
                               std::string(kind),
                               std::string(operation),
                               std::string(request_id),
                               std::string(event)});
        } catch (...) {
            // Diagnostics must not change the measured command's behavior.
            frame_trace_complete_.store(false, std::memory_order_relaxed);
        }
    }
    Snapshot snapshot() const {
        Snapshot result;
        result.identity = identity_;
        for (std::size_t stage = 0; stage < cells_.size(); ++stage) {
            result.covered[stage] = covered_[stage].load(std::memory_order_relaxed);
            for (std::size_t metric = 0; metric < cells_[stage].size(); ++metric) {
                const auto& cell = cells_[stage][metric];
                if (cell.state.load(std::memory_order_relaxed) == 1)
                    result.values[stage][metric] = cell.value.load(std::memory_order_relaxed);
            }
        }
        {
            std::lock_guard lock(frame_mutex_);
            result.frames = frames_;
            result.frame_trace_complete = frame_trace_complete_.load(std::memory_order_relaxed);
        }
        return result;
    }

  private:
    struct Cell {
        std::atomic<std::uint64_t> value{};
        std::atomic<unsigned> state{}; // 0: unmeasured, 1: measured, 2: unknown component.
    };
    Identity identity_;
    std::string trigger_operation_;
    mutable std::mutex frame_mutex_;
    std::vector<FrameTrace> frames_;
    std::map<std::string, std::string, std::less<>> requests_;
    std::atomic<bool> frame_trace_complete_{true};
    std::array<std::atomic<bool>, static_cast<std::size_t>(Stage::count)> covered_{};
    std::array<std::array<Cell, static_cast<std::size_t>(Metric::count)>,
               static_cast<std::size_t>(Stage::count)>
        cells_{};
};

inline std::shared_ptr<OperationLedger> process_ledger;
inline std::shared_ptr<OperationLedger> pending_ledger;
inline thread_local std::shared_ptr<OperationLedger> scoped_ledger;
// The trusted test host may keep a run active across queued callbacks/events.
inline void activate(std::shared_ptr<OperationLedger> ledger) noexcept {
    std::atomic_store_explicit(&process_ledger, std::move(ledger), std::memory_order_release);
}
inline void arm(std::shared_ptr<OperationLedger> ledger) noexcept {
    std::atomic_store_explicit(&pending_ledger, std::move(ledger), std::memory_order_release);
}
inline std::shared_ptr<OperationLedger> pending() noexcept {
    return std::atomic_load_explicit(&pending_ledger, std::memory_order_acquire);
}
inline bool activate_pending_for(std::string_view operation) noexcept {
    auto candidate = pending();
    if (!candidate || candidate->trigger_operation() != operation)
        return false;
    if (!std::atomic_compare_exchange_strong_explicit(&pending_ledger,
                                                      &candidate,
                                                      std::shared_ptr<OperationLedger>{},
                                                      std::memory_order_acq_rel,
                                                      std::memory_order_acquire))
        return false;
    activate(std::move(candidate));
    return true;
}
inline void activate_pending() noexcept {
    if (auto ledger = std::atomic_exchange_explicit(
            &pending_ledger, std::shared_ptr<OperationLedger>{}, std::memory_order_acq_rel))
        activate(std::move(ledger));
}
inline std::shared_ptr<OperationLedger> current() noexcept {
    return scoped_ledger ? scoped_ledger
                         : std::atomic_load_explicit(&process_ledger, std::memory_order_acquire);
}
class Scope {
  public:
    explicit Scope(std::shared_ptr<OperationLedger> ledger) noexcept
        : previous_(std::move(scoped_ledger)) {
        scoped_ledger = std::move(ledger);
    }
    ~Scope() {
        scoped_ledger = std::move(previous_);
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

  private:
    std::shared_ptr<OperationLedger> previous_;
};
inline void cover(Stage stage) noexcept {
    if (const auto ledger = current())
        ledger->cover(stage);
}
inline void add(Stage stage, Metric metric, std::uint64_t amount) noexcept {
    if (const auto ledger = current())
        ledger->add(stage, metric, amount);
}
inline void unknown(Stage stage, Metric metric) noexcept {
    if (const auto ledger = current())
        ledger->unknown(stage, metric);
}
inline void frame(Stage stage,
                  std::uint64_t bytes,
                  std::string_view kind,
                  std::string_view operation,
                  std::string_view request_id,
                  std::string_view event = {}) noexcept {
    if (const auto ledger = current())
        ledger->frame(stage, bytes, kind, operation, request_id, event);
}

} // namespace qcae::ledger
