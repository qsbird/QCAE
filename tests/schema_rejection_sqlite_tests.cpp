#include "runtime_test_support.hpp"
#include "qcae/geometry_features.hpp"
#include "qcae/line_mesh_task.hpp"
#include "qcae/nastran_codec.hpp"
#include "qcae/sqlite_store.hpp"
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <tuple>
#include <unistd.h>

using namespace runtime_test;
namespace {
namespace fs = std::filesystem;
namespace r = qcae::records;
const EntityId material_id("M-material"), section_id("M-section");
const EntityId part_id("M-part"), assembly_id("M-assembly");
const EntityId force_id("M-force"), constraint_id("M-constraint");
const EntityId case_id("M-LC1"), analysis_id("M-analysis");

void require(bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error(message);
}
struct Sandbox {
    fs::path path;
    Sandbox() {
        auto pattern = (fs::temp_directory_path() / "qcae-schema-rejection-XXXXXX").string();
        const auto created = ::mkdtemp(pattern.data());
        require(created, "Could not create private schema test directory");
        path = created;
    }
    ~Sandbox() {
        fs::remove_all(path);
    }
};
RecordApplicationOptions settings(const std::shared_ptr<SqliteWorkspaceStore>& store) {
    auto result = options(store);
    result.projects = store;
    result.material_count = [](const DocumentView& view) {
        return view.count(RecordTraits<r::Material>::type_id);
    };
    return result;
}
template <class T, class IdType> const T& get(const DocumentView& view, const IdType& id) {
    const auto record = view.find<T>(id);
    require(bool(record), "Complete M record is missing: " + id.value);
    return record->template get<T>();
}
using ModelImage = std::map<RecordKey, std::string>;
using RowImage = std::map<StoreKey, std::string>;
ModelImage model_image(const DocumentView& view) {
    ModelImage result;
    view.visit([&](const Record& record) { result.emplace(record->key(), record->encoded()); });
    return result;
}
RowImage row_image(const LoadedRows& loaded, bool exclude_recovery_metadata = false) {
    require(!loaded.legacy, "Expected the actual record SQLite workspace");
    RowImage result;
    for (const auto& row : loaded.rows) {
        if (exclude_recovery_metadata && (row.key.space == StoreSpace::document_metadata ||
                                          row.key.space == StoreSpace::host_operation_fact))
            continue;
        require(bool(row.value), "Persisted row has no bytes");
        require(result.emplace(row.key, *row.value).second, "Persisted row key is duplicated");
    }
    return result;
}
void same_info(const DocumentInfo& before, const DocumentInfo& after, bool recovery = false) {
    require(before.document.id == after.document.id, "Document identity changed");
    require(recovery ? before.document.epoch != after.document.epoch
                     : before.document.epoch == after.document.epoch,
            recovery ? "Explicit recovery must create a fresh epoch" : "Document epoch changed");
    require(std::tie(before.revision,
                     before.content_state,
                     before.name,
                     before.project_id,
                     before.saved_path,
                     before.saved_content_state,
                     before.material_count,
                     before.dirty,
                     before.durable) == std::tie(after.revision,
                                                 after.content_state,
                                                 after.name,
                                                 after.project_id,
                                                 after.saved_path,
                                                 after.saved_content_state,
                                                 after.material_count,
                                                 after.dirty,
                                                 after.durable),
            "A DocumentInfo field changed");
}
void same_history(const HistorySnapshot& before, const HistorySnapshot& after) {
    require(before.cursor == after.cursor && before.revision == after.revision &&
                before.items.size() == after.items.size(),
            "History size, cursor or revision changed");
    for (std::size_t index = 0; index < before.items.size(); ++index) {
        const auto& left = before.items[index];
        const auto& right = after.items[index];
        require(left.transaction == right.transaction && left.label == right.label &&
                    left.applied == right.applied,
                "History transaction, label or applied state changed");
    }
}
struct State {
    DocumentInfo info;
    ModelImage model;
    HistorySnapshot history;
    LoadedRows rows;
};
State capture(RecordApplication& app, IRecordStore& store) {
    auto info = good(app.current_document());
    const auto snapshot = good(app.snapshot(info.document));
    same_info(info, snapshot.info);
    require(snapshot.records.matches_version({info.document, info.revision}),
            "Record view and document authority disagree");
    return {
        info, model_image(snapshot.records), good(app.history(info.document)), store.load_rows()};
}
void unchanged(const State& before, const State& after) {
    same_info(before.info, after.info);
    require(before.model == after.model, "A complete M record or field changed");
    same_history(before.history, after.history);
    require(before.rows.generation == after.rows.generation &&
                row_image(before.rows) == row_image(after.rows),
            "Persistent model/history/idempotency/task/metadata rows or generation changed");
}

struct M {
    std::vector<EntityId> nodes, beams;
    records::GeometryId geometry;
    records::MeshId mesh;
    std::string task;
};
M create_m(RecordApplication& app, const fs::path& directory) {
    const auto profile = NastranCodec().definition().reference;
    auto info = good(app.create_document(caller, "Schema rejection complete M", "create-M"));
    const LineGeometryInput input{{0, 0, 0}, {1000, 0, 0}};
    const auto line = good(app.execute(caller,
                                       at(info),
                                       "geometry.create_line",
                                       line_geometry_signature(input),
                                       create_line_handler(input),
                                       "M-line"));
    M m;
    m.geometry = records::GeometryId(line.primary_entity.value);
    info = good(app.current_document());
    {
        TaskService service(record_task_publisher(
            app, [profile](const ProfileRef& value) { return value == profile; }));
        const auto snapshot = good(app.snapshot(info.document));
        const auto task = good(service.start(
            line_mesh_task(snapshot, caller, profile, {m.geometry, 10, std::nullopt}, "M-mesh")));
        const auto completed = good(service.wait(caller, task.id));
        require(completed.state == TaskState::succeeded && completed.receipt,
                "Actual line meshing did not commit");
        info = good(app.current_document());
        require(info.revision == snapshot.info.revision + 1,
                "Actual line meshing must create one model transaction");
        m.task = task.id;
        m.mesh = line_mesh_identity(task.id);
        for (unsigned index = 0; index <= 10; ++index) {
            m.nodes.emplace_back("node-" + task.id + "-" + std::to_string(index));
            if (index < 10)
                m.beams.emplace_back("line-" + task.id + "-" + std::to_string(index));
        }
    }
    good(app.execute(
        caller,
        at(info),
        "test.complete_M",
        "complete-M-physics-v1",
        [m, profile](const DocumentView& view,
                     const RecordIdentityAllocator&) -> Result<RecordPreparedOperation> {
            EditSession edit(view);
            edit.put(r::Material{material_id, "Steel", 210000, .3});
            edit.put(
                r::BeamSection{section_id, "M section", material_id, 100, 833.333, 833.333, 1400});
            for (const auto& beam : m.beams)
                edit.update<r::Beam>(beam, [](auto& value) { value.section = section_id; });
            edit.put(r::Part{part_id, "Beam", m.beams});
            edit.put(r::Assembly{assembly_id, "Assembly", {part_id}});
            edit.put(r::EntitySet{EntityId("M-fixed-set"), "Fixed", {m.nodes.front()}});
            edit.put(r::EntitySet{EntityId("M-loaded-set"), "Loaded", {m.nodes.back()}});
            edit.put(r::NodalForce{force_id, m.nodes.back(), {0, -1, 0}});
            edit.put(r::Constraint{constraint_id, {m.nodes.front()}, "123456"});
            edit.put(r::LoadCase{case_id, "LC1", {force_id}, {constraint_id}});
            edit.put(r::AnalysisDefinition{analysis_id,
                                           "M static",
                                           {profile, "linear_static"},
                                           {},
                                           {},
                                           std::vector<EntityId>{case_id}});
            return {Status::success,
                    RecordPreparedOperation{edit.prepare(),
                                            "Complete M physics",
                                            material_id,
                                            "complete-M-physics-v1",
                                            0,
                                            false},
                    {}};
        },
        "M-physics"));
    // Keep a real redo tail: failed planning must not truncate an undone transaction.
    info = good(app.current_document());
    good(app.execute(
        caller,
        at(info),
        "test.material",
        "M-modulus-200000",
        [](const DocumentView& view,
           const RecordIdentityAllocator&) -> Result<RecordPreparedOperation> {
            EditSession edit(view);
            edit.update<r::Material>(material_id,
                                     [](auto& value) { value.young_modulus_mpa = 200000; });
            return {
                Status::success,
                RecordPreparedOperation{
                    edit.prepare(), "M modulus", material_id, "M-modulus-200000", 200000, false},
                {}};
        },
        "M-redo-tail"));
    good(app.undo(caller, at(good(app.current_document())), "M-undo-redo-tail"));
    info = good(app.current_document());
    good(app.save_document(caller, at(info), (directory / "M.qcae").string(), false, "M-save"));
    return m;
}
void require_complete_m(const DocumentView& view, const M& m) {
    require(view.size() == 33 && view.count(RecordTraits<r::Node>::type_id) == 11 &&
                view.count(RecordTraits<r::Beam>::type_id) == 10,
            "Fixture must be the complete original 33-record M, eleven nodes and ten beams");
    const auto& geometry = get<r::GeometryLine>(view, m.geometry);
    const auto& mesh = get<r::Mesh>(view, m.mesh);
    require(geometry.start == std::array<double, 3>{0, 0, 0} &&
                geometry.end == std::array<double, 3>{1000, 0, 0} && mesh.geometry == m.geometry &&
                mesh.geometry_revision == geometry.geometry_revision && !mesh.stale,
            "Frozen geometry and actual mesh binding changed");
    for (unsigned index = 0; index <= 10; ++index) {
        const auto& node = get<r::Node>(view, m.nodes[index]);
        require(node.position == std::array<double, 3>{100. * index, 0, 0} && node.mesh == m.mesh,
                "Frozen node coordinate or actual mesh owner changed");
        if (index < 10) {
            const auto& beam = get<r::Beam>(view, m.beams[index]);
            require(beam.nodes == std::array{m.nodes[index], m.nodes[index + 1]} &&
                        beam.section == section_id && beam.mesh == m.mesh &&
                        beam.orientation == std::array<double, 3>{0, 0, 1},
                    "Frozen beam connectivity, section or actual mesh owner changed");
        }
    }
    require(get<r::Material>(view, material_id) == r::Material{material_id, "Steel", 210000, .3},
            "Frozen M material changed");
    require(get<r::BeamSection>(view, section_id) ==
                r::BeamSection{section_id, "M section", material_id, 100, 833.333, 833.333, 1400},
            "Frozen M section changed");
    require(get<r::Part>(view, part_id) == r::Part{part_id, "Beam", m.beams} &&
                get<r::Assembly>(view, assembly_id) ==
                    r::Assembly{assembly_id, "Assembly", {part_id}},
            "Frozen M organization changed");
    require(get<r::EntitySet>(view, EntityId("M-fixed-set")) ==
                    r::EntitySet{EntityId("M-fixed-set"), "Fixed", {m.nodes.front()}} &&
                get<r::EntitySet>(view, EntityId("M-loaded-set")) ==
                    r::EntitySet{EntityId("M-loaded-set"), "Loaded", {m.nodes.back()}},
            "Frozen M sets changed");
    require(get<r::NodalForce>(view, force_id) ==
                    r::NodalForce{force_id, m.nodes.back(), {0, -1, 0}} &&
                get<r::Constraint>(view, constraint_id) ==
                    r::Constraint{constraint_id, {m.nodes.front()}, "123456"} &&
                get<r::LoadCase>(view, case_id) ==
                    r::LoadCase{case_id, "LC1", {force_id}, {constraint_id}},
            "Frozen M force, constraint or LC1 references changed");
    const auto profile = NastranCodec().definition().reference;
    require(get<r::AnalysisDefinition>(view, analysis_id) ==
                r::AnalysisDefinition{analysis_id,
                                      "M static",
                                      {profile, "linear_static"},
                                      {},
                                      {},
                                      std::vector<EntityId>{case_id}},
            "Frozen M analysis target or load case changed");
}

struct Negative {
    const char* name;
    RecordInput input;
    ErrorCode code;
    const char* message;
    std::string field;
    bool reaches_candidate_validation{};
};
RecordFieldInput& field(RecordInput& input, unsigned id) {
    const auto found = std::find_if(input.fields.begin(),
                                    input.fields.end(),
                                    [&](const auto& item) { return item.id == RecordFieldId{id}; });
    require(found != input.fields.end(), "Golden record field is missing");
    return *found;
}
std::vector<Negative> negatives(const DocumentView& view, const M& m) {
    const auto material = record_wire::decode(view.find<r::Material>(material_id)->encoded());
    const auto node = record_wire::decode(view.find<r::Node>(m.nodes.front())->encoded());
    const auto section = record_wire::decode(view.find<r::BeamSection>(section_id)->encoded());
    std::vector<Negative> result;
    auto input = material;
    std::erase_if(input.fields, [](const auto& value) { return value.id == RecordFieldId{2}; });
    result.push_back({"missing-required",
                      input,
                      ErrorCode::missing_input,
                      "Required record field is missing",
                      "name"});
    input = node;
    field(input, 2).kind = RecordFieldKind::text;
    result.push_back({"wrong-field-type",
                      input,
                      ErrorCode::invalid_input,
                      "Wrong record field type",
                      "position"});
    input = node;
    field(input, 2).unit.clear();
    result.push_back({"missing-unit",
                      input,
                      ErrorCode::invalid_unit,
                      "Required field unit is missing",
                      "position"});
    input = node;
    field(input, 2).unit = "furlong";
    result.push_back({"unknown-unit",
                      input,
                      ErrorCode::invalid_unit,
                      "Unknown or incompatible field unit",
                      "position"});
    input = material;
    field(input, 3).payload = record_wire::real(-1);
    result.push_back({"out-of-range",
                      input,
                      ErrorCode::invalid_input,
                      "Field is outside its allowed range",
                      "young_modulus_mpa"});
    input = section;
    field(input, 3).payload = record_wire::text(m.nodes.front().value);
    result.push_back({"wrong-reference-type",
                      input,
                      ErrorCode::invalid_input,
                      "Reference is missing or has the wrong record type",
                      section_id.value + ".3",
                      true});
    input = section;
    field(input, 3).payload = record_wire::text("M-absent-material");
    require(!view.find_identity("M-absent-material"), "Dangling target must genuinely be absent");
    result.push_back({"dangling-reference",
                      input,
                      ErrorCode::invalid_input,
                      "Reference is missing or has the wrong record type",
                      section_id.value + ".3",
                      true});
    input = material;
    ++input.version;
    require(input.version > view.registry()->find(input.key.type)->current_version,
            "Future input must be an unsupported record version");
    result.push_back({"future-record-version",
                      input,
                      ErrorCode::schema_unsupported,
                      "Unsupported future record version",
                      ""});
    require(result.size() == 8, "Original eight schema categories must remain the denominator");
    return result;
}
EntityId reject_once(RecordApplication& app,
                     IRecordStore& store,
                     const Negative& negative,
                     unsigned& calls) {
    const auto before = capture(app, store);
    const std::string operation = "test.schema_rejection";
    const std::string signature = std::string("schema-negative:") + negative.name;
    const std::string key = std::string("reject:") + negative.name;
    bool decoded{};
    EntityId allocated;
    const auto result = app.execute(
        caller,
        at(before.info),
        operation,
        signature,
        [&](const DocumentView& view,
            const RecordIdentityAllocator& allocate) -> Result<RecordPreparedOperation> {
            ++calls;
            allocated = allocate();
            require(!allocated.value.empty(), "Planner needs an actual temporary identity");
            EditSession edit(view);
            edit.update<r::Material>(material_id, [](auto& value) {
                value.description = "Valid local edit before rejected record";
            });
            // Exercise the real encoded-record decoder, not an operation-version check.
            auto record = view.registry()->decode(record_wire::encode(negative.input));
            decoded = true;
            edit.put(std::move(record));
            return {Status::success,
                    RecordPreparedOperation{
                        edit.prepare(), "Rejected schema input", material_id, signature, 0, false},
                    {}};
        },
        key);
    require(
        !result.ok() && !result.value && result.status == Status::failed && result.error &&
            result.error->code == negative.code && result.error->message == negative.message &&
            result.error->field == negative.field,
        std::string(negative.name) + ": wrong status/code/message/field; actual_status=" +
            std::to_string(static_cast<unsigned>(result.status)) +
            (result.error
                 ? "; actual_code=" + std::to_string(static_cast<unsigned>(result.error->code)) +
                       "; message=" + result.error->message + "; field=" + result.error->field
                 : "; no diagnostic"));
    require(decoded == negative.reaches_candidate_validation,
            std::string(negative.name) + ": rejected at the wrong validation stage");
    const auto outcome = app.action_outcome(caller, before.info.document, operation, key);
    require(!outcome.ok() && !outcome.value && outcome.status == Status::failed && outcome.error &&
                outcome.error->code == ErrorCode::entity_not_found,
            "Rejected input acquired an idempotency success fact");
    unchanged(before, capture(app, store));
    std::cout << "REJECT " << negative.name << " status=" << static_cast<unsigned>(result.status)
              << " code=" << static_cast<unsigned>(result.error->code)
              << " message=" << result.error->message << " field=" << result.error->field
              << " decoded=" << decoded << " unchanged_rows=1 generation=" << before.rows.generation
              << " allocated_id=" << allocated.value << '\n';
    return allocated;
}
void run() {
    Sandbox directory;
    const auto database = (directory.path / "workspace.sqlite").string();
    auto store = std::make_shared<SqliteWorkspaceStore>(database);
    auto app = std::make_unique<RecordApplication>(settings(store));
    const auto m = create_m(*app, directory.path);
    const auto golden = capture(*app, *store);
    require_complete_m(good(app->snapshot(golden.info.document)).records, m);
    require(golden.history.items.size() == 4 && golden.history.cursor == 3 &&
                golden.info.revision == 5 && !golden.history.items.back().applied,
            "Fixture needs real line/mesh/physics history and an undone modulus redo tail");
    std::cout << "M records=" << golden.model.size() << " nodes=" << m.nodes.size()
              << " beams=" << m.beams.size() << " history_items=" << golden.history.items.size()
              << " cursor=" << golden.history.cursor << " revision=" << golden.info.revision
              << " mesh_task=" << m.task << '\n';
    const auto cases = negatives(good(app->snapshot(golden.info.document)).records, m);
    for (const auto& negative : cases) {
        unsigned calls{};
        const auto first_id = reject_once(*app, *store, negative, calls);
        const auto retry_id = reject_once(*app, *store, negative, calls);
        require(first_id == retry_id, "Failed same-epoch planning consumed a candidate-local ID");
        require(calls == 2, "Same-key rejection must revalidate and never replay a success");
        const auto before_recovery = capture(*app, *store);
        app.reset();
        store.reset();
        store = std::make_shared<SqliteWorkspaceStore>(database);
        const auto persisted = store->load_rows();
        require(persisted.generation == before_recovery.rows.generation &&
                    row_image(persisted) == row_image(before_recovery.rows),
                "Closing/reopening SQLite changed retained rows or generation");
        app = std::make_unique<RecordApplication>(settings(store));
        require(app->recovery_available() && !app->current_document().ok(),
                "Retained M must require explicit recovery before editing");
        good(app->recover_document(caller, std::string("recover:") + negative.name));
        const auto recovered = capture(*app, *store);
        same_info(before_recovery.info, recovered.info, true);
        require(recovered.model == before_recovery.model && recovered.model == golden.model,
                "Explicit recovery changed an original M field or identity");
        same_history(before_recovery.history, recovered.history);
        same_history(golden.history, recovered.history);
        require(row_image(before_recovery.rows, true) == row_image(recovered.rows, true),
                "Recovery changed model/history/operation/task rows beyond host/epoch metadata");
        require_complete_m(good(app->snapshot(recovered.info.document)).records, m);
        bool entered{};
        const RecordPrepare fence =
            [&](const DocumentView&,
                const RecordIdentityAllocator&) -> Result<RecordPreparedOperation> {
            entered = true;
            return {};
        };
        const auto expired = app->execute(caller,
                                          at(before_recovery.info),
                                          "test.recovery_fence",
                                          "old-epoch",
                                          fence,
                                          std::string("fence:") + negative.name);
        require(!expired.ok() && !expired.value && expired.status == Status::conflict &&
                    expired.error && expired.error->code == ErrorCode::document_epoch_expired &&
                    !entered,
                "Old epoch must reject before entering the planner after explicit recovery");
        unchanged(recovered, capture(*app, *store));
        std::cout << "FENCE " << negative.name << " old_epoch_rejected=1 planner_entered=0\n";
        reject_once(*app, *store, negative, calls);
        require(calls == 3, "Same rejected key must still revalidate after durable recovery");
        std::cout << "PASS schema_rejection " << negative.name
                  << " attempts=3 unchanged_model_history_rows=1 fresh_recovery_epoch=1\n";
    }
    std::cout << "PASS: eight real application/SQLite schema rejections, same-key retries and "
                 "explicit recovery on complete M\n";
}
} // namespace
int main() {
    try {
        run();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
