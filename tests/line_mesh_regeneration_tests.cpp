#include "runtime_test_support.hpp"
#include "qcae/geometry_features.hpp"
#include "qcae/line_mesh_task.hpp"
#include <algorithm>
#include <iostream>

using namespace runtime_test;
namespace {
void regeneration_can_replace_an_unreferenced_chain() {
    auto store = std::make_shared<Store>();
    RecordApplication app(options(store));
    auto info = good(app.create_document(caller, "Replacement", "create"));
    const LineGeometryInput line{{0, 0, 0}, {1000, 0, 0}};
    const auto receipt = good(app.execute(caller,
                                          at(info),
                                          "geometry.create_line",
                                          line_geometry_signature(line),
                                          create_line_handler(line),
                                          "line"));
    const records::GeometryId geometry(receipt.primary_entity.value);
    const auto base = good(app.snapshot(info.document)).records;
    for (double u : {0., .5, 1.})
        check(evaluate_line(base, geometry, u) == std::array<double, 3>{1000 * u, 0, 0},
              "Line evaluation returned incorrect millimetres");
    for (double u : {-1., 2., std::numeric_limits<double>::quiet_NaN()}) {
        bool rejected{};
        try {
            (void)evaluate_line(base, geometry, u);
        } catch (const RecordError& error) {
            rejected = error.code() == ErrorCode::invalid_input;
        }
        check(rejected, "Line evaluation accepted an invalid parameter");
    }
    TaskService tasks(publisher(app));
    const auto initial = good(tasks.start(
        line_mesh_task(good(app.snapshot(info.document)), caller, {}, {geometry, 2, {}}, "mesh")));
    check(good(tasks.wait(caller, initial.id)).state == TaskState::succeeded,
          "Initial two-beam chain failed");
    const auto mesh = line_mesh_identity(initial.id);
    const auto before = good(app.snapshot(info.document)).records;
    const auto replaced = good(tasks.start(regenerate_line_mesh_task(
        good(app.snapshot(info.document)), caller, mesh, 4, "reject_unmapped", "replace")));
    check(good(tasks.wait(caller, replaced.id)).state == TaskState::succeeded,
          "Unreferenced replacement failed");
    const auto after = good(app.snapshot(info.document)).records;
    check(after.count(RecordTraits<records::Mesh>::type_id) == 1 &&
              after.count(RecordTraits<records::Node>::type_id) == 5 &&
              after.count(RecordTraits<records::Beam>::type_id) == 4,
          "Replacement retained old entities or duplicated its mesh");
    before.visit(RecordTraits<records::Node>::type_id, [&](const Record& record) {
        check(!after.find_identity(record->get<records::Node>().id.value),
              "Old node remains after replacement");
    });
    before.visit(RecordTraits<records::Beam>::type_id, [&](const Record& record) {
        check(!after.find_identity(record->get<records::Beam>().id.value),
              "Old beam remains after replacement");
    });
    good(app.undo(caller, at(good(app.current_document())), "undo-replace"));
    check(diff_record_views(before, good(app.snapshot(info.document)).records).empty(),
          "Replacement undo lost old identities");
    good(app.redo(caller, at(good(app.current_document())), "redo-replace"));
    check(diff_record_views(after, good(app.snapshot(info.document)).records).empty(),
          "Replacement redo lost new identities");
}
void regeneration_preserves_identity_and_rejects_unmapped_references() {
    auto store = std::make_shared<Store>();
    RecordApplication app(options(store));
    auto info = good(app.create_document(caller, "Line regeneration", "create"));
    const LineGeometryInput input{{0, 0, 0}, {1000, 0, 0}};
    const auto created = good(app.execute(caller,
                                          at(info),
                                          "geometry.create_line",
                                          line_geometry_signature(input),
                                          create_line_handler(input),
                                          "line"));
    info = good(app.current_document());
    TaskService tasks(publisher(app));
    const records::GeometryId geometry(created.primary_entity.value);
    const auto initial = good(tasks.start(
        line_mesh_task(good(app.snapshot(info.document)), caller, {}, {geometry, 10, {}}, "mesh")));
    check(good(tasks.wait(caller, initial.id)).state == TaskState::succeeded,
          "Initial mesher failed");
    const auto mesh = line_mesh_identity(initial.id);
    std::vector<records::Node> nodes;
    std::vector<records::Beam> beams;
    auto view = good(app.snapshot(info.document)).records;
    view.visit(RecordTraits<records::Node>::type_id,
               [&](const Record& record) { nodes.push_back(record->get<records::Node>()); });
    view.visit(RecordTraits<records::Beam>::type_id,
               [&](const Record& record) { beams.push_back(record->get<records::Beam>()); });
    std::sort(nodes.begin(), nodes.end(), [](const auto& a, const auto& b) {
        return a.position[0] < b.position[0];
    });
    good(app.execute(
        caller,
        at(good(app.current_document())),
        "test.references",
        "references",
        [nodes, beams](const DocumentView& base, const RecordIdentityAllocator&) {
            EditSession edit(base);
            edit.put(records::EntitySet{EntityId("fixed-set"), "Fixed", {nodes.front().id}});
            edit.put(records::Constraint{EntityId("fixed"), {nodes.front().id}, "123456"});
            edit.put(records::NodalForce{EntityId("force"), nodes.back().id, {0, -1, 0}});
            std::vector<EntityId> members;
            for (const auto& beam : beams)
                members.push_back(beam.id);
            edit.put(records::Part{EntityId("part"), "Part", members});
            return Result<RecordPreparedOperation>{
                Status::success,
                RecordPreparedOperation{
                    edit.prepare(), "References", EntityId("fixed-set"), "references", 0, false},
                {}};
        },
        "references"));
    const std::array<double, 3> end{1200, 0, 0};
    good(app.execute(caller,
                     at(good(app.current_document())),
                     "geometry.move_endpoint",
                     line_endpoint_signature(geometry, end),
                     move_line_endpoint_handler(geometry, end),
                     "endpoint"));
    const auto before = good(app.snapshot(info.document)).records;
    check(before.find<records::Mesh>(mesh)->get<records::Mesh>().stale,
          "Changed geometry did not stale its old mesh");
    const auto input_version = good(app.current_document());
    auto request = regenerate_line_mesh_task(
        good(app.snapshot(info.document)), caller, mesh, 10, "reject_unmapped", "regenerate");
    const auto original_request = request;
    const auto accepted = good(tasks.start(std::move(request)));
    const auto complete = good(tasks.wait(caller, accepted.id));
    check(complete.state == TaskState::succeeded && complete.receipt,
          "Regeneration did not commit");
    const auto after = good(app.snapshot(info.document)).records;
    const auto& binding = after.find<records::Mesh>(mesh)->get<records::Mesh>();
    check(!binding.stale &&
              binding.geometry_revision == after.find<records::GeometryLine>(geometry)
                                               ->get<records::GeometryLine>()
                                               .geometry_revision,
          "Regenerated geometry binding is not current");
    check(after.count(RecordTraits<records::Node>::type_id) == 11 &&
              after.count(RecordTraits<records::Beam>::type_id) == 10,
          "Regeneration duplicated nodes or beams");
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        const auto node = after.find<records::Node>(nodes[index].id);
        check(node && node->get<records::Node>().position ==
                          std::array<double, 3>{double(index) * 120, 0, 0},
              "Regeneration lost an identity or the exact 120 mm spacing");
    }
    for (const auto& beam : beams)
        check(after.find<records::Beam>(beam.id)->get<records::Beam>().nodes == beam.nodes,
              "Regeneration changed existing connectivity");
    for (const auto* id : {"fixed-set", "fixed", "force", "part"})
        check(before.find_identity(id)->encoded() == after.find_identity(id)->encoded(),
              "Regeneration rewrote an external reference");
    check(good(app.current_document()).revision == input_version.revision + 1,
          "Regeneration committed more than once");
    good(app.undo(caller, at(good(app.current_document())), "undo-regenerate"));
    check(diff_record_views(before, good(app.snapshot(info.document)).records).empty(),
          "Undo did not restore the complete stale pre-image");
    check(good(tasks.start(original_request)).id == accepted.id,
          "Regeneration replay started another task after undo");
    check(diff_record_views(before, good(app.snapshot(info.document)).records).empty(),
          "Replay reapplied an undone regeneration");
    good(app.redo(caller, at(good(app.current_document())), "redo-regenerate"));
    check(diff_record_views(after, good(app.snapshot(info.document)).records).empty(),
          "Redo did not restore the full regenerated image");
    const auto before_reject = good(app.current_document());
    const auto history = good(app.history(before_reject.document));
    const auto replacement = good(tasks.start(regenerate_line_mesh_task(
        good(app.snapshot(info.document)), caller, mesh, 11, "reject_unmapped", "unmapped")));
    const auto rejected = good(tasks.wait(caller, replacement.id));
    check(rejected.state == TaskState::failed && rejected.diagnostic,
          "Changed segmentation accepted unmapped references");
    check(good(app.current_document()).revision == before_reject.revision &&
              good(app.history(info.document)).items.size() == history.items.size() &&
              diff_record_views(after, good(app.snapshot(info.document)).records).empty(),
          "Failed replacement partially changed the model or history");
}
} // namespace
int main() {
    try {
        regeneration_can_replace_an_unreferenced_chain();
        regeneration_preserves_identity_and_rejects_unmapped_references();
        std::cout << "PASS: BP-04 geometry stale/rebuild, stable references, undo/replay/redo and "
                     "reject_unmapped\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
