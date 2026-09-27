#include "qcae/core.hpp"
#include "qcae/query.hpp"
#include "qcae/record_application.hpp"
#include "qcae/records.hpp"
#include "qcae/render_projector.hpp"
#include "qcae/render_wire.hpp"
#include "qcae/sqlite_store.hpp"
#include "qcae/typed_host.hpp"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <array>
#include <cstdio>
#include <cstdint>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace qcae;
namespace r = qcae::records;

constexpr std::array<std::size_t, 3> model_sizes{1000, 10000, 100000};
constexpr std::uint64_t node_limit = 589824;
constexpr std::uint64_t material_limit = 69632;
constexpr std::uint64_t metadata_limit = 65536;

void check(bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error(message);
}

template <class T> T good(Result<T> value, const char* what) {
    if (!value.ok()) {
        const auto detail = value.error ? value.error->message : "missing result";
        throw std::runtime_error(std::string(what) + ": " + detail);
    }
    return std::move(*value.value);
}

std::string entity_id(const char* kind, std::size_t index) {
    char digits[13]{};
    std::snprintf(digits, sizeof(digits), "%012zu", index);
    return std::string(kind) + "-" + digits;
}

struct StoreSnapshot {
    std::uint64_t calls{};
    std::uint64_t logical_payload_bytes{};
    std::array<std::uint64_t, 8> payload_by_space{};
    std::array<std::uint64_t, 8> rows_by_space{};
};

StoreSnapshot difference(const StoreSnapshot& end, const StoreSnapshot& start) {
    StoreSnapshot result;
    result.calls = end.calls - start.calls;
    result.logical_payload_bytes = end.logical_payload_bytes - start.logical_payload_bytes;
    for (std::size_t i = 0; i < result.payload_by_space.size(); ++i) {
        result.payload_by_space[i] = end.payload_by_space[i] - start.payload_by_space[i];
        result.rows_by_space[i] = end.rows_by_space[i] - start.rows_by_space[i];
    }
    return result;
}

class CountingSqliteStore final : public IWorkspaceStore, public IRecordStore {
  public:
    explicit CountingSqliteStore(const std::string& path)
        : store_(std::make_shared<SqliteWorkspaceStore>(path)) {}

    std::optional<StoredWorkspace> load() override {
        return store_->load();
    }
    std::uint64_t commit(std::uint64_t expected_generation, const std::string& payload) override {
        return store_->commit(expected_generation, payload);
    }
    std::string acquire_project(const std::string& path) override {
        return store_->acquire_project(path);
    }
    void release_projects_except(const std::string& path) noexcept override {
        store_->release_projects_except(path);
    }
    StoredProject read_project(const std::string& path) override {
        return store_->read_project(path);
    }
    void publish_project(const std::string& path, const StoredProject& project) override {
        store_->publish_project(path, project);
    }
    LoadedRows load_rows() override {
        return store_->load_rows();
    }
    BatchReceipt commit_rows(const StoreBatch& batch) override {
        const auto receipt = store_->commit_rows(batch);
        ++counts_.calls;
        for (const auto& mutation : batch.mutations) {
            if (!mutation.after)
                continue;
            const auto space = static_cast<std::size_t>(mutation.key.space);
            if (space < counts_.payload_by_space.size()) {
                counts_.payload_by_space[space] += mutation.after->size();
                ++counts_.rows_by_space[space];
            }
            counts_.logical_payload_bytes += mutation.after->size();
        }
        return receipt;
    }
    StoreSnapshot snapshot() const {
        return counts_;
    }

  private:
    std::shared_ptr<SqliteWorkspaceStore> store_;
    StoreSnapshot counts_;
};

struct RecordStatsDelta {
    std::uint64_t model_bytes_copied{};
    std::uint64_t model_bytes_encoded{};
    std::uint64_t metadata_bytes_copied{};
    std::uint64_t changed_records{};
    std::uint64_t dirty_pages{};
    std::uint64_t whole_model_serializations{};
    std::uint64_t whole_model_materializations{};
};

RecordStatsDelta difference(const RecordStats& end, const RecordStats& start) {
    return {end.model_bytes_copied - start.model_bytes_copied,
            end.model_bytes_encoded - start.model_bytes_encoded,
            end.metadata_bytes_copied - start.metadata_bytes_copied,
            end.changed_records - start.changed_records,
            end.dirty_pages - start.dirty_pages,
            end.whole_model_serializations - start.whole_model_serializations,
            end.whole_model_materializations - start.whole_model_materializations};
}

