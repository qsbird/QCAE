#include "runtime_test_support.hpp"
#include "qcae/geometry_features.hpp"
#include "qcae/line_mesh_task.hpp"
#include "qcae/nastran_codec.hpp"
#include "qcae/sqlite_store.hpp"
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <tuple>
#include <unistd.h>

using namespace runtime_test;
namespace {
namespace fs = std::filesystem;
namespace r = qcae::records;
const EntityId section_id("M-section"), part_id("M-part"), assembly_id("M-assembly");
const EntityId force_id("M-force"), constraint_id("M-constraint");
const EntityId case_id("M-LC1"), analysis_id("M-analysis");

// This fixture exercises a declared reader format, not a historical R0 producer.
void require(bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error(message);
}
struct Sandbox {
    fs::path path;
    Sandbox() {
        auto pattern = (fs::temp_directory_path() / "qcae-record-v1-XXXXXX").string();
        const auto created = ::mkdtemp(pattern.data());
        require(created, "Could not create private compatibility test directory");
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
struct M {
    std::vector<EntityId> nodes, beams;
    records::GeometryId geometry;
    records::MeshId mesh;
    std::string task;
    EntityId material;
};
M create_m(RecordApplication& app, const fs::path& directory, const EntityId& material_id) {
    const auto profile = NastranCodec().definition().reference;
    auto info =
        good(app.create_document(caller, "Synthetic declared-reader-format1 M", "create-M"));
    const LineGeometryInput input{{0, 0, 0}, {1000, 0, 0}};
    const auto line = good(app.execute(caller,
                                       at(info),
                                       "geometry.create_line",
                                       line_geometry_signature(input),
                                       create_line_handler(input),
                                       "M-line"));
    M m;
    m.material = material_id;
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
        [m, profile, material_id](const DocumentView& view, const RecordIdentityAllocator&)
            -> Result<RecordPreparedOperation> {
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
    info = good(app.current_document());
    good(app.save_document(
        caller, at(info), (directory / "source-M.qcae").string(), false, "M-save"));
    return m;
}
void require_complete_m(const DocumentView& view, const M& m) {
    const auto& material_id = m.material;
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

void same_rows(const LoadedRows& before, IRecordStore& store) {
    const auto after = store.load_rows();
    require(before.generation == after.generation && row_image(before) == row_image(after),
            "Compatibility operation changed persisted rows or generation");
}
std::string file_bytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(bool(input), "Cannot read private project fixture");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
void number(std::string& out, std::uint64_t value) {
    for (unsigned byte = 0; byte < 8; ++byte)
        out.push_back(static_cast<char>((value >> (8 * byte)) & 255));
}
void text(std::string& out, std::string_view value) {
    number(out, value.size());
    out.append(value);
}
std::string project_prefix(std::uint64_t version) {
    std::string out;
    text(out, "QCAE-RECORD-PROJECT");
    number(out, version);
    return out;
}
// Explicit synthetic format1: three project fields and actual physical records.
// It contains current entity schemas, no owned-row suffix, and no history.
std::string synthetic_project(const DocumentInfo& info,
                              const std::vector<std::string>& records,
                              std::uint64_t version = 1) {
    auto out = project_prefix(version);
    for (const auto& value : {info.project_id, info.name, info.content_state})
        text(out, value);
    number(out, records.size());
    for (const auto& record : records)
        text(out, record);
    return out;
}
void publish_fixture(SqliteWorkspaceStore& store,
                     const fs::path& path,
                     const std::string& payload) {
    // Input snapshots only; active authoritative workspace writes use RecordApplication.
    const auto canonical = store.acquire_project(path.string());
    store.publish_project(canonical, {"synthetic-declared-reader-format1", payload});
}
void inactive(RecordApplication& app, bool recovery) {
    const auto current = app.current_document();
    require(!current.ok() && !current.value && current.status == Status::failed && current.error &&
                current.error->code == ErrorCode::document_not_found &&
                app.recovery_available() == recovery,
            "Unexpected active/recovery lifecycle");
}
void normal_opened(RecordApplication& app,
                   const DocumentInfo& source,
                   const ModelImage& golden,
                   const M& m,
                   const DocumentInfo& opened) {
    const auto snapshot = good(app.snapshot(opened.document));
    same_info(opened, snapshot.info);
    require(snapshot.records.matches_version({opened.document, opened.revision}) &&
                model_image(snapshot.records) == golden,
            "Normal open changed an original entity ID, field or record version");
    require_complete_m(snapshot.records, m);
    const auto history = good(app.history(opened.document));
    require(
        opened.document.id != source.document.id &&
            opened.document.epoch != source.document.epoch && opened.revision == 0 &&
            history.revision == 0 && history.items.empty() && history.cursor == 0 &&
            opened.project_id == source.project_id && opened.name == source.name &&
            opened.content_state == source.content_state &&
            opened.saved_content_state == source.content_state && opened.durable && !opened.dirty &&
            opened.material_count == 1 && !app.recovery_available(),
        "Normal open must create a fresh clean authority with the same project and empty history");
}
std::array<EntityId, 4> allocator_probe(RecordApplication& app, IRecordStore& store) {
    const auto info = good(app.current_document());
    const auto before = store.load_rows();
    std::array<EntityId, 4> ids;
    const auto result = app.preview(
        caller,
        at(info),
        [&](const DocumentView&,
            const RecordIdentityAllocator& allocate) -> Result<RecordPreparedOperation> {
            for (auto& id : ids)
                id = allocate();
            throw RecordError(ErrorCode::invalid_input, "Synthetic allocator fixture probe");
        });
    require(!result.ok() && !result.value && result.status == Status::failed && result.error &&
                result.error->code == ErrorCode::invalid_input &&
                result.error->message == "Synthetic allocator fixture probe",
            "Fixture probe must fail without publishing an ID reservation");
    same_info(info, good(app.current_document()));
    same_rows(before, store);
    return ids;
}
struct BadProject {
    const char* name;
    std::string payload;
    const char* message;
};
void run() {
    Sandbox directory;
    const auto database = (directory.path / "reader.sqlite").string();
    auto store = std::make_shared<SqliteWorkspaceStore>(database);
    auto app = std::make_unique<RecordApplication>(settings(store));
    auto scratch = good(app->create_document(caller, "Synthetic allocator fixture", "scratch"));
    const auto ids = allocator_probe(*app, *store);
    good(app->close_document(caller, at(scratch), ClosePolicy::discard, "discard-scratch"));

    M m;
    DocumentInfo source;
    ModelImage golden;
    std::vector<std::string> encoded;
    {
        auto producer_store = std::make_shared<SqliteWorkspaceStore>(
            (directory.path / "current-fixture-builder.sqlite").string());
        RecordApplication builder(settings(producer_store));
        // Normal-open allocates DocId+Epoch before the next entity. Seed that public
        // candidate ID as a real Material via execute; no encoded reference rewriting.
        m = create_m(builder, directory.path, ids[2]);
        source = good(builder.current_document());
        const auto snapshot = good(builder.snapshot(source.document));
        require_complete_m(snapshot.records, m);
        golden = model_image(snapshot.records);
        snapshot.records.visit([&](const Record& record) { encoded.push_back(record->encoded()); });
        const auto history = good(builder.history(source.document));
        require(source.revision == 3 && history.items.size() == 3 && history.cursor == 3,
                "M must originate from real geometry, task-mesh and physics transactions");
    }
    const auto payload = synthetic_project(source, encoded);
    const auto input = directory.path / "synthetic-format1-input.qcae";
    const auto working = directory.path / "working-upgrade.qcae";
    publish_fixture(*store, input, payload);
    publish_fixture(*store, working, payload);
    const auto input_bytes = file_bytes(input);
    auto invalid_records = encoded;
    bool changed{};
    for (auto& record : invalid_records) {
        auto decoded = record_wire::decode(record);
        if (decoded.key == RecordKey{RecordTraits<r::Material>::type_id, m.material.value}) {
            std::erase_if(decoded.fields,
                          [](const auto& field) { return field.id == RecordFieldId{2}; });
            record = record_wire::encode(decoded);
            changed = true;
        }
    }
    require(changed, "Invalid-record fixture must remove the actual M Material required field");
    const std::array bad{BadProject{"future-format",
                                    synthetic_project(source, encoded, 3),
                                    "Unsupported record project schema"},
                         BadProject{"truncated",
                                    payload.substr(0, payload.size() - 1),
                                    "Malformed record application image"},
                         BadProject{"trailing",
                                    payload + "unexpected-trailing-bytes",
                                    "Malformed record application image"},
                         BadProject{"invalid-record",
                                    synthetic_project(source, invalid_records),
                                    "Required record field is missing"}};
    for (const auto& negative : bad)
        publish_fixture(
            *store, directory.path / (std::string(negative.name) + ".qcae"), negative.payload);

    const auto opened = good(app->open_document(caller, working.string(), "open-v1"));
    normal_opened(*app, source, golden, m, opened);
    require(opened.document.id != scratch.document.id &&
                opened.document.epoch != scratch.document.epoch &&
                opened.saved_path == fs::canonical(working).string(),
            "Normal-open must not reuse scratch authority or another path");
    const auto physical_rows = store->load_rows();
    require(std::none_of(physical_rows.rows.begin(),
                         physical_rows.rows.end(),
                         [](const auto& row) { return row.key.space == StoreSpace::task_record; }),
            "Format1 must not invent the current builder's owned task facts");

    EntityId allocated;
    const auto receipt = good(app->execute(
        caller,
        at(opened),
        "test.allocate_after_v1",
        "synthetic-collision",
        [&](const DocumentView& view,
            const RecordIdentityAllocator& allocate) -> Result<RecordPreparedOperation> {
            allocated = allocate();
            require(allocated == ids[3] && allocated != m.material &&
                        !view.find_identity(allocated.value),
                    "Allocator failed to skip the imported synthetic candidate-ID collision");
            EditSession edit(view);
            edit.put(r::Material{allocated, "New allocated material", 70000, .3});
            return {Status::success,
                    RecordPreparedOperation{edit.prepare(),
                                            "Allocate after format1",
                                            allocated,
                                            "synthetic-collision",
                                            70000,
                                            true},
                    {}};
        },
        "allocate-after-v1"));
    auto current = good(app->current_document());
    const auto with_new = good(app->snapshot(current.document)).records;
    require(receipt.primary_entity == allocated && current.revision == 1 && with_new.size() == 34,
            "New material must be one actual transaction with an independent ID");
    for (const auto& [key, bytes] : golden)
        require(with_new.find(key) && with_new.find(key)->encoded() == bytes,
                "Allocating after format1 overwrote an imported M record");
    good(app->undo(caller, at(current), "undo-new-material"));
    current = good(app->current_document());
    require(model_image(good(app->snapshot(current.document)).records) == golden,
            "Undo must restore every complete M field");
    const auto history = good(app->history(current.document));
    require(current.revision == 2 && history.items.size() == 1 && history.cursor == 0 &&
                !history.items.front().applied,
            "Undo must preserve the real creation transaction as a redo tail");
    const auto saved = good(app->save_document(caller, at(current), "", false, "save-current-v2"));
    require(saved.project_id == source.project_id && saved.revision == current.revision &&
                !saved.dirty,
            "Normal save must preserve project identity and model revision");
    const auto current_project = store->read_project(saved.saved_path);
    require(
        current_project.payload.starts_with(project_prefix(2)) && file_bytes(input) == input_bytes,
        "Current writer must emit format2 without modifying the original synthetic format1 input");

    good(app->close_document(caller, at(saved), ClosePolicy::keep_recovery, "keep-recovery"));
    const auto retained = store->load_rows();
    app.reset();
    store.reset();
    store = std::make_shared<SqliteWorkspaceStore>(database);
    same_rows(retained, *store);
    app = std::make_unique<RecordApplication>(settings(store));
    inactive(*app, true);
    // This is a lifecycle guard, deliberately separate from decoder negatives.
    const auto guarded = app->open_document(
        caller, (directory.path / "future-format.qcae").string(), "retained-document-open-guard");
    require(!guarded.ok() && !guarded.value && guarded.status == Status::conflict &&
                guarded.error && guarded.error->code == ErrorCode::document_already_open,
            "Retained recovery M must prevent normal-open before decoding");
    same_rows(retained, *store);
    inactive(*app, true);
    const auto recovered = good(app->recover_document(caller, "recover-v1-upgrade"));
    same_info(saved, recovered, true);
    same_history(history, good(app->history(recovered.document)));
    require(model_image(good(app->snapshot(recovered.document)).records) == golden,
            "Explicit recovery changed the complete saved M");
    good(app->close_document(caller, at(recovered), ClosePolicy::discard, "discard-recovered"));
    const auto reopened = good(app->open_document(caller, working.string(), "reopen-current-v2"));
    normal_opened(*app, recovered, golden, m, reopened);
    good(app->close_document(
        caller, at(reopened), ClosePolicy::discard, "discard-before-negatives"));
    inactive(*app, false);
    const auto empty = store->load_rows();
    require(empty.generation > 0 && !empty.rows.empty(),
            "Decoder negatives need real persisted host/metadata state, not an unused database");
    for (const auto& negative : bad) {
        const auto path = directory.path / (std::string(negative.name) + ".qcae");
        const auto before_input = file_bytes(path);
        const auto before = store->load_rows();
        const auto key = std::string("decoder-negative:") + negative.name;
        for (unsigned attempt = 0; attempt < 2; ++attempt) {
            inactive(*app, false);
            const auto result = app->open_document(caller, path.string(), key);
            require(!result.ok() && !result.value && result.status == Status::failed &&
                        result.error && result.error->code == ErrorCode::schema_unsupported &&
                        result.error->message == negative.message,
                    std::string(negative.name) + ": did not reach the intended decoder failure");
            const auto fact = app->host_operation(caller, "open_document", key);
            require(!fact.ok() && fact.error && fact.error->code == ErrorCode::entity_not_found,
                    "Rejected format input acquired a successful open fact");
            same_rows(before, *store);
            inactive(*app, false);
            require(file_bytes(path) == before_input, "Decoder failure modified its input file");
        }
        std::cout << "PASS synthetic-v1 decoder-negative " << negative.name
                  << " same-key-attempts=2 message=" << negative.message << '\n';
    }
    const auto valid_after_failures =
        good(app->open_document(caller, input.string(), "valid-after-negatives"));
    normal_opened(*app, source, golden, m, valid_after_failures);
    require(file_bytes(input) == input_bytes, "Original synthetic input bytes changed");
    std::cout << "PASS synthetic-declared-reader-format1 completeM=33 nodes=11 beams=10 "
                 "current-writer-format=2 allocator-collision-skip=1 decoder-negative-cases=4 "
                 "recovery-guard-separate=1 historical-R0-producer=0\n";
}
} // namespace
int main() {
    try {
        run();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL synthetic-declared-reader-format1: " << error.what() << '\n';
        return 1;
    }
}
