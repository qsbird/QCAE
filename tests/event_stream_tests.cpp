#include "runtime_test_support.hpp"
#include "qcae/event_stream.hpp"
#include <QCoreApplication>
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
        gaps_bounds_and_storage_uncertainty();
        actual_task_fact_events();
        std::cout << "event stream tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