RecordActivityCounters difference(const RecordActivityCounters& end,
                                  const RecordActivityCounters& start) {
    return {end.whole_model_serializations - start.whole_model_serializations,
            end.whole_model_materializations - start.whole_model_materializations};
}

QJsonArray values(const std::array<double, 3>& point) {
    return {point[0], point[1], point[2]};
}

QJsonObject dispatch(ipc::TypedHost& host,
                     const Caller& caller,
                     const DocumentInfo& info,
                     const std::string& operation,
                     const QJsonObject& parameters,
                     const std::string& key) {
    const auto request_id = QString::fromStdString("locality-" + key);
    const auto response = host.dispatch(
        QJsonObject{{"request_id", request_id},
                    {"operation", QString::fromStdString(operation)},
                    {"parameters", parameters},
                    {"document_id", QString::fromStdString(info.document.id.value)},
                    {"document_epoch", QString::fromStdString(info.document.epoch.value)},
                    {"expected_revision", QString::number(info.revision)},
                    {"idempotency_key", QString::fromStdString(key)}},
        caller);
    check(response.value("request_id") == request_id,
          operation + " returned a mismatched request ID");
    if (response.value("status") != "success")
        throw std::runtime_error(
            operation +
            " failed: " + QJsonDocument(response).toJson(QJsonDocument::Compact).toStdString());
    return response;
}

void seed(RecordApplication& app,
          const Caller& caller,
          const DocumentInfo& info,
          std::size_t node_count) {
    const auto material = EntityId("material-locality-0001");
    const auto section = EntityId("section-locality-0001");
    const auto signature = "fixture:" + std::to_string(node_count);
    const auto prepared = [node_count, material, section](
                              const DocumentView& view,
                              const RecordIdentityAllocator&) -> Result<RecordPreparedOperation> {
        EditSession edit(view);
        edit.put(r::Material{material, "Locality steel", 210000.0, 0.3});
        edit.put(
            r::BeamSection{section, "Locality section", material, 100.0, 833.333, 833.333, 1400.0});
        for (std::size_t index = 0; index < node_count; ++index) {
            std::array<double, 3> position{static_cast<double>(index), 0.0, 0.0};
            if (index == 0)
                position = {0.0, -100.0, -100.0};
            else if (index + 1 == node_count)
                position = {static_cast<double>(index), 100.0, 100.0};
            edit.put(r::Node{EntityId(entity_id("node", index)), position, std::nullopt});
        }
        for (std::size_t index = 0; index + 1 < node_count; ++index) {
            edit.put(r::Beam{
                EntityId(entity_id("beam", index)),
                section,
                {EntityId(entity_id("node", index)), EntityId(entity_id("node", index + 1))},
                {0.0, 1.0, 0.0},
                std::nullopt});
        }
        return {Status::success,
                RecordPreparedOperation{edit.prepare(),
                                        "Seed locality chain",
                                        material,
                                        "fixture:" + std::to_string(node_count),
                                        0.0,
                                        false},
                std::nullopt};
    };
    good(app.execute(caller,
                     {info.document, info.revision},
                     "test.locality.seed",
                     signature,
                     prepared,
                     "fixture"),
         "seed SQLite locality fixture");
}

std::vector<DirectedRenderChange> directed(const RecordChangeBatch& batch) {
    std::vector<DirectedRenderChange> result;
    result.reserve(batch.changes.size());
    for (const auto& change : batch.changes)
        result.push_back(
            {change.base_revision, change.revision, change.changes.get(), change.direction});
    return result;
}

QJsonObject space_counts(const StoreSnapshot& store) {
    QJsonObject result;
    for (std::size_t index = 1; index < store.payload_by_space.size(); ++index) {
        result.insert(
            QString::number(static_cast<qulonglong>(index)),
            QJsonObject{{"rows", static_cast<qint64>(store.rows_by_space[index])},
                        {"payload_bytes", static_cast<qint64>(store.payload_by_space[index])}});
    }
    return result;
}

