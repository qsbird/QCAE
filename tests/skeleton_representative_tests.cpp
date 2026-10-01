#include "runtime_test_support.hpp"
#include "qcae/geometry_features.hpp"
#include "qcae/line_mesh_task.hpp"
#include "qcae/nastran_codec.hpp"
#include "qcae/quantities.hpp"
#include "qcae/query.hpp"
#include "qcae/records_model_bridge.hpp"
#include "qcae/sqlite_store.hpp"
#include <chrono>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <random>
#include <spawn.h>
#include <sstream>
#include <unistd.h>
#include <sys/wait.h>

extern char** environ;

using namespace runtime_test;
namespace {
namespace fs = std::filesystem;
std::string executable;
std::string json_string(std::string_view text) {
    std::string out = "\"";
    for (const auto c : text) {
        if (c == '"' || c == '\\')
            out += '\\';
        if (c == '\n')
            out += "\\n";
        else
            out += c;
    }
    return out + '"';
}
struct Audit {
    std::vector<std::pair<std::string, bool>> assertions;
    std::map<std::string, std::string> actual;
    void require(bool passed, std::string label) {
        assertions.emplace_back(label, passed);
        if (!passed)
            throw std::runtime_error(label);
    }
    std::string json(std::string_view id,
                     std::string_view source,
                     std::string_view run,
                     std::string_view error) const {
        std::ostringstream out;
        out << "{\"case_id\":" << json_string(id)
            << ",\"source_tree_sha256\":" << json_string(source)
            << ",\"run_id\":" << json_string(run)
            << ",\"passed\":" << (error.empty() ? "true" : "false")
            << ",\"error\":" << json_string(error) << ",\"assertions\":[";
        bool comma{};
        for (const auto& [expected, passed] : assertions) {
            if (comma)
                out << ',';
            out << "{\"expected\":" << json_string(expected)
                << ",\"actual\":" << (passed ? "true" : "false")
                << ",\"passed\":" << (passed ? "true" : "false") << '}';
            comma = true;
        }
        out << "],\"actual\":{";
        comma = false;
        for (const auto& [key, value] : actual) {
            if (comma)
                out << ',';
            out << json_string(key) << ':' << json_string(value);
            comma = true;
        }
        return out.str() + "}}";
    }
};
struct Sandbox {
    fs::path path;
    Sandbox() {
        auto pattern = (fs::temp_directory_path() / "qcae-sk-bp-XXXXXX").string();
        auto* created = ::mkdtemp(pattern.data());
        check(created, "mkdtemp failed");
        path = created;
    }
    ~Sandbox() {
        fs::remove_all(path);
    }
};
RecordApplicationOptions settings(const std::shared_ptr<SqliteWorkspaceStore>& store) {
    auto result = options(store);
    result.projects = store;
    return result;
}
std::string model_bytes(const DocumentView& view) {
    std::string result;
    view.visit([&](const Record& record) {
        result += std::to_string(record->encoded().size()) + ':' + record->encoded();
    });
    return result;
}
std::string history_bytes(const HistorySnapshot& history) {
    std::string result = std::to_string(history.cursor) + ':' + std::to_string(history.revision);
    for (const auto& item : history.items)
        result +=
            ':' + item.transaction.value + ':' + item.label + ':' + (item.applied ? "1" : "0");
    return result;
}
void geometry_evaluation(Audit& audit) {
    Sandbox directory;
    auto store = std::make_shared<SqliteWorkspaceStore>((directory.path / "work.sqlite").string());
    RecordApplication app(settings(store));
    auto info = good(app.create_document(caller, "Line evaluation", "create"));
    const LineGeometryInput input{{0, 0, 0}, {1000, 0, 0}};
    const auto created = good(app.execute(caller,
                                          at(info),
                                          "geometry.create_line",
                                          line_geometry_signature(input),
                                          create_line_handler(input),
                                          "line"));
    const records::GeometryId identity(created.primary_entity.value);
    for (const double u : {0., .5, 1.}) {
        const auto value = evaluate_line(good(app.snapshot(info.document)).records, identity, u);
        audit.require(value == std::array<double, 3>{1000 * u, 0, 0},
                      "Real geometry evaluator returns the frozen position at u=" +
                          std::to_string(u));
        audit.actual["position_at_" + std::to_string(u)] = std::to_string(value[0]) + ",0,0 mm";
    }
    info = good(app.current_document());
    info = good(app.save_document(
        caller, at(info), (directory.path / "line.qcae").string(), false, "save"));
    const auto original = info;
    good(app.close_document(caller, at(info), ClosePolicy::discard, "close"));
    const auto reopened = good(app.open_document(caller, original.saved_path, "open"));
    const auto records = good(app.snapshot(reopened.document)).records;
    audit.require(reopened.document.id != original.document.id &&
                      records.find<records::GeometryLine>(identity),
                  "Normal reopen retains the same GeometryId under a new DocumentId");
    audit.require(evaluate_line(records, identity, .5) == std::array<double, 3>{500, 0, 0},
                  "Reopened geometry evaluates to the same 500mm midpoint");
    audit.actual["geometry_id"] = identity.value;
    audit.actual["old_document_id"] = original.document.id.value;
    audit.actual["new_document_id"] = reopened.document.id.value;
}
void mesh_negative_candidates(Audit& audit) {
    Sandbox directory;
    auto store = std::make_shared<SqliteWorkspaceStore>((directory.path / "work.sqlite").string());
    RecordApplication app(settings(store));
    auto info = good(app.create_document(caller, "Malformed mesher outputs", "create"));
    const LineGeometryInput input{{0, 0, 0}, {1000, 0, 0}};
    const auto created = good(app.execute(caller,
                                          at(info),
                                          "geometry.create_line",
                                          line_geometry_signature(input),
                                          create_line_handler(input),
                                          "line"));
    info = good(app.current_document());
    const auto base = good(app.snapshot(info.document));
    const auto history = good(app.history(info.document));
    TaskService service(publisher(app));
    for (const unsigned mode : {0U, 1U}) {
        auto request = line_mesh_task(base,
                                      caller,
                                      {},
                                      {records::GeometryId(created.primary_entity.value), 10, {}},
                                      "negative-" + std::to_string(mode));
        auto real_work = std::move(request.work);
        request.work = [base, real_work, mode](
                           const TaskControl& control) -> std::shared_ptr<const TaskPayload> {
            const auto generated =
                std::dynamic_pointer_cast<const RecordTaskPayload>(real_work(control));
            check(bool(generated), "Real mesher failed to produce a candidate");
            auto operation = generated->operation;
            if (!mode) {
                operation.change = EditSession(base.records).prepare();
            } else {
                EntityId beam;
                operation.change.candidate.visit(RecordTraits<records::Beam>::type_id,
                                                 [&](const Record& record) {
                                                     if (beam.value.empty())
                                                         beam = record->get<records::Beam>().id;
                                                 });
                EditSession broken(operation.change.candidate);
                broken.update<records::Beam>(
                    beam, [](auto& record) { record.nodes[1] = record.nodes[0]; });
                // The invalid self-loop is an actual worker output mutation; validation must
                // reject it before any authority transaction can be published.
                operation.change = broken.prepare();
            }
            return std::make_shared<const RecordTaskPayload>(std::move(operation));
        };
        const auto task = good(service.start(std::move(request)));
        const auto final = good(service.wait(caller, task.id));
        audit.require(
            final.state == TaskState::failed && !final.receipt,
            mode == 0 ? "An actual empty mesh worker candidate cannot claim successful publication"
                      : "An actual self-loop mesh connection candidate cannot be committed");
        const auto current = good(app.snapshot(info.document));
        const auto after_history = good(app.history(info.document));
        audit.require(current.info.revision == info.revision &&
                          diff_record_views(base.records, current.records).empty() &&
                          after_history.cursor == history.cursor &&
                          after_history.items.size() == history.items.size(),
                      "Rejected mesh candidate preserves full model/history/revision");
        audit.actual[mode == 0 ? "empty_candidate_state" : "wrong_connection_state"] =
            task_state_name(final.state);
        audit.actual[mode == 0 ? "empty_candidate_error" : "wrong_connection_error"] =
            final.diagnostic ? final.diagnostic->message : "none";
    }
}
Model scrambled_m(const NastranCodec& codec, Audit& audit) {
    Model model;
    const auto profile = codec.definition().reference;
    const EntityId material("material-random-41"), section("section-random-17"),
        analysis("analysis-random-8"), root("include-root");
    model.materials.push_back({material, "Steel", 210000, .3});
    model.sections.push_back({section, "M section", material, 100, 833.333, 833.333, 1400});
    std::array<unsigned, 11> identity_order{}, number_order{};
    std::iota(identity_order.begin(), identity_order.end(), 0);
    std::iota(number_order.begin(), number_order.end(), 101);
    std::mt19937 identities(6101), numbers(6102), containers(6103);
    std::shuffle(identity_order.begin(), identity_order.end(), identities);
    std::shuffle(number_order.begin(), number_order.end(), numbers);
    std::vector<EntityId> nodes, beams, include_members{material, section};
    std::ostringstream mapping;
    for (unsigned index = 0; index < 11; ++index) {
        const EntityId node("node-random-" + std::to_string(identity_order[index]));
        nodes.push_back(node);
        include_members.push_back(node);
        model.nodes.push_back({node, {100. * index, 0, 0}});
        model.sources.push_back({node, "M", root, profile, "GRID", number_order[index]});
        mapping << "geometric_ordinal=" << index << ",EntityId=" << node.value
                << ",GRID=" << number_order[index] << '\n';
    }
    std::array<unsigned, 10> beam_ids{}, beam_numbers{};
    std::iota(beam_ids.begin(), beam_ids.end(), 0);
    std::iota(beam_numbers.begin(), beam_numbers.end(), 201);
    std::shuffle(beam_ids.begin(), beam_ids.end(), identities);
    std::shuffle(beam_numbers.begin(), beam_numbers.end(), numbers);
    for (unsigned index = 0; index < 10; ++index) {
        const EntityId beam("beam-random-" + std::to_string(beam_ids[index]));
        beams.push_back(beam);
        include_members.push_back(beam);
        model.beams.push_back({beam, section, {nodes[index], nodes[index + 1]}, {0, 1, 0}});
        model.sources.push_back({beam, "M", root, profile, "CBAR", beam_numbers[index]});
        mapping << "beam_ordinal=" << index << ",EntityId=" << beam.value
                << ",CBAR=" << beam_numbers[index] << ",nodes=" << nodes[index].value << ','
                << nodes[index + 1].value << '\n';
    }
    const EntityId force("force"), constraint("constraint"), part("part"), assembly("assembly");
    model.parts.push_back({part, "Beam", beams});
    model.assemblies.push_back({assembly, "Assembly", {part}});
    model.sets.push_back({EntityId("fixed-set"), "Fixed", {nodes.front()}});
    model.sets.push_back({EntityId("loaded-set"), "Loaded", {nodes.back()}});
    model.forces.push_back({force, nodes.back(), {0, -1, 0}});
    model.constraints.push_back({constraint, {nodes.front()}, "123456"});
    model.analyses.push_back(
        {analysis, "M static", {profile, "linear_static"}, {force}, {constraint}});
    include_members.insert(include_members.end(),
                           {force,
                            constraint,
                            analysis,
                            part,
                            assembly,
                            EntityId("fixed-set"),
                            EntityId("loaded-set")});
    model.includes.push_back({root, "model.bdf", {}, include_members});
    for (const auto& entry : {std::pair{material, "MAT1"},
                              std::pair{section, "PBAR"},
                              std::pair{force, "FORCE"},
                              std::pair{constraint, "SPC1"}})
        model.sources.push_back({entry.first,
                                 "M",
                                 root,
                                 profile,
                                 entry.second,
                                 std::string(entry.second) == "FORCE" ? 307U : 313U});
    std::shuffle(model.nodes.begin(), model.nodes.end(), containers);
    std::shuffle(model.beams.begin(), model.beams.end(), containers);
    std::shuffle(model.sources.begin(), model.sources.end(), containers);
    for (std::size_t index = 0; index < model.nodes.size(); ++index)
        mapping << "container_index=" << index << ",EntityId=" << model.nodes[index].id.value
                << '\n';
    audit.actual["independent_permutations"] = mapping.str();
    return model;
}
void identity_numbering(Audit& audit, const fs::path& evidence) {
    const NastranCodec codec;
    const auto model = scrambled_m(codec, audit);
    audit.require(validate_model(model).empty(),
                  "Scrambled M preserves all model references and frozen physical properties");
    EditSession complete(records_from_model(model, make_record_registry()));
    const EntityId load_case("LC1");
    complete.put(records::LoadCase{
        load_case, "LC1", {model.forces.front().id}, {model.constraints.front().id}});
    complete.update<records::AnalysisDefinition>(model.analyses.front().id, [&](auto& record) {
        record.forces.clear();
        record.constraints.clear();
        record.load_cases = std::vector<EntityId>{load_case};
    });
    const records::GeometryId geometry("M-geometry");
    const records::MeshId mesh("M-mesh");
    complete.put(records::GeometryLine{geometry, {0, 0, 0}, {1000, 0, 0}, 1});
    complete.put(records::Mesh{mesh, "M mesh", "geometry", geometry, 1, false});
    for (const auto& node : model.nodes)
        complete.update<records::Node>(node.id, [&](auto& record) { record.mesh = mesh; });
    for (const auto& beam : model.beams)
        complete.update<records::Beam>(beam.id, [&](auto& record) { record.mesh = mesh; });
    const auto view = complete.prepare().candidate;
    audit.require(view.count(RecordTraits<records::LoadCase>::type_id) == 1 &&
                      view.find<records::AnalysisDefinition>(model.analyses.front().id)
                              ->get<records::AnalysisDefinition>()
                              .load_cases == std::optional<std::vector<EntityId>>{{load_case}},
                  "Native scrambled M retains one LoadCase referenced by its AnalysisDefinition");
    const auto node_page = good(query_entities(view, 0, 100, "node"));
    std::set<EntityId> queried;
    for (const auto& row : node_page.entities)
        queried.insert(row.id);
    std::set<EntityId> wanted;
    for (const auto& node : model.nodes)
        wanted.insert(node.id);
    audit.require(queried == wanted && wanted.size() == 11,
                  "Record query returns all original stable identities despite scrambled "
                  "container/number order");
    const auto encoded =
        codec.encode(model, model.analyses.front().id, codec.definition().reference);
    audit.require(encoded.report.complete && encoded.artifact,
                  "Nastran codec exports scrambled M with an explicit identifier map");
    for (const auto& source : model.sources) {
        const auto found = std::find_if(
            encoded.artifact->identities.begin(),
            encoded.artifact->identities.end(),
            [&](const ExportIdentifier& entry) { return entry.entity == source.entity; });
        audit.require(found != encoded.artifact->identities.end() &&
                          found->number == source.number && found->name_space == source.name_space,
                      "Export numbering retains its independent namespace mapping for " +
                          source.entity.value);
    }
    for (const auto& resource : encoded.artifact->resources) {
        const auto path = evidence / "BP-06-export" / resource.path;
        fs::create_directories(path.parent_path());
        std::ofstream(path) << resource.text;
    }
    const auto decoded = codec.decode({encoded.artifact->root_resource,
                                       encoded.artifact->resources,
                                       codec.definition().reference,
                                       "readback",
                                       "mm-N-MPa"});
    audit.require(decoded.report.complete && decoded.candidate,
                  "Actual BDF text is decoded again through the public codec");
    std::map<EntityId, EntityId> bijection;
    for (const auto& imported : decoded.candidate->sources) {
        const auto original = std::find_if(encoded.artifact->identities.begin(),
                                           encoded.artifact->identities.end(),
                                           [&](const ExportIdentifier& entry) {
                                               return entry.number == imported.number &&
                                                      entry.name_space == imported.name_space;
                                           });
        audit.require(
            original != encoded.artifact->identities.end() &&
                bijection.emplace(imported.entity, original->entity).second,
            "Readback identity has one explicit namespace/number-to-original-ID bijection");
    }
    std::ostringstream readback;
    for (const auto& node : decoded.candidate->nodes) {
        const auto original =
            std::find_if(model.nodes.begin(), model.nodes.end(), [&](const Node& candidate) {
                return candidate.id == bijection.at(node.id);
            });
        audit.require(original != model.nodes.end() && node.position == original->position,
                      "Readback node coordinate preserves its mapped original stable identity");
    }
    for (const auto& beam : decoded.candidate->beams) {
        const auto original =
            std::find_if(model.beams.begin(), model.beams.end(), [&](const Beam& candidate) {
                return candidate.id == bijection.at(beam.id);
            });
        audit.require(original != model.beams.end() &&
                          bijection.at(beam.nodes[0]) == original->nodes[0] &&
                          bijection.at(beam.nodes[1]) == original->nodes[1] &&
                          bijection.at(beam.section) == original->section &&
                          beam.orientation == original->orientation,
                      "Readback beam preserves exact ordered connectivity and section references "
                      "through the bijection");
        readback << original->id.value << ':' << bijection.at(beam.nodes[0]).value << ','
                 << bijection.at(beam.nodes[1]).value << '\n';
    }
    audit.require(decoded.candidate->nodes.size() == 11 && decoded.candidate->beams.size() == 10 &&
                      decoded.candidate->materials.front().young_modulus_mpa == 210000 &&
                      decoded.candidate->materials.front().poisson_ratio == .3 &&
                      decoded.candidate->sections.front().area_mm2 == 100 &&
                      decoded.candidate->sections.front().i1_mm4 == 833.333 &&
                      decoded.candidate->sections.front().i2_mm4 == 833.333 &&
                      decoded.candidate->sections.front().torsion_mm4 == 1400 &&
                      decoded.candidate->forces.front().force_n == Vec3{0, -1, 0} &&
                      decoded.candidate->constraints.front().dofs == "123456",
                  "Readback retains all frozen M physical values and fixed DOFs");
    audit.require(bijection.at(decoded.candidate->forces.front().node) ==
                          model.forces.front().node &&
                      bijection.at(decoded.candidate->constraints.front().nodes.front()) ==
                          model.constraints.front().nodes.front(),
                  "Readback load/constraint references retain the original endpoint identities");
    audit.actual["readback_connections"] = readback.str();
    audit.actual["bijective_entity_count"] = std::to_string(bijection.size());
    audit.actual["export_number_count"] = std::to_string(encoded.artifact->identities.size());
}
void units_coordinates_table(Audit& audit) {
    using namespace parameters;
    const auto length = good(canonical_quantity({1, "m"}, Dimension::length));
    const auto modulus = good(canonical_quantity({210, "GPa"}, Dimension::pressure));
    audit.require(length.value == 1000 && length.unit == "mm" && modulus.value == 210000 &&
                      modulus.unit == "MPa",
                  "1m→1000mm and 210GPa→210000MPa exactly");
    CoordinateFrame frame;
    frame.origin_mm = {10, 20, 30};
    const auto global = good(local_to_global(frame, {1, 2, 3}));
    audit.require(global == Vector3{11, 22, 33},
                  "Identity-basis frame translates local(1,2,3) to global(11,22,33)mm");
    const auto table = good(ScalarTable1D::create(
        Dimension::length, Dimension::force, {{{0, "mm"}, {1, "N"}}, {{1, "mm"}, {2, "N"}}}));
    const auto value = good(table.evaluate({.5, "mm"}, OutsidePolicy::reject));
    audit.require(value.value == 1.5 && value.unit == "N",
                  "Table(0,1),(1,2) interpolates x=.5 to1.5");
    audit.actual = {{"length", "1000 mm"},
                    {"modulus", "210000 MPa"},
                    {"global_coordinate", "11,22,33 mm"},
                    {"interpolated_value", "1.5 N"}};
}
int crash_active_tasks(const std::string& database, const std::string& task_file) {
    auto store = std::make_shared<SqliteWorkspaceStore>(database);
    RecordApplication app(settings(store));
    good(app.recover_document(caller, "child-task-recover"));
    TaskService service(publisher(app));
    auto gate = std::make_shared<Gate>();
    struct ReleaseGate {
        std::shared_ptr<Gate> value;
        ~ReleaseGate() {
            value->release();
        }
    } release_on_failure{gate};
    std::vector<TaskRecord> tasks;
    for (unsigned index = 0; index < 2; ++index)
        tasks.push_back(good(service.start(
            material_task(app, "real-running-" + std::to_string(index), 70000, gate))));
    gate->wait_for(2);
    for (unsigned index = 0; index < 8; ++index)
        tasks.push_back(good(service.start(
            material_task(app, "real-queued-" + std::to_string(index), 70000, gate))));
    const auto exceeded = service.start(material_task(app, "real-ninth-pending", 70000, gate));
    check(!exceeded.ok() && exceeded.error && exceeded.error->code == ErrorCode::resource_limit,
          "Ninth pending real task did not return RESOURCE_LIMIT");
    check(service.active_workers() == 2 && service.queued_tasks() == 8,
          "Real worker/queue bounds differ");
    {
        std::ofstream file(task_file);
        for (const auto& task : tasks) {
            const auto current = good(service.query(caller, task.id));
            file << current.id << ' ' << task_state_name(current.state) << ' '
                 << current.events.size();
            for (const auto& event : current.events)
                file << ' ' << event.sequence << ':' << task_state_name(event.state);
            file << '\n';
        }
        file.flush();
        check(bool(file), "Cannot persist real task transition observations");
    }
    // Both workers are still blocked on the explicit Gate and all queued work remains pending.
    // This real process termination bypasses destructors; no synthetic running facts are used.
    ::_Exit(86);
}
void task_bounds_restart(Audit& audit, const fs::path& evidence) {
    Sandbox directory;
    const auto database = (directory.path / "work.sqlite").string();
    const auto task_file = (evidence / "BP-20-before-termination.txt").string();
    DocumentInfo original;
    std::string golden_model, golden_history;
    {
        auto store = std::make_shared<SqliteWorkspaceStore>(database);
        RecordApplication app(settings(store));
        seed(app);
        original = good(app.current_document());
        golden_model = model_bytes(good(app.snapshot(original.document)).records);
        golden_history = history_bytes(good(app.history(original.document)));
    }
    std::vector<std::string> words{executable, "--task-crash", database, task_file};
    std::vector<char*> arguments;
    for (auto& word : words)
        arguments.push_back(word.data());
    arguments.push_back(nullptr);
    pid_t child{};
    const auto spawned =
        ::posix_spawn(&child, executable.c_str(), nullptr, nullptr, arguments.data(), environ);
    if (spawned)
        throw std::runtime_error(std::strerror(spawned));
    int status{};
    pid_t waited{};
    do {
        waited = ::waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    audit.require(waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 86,
                  "Actual process terminates with two running workers, eight queued tasks and the "
                  "ninth rejected");
    auto store = std::make_shared<SqliteWorkspaceStore>(database);
    RecordApplication app(settings(store));
    const auto recovered = good(app.recover_document(caller, "parent-task-recover"));
    TaskService service(publisher(app));
    std::ifstream before(task_file);
    std::string id, state, observations;
    unsigned count{}, running{}, queued{};
    std::ostringstream transitions;
    while (before >> id >> state) {
        std::getline(before, observations);
        ++count;
        running += state == "running";
        queued += state == "queued";
        const auto result = good(service.query(caller, id));
        audit.require(result.state == TaskState::interrupted && !result.receipt,
                      "The actual pre-termination " + state +
                          " task recovers as interrupted without execution");
        std::uint64_t sequence{};
        transitions << id << ":before=" << state << ";after=" << task_state_name(result.state);
        for (const auto& event : result.events) {
            audit.require(event.sequence > sequence,
                          "Recovered task transition event sequence strictly increases");
            sequence = event.sequence;
            transitions << ";event=" << event.sequence << ':' << task_state_name(event.state);
        }
        audit.require(result.events.back().state == TaskState::interrupted,
                      "Recovered task retains the terminal interrupted transition");
        transitions << '\n';
    }
    audit.require(count == 10 && running == 2 && queued == 8 && service.active_workers() == 0 &&
                      service.queued_tasks() == 0,
                  "All ten actual tasks recover with original bounded worker/queue observations "
                  "and no automatic rerun");
    audit.require(
        recovered.document.id == original.document.id && recovered.revision == original.revision &&
            model_bytes(good(app.snapshot(recovered.document)).records) == golden_model &&
            history_bytes(good(app.history(recovered.document))) == golden_history,
        "Terminated pending tasks add zero model transactions and preserve the original complete "
        "model/history");
    audit.actual = {{"active_workers_before", "2"},
                    {"queued_before", "8"},
                    {"ninth_pending", "RESOURCE_LIMIT"},
                    {"child_exit", "86"},
                    {"recovered_tasks", "10"},
                    {"transitions", transitions.str()},
                    {"revision_delta", "0"}};
}
} // namespace
int main(int argc, char** argv) {
    executable = fs::absolute(argv[0]).string();
    if (argc == 4 && std::string_view(argv[1]) == "--task-crash") {
        try {
            return crash_active_tasks(argv[2], argv[3]);
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            return 2;
        }
    }
    const fs::path evidence =
        argc > 1 ? argv[1] : fs::temp_directory_path() / "qcae-representatives";
    const std::string source = argc > 2 ? argv[2] : "mutable-diagnostic";
    const auto nonce = std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) +
                       '-' + std::to_string(::getpid());
    fs::create_directories(evidence);
    std::ofstream output(evidence / "mechanism-representatives.jsonl");
    unsigned failures{};
    for (const auto id : {"BP-03", "BP-05-negative", "BP-06", "BP-09", "BP-20"}) {
        Audit audit;
        std::string error;
        try {
            if (std::string_view(id) == "BP-03")
                geometry_evaluation(audit);
            else if (std::string_view(id) == "BP-05-negative")
                mesh_negative_candidates(audit);
            else if (std::string_view(id) == "BP-06")
                identity_numbering(audit, evidence);
            else if (std::string_view(id) == "BP-20")
                task_bounds_restart(audit, evidence);
            else
                units_coordinates_table(audit);
        } catch (const std::exception& problem) {
            error = problem.what();
            ++failures;
        }
        const auto line = audit.json(id, source, "mechanism-" + nonce + '-' + id, error);
        output << line << '\n';
        output.flush();
        std::cout << line << '\n';
    }
    return failures ? 1 : 0;
}
