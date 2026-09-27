#pragma once
#include "qcae/task_application.hpp"
#include "qcae/records.hpp"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <stdexcept>

namespace runtime_test {
using namespace qcae;
inline void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
template <class T> T good(Result<T> value) {
    if (!value.ok())
        throw std::runtime_error(value.error ? value.error->message : "Missing result");
    return std::move(*value.value);
}
struct Store final : IRecordStore {
    enum class Fault { none, before_model, after_model };
    std::atomic<Fault> fault{};
    std::atomic<unsigned> task_write_failure{};
    mutable std::mutex mutex;
    std::uint64_t generation{};
    std::map<StoreKey, SharedStoreBytes> rows;
    StoreBatch last;
    LoadedRows load_rows() override {
        std::lock_guard lock(mutex);
        LoadedRows result;
        result.generation = generation;
        for (const auto& [key, value] : rows)
            result.rows.push_back({key, value});
        return result;
    }
    BatchReceipt commit_rows(const StoreBatch& batch) override {
        std::lock_guard lock(mutex);
        check(batch.expected_generation == generation, "generation mismatch");
        const bool model =
            std::any_of(batch.mutations.begin(), batch.mutations.end(), [](const auto& row) {
                return row.key.space == StoreSpace::document_record;
            });
        const bool task =
            std::any_of(batch.mutations.begin(), batch.mutations.end(), [](const auto& row) {
                return row.key.space == StoreSpace::task_record;
            });
        if (task && task_write_failure.load() && task_write_failure.fetch_sub(1) == 1)
            throw StorageError("before task-state batch");
        const auto fail = model ? fault.exchange(Fault::none) : Fault::none;
        if (fail == Fault::before_model)
            throw StorageError("before model batch");
        auto next = rows;
        for (const auto& update : batch.mutations)
            if (update.after)
                next[update.key] = update.after;
            else
                next.erase(update.key);
        auto prepared = batch;
        rows.swap(next);
        last = std::move(prepared);
        ++generation;
        if (fail == Fault::after_model)
            throw StorageError("after model batch", true);
        return {generation, batch.mutations.size(), 0};
    }
};
struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    std::size_t arrivals{};
    bool released{};
    void arrive_and_wait() {
        std::unique_lock lock(mutex);
        ++arrivals;
        changed.notify_all();
        changed.wait(lock, [&] { return released; });
    }
    void wait_for(std::size_t count = 1) {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return arrivals >= count; });
    }
    void release() {
        std::lock_guard lock(mutex);
        released = true;
        changed.notify_all();
    }
};
inline const ProfileRef profile{"nastran", "1", "runtime-test-semantic-v1"};
inline const Caller caller{"runtime-tests"};
inline RecordApplicationOptions options(const std::shared_ptr<IRecordStore>& store) {
    RecordApplicationOptions value;
    value.registry = make_record_registry();
    value.records = store;
    value.owned_row_handlers.push_back(task_row_handler());
    return value;
}
inline TaskPublisher publisher(RecordApplication& app) {
    return record_task_publisher(app, [](const ProfileRef& value) { return value == profile; });
}
inline WriteContext at(const DocumentInfo& info) {
    return {info.document, info.revision};
}
inline std::string modulus_signature(double value) {
    return "material:" + std::to_string(value);
}
inline RecordPrepare material_change(double value, bool create = false) {
    return [value, create](const DocumentView& view,
                           const RecordIdentityAllocator&) -> Result<RecordPreparedOperation> {
        EditSession edit(view);
        if (create)
            edit.put(records::Material{EntityId("material"), "Steel", value, .3});
        else
            edit.update<records::Material>(
                EntityId("material"), [&](auto& material) { material.young_modulus_mpa = value; });
        return {Status::success,
                RecordPreparedOperation{edit.prepare(),
                                        "Material",
                                        EntityId("material"),
                                        modulus_signature(value),
                                        value,
                                        create},
                {}};
    };
}
inline void seed(RecordApplication& app) {
    auto info = good(app.create_document(caller, "Task tests", "create"));
    good(app.execute(caller,
                     at(info),
                     "test.material",
                     modulus_signature(210000),
                     material_change(210000, true),
                     "seed"));
}
inline TaskRequest material_task(RecordApplication& app,
                                 std::string key,
                                 double value,
                                 std::shared_ptr<Gate> gate = {}) {
    const auto info = good(app.current_document());
    const auto snapshot = good(app.snapshot(info.document));
    TaskRequest result;
    result.caller = caller;
    result.operation = "test.material.task";
    result.signature = modulus_signature(value);
    result.idempotency_key = std::move(key);
    result.input = {info.document, info.revision, profile};
    result.work = [base = snapshot.records, value, gate](
                      const TaskControl& control) -> std::shared_ptr<const TaskPayload> {
        if (gate)
            gate->arrive_and_wait();
        control.checkpoint();
        auto operation = good(material_change(value)(base, [] { return EntityId("unused"); }));
        return std::make_shared<const RecordTaskPayload>(std::move(operation));
    };
    return result;
}
} // namespace runtime_test