QJsonObject measure_one(std::size_t model_size,
                        std::size_t node_count,
                        std::size_t sample,
                        bool node_edit,
                        MemoryApplication& application,
                        CountingSqliteStore& store,
                        ipc::TypedHost& host,
                        SelectionService& selection,
                        RenderProjector& projector,
                        Caller caller,
                        ViewSession& view,
                        const DocumentRef& document) {
    auto info_before =
        good(application.record_application().current_document(), "current document");
    auto snapshot_before =
        good(application.record_application().snapshot(document), "pre-edit snapshot");
    const auto start_stats = application.record_application().stats();
    const auto start_activity = record_activity_counters();
    const auto start_store = store.snapshot();
    const auto key = std::string(node_edit ? "node-" : "material-") + std::to_string(model_size) +
                     "-" + std::to_string(sample);
    const auto middle = node_count / 2;
    const auto target_id = node_edit ? entity_id("node", middle) : "material-locality-0001";
    const auto target_value =
        node_edit ? static_cast<double>(sample + 1) : 211000.0 + static_cast<double>(sample * 1000);
    if (node_edit) {
        const std::array<double, 3> position{static_cast<double>(middle), target_value, 0.0};
        dispatch(
            host,
            caller,
            info_before,
            "node.move",
            {{"entity_id", QString::fromStdString(target_id)}, {"position_mm", values(position)}},
            key);
    } else {
        dispatch(host,
                 caller,
                 info_before,
                 "material.set_young_modulus",
                 {{"entity_id", QString::fromStdString(target_id)},
                  {"young_modulus", QJsonObject{{"value", target_value}, {"unit", "MPa"}}}},
                 key);
    }

    const auto info_after =
        good(application.record_application().current_document(), "post-edit document");
    check(info_after.revision == info_before.revision + 1,
          "Typed local edit did not commit one revision");
    const auto snapshot_after =
        good(application.record_application().snapshot(document), "post-edit snapshot");
    const auto journal =
        good(application.record_application().changes_since(document, info_before.revision),
             "post-edit change journal");
    check(!journal.resync_required && journal.changes.size() == 1,
          "A single typed edit was not available as one retained journal entry");
    check(journal.changes.front().base_revision == info_before.revision &&
              journal.changes.front().revision == info_after.revision &&
              journal.changes.front().changes &&
              journal.changes.front().changes->records.size() == 1,
          "Typed edit journal did not retain exactly one stable-record change");
    const auto& only_change = journal.changes.front().changes->records.front();
    check(only_change.key.identity == target_id,
          "Typed edit journal changed a different entity identity");

    view = good(selection.update_view(snapshot_after.records, caller, view.id, {}, "c3-locality"),
                "rebase display view");
    const auto changes = directed(journal);
    const auto render = good(projector.update(snapshot_after.records, view, changes),
                             "incremental render projection");
    check(!render.full && render.work.full_rebuilds == 0,
          "Local edit rebuilt the full display packet");
    if (node_edit)
        check(render.delta.points.size() == 1 && render.delta.geometry_lines.empty(),
              "Node move did not produce exactly one point delta");
    else
        check(render.delta.points.empty() && render.delta.geometry_lines.empty(),
              "Material-only change produced visual record updates");
    const auto encoded_delta = transport::encode_render_delta(render.delta);
    const auto decoded_delta =
        good(transport::decode_render_delta(encoded_delta), "decode render delta");
    check(decoded_delta.revision == info_after.revision && decoded_delta.view_session_id == view.id,
          "Encoded render delta lost its current model/view version");
    if (node_edit) {
        check(decoded_delta.points.size() == 1 &&
                  decoded_delta.points.front().point.entity.value == target_id &&
                  decoded_delta.points.front().point.position_mm[1] == target_value,
              "Render delta did not carry the selected node's new coordinates");
    }

    const auto end_stats = application.record_application().stats();
    const auto stats = difference(end_stats, start_stats);
    const auto activity = difference(record_activity_counters(), start_activity);
    const auto batch = difference(store.snapshot(), start_store);
    const auto threshold = node_edit ? node_limit : material_limit;
    const auto kind = node_edit ? "node" : "material";
    const auto prefix = std::string(kind) + " locality " + std::to_string(model_size) + " sample " +
                        std::to_string(sample);
    const auto changed_model_records = journal.changes.front().changes->records.size();
    const auto measured_model_copy_bytes =
        stats.model_bytes_copied + render.work.model_bytes_copied;
    const auto measured_encoded_bytes =
        stats.model_bytes_encoded + static_cast<std::uint64_t>(encoded_delta.size());
    QJsonArray violations;
    const auto require_metric = [&](bool condition, const char* metric) {
        if (!condition)
            violations.append(QString::fromStdString(prefix + " failed " + metric));
    };
    require_metric(batch.calls == 1, "one SQLite StoreBatch");
    require_metric(changed_model_records == 1, "one changed model record");
    require_metric(stats.metadata_bytes_copied <= metadata_limit, "64KiB transaction metadata cap");
    require_metric(measured_model_copy_bytes <= threshold,
                   "combined record and render model-copy byte bound");
    require_metric(measured_encoded_bytes <= threshold,
                   "combined record and render-delta encoded byte bound");
    require_metric(batch.logical_payload_bytes <= threshold,
                   "SQLite logical StoreBatch payload bound");
    require_metric(render.work.model_bytes_copied <= threshold,
                   "local render projection copy bound");
    require_metric(static_cast<std::uint64_t>(encoded_delta.size()) <= threshold,
                   "encoded render-delta payload bound");
    require_metric(
        stats.whole_model_serializations == 0 && stats.whole_model_materializations == 0 &&
            activity.whole_model_serializations == 0 && activity.whole_model_materializations == 0,
        "zero whole-model serialization/materialization calls");
    require_metric(
        batch.rows_by_space[static_cast<std::size_t>(StoreSpace::document_record)] == 1 &&
            batch.rows_by_space[static_cast<std::size_t>(StoreSpace::history_entry)] == 1,
        "one record row and one history row");

    return {
        {"model_size", static_cast<qint64>(model_size)},
        {"node_count", static_cast<qint64>(node_count)},
        {"sample", static_cast<qint64>(sample)},
        {"kind", kind},
        {"passed", violations.isEmpty()},
        {"violations", violations},
        {"changed_model_records", static_cast<qint64>(changed_model_records)},
        {"record_work_changed_records", static_cast<qint64>(stats.changed_records)},
        {"combined_model_copy_bytes", static_cast<qint64>(measured_model_copy_bytes)},
        {"combined_encoded_payload_bytes", static_cast<qint64>(measured_encoded_bytes)},
        {"record_stats",
         QJsonObject{
             {"model_bytes_copied", static_cast<qint64>(stats.model_bytes_copied)},
             {"model_bytes_encoded", static_cast<qint64>(stats.model_bytes_encoded)},
             {"metadata_bytes_copied", static_cast<qint64>(stats.metadata_bytes_copied)},
             {"dirty_pages", static_cast<qint64>(stats.dirty_pages)},
             {"whole_model_serializations", static_cast<qint64>(stats.whole_model_serializations)},
             {"whole_model_materializations",
              static_cast<qint64>(stats.whole_model_materializations)}}},
        {"record_activity",
         QJsonObject{{"whole_model_serializations",
                      static_cast<qint64>(activity.whole_model_serializations)},
                     {"whole_model_materializations",
                      static_cast<qint64>(activity.whole_model_materializations)}}},
        {"sqlite_store_batch",
         QJsonObject{{"calls", static_cast<qint64>(batch.calls)},
                     {"logical_payload_bytes", static_cast<qint64>(batch.logical_payload_bytes)},
                     {"rows_by_space", space_counts(batch)}}},
        {"render_projection",
         QJsonObject{{"full_rebuilds", static_cast<qint64>(render.work.full_rebuilds)},
                     {"projected_records", static_cast<qint64>(render.work.projected_records)},
                     {"model_bytes_copied", static_cast<qint64>(render.work.model_bytes_copied)},
                     {"dirty_blocks", static_cast<qint64>(render.work.dirty_blocks)}}},
        {"render_delta_payload_bytes", encoded_delta.size()},
        {"render_delta_sha256",
         QString::fromLatin1(
             QCryptographicHash::hash(encoded_delta, QCryptographicHash::Sha256).toHex())},
        {"sqlite_wal_physical_bytes", QJsonValue::Null},
        {"socket_frame_bytes", QJsonValue::Null}};
}

