#include "runtime_test_support.hpp"
#include "qcae/event_stream.hpp"
#include "qcae/ipc_model.hpp"
#include "qcae/operation_ledger.hpp"
#include <QCoreApplication>
#include <QJsonDocument>
#include <iostream>

using namespace runtime_test;
namespace {
struct Projects final : IWorkspaceStore {
    std::map<std::string, StoredProject> projects;
    std::optional<StoredWorkspace> load() override {
        return {};
    }
    std::uint64_t commit(std::uint64_t, const std::string&) override {
        throw StorageError("Legacy commit is unavailable");
    }
    std::string acquire_project(const std::string& path) override {
        return path;
    }
    void release_projects_except(const std::string&) noexcept override {}
    StoredProject read_project(const std::string& path) override {
        const auto found = projects.find(path);
        if (found == projects.end())
            throw StorageError("project_not_found");
        return found->second;
    }
    void publish_project(const std::string& path, const StoredProject& project) override {
        projects[path] = project;
    }
};
QJsonObject request(const QString& operation,
                    const QString& instance = "engine-a",
                    std::uint64_t cursor = 0,
                    int limit = 64) {
    return {{"api_version", "1.1"},
            {"request_id", "request"},
            {"operation", operation},
            {"parameters",
             QJsonObject{{"engine_instance_id", instance},
                         {"after_sequence", QString::number(cursor)},
                         {"limit", QString::number(limit)}}}};
}
QJsonObject data(const QJsonObject& response) {
    check(response.value("status") == "success", "event operation success");
    return response.value("data").toObject();
}
QJsonObject with_summary(QJsonObject request) {
    auto parameters = request.value("parameters").toObject();
    parameters.insert("include_document_summary", true);
    request.insert("parameters", parameters);
    return request;
}
std::uint64_t sequence(const QJsonObject& event, const char* key = "sequence") {
    bool valid = false;
    const auto value = event.value(QLatin1String(key)).toString().toULongLong(&valid);
    check(valid, "sequence is an exact integer string");
    return value;
}
void change(RecordApplication& app, double value, const std::string& key) {
    const auto info = good(app.current_document());
    good(app.execute(
        caller, at(info), "test.material", modulus_signature(value), material_change(value), key));
}
void latest_summary_preserves_legacy_journal() {
    auto store = std::make_shared<Store>();
    auto config = options(store);
    config.projects = std::make_shared<Projects>();
    RecordApplication app(config);
    seed(app);
    ipc::EventStream stream(app, "engine-a");
    stream.refresh();
    const auto baseline = data(stream.dispatch(request("events.subscribe", ""), "legacy"));
    data(stream.dispatch(with_summary(request("events.subscribe", "")), "modern"));
    const auto cursor = sequence(baseline, "next_sequence");
    change(app, 180000, "summary-edit");
    good(app.undo(caller, at(good(app.current_document())), "summary-undo"));
    good(app.redo(caller, at(good(app.current_document())), "summary-redo"));
    const auto current = good(app.current_document());
    stream.refresh();
    const auto modern = stream.drain("modern");
    const auto legacy = stream.drain("legacy");
    check(modern.size() == 6 && legacy.size() == 6, "summary does not coalesce journal events");
    for (qsizetype index = 0; index < modern.size(); ++index) {
        auto event = modern[index].toObject();
        auto payload = event.value("data").toObject();
        check(payload.contains("document_summary") == (index == 4),
              "only the final current DocumentChanged has a proven summary");
        if (index == 4) {
            const auto summary = payload.value("document_summary").toObject();
            check(summary == ipc::info_json(current) &&
                      summary.value("revision") == event.value("revision") &&
                      summary.value("document_id") == event.value("document_id") &&
                      summary.value("document_epoch") == event.value("document_epoch"),
                  "summary is the exact authoritative current info, not relabeled history");
            payload.remove("document_summary");
            event.insert("data", payload);
        }
        check(event == legacy[index].toObject() && sequence(event) == cursor + index + 1,
              "legacy payload, sequence and ordered document/history pairs are unchanged");
    }
    check(data(stream.dispatch(with_summary(request("events.read", "engine-a", cursor)), {}))
                      .value("events")
                      .toArray() == modern &&
              data(stream.dispatch(request("events.read", "engine-a", cursor), {}))
                      .value("events")
                      .toArray() == legacy,
          "read and subscriptions apply the same explicit summary selection");
    for (const QJsonValue& value : {QJsonValue("true"), QJsonValue(1), QJsonValue(QJsonArray{})}) {
        auto invalid = request("events.read");
        auto parameters = invalid.value("parameters").toObject();
        parameters.insert("include_document_summary", value);
        invalid.insert("parameters", parameters);
        check(stream.dispatch(invalid, {}).value("error").toObject().value("code") ==
                  "INVALID_INPUT",
              "summary opt-in requires a boolean");
    }
    good(app.save_document(caller, at(current), "/summary.qcae", false, "summary-save"));
    stream.refresh();
    const auto metadata = stream.drain("modern");
    check(metadata.size() == 1 &&
              metadata[0].toObject().value("event") == "ProjectMetadataChanged" &&
              !metadata[0].toObject().value("data").toObject().contains("document_summary"),
          "same-revision metadata keeps the authoritative query fallback");

    ipc::EventStream bounded(app, "engine-a", {2, 2});
    bounded.refresh();
    data(bounded.dispatch(request("events.subscribe", ""), "legacy"));
    data(bounded.dispatch(with_summary(request("events.subscribe", "")), "modern"));
    change(app, 170000, "summary-overflow-one");
    change(app, 160000, "summary-overflow-two");
    bounded.refresh();
    check(bounded.drain("modern")[0].toObject().value("frame_type") == "event_gap" &&
              bounded.drain("legacy")[0].toObject().value("frame_type") == "event_gap",
          "summary subscribers retain ordinary gap behavior");
    change(app, 150000, "summary-after-gap");
    bounded.refresh();
    const auto resumed = bounded.drain("modern");
    const auto old = bounded.drain("legacy");
    check(resumed.size() == 2 && old.size() == 2 &&
              resumed[0].toObject().value("data").toObject().contains("document_summary") &&
              !old[0].toObject().value("data").toObject().contains("document_summary"),
          "a gap advances the cursor without forgetting negotiated summary selection");
}
void canonical_wire_cache_is_shared_and_bounded() {
    RecordApplication app(options(std::make_shared<Store>()));
    seed(app);
    ipc::EventStream stream(app, "engine-a", {2, 2});
    stream.refresh();
    data(stream.dispatch(with_summary(request("events.subscribe", "")), "modern"));
    data(stream.dispatch(request("events.subscribe", ""), "legacy"));
    change(app, 180000, "cache-one");
    const auto stats_before = stream.stats();
    const auto observation =
        std::make_shared<ledger::OperationLedger>(ledger::Identity{"event-cache", {}, {}, 0});
    ledger::activate(observation);
    stream.refresh();
    const auto modern = stream.drain("modern");
    const auto legacy = stream.drain("legacy");
    const auto before_cache = observation->snapshot();
    std::uint64_t logical_encoded{};
    for (const auto& value : modern) {
        const auto event = value.toObject();
        const auto encoded = stream.encoded_frame(event);
        const auto shared = stream.encoded_frame(event);
        check(encoded && shared &&
                  *encoded == QJsonDocument(event).toJson(QJsonDocument::Compact) + '\n' &&
                  encoded->constData() == shared->constData(),
              "Canonical events borrow the exact immutable encoded frame");
        logical_encoded += encoded->size() - 1;
        auto changed = event;
        changed.insert("revision", "999");
        check(!stream.encoded_frame(changed), "Modified fields cannot reuse canonical wire bytes");
    }
    const auto after_cache = observation->snapshot();
    const auto stage = static_cast<std::size_t>(ledger::Stage::socket_send);
    check(before_cache.values[stage] == after_cache.values[stage],
          "Cache lookup must not fabricate a new encoding or owned payload copy");
    ledger::activate({});
    check(stream.stats().metadata_bytes_encoded - stats_before.metadata_bytes_encoded ==
              logical_encoded,
          "The event encoded-statistic preserves its original logical JSON size");
    check(!stream.encoded_frame(legacy[0].toObject()) &&
              stream.encoded_frame(legacy[1].toObject()).has_value(),
          "Legacy stripped summary falls back while unchanged history still borrows wire");
    const auto retained_event = modern[0].toObject();
    auto detached = *stream.encoded_frame(retained_event);
    const auto original = detached;
    detached.append('x');
    check(*stream.encoded_frame(retained_event) == original,
          "Caller mutation cannot overwrite cached immutable bytes");
    change(app, 170000, "cache-two");
    stream.refresh();
    check(!stream.encoded_frame(retained_event), "Expired retained events cannot borrow cache");
    check(!stream.encoded_frame(QJsonObject{{"frame_type", "event_gap"}, {"sequence", "3"}}),
          "Gaps always use normal encoding");
    for (const auto text : {"", "+1", "-1", " 1", "18446744073709551616"}) {
        auto invalid = request("events.read");
        auto parameters = invalid.value("parameters").toObject();
        parameters.insert("after_sequence", text);
        invalid.insert("parameters", parameters);
        check(stream.dispatch(invalid, {}).value("error").toObject().value("code") ==
                  "INVALID_INPUT",
              "Borrowed cursor parsing preserves strict failures");
    }
}
void same_snapshot_rows_are_complete_or_empty() {
    RecordApplication app(options(std::make_shared<Store>()));
    seed(app);
    const auto before = good(app.current_document());
    good(app.execute(
        caller,
        at(before),
        "test.sources",
        "source",
        [](const DocumentView& view,
           const RecordIdentityAllocator&) -> Result<RecordPreparedOperation> {
            EditSession edit(view);
            edit.put(records::IncludeDocument{
                EntityId("include"), "model.bdf", {}, {EntityId("material")}});
            edit.put(records::SourceIdentifier{EntityId("source"),
                                               EntityId("material"),
                                               "import",
                                               EntityId("include"),
                                               profile,
                                               "MAT1",
                                               42});
            return {Status::success,
                    RecordPreparedOperation{
                        edit.prepare(), "Source", EntityId("material"), "source", 0., false},
                    {}};
        },
        "rows-source"));
    const auto snapshot = good(app.snapshot(before.document));
    const std::vector<std::string> ids{"material"};
    const auto rows = good(ipc::entity_rows_json(snapshot, ids));
    check(rows.complete && rows.rows.size() == 1 &&
              rows.encoded_byte_upper_bound >=
                  static_cast<std::uint64_t>(
                      QJsonDocument(rows.rows).toJson(QJsonDocument::Compact).size()),
          "complete row encoding stays inside its conservative wire budget");
    const auto material = rows.rows[0].toObject();
    const auto sources = material.value("sources").toArray();
    check(material.value("entity_id") == "material" && material.value("kind") == "material" &&
              material.value("name") == "Steel" &&
              material.value("young_modulus_mpa").toDouble() == 210000 && sources.size() == 1 &&
              sources[0].toObject().value("number") == "42" &&
              sources[0].toObject().value("namespace") == "MAT1" &&
              sources[0].toObject().value("include_id") == "include",
          "shared entity DTO preserves actual material fields and source projection");
    change(app, 180000, "row-later-revision");
    check(good(ipc::entity_rows_json(snapshot, ids))
                  .rows[0]
                  .toObject()
                  .value("young_modulus_mpa")
                  .toDouble() == 210000,
          "row serializer borrows the supplied immutable snapshot, not a later current model");
    for (const auto& incomplete_ids :
         {std::vector<std::string>{"source"}, std::vector<std::string>{"missing"}}) {
        const auto incomplete = good(ipc::entity_rows_json(snapshot, incomplete_ids));
        check(!incomplete.complete && incomplete.rows.isEmpty(),
              "non-query and missing records cannot masquerade as complete empty changes");
    }
    const auto too_many =
        good(ipc::entity_rows_json(snapshot, std::vector<std::string>{"material", "include"}, 1));
    const auto too_large = good(ipc::entity_rows_json(snapshot, ids, 1000, 64));
    check(!too_many.complete && too_many.rows.isEmpty() && !too_large.complete &&
              too_large.rows.isEmpty(),
          "row and byte limits return no partially applicable rows");
    check(!ipc::entity_rows_json(snapshot, std::vector<std::string>{"material", "material"}).ok() &&
              !ipc::entity_rows_json(snapshot, std::vector<std::string>{""}).ok() &&
              !ipc::entity_rows_json(snapshot, ids, 0).ok() &&
              !ipc::entity_rows_json(snapshot, ids, 1001).ok() &&
              !ipc::entity_rows_json(snapshot, ids, 1000, 0).ok(),
          "duplicate IDs and invalid budgets are structured input failures");
    const auto empty = good(ipc::entity_rows_json(snapshot, {}));
    check(empty.complete && empty.rows.isEmpty() && empty.encoded_byte_upper_bound == 2,
          "an actually empty changed-ID set is complete without a whole-model query");
}
void journal_events_and_metadata() {
    auto store = std::make_shared<Store>();
    auto config = options(store);
    config.projects = std::make_shared<Projects>();
    RecordApplication app(config);
    seed(app);
    ipc::EventStream stream(app, "engine-a");
    stream.refresh();
    const auto initial = data(stream.dispatch(request("events.subscribe", ""), "peer"));
    check(initial.value("resync_required").toBool(), "fresh subscriptions require current state");
    auto cursor = sequence(initial, "next_sequence");
    check(stream.drain("peer").isEmpty(), "subscription establishes a current baseline");
    stream.refresh();
    check(stream.stats().emitted_events == 2, "unchanged refresh publishes no duplicate facts");
    const auto info = good(app.current_document());
    change(app, 180000, "edit");
    good(app.undo(caller, at(good(app.current_document())), "undo"));
    good(app.redo(caller, at(good(app.current_document())), "redo"));
    const auto model_before = app.stats();
    stream.refresh();
    check(app.stats().model_bytes_copied == model_before.model_bytes_copied,
          "event refresh borrows committed changes without model copies");
    const auto events = stream.drain("peer");
    check(events.size() == 6, "one document/history pair per actual commit, undo and redo");
    for (qsizetype i = 0; i < events.size(); ++i) {
        const auto event = events[i].toObject();
        check(sequence(event) == ++cursor &&
                  sequence(event, "revision") == info.revision + i / 2 + 1 &&
                  event.value("document_id").toString().toStdString() == info.document.id.value &&
                  !event.value("data").toObject().value("resync_required").toBool(),
              "ordered event context matches the authoritative committed journal");
    }
    good(app.execute(caller,
                     at(info),
                     "test.material",
                     modulus_signature(180000),
                     material_change(180000),
                     "edit"));
    stream.refresh();
    check(stream.drain("peer").isEmpty(), "idempotent replay creates no event");
    const auto now = good(app.current_document());
    good(app.save_document(caller, at(now), "/project-a.qcae", false, "save"));
    stream.refresh();
    auto metadata = stream.drain("peer");
    check(metadata.size() == 1 &&
              metadata[0].toObject().value("event") == "ProjectMetadataChanged" &&
              sequence(metadata[0].toObject(), "revision") == now.revision,
          "same-revision save invalidates project dirty/path/title metadata");
    const auto saved = good(app.current_document());
    good(app.save_document(caller, at(saved), "/project-b.qcae", true, "save-as"));
    const auto metadata_before = app.stats();
    stream.refresh();
    metadata = stream.drain("peer");
    check(metadata.size() == 1 && metadata[0].toObject().value("event") == "ProjectMetadataChanged",
          "save-as invalidates metadata without inventing a model commit");
    check(app.stats().model_bytes_copied == metadata_before.model_bytes_copied &&
              stream.stats().metadata_bytes_encoded > 0 && stream.stats().metadata_bytes_copied > 0,
          "event counters exclude shared immutable model bytes");
    const auto closed = good(app.current_document());
    good(app.close_document(caller, at(closed), ClosePolicy::discard, "close"));
    stream.refresh();
    const auto close_events = stream.drain("peer");
    check(close_events.size() == 2 &&
              !close_events[0].toObject().value("data").toObject().value("active").toBool(),
          "closing a document invalidates active state immediately");
    good(app.create_document(caller, "New", "new"));
    stream.refresh();
    check(stream.drain("peer").size() == 2,
          "new document context produces state/history invalidation");
}
void gaps_bounds_and_storage_uncertainty() {
    auto store = std::make_shared<Store>();
    auto config = options(store);
    config.max_change_journal_entries = 1;
    RecordApplication app(config);
    seed(app);
    ipc::EventStream stream(app, "engine-a", {2, 1});
    stream.refresh();
    const auto baseline = data(stream.dispatch(request("events.subscribe", ""), "peer"));
    const auto cursor = sequence(baseline, "next_sequence");
    check(stream.dispatch(request("events.subscribe"), "second")
                  .value("error")
                  .toObject()
                  .value("code") == "RESOURCE_LIMIT",
          "subscriber count is bounded");
    change(app, 160000, "one");
    stream.refresh();
    change(app, 150000, "two");
    stream.refresh();
    const auto expired = data(stream.dispatch(request("events.read", "engine-a", cursor), {}));
    check(expired.value("resync_required").toBool() && expired.value("events").toArray().isEmpty(),
          "expired retained event cursor requires authoritative resync");
    auto gaps = stream.drain("peer");
    check(gaps.size() == 1 && gaps[0].toObject().value("frame_type") == "event_gap",
          "slow unsolicited subscriber receives explicit gap");
    check(data(stream.dispatch(
                   request("events.read", "other-engine", stream.stats().emitted_events), {}))
              .value("resync_required")
              .toBool(),
          "engine restart expires old cursor identity");
    change(app, 140000, "three");
    change(app, 130000, "four");
    stream.refresh();
    const auto journal_gap = stream.drain("peer");
    check(journal_gap.size() == 2 &&
              journal_gap[0].toObject().value("data").toObject().value("resync_required").toBool(),
          "journal loss invalidates incremental consumers");
    store->fault = Store::Fault::after_model;
    const auto before = good(app.current_document());
    check(!app.execute(caller,
                       at(before),
                       "test.material",
                       modulus_signature(120000),
                       material_change(120000),
                       "unknown")
               .ok(),
          "uncertain durable write");
    stream.refresh();
    gaps = stream.drain("peer");
    check(gaps.size() == 1 && gaps[0].toObject().value("frame_type") == "event_gap",
          "poisoned storage cannot silently advance unchanged RAM event cursor");
    stream.refresh();
    check(stream.drain("peer").isEmpty(), "unchanged poison does not flood repeated gap signals");
    const auto recovered = good(app.recover_document(caller, "recover"));
    stream.refresh();
    check(stream.drain("peer").size() == 2 && recovered.document.epoch != before.document.epoch,
          "explicit recovery supplies new epoch invalidation");
    auto invalid = request("events.read");
    invalid.insert("parameters", "wrong");
    check(stream.dispatch(invalid, {}).value("status") == "failed", "invalid parameters rejected");
    invalid = request("events.read");
    invalid.insert("document_id", "foreign");
    check(stream.dispatch(invalid, {}).value("status") == "failed",
          "foreign envelope field rejected");
    invalid = request("events.read");
    invalid.insert("api_version", "2");
    check(stream.dispatch(invalid, {}).value("error").toObject().value("code") ==
              "API_VERSION_UNSUPPORTED",
          "event API version is negotiated strictly");
    invalid = request("events.read");
    invalid.insert("requested_version", 2);
    check(stream.dispatch(invalid, {}).value("error").toObject().value("code") ==
              "SCHEMA_UNSUPPORTED",
          "event operation contract version is validated");
    stream.unsubscribe("peer");
    check(stream.stats().subscribers == 0 && stream.stats().retained_events <= 2,
          "disconnect releases subscriber while retention remains bounded");
}
void actual_task_fact_events() {
    auto store = std::make_shared<Store>();
    RecordApplication app(options(store));
    seed(app);
    ipc::EventStream stream(app, "engine-a");
    stream.refresh();
    data(stream.dispatch(request("events.subscribe", ""), "peer"));
    TaskService service(publisher(app));
    auto gate = std::make_shared<Gate>();
    const auto task = good(service.start(material_task(app, "event-task", 123000, gate)));
    gate->wait_for();
    stream.refresh();
    auto events = stream.drain("peer");
    check(events.size() == 1 && events[0].toObject().value("event") == "JobChanged" &&
              events[0].toObject().value("data").toObject().value("state") == "running",
          "authoritative running task fact creates job invalidation");
    stream.refresh();
    check(stream.drain("peer").isEmpty(), "unchanged task facts are not serialized again");
    gate->release();
    const auto completed = good(service.wait(caller, task.id));
    stream.refresh();
    events = stream.drain("peer");
    check(completed.state == TaskState::succeeded && events.size() == 3 &&
              events[2].toObject().value("event") == "JobChanged" &&
              events[2].toObject().value("data").toObject().value("state") == "succeeded" &&
              sequence(events[2].toObject().value("data").toObject(), "committed_revision") ==
                  completed.receipt->committed_revision,
          "atomic task success yields committed document/history and owned task fact events");
}
} // namespace
int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        journal_events_and_metadata();
        latest_summary_preserves_legacy_journal();
        canonical_wire_cache_is_shared_and_bounded();
        same_snapshot_rows_are_complete_or_empty();
        gaps_bounds_and_storage_uncertainty();
        actual_task_fact_events();
        std::cout << "event stream tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
