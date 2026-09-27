#include "qcae/record_application.hpp"
#include "qcae/records.hpp"
#include "qcae/records_model_bridge.hpp"
#include <stdexcept>

using namespace qcae;
namespace {
void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
template <class T> T good(Result<T> value) {
    if (!value.ok())
        throw std::runtime_error(value.error ? value.error->message : "Missing result");
    return std::move(*value.value);
}
struct Store final : IRecordStore {
    std::uint64_t generation{};
    std::map<StoreKey, SharedStoreBytes> rows;
    StoreBatch last;
    enum class Fault { none, before, after } fault{};
    LoadedRows load_rows() override {
        LoadedRows result;
        result.generation = generation;
        for (const auto& [key, value] : rows)
            result.rows.push_back({key, value});
        return result;
    }
    BatchReceipt commit_rows(const StoreBatch& batch) override {
        check(batch.expected_generation == generation, "Store generation mismatch");
        if (fault == Fault::before) {
            fault = Fault::none;
            throw StorageError("Before commit");
        }
        auto next = rows;
        for (const auto& row : batch.mutations)
            if (row.after)
                next[row.key] = row.after;
            else
                next.erase(row.key);
        auto cached = batch;
        rows.swap(next);
        last = std::move(cached);
        ++generation;
        if (fault == Fault::after) {
            fault = Fault::none;
            throw StorageError("After commit", true);
        }
        return {generation, batch.mutations.size(), 0};
    }
};
WriteContext at(const DocumentInfo& info) {
    return {info.document, info.revision};
}
RecordApplicationOptions options(const std::shared_ptr<Store>& store) {
    RecordApplicationOptions result;
    result.registry = make_record_registry();
    result.records = store;
    return result;
}
Result<RecordPreparedOperation> change_modulus(const DocumentView& view, double value) {
    EditSession edit(view);
    edit.update<records::Material>(EntityId("material"),
                                   [&](auto& material) { material.young_modulus_mpa = value; });
    return {Status::success,
            RecordPreparedOperation{
                edit.prepare(), "Set modulus", EntityId("material"), "modulus", value, false},
            {}};
}
void bounded_commit_and_recovery() {
    auto store = std::make_shared<Store>();
    Caller caller{"record-tests"};
    DocumentInfo info;
    {
        RecordApplication app(options(store));
        info = good(app.create_document(caller, "Records", "create"));
        auto preview = good(app.preview(
            caller,
            at(info),
            [](const DocumentView& view,
               const RecordIdentityAllocator&) -> Result<RecordPreparedOperation> {
                EditSession edit(view);
                edit.put(records::Material{EntityId("material"), "Steel", 210000, .3});
                for (int i = 0; i < 2048; ++i)
                    edit.put(records::Node{
                        EntityId("node-" + std::to_string(i)), {double(i), 0, 0}, std::nullopt});
                return {Status::success,
                        RecordPreparedOperation{
                            edit.prepare(), "Seed", EntityId("material"), "seed", 0, false},
                        {}};
            }));
        good(app.commit(caller, at(info), preview.id, "seed"));
        info = good(app.current_document());
        auto before = good(app.snapshot(info.document));
        const auto start = app.stats();
        const auto activity_start = record_activity_counters();
        preview = good(app.preview(
            caller, at(info), [](const DocumentView& view, const RecordIdentityAllocator&) {
                return change_modulus(view, 70000);
            }));
        auto receipt = good(app.commit(caller, at(info), preview.id, "modulus"));
        auto after = good(app.snapshot(info.document));
        const auto end = app.stats();
        const auto activity_end = record_activity_counters();
        check(activity_end.whole_model_serializations ==
                      activity_start.whole_model_serializations &&
                  activity_end.whole_model_materializations ==
                      activity_start.whole_model_materializations,
              "Uninstrumented whole-model work in local commit");
        check(after.records.find<records::Node>(EntityId("node-1024")) ==
                  before.records.find<records::Node>(EntityId("node-1024")),
              "Unchanged record allocation was copied");
        check(end.whole_model_serializations == start.whole_model_serializations &&
                  end.whole_model_materializations == start.whole_model_materializations,
              "Local commit serialized/materialized whole model");
        check(end.model_bytes_copied - start.model_bytes_copied < 4096 &&
                  end.model_bytes_encoded - start.model_bytes_encoded < 4096,
              "Local record work grew with whole document");
        std::size_t document_rows{}, history_rows{};
        for (const auto& mutation : store->last.mutations) {
            if (mutation.key.space == StoreSpace::document_record) {
                ++document_rows;
                check(mutation.after.get() ==
                          &after.records.find<records::Material>(EntityId("material"))->encoded(),
                      "Store copied cached record bytes");
            }
            if (mutation.key.space == StoreSpace::history_entry)
                ++history_rows;
        }
        check(document_rows == 1 && history_rows == 1 && store->last.mutations.size() == 4,
              "Local batch rewrote unrelated state");
        info = good(app.current_document());
        good(app.undo(caller, at(info), "undo"));
        info = good(app.current_document());
        auto generation = store->generation;
        auto replay = good(app.commit(caller, preview.context, preview.id, "modulus"));
        check(replay.replayed && replay.transaction == receipt.transaction &&
                  replay.current_revision == info.revision && store->generation == generation,
              "Replay after undo changed authority");
    }
    {
        RecordApplication app(options(store));
        check(app.recovery_available(), "Recovery not exposed");
        info = good(app.recover_document(caller, "recover"));
        check(good(app.snapshot(info.document))
                      .records.find<records::Material>(EntityId("material"))
                      ->get<records::Material>()
                      .young_modulus_mpa == 210000,
              "Recovery lost undone content");
        good(app.redo(caller, at(info), "redo"));
        info = good(app.current_document());
        check(good(app.snapshot(info.document))
                      .records.find<records::Material>(EntityId("material"))
                      ->get<records::Material>()
                      .young_modulus_mpa == 70000,
              "Redo history not persisted");
        auto preview = good(app.preview(
            caller, at(info), [](const DocumentView& view, const RecordIdentityAllocator&) {
                return change_modulus(view, 90000);
            }));
        store->fault = Store::Fault::before;
        check(!app.commit(caller, at(info), preview.id, "fail").ok(), "Definite failure accepted");
        check(good(app.current_document()).revision == info.revision, "Definite failure published");
        store->fault = Store::Fault::after;
        auto uncertain = app.commit(caller, at(info), preview.id, "fail");
        check(!uncertain.ok() && uncertain.error->code == ErrorCode::storage_uncertain,
              "Uncertain failure not poisoned");
        info = good(app.recover_document(caller, "recover-after-fault"));
        check(good(app.snapshot(info.document))
                      .records.find<records::Material>(EntityId("material"))
                      ->get<records::Material>()
                      .young_modulus_mpa == 90000,
              "Durable commit lost across publication gap");
    }
}
} // namespace
int main() {
    bounded_commit_and_recovery();
}