QJsonObject run_size(std::size_t count) {
    const auto node_count = count;
    QTemporaryDir directory(QStringLiteral("/tmp/qc3-loc-XXXXXX"));
    check(directory.isValid(), "Could not create short temporary SQLite directory");
    const auto database = directory.filePath(QStringLiteral("workspace.sqlite")).toStdString();
    auto store = std::make_shared<CountingSqliteStore>(database);
    Limits fixture_limits;
    fixture_limits.max_entities = 250000;
    MemoryApplication application(fixture_limits, store);
    const auto activity_start = record_activity_counters();
    const Caller caller{"c3-locality-bench"};
    auto info = good(application.create_document(caller, "SK12 locality", "create"),
                     "create locality document");
    seed(application.record_application(), caller, info, node_count);
    info = good(application.record_application().current_document(), "seeded document");
    auto baseline =
        good(application.record_application().snapshot(info.document), "baseline snapshot");
    check(baseline.records.count(RecordTraits<r::Node>::type_id) == node_count &&
              baseline.records.count(RecordTraits<r::Beam>::type_id) == node_count - 1 &&
              baseline.records.size() == count * 2 + 1,
          "Locality fixture does not have the requested chain size");
    const auto middle = node_count / 2;
    check(middle > 0 && middle + 1 < node_count, "Selected locality node must be an interior node");
    std::size_t degree{};
    baseline.records.visit(RecordTraits<r::Beam>::type_id, [&](const Record& record) {
        const auto& beam = record->get<r::Beam>();
        if (beam.nodes[0].value == entity_id("node", middle) ||
            beam.nodes[1].value == entity_id("node", middle))
            ++degree;
    });
    check(degree == 2, "Selected node does not have fixed degree two");
    const auto baseline_node = baseline.records.find<r::Node>(EntityId(entity_id("node", middle)));
    check(baseline_node && baseline_node->get<r::Node>().position[1] == 0.0,
          "Selected node does not start inside the fixed bounds");
    const auto baseline_material =
        baseline.records.find<r::Material>(EntityId("material-locality-0001"));
    check(baseline_material != nullptr, "Locality fixture is missing its measured material");

    ipc::TypedHost host(application.record_application(), [](const ProfileRef&) { return true; });
    SelectionService selection;
    auto view = good(selection.create_view(baseline.records, caller, {}, "c3-locality"),
                     "create locality display view");
    RenderProjector projector;
    const auto initial =
        good(projector.update(baseline.records, view, {}, true), "warm full display projection");
    check(initial.full.has_value(), "Initial display projection did not build a full packet");

    QJsonArray samples;
    for (std::size_t sample = 0; sample < 10; ++sample) {
        samples.append(measure_one(count,
                                   node_count,
                                   sample,
                                   true,
                                   application,
                                   *store,
                                   host,
                                   selection,
                                   projector,
                                   caller,
                                   view,
                                   info.document));
        auto current =
            good(application.record_application().current_document(), "before node reset");
        auto reset = good(application.record_application().undo(
                              caller,
                              {current.document, current.revision},
                              "reset-node-" + std::to_string(count) + "-" + std::to_string(sample)),
                          "undo node sample");
        (void)reset;
        const auto restored = good(application.record_application().snapshot(info.document),
                                   "restored node snapshot");
        view = good(selection.update_view(restored.records, caller, view.id, {}, "c3-locality"),
                    "rebase restored node view");
        const auto reset_batch =
            good(application.record_application().changes_since(info.document, current.revision),
                 "node reset journal");
        check(!reset_batch.resync_required && reset_batch.changes.size() == 1,
              "Node reset did not retain one reverse change");
        const auto reset_changes = directed(reset_batch);
        const auto reset_render = good(projector.update(restored.records, view, reset_changes),
                                       "project node reset outside sample");
        check(!reset_render.full && reset_render.delta.points.size() == 1,
              "Resetting a node required a full display rebuild");
        const auto restored_node =
            restored.records.find<r::Node>(EntityId(entity_id("node", middle)));
        check(restored_node && restored_node->get<r::Node>().position[1] == 0.0 &&
                  restored_node->encoded() == baseline_node->encoded(),
              "Node reset did not restore the golden semantic state");
    }
    for (std::size_t sample = 0; sample < 10; ++sample) {
        samples.append(measure_one(count,
                                   node_count,
                                   sample,
                                   false,
                                   application,
                                   *store,
                                   host,
                                   selection,
                                   projector,
                                   caller,
                                   view,
                                   info.document));
        auto current =
            good(application.record_application().current_document(), "before material reset");
        good(application.record_application().undo(caller,
                                                   {current.document, current.revision},
                                                   "reset-material-" + std::to_string(count) + "-" +
                                                       std::to_string(sample)),
             "undo material sample");
        const auto restored = good(application.record_application().snapshot(info.document),
                                   "restored material snapshot");
        view = good(selection.update_view(restored.records, caller, view.id, {}, "c3-locality"),
                    "rebase restored material view");
        const auto reset_batch =
            good(application.record_application().changes_since(info.document, current.revision),
                 "material reset journal");
        check(!reset_batch.resync_required && reset_batch.changes.size() == 1,
              "Material reset did not retain one reverse change");
        const auto reset_changes = directed(reset_batch);
        const auto reset_render = good(projector.update(restored.records, view, reset_changes),
                                       "project material reset outside sample");
        check(!reset_render.full && reset_render.delta.points.empty() &&
                  reset_render.delta.geometry_lines.empty(),
              "Resetting material produced display record updates");
        const auto restored_material =
            restored.records.find<r::Material>(EntityId("material-locality-0001"));
        check(restored_material &&
                  restored_material->get<r::Material>().young_modulus_mpa == 210000.0 &&
                  restored_material->encoded() == baseline_material->encoded(),
              "Material reset did not restore the golden semantic state");
    }
    const auto final_snapshot =
        good(application.record_application().snapshot(info.document), "final locality snapshot");
    check(diff_record_views(baseline.records, final_snapshot.records).empty(),
          "All measured edits did not restore the initial semantic model");
    return {
        {"model_size", static_cast<qint64>(count)},
        {"record_count", static_cast<qint64>(baseline.records.size())},
        {"node_count", static_cast<qint64>(node_count)},
        {"fixture_max_entities", 250000},
        {"production_default_max_entities", 100000},
        {"node_degree", 2},
        {"samples", samples},
        {"sample_count", samples.size()},
        {"full_display_rebuilds_total", static_cast<qint64>(projector.stats().full_rebuilds)},
        {"whole_model_serialization_total",
         static_cast<qint64>(application.record_application().stats().whole_model_serializations)},
        {"whole_model_materialization_total",
         static_cast<qint64>(
             application.record_application().stats().whole_model_materializations)},
        {"record_activity_total",
         QJsonObject{{"whole_model_serializations",
                      static_cast<qint64>(difference(record_activity_counters(), activity_start)
                                              .whole_model_serializations)},
                     {"whole_model_materializations",
                      static_cast<qint64>(difference(record_activity_counters(), activity_start)
                                              .whole_model_materializations)}}},
        {"sqlite_wal_physical_bytes", QJsonValue::Null},
        {"sqlite_wal_status", "not measured"},
        {"socket_frame_bytes_status", "not measured by this in-process typed-host benchmark"},
        {"restored_to_initial_semantics", true}};
}

} // namespace

