#include "qcae/record_application.hpp"
#include "qcae/records.hpp"
#include "qcae/records_model_bridge.hpp"
#include <algorithm>
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
void prepared_candidate_and_preview_atomicity() {
    const auto store = std::make_shared<Store>();
    auto settings = options(store);
    settings.limits.max_previews = 1;
    RecordApplication app(std::move(settings));
    const Caller caller{"candidate-tests"};
    auto info = good(app.create_document(caller, "Candidate trust", "create"));
    auto seeded = good(app.execute(
        caller,
        at(info),
        "seed",
        "seed",
        [](const DocumentView& view, const RecordIdentityAllocator&) {
            EditSession edit(view);
            edit.put(records::Material{EntityId("material"), "Steel", 210000, .3});
            edit.put(records::Node{EntityId("node"), {1, 0, 0}, {}});
            return Result<RecordPreparedOperation>{
                Status::success,
                RecordPreparedOperation{
                    edit.prepare(), "Seed", EntityId("material"), "seed", 0, false},
                {}};
        },
        "seed"));
    (void)seeded;
    info = good(app.current_document());
    const auto generation = store->generation;
    const auto before_rows = store->rows;
    const auto before = good(app.snapshot(info.document));
    EntityId failed_id;
    const auto rejected = app.preview(
        caller, at(info), [&](const DocumentView& view, const RecordIdentityAllocator& allocate) {
            failed_id = allocate();
            EditSession edit(view);
            edit.update<records::Material>(EntityId("material"),
                                           [](auto& record) { record.young_modulus_mpa = 70000; });
            edit.update<records::Node>(EntityId("node"),
                                       [](auto& record) { record.position[0] = 2; });
            auto prepared = edit.prepare();
            prepared.changes.records.erase(
                std::remove_if(prepared.changes.records.begin(),
                               prepared.changes.records.end(),
                               [](const auto& change) { return change.key.identity == "node"; }),
                prepared.changes.records.end());
            return Result<RecordPreparedOperation>{
                Status::success,
                RecordPreparedOperation{
                    std::move(prepared), "Forged subset", EntityId("material"), "forged", 0, false},
                {}};
        });
    check(!rejected.ok(), "Unreported candidate modification accepted");
    const auto after = good(app.snapshot(info.document));
    check(good(app.current_document()).revision == info.revision &&
              store->generation == generation && store->rows == before_rows &&
              after.records.find_identity("node") == before.records.find_identity("node") &&
              after.records.find_identity("material") == before.records.find_identity("material"),
          "Rejected candidate changed model, database or history");
    EntityId next_id;
    const auto preview = good(app.preview(
        caller, at(info), [&](const DocumentView& view, const RecordIdentityAllocator& allocate) {
            next_id = allocate();
            return change_modulus(view, 70000);
        }));
    check(next_id == failed_id, "Rejected preview consumed an entity ID");
    good(app.commit(caller, at(info), preview.id, "valid"));
    check(good(app.snapshot(info.document))
                  .records.find<records::Node>(EntityId("node"))
                  ->get<records::Node>()
                  .position[0] == 1,
          "Valid subset commit published unreported node change");
}
void immutable_fact_balance_and_snapshot() {
    for (const bool reverse : {false, true}) {
        ImmutableFactMap<std::string> facts;
        for (int index = 0; index < 16384; ++index) {
            const auto value = reverse ? 16383 - index : index;
            auto key = std::to_string(value);
            key.insert(0, 5 - key.size(), '0');
            check(facts.emplace(key, "immutable-" + key).second,
                  "Unique operation fact insertion failed");
            check(facts.depth() <= 29, "Monotonic fact keys produced unbounded tree depth");
        }
        auto snapshot = facts;
        const auto* retained = &snapshot.find("01000")->second;
        facts.emplace("new-fact", "new-value");
        check(&facts.find("01000")->second == retained && snapshot.size() == 16384 &&
                  snapshot.find("new-fact") == snapshot.end(),
              "Fact insertion copied old payload or changed an old snapshot");
        facts.replace("01000", "replaced");
        check(snapshot.find("01000")->second == "immutable-01000" &&
                  facts.find("01000")->second == "replaced",
              "Fact replacement changed an immutable old snapshot");
        std::string previous;
        std::size_t count{};
        for (const auto& [key, unused] : snapshot) {
            (void)unused;
            check(previous.empty() || previous < key, "Immutable fact iteration lost ordering");
            previous = key;
            ++count;
        }
        check(count == snapshot.size(), "Immutable fact iteration lost entries");
    }
}
void persistent_wire_and_reserved_writer() {
    const auto store = std::make_shared<Store>();
    const auto settings = options(store);
    RecordApplication app(settings);
    const Caller caller{"wire-tests"};
    auto info =
        good(app.create_document(caller, "Persistent wire \xe6\xb5\x8b\xe8\xaf\x95", "create"));
    good(app.execute(
        caller,
        at(info),
        "seed",
        "seed",
        [](const DocumentView& view, const RecordIdentityAllocator&) {
            EditSession edit(view);
            edit.put(records::Material{EntityId("material"), "Steel", 210000, .3});
            return Result<RecordPreparedOperation>{
                Status::success,
                RecordPreparedOperation{
                    edit.prepare(), "Seed", EntityId("material"), "seed", 0, false},
                {}};
        },
        "seed"));
    for (int index = 0; index < 32; ++index) {
        info = good(app.current_document());
        const auto signature = std::string(120, 's') + std::string("\0end", 4);
        good(app.execute(
            caller,
            at(info),
            "set",
            signature,
            [&](const DocumentView& view, const RecordIdentityAllocator&) {
                auto result = change_modulus(view, 70000 + index);
                result.value->signature = signature;
                return result;
            },
            "set-" + std::to_string(index)));
    }
    auto state = decode_record_state_image(store->load_rows().rows, settings.registry);
    const auto number = [](std::string& bytes, std::uint64_t value) {
        for (unsigned index = 0; index < 8; ++index)
            bytes.push_back(static_cast<char>((value >> (8 * index)) & 255));
    };
    const auto text = [&](std::string& bytes, std::string_view value) {
        number(bytes, value.size());
        bytes.append(value);
    };
    const auto write_info = [&](std::string& bytes, const DocumentInfo& value) {
        text(bytes, value.document.id.value);
        text(bytes, value.document.epoch.value);
        number(bytes, value.revision);
        for (const auto* field : {&value.content_state,
                                  &value.name,
                                  &value.project_id,
                                  &value.saved_path,
                                  &value.saved_content_state})
            text(bytes, *field);
        number(bytes, value.material_count);
        number(bytes, value.dirty);
        number(bytes, value.durable);
    };
    for (const auto& [key, fact] : state.operations) {
        std::string expected;
        text(expected, "QCAE-OPERATION-FACT");
        number(expected, 1);
        text(expected, fact.signature);
        text(expected, fact.receipt.transaction.value);
        number(expected, fact.receipt.committed_revision);
        number(expected, fact.receipt.current_revision);
        text(expected, fact.receipt.current_content_state);
        text(expected, fact.receipt.primary_entity.value);
        check(fact.encoded && *fact.encoded == expected &&
                  store->rows.at({StoreSpace::operation_fact, key}) == fact.encoded,
              "Preencoding changed the v1 fact wire or replaced its cached payload");
        auto unencoded = fact;
        unencoded.encoded.reset();
        state.operations.replace(key, std::move(unencoded));
    }
    for (const auto& [key, fact] : state.host_operations) {
        auto unencoded = fact;
        unencoded.encoded.reset();
        state.host_operations.replace(key, std::move(unencoded));
    }
    for (const bool save_intent : {false, true}) {
        if (save_intent) {
            auto intent = std::make_shared<RecordSaveImage>();
            intent->host_key = "save-key";
            intent->signature.assign(90, 'g');
            intent->path = "destination.qcae";
            intent->token = "token";
            intent->project_id = "project";
            intent->snapshot.assign(512, 'p');
            intent->save_as = true;
            state.save_intent = std::move(intent);
        }
        std::string expected;
        text(expected, "QCAE-RECORD-WORKSPACE");
        number(expected, 1);
        text(expected, state.application_nonce);
        number(expected, state.next_id);
        number(expected, state.document.has_value());
        write_info(expected, *state.document);
        text(expected, state.initial_content_state);
        number(expected, state.history.size());
        for (const auto& history : state.history)
            text(expected, history->transaction.value);
        number(expected, state.cursor);
        number(expected, state.recoverable);
        number(expected, save_intent);
        if (save_intent) {
            const auto& intent = *state.save_intent;
            for (const auto* field : {&intent.host_key,
                                      &intent.signature,
                                      &intent.path,
                                      &intent.token,
                                      &intent.project_id,
                                      &intent.snapshot})
                text(expected, *field);
            number(expected, intent.save_as);
        }
        const auto operation = std::make_shared<ledger::OperationLedger>(
            ledger::Identity{"reserved-writer", {}, {}, 0});
        std::vector<StoredRow> encoded;
        {
            ledger::Scope scope(operation);
            encoded = encode_record_state_image(state);
        }
        std::uint64_t written{};
        for (const auto& row : encoded) {
            if (row.key.space == StoreSpace::document_metadata)
                check(*row.value == expected, "Reservation changed the existing metadata wire");
            if (row.key.space == StoreSpace::document_metadata ||
                row.key.space == StoreSpace::operation_fact ||
                row.key.space == StoreSpace::host_operation_fact)
                written += row.value->size();
        }
        const auto snapshot = operation->snapshot();
        const auto counted =
            snapshot.values[static_cast<std::size_t>(ledger::Stage::application)]
                           [static_cast<std::size_t>(ledger::Metric::metadata_copy_bytes)];
        check(counted && *counted == written,
              "Reserved writers must count every output byte and avoid old-prefix growth copies");
    }
}
void bounded_owned_row_prefix_reads() {
    auto store = std::make_shared<Store>();
    auto config = options(store);
    for (const auto space : {StoreSpace::artifact_record, StoreSpace::task_record})
        for (const auto* owner : {"prefix-owner", "second-owner"})
            config.owned_row_handlers.push_back(
                {space, owner, [](const OwnedRowImage&) {}, {}, {}});
    RecordApplication application(std::move(config));
    const Caller caller{"prefix-tests"};
    const auto info = good(application.create_document(caller, "Indexed facts", "create"));
    std::vector<OwnedRowUpdate> updates;
    const auto payload = std::make_shared<const std::string>(1024, 'p');
    auto add = [&](StoreSpace space, std::string id, std::string owner) {
        auto row = std::make_shared<const OwnedRowImage>(
            OwnedRowImage{{space, std::move(id)}, std::move(owner), 1, payload, {}});
        updates.push_back({row->key, {}, row});
    };
    for (unsigned index = 0; index < 40; ++index) {
        const auto suffix = std::to_string(100 + index);
        add(StoreSpace::artifact_record,
            "validation-run/" + suffix,
            index % 2 ? "second-owner" : "prefix-owner");
        add(StoreSpace::task_record, "validation-run/" + suffix, "prefix-owner");
    }
    add(StoreSpace::artifact_record, "validation-other/100", "prefix-owner");
    add(StoreSpace::artifact_record, "validation-run2/100", "prefix-owner");
    good(application.update_owned_rows(caller, info.document, updates));
    const auto page = good(application.owned_rows_with_prefix(
        info.document, StoreSpace::artifact_record, "validation-run/", 32));
    check(page.rows.size() == 32 && page.overflow,
          "An indexed prefix query must stop at its limit and expose overflow");
    for (std::size_t index = 0; index < page.rows.size(); ++index)
        check(page.rows[index]->key.identity == "validation-run/" + std::to_string(100 + index) &&
                  page.rows[index]->payload == payload,
              "Prefix pages include different owners in key order without copying payloads");
    const auto exact = good(application.owned_rows_with_prefix(
        info.document, StoreSpace::artifact_record, "validation-run/139", 1));
    check(exact.rows.size() == 1 && !exact.overflow,
          "An exact one-row prefix cannot report an unrelated following row as overflow");
    const auto empty = good(application.owned_rows_with_prefix(
        info.document, StoreSpace::artifact_record, "validation-absent/", 1));
    check(empty.rows.empty() && !empty.overflow, "A missing prefix returns a complete empty page");
    const auto other_space = good(application.owned_rows_with_prefix(
        info.document, StoreSpace::task_record, "validation-run/", 1));
    check(other_space.rows.size() == 1 && other_space.overflow &&
              other_space.rows[0]->key.space == StoreSpace::task_record,
          "Prefix bounds keep storage spaces separate");
    for (const auto& prefix : {std::string{}, std::string(257, 'x'), std::string("a\0b", 3)})
        check(!application
                   .owned_rows_with_prefix(info.document, StoreSpace::artifact_record, prefix, 1)
                   .ok(),
              "Unbounded or malformed prefix input must fail");
    for (const auto limit : {std::size_t(0), std::size_t(33)})
        check(!application
                   .owned_rows_with_prefix(
                       info.document, StoreSpace::artifact_record, "validation-run/", limit)
                   .ok(),
              "Prefix page limits must stay within the explicit bound");
    auto stale = info.document;
    stale.epoch = DocumentEpoch("old-epoch");
    check(!application
               .owned_rows_with_prefix(stale, StoreSpace::artifact_record, "validation-run/", 1)
               .ok(),
          "Indexed owned facts retain the document epoch fence");
    check(good(application.current_document()).revision == info.revision &&
              good(application.history(info.document)).items.empty(),
          "Auxiliary publication and bounded reads do not add model history");
}
void owned_row_revision_fences() {
    auto store = std::make_shared<Store>();
    auto config = options(store);
    config.owned_row_handlers.push_back(
        {StoreSpace::artifact_record, "fenced-facts", [](const OwnedRowImage&) {}, {}, {}});
    RecordApplication application(std::move(config));
    const Caller caller{"fenced-publisher"};
    const auto initial = good(application.create_document(caller, "Fenced facts", "create"));
    auto row = [](const char* identity) {
        return std::make_shared<const OwnedRowImage>(
            OwnedRowImage{{StoreSpace::artifact_record, identity},
                          "fenced-facts",
                          1,
                          std::make_shared<const std::string>("immutable report"),
                          {}});
    };
    const auto first = row("fact-1");
    const std::array first_update{OwnedRowUpdate{first->key, {}, first}};
    good(application.update_owned_rows(caller, at(initial), first_update));
    good(application.execute(
        caller,
        at(initial),
        "material.create",
        "create",
        [](const DocumentView& view, const RecordIdentityAllocator&) {
            EditSession edit(view);
            edit.put(records::Material{EntityId("material"), "Steel", 210000, .3});
            return Result<RecordPreparedOperation>{
                Status::success,
                RecordPreparedOperation{
                    edit.prepare(), "Create material", EntityId("material"), "create", 0, false},
                {}};
        },
        "physical-change"));
    const auto current = good(application.current_document());
    const auto second = row("fact-2");
    const std::array second_update{OwnedRowUpdate{second->key, {}, second}};
    const auto generation = store->generation;
    const auto stale = application.update_owned_rows(caller, at(initial), second_update);
    check(stale.status == Status::conflict && stale.error->code == ErrorCode::revision_conflict &&
              stale.error->field == "expected_revision" && store->generation == generation,
          "Revision admission and auxiliary CAS share one lock before any persistence");
    check(
        good(application.owned_rows(current.document, StoreSpace::artifact_record, "fenced-facts"))
                .size() == 1,
        "A rejected old revision cannot publish a report");
    auto expired = at(current);
    expired.document.epoch = DocumentEpoch("expired");
    check(application.update_owned_rows(caller, expired, second_update).status ==
                  Status::conflict &&
              application.update_owned_rows(Caller{}, at(current), second_update).status ==
                  Status::needs_input &&
              store->generation == generation,
          "The fenced overload preserves epoch and trusted caller rejection");
    good(application.update_owned_rows(caller, at(current), second_update));
    const auto committed_generation = store->generation;
    check(application.update_owned_rows(caller, at(current), second_update).status ==
                  Status::conflict &&
              store->generation == committed_generation,
          "A correct revision still cannot bypass immutable row CAS");
    const auto third = row("fact-3");
    const std::array third_update{OwnedRowUpdate{third->key, {}, third}};
    good(application.update_owned_rows(caller, current.document, third_update));
    check(
        good(application.current_document()).revision == current.revision &&
            good(application.history(current.document)).items.size() == 1 &&
            good(application.owned_rows(
                     current.document, StoreSpace::artifact_record, "fenced-facts"))
                    .size() == 3,
        "Legacy task publication stays document fenced and auxiliary writes add no model history");
}
} // namespace
int main() {
    bounded_commit_and_recovery();
    prepared_candidate_and_preview_atomicity();
    immutable_fact_balance_and_snapshot();
    persistent_wire_and_reserved_writer();
    bounded_owned_row_prefix_reads();
    owned_row_revision_fences();
}