int main() {
    try {
        QJsonArray models;
        bool passed = true;
        std::uint64_t serialization_total{};
        std::uint64_t materialization_total{};
        for (const auto count : model_sizes) {
            const auto model = run_size(count);
            models.append(model);
            serialization_total += static_cast<std::uint64_t>(
                model.value("whole_model_serialization_total").toDouble());
            materialization_total += static_cast<std::uint64_t>(
                model.value("whole_model_materialization_total").toDouble());
            for (const auto sample : model.value("samples").toArray())
                passed = passed && sample.toObject().value("passed").toBool();
        }
        QJsonObject report{
            {"acceptance", "SK-12-local-edit-locality"},
            {"measurement_scope",
             "in-process SQLite StoreBatch + TypedHost + change journal + RenderProjector + binary "
             "render codec; excludes ResourceClient, VTK, UI, socket framing, physical WAL pages "
             "and index scans"},
            {"passed", passed},
            {"model_sizes", models},
            {"node_edit_limit_bytes", static_cast<qint64>(node_limit)},
            {"material_edit_limit_bytes", static_cast<qint64>(material_limit)},
            {"transaction_metadata_limit_bytes", static_cast<qint64>(metadata_limit)},
            {"full_model_serialization_calls", static_cast<qint64>(serialization_total)},
            {"full_model_materialization_calls", static_cast<qint64>(materialization_total)},
            {"physical_wal_bytes", QJsonValue::Null},
            {"socket_frame_bytes", QJsonValue::Null}};
        std::cout << QJsonDocument(report).toJson(QJsonDocument::Compact).toStdString() << '\n';
        return passed ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "SK12 locality benchmark failed: " << error.what() << '\n';
        return 1;
    }
}
