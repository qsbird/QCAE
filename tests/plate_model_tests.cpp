#include "qcae/analysis_features.hpp"
#include "qcae/core.hpp"
#include "qcae/geometry_operations.hpp"
#include "qcae/material_operations.hpp"
#include "qcae/mesh_editing_operations.hpp"
#include "qcae/records.hpp"
#include <cmath>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>

using namespace qcae;
using namespace qcae::operations;
namespace {
using Point = std::array<double, 3>;

void check(bool value, const std::string& message) {
    if (!value)
        throw std::runtime_error(message);
}
template <class T> T good(Result<T> value, const std::string& message) {
    if (!value.ok())
        throw std::runtime_error(message + ": " + (value.error ? value.error->message : ""));
    return std::move(*value.value);
}

struct MemoryRows final : IRecordStore {
    std::uint64_t generation{};
    std::map<StoreKey, SharedStoreBytes> rows;
    LoadedRows load_rows() override {
        LoadedRows loaded;
        loaded.generation = generation;
        for (const auto& [key, value] : rows)
            loaded.rows.push_back({key, value});
        return loaded;
    }
    BatchReceipt commit_rows(const StoreBatch& batch) override {
        check(batch.expected_generation == generation, "Row generation must agree");
        std::uint64_t bytes{};
        for (const auto& row : batch.mutations) {
            if (row.after) {
                rows[row.key] = row.after;
                bytes += row.after->size();
            } else
                rows.erase(row.key);
        }
        return {++generation, batch.mutations.size(), bytes};
    }
};

Value quantity(double value, const char* unit) {
    return Value(Value::Object{{"value", Value(value)}, {"unit", Value(unit)}});
}
Value corners(std::initializer_list<Point> points) {
    Value::Array result;
    for (const auto& point : points)
        result.emplace_back(Value::Object{
            {"position_mm",
             Value(Value::Array{Value(point[0]), Value(point[1]), Value(point[2])})}});
    return Value(Value::Object{{"corners", Value(std::move(result))}});
}
std::string text(const Value& value, const std::string& field) {
    return std::get<std::string>(std::get<Value::Object>(value.data).at(field).data);
}

// A 200 x 100 plate meshed by hand. The bottom edge has nodes at x = 0, 50, 150 and 200, so its
// nodes stand for 1/8, 3/8, 3/8 and 1/8 of the edge; the other edges have only their corners.
struct Plate {
    std::shared_ptr<MemoryRows> rows = std::make_shared<MemoryRows>();
    RecordApplication app{[&] {
        RecordApplicationOptions options;
        options.registry = make_record_registry();
        options.records = rows;
        return options;
    }()};
    OperationRegistry registry;
    Caller caller{"plate-model-tests"};
    DocumentInfo document;
    std::string material, section, mesh, load_case;
    std::vector<EntityId> bottom, top;

    Plate() {
        document = good(app.create_document(caller, "Plate", "create"), "create document");
        good(features::geometry::register_handlers(registry, app), "register geometry");
        good(features::materials::register_handlers(registry, app), "register materials");
        good(features::mesh_editing::register_handlers(registry, app), "register mesh editing");
        good(features::analysis::register_handlers(registry, app), "register analysis");
        const auto region = records::GeometryId(text(
            invoke("geometry.create_region",
                   Value(Value::Object{
                       {"outer", corners({{0, 0, 0}, {200, 0, 0}, {200, 100, 0}, {0, 100, 0}})}}),
                   "region"),
            "entity_id"));
        mesh = good(write("mesh",
                          [&](EditSession& edit, const RecordIdentityAllocator& allocate) {
                              return mesh_records(edit, allocate, region);
                          }),
                    "write mesh")
                   .primary_entity.value;
        material = text(invoke("material.create",
                               Value(Value::Object{{"name", Value("Steel")},
                                                   {"young_modulus", quantity(210, "GPa")},
                                                   {"poisson_ratio", Value(0.3)}}),
                               "material"),
                        "entity_id");
        section =
            text(good(create_shell_section(2, "section"), "create shell section"), "entity_id");
        load_case = text(invoke("load_case.create",
                                Value(Value::Object{{"name", Value("Tension")},
                                                    {"force_ids", Value(Value::Array{})},
                                                    {"constraint_ids", Value(Value::Array{})}}),
                                "load-case"),
                         "entity_id");
    }
    OperationContext context(std::string key) {
        const auto info = good(app.current_document(), "current document");
        return {caller, info.document, info.revision, std::move(key), std::nullopt, "request"};
    }
    Result<Value> call(std::string_view operation, Value input, std::string key) {
        return registry.invoke(operation, 1, context(std::move(key)), input);
    }
    Value invoke(std::string_view operation, Value input, std::string key) {
        return good(call(operation, std::move(input), std::move(key)), std::string(operation));
    }
    Result<Value> create_shell_section(double thickness, std::string key) {
        return call("shell_section.create",
                    Value(Value::Object{{"name", Value("Plate")},
                                        {"material_id", Value(material)},
                                        {"thickness_mm", Value(thickness)}}),
                    std::move(key));
    }
    Result<ChangeReceipt>
    write(const std::string& key,
          std::function<EntityId(EditSession&, const RecordIdentityAllocator&)> build) {
        const auto info = good(app.current_document(), "current document");
        return app.execute(
            caller,
            {info.document, info.revision},
            "test.write",
            key,
            [build, key](const DocumentView& view, const RecordIdentityAllocator& allocate)
                -> Result<RecordPreparedOperation> {
                EditSession edit(view);
                const auto primary = build(edit, allocate);
                return {Status::success,
                        RecordPreparedOperation{edit.prepare(), "Write", primary, key, 0, true},
                        {}};
            },
            key);
    }
    EntityId mesh_records(EditSession& edit,
                          const RecordIdentityAllocator& allocate,
                          const records::GeometryId& region) {
        const records::MeshId id(allocate().value);
        edit.put(records::Mesh{id, "Hand mesh", "geometry", region, 1, false});
        auto node = [&](Point position) {
            const EntityId node_id = allocate();
            edit.put(records::Node{node_id, position, id});
            return node_id;
        };
        for (double x : {0., 50., 150., 200.})
            bottom.push_back(node({x, 0, 0}));
        for (double x : {0., 100., 200.})
            top.push_back(node({x, 100, 0}));
        const auto& b = bottom;
        const auto& t = top;
        for (const auto& corners :
             std::initializer_list<std::array<EntityId, 3>>{{b[0], b[1], t[0]},
                                                            {b[1], t[1], t[0]},
                                                            {b[1], b[2], t[1]},
                                                            {b[2], t[2], t[1]},
                                                            {b[2], b[3], t[2]}})
            edit.put(records::Tri3{allocate(), corners, id, std::nullopt});
        return EntityId(id.value);
    }
    DocumentView view() {
        return good(app.snapshot(document.document), "snapshot").records;
    }
    Value edge_input(std::uint64_t loop, std::uint64_t edge) {
        return Value(Value::Object{{"load_case_id", Value(load_case)},
                                   {"mesh_id", Value(mesh)},
                                   {"loop", Value(static_cast<std::int64_t>(loop))},
                                   {"edge", Value(static_cast<std::int64_t>(edge))}});
    }
    Value edge_force(std::uint64_t loop, std::uint64_t edge, Value x) {
        auto input = std::get<Value::Object>(edge_input(loop, edge).data);
        input.emplace("x", std::move(x));
        input.emplace("y", quantity(0, "N"));
        input.emplace("z", quantity(0, "N"));
        return Value(std::move(input));
    }
    const records::LoadCase& load_case_record(const DocumentView& view) {
        return view.find<records::LoadCase>(EntityId(load_case))->get<records::LoadCase>();
    }
};

void refused(const Result<Value>& result, const std::string& field, const std::string& message) {
    check(!result.ok() && result.error->field == field, message);
}

void a_shell_section_gives_the_plate_its_thickness() {
    Plate plate;
    const auto section = plate.view().find<records::ShellSection>(EntityId(plate.section));
    check(section && section->get<records::ShellSection>().thickness_mm == 2 &&
              section->get<records::ShellSection>().material == EntityId(plate.material),
          "The shell section must keep its thickness and material");
    refused(
        plate.create_shell_section(0, "zero"), "thickness_mm", "A zero thickness must be refused");
    refused(plate.create_shell_section(-1, "negative"),
            "thickness_mm",
            "A negative thickness must be refused");
    const auto missing = plate.call("shell_section.create",
                                    Value(Value::Object{{"name", Value("Ghost")},
                                                        {"material_id", Value("no-material")},
                                                        {"thickness_mm", Value(2.0)}}),
                                    "missing");
    check(!missing.ok(), "A shell section with a missing material must be refused");
}

void a_shell_section_is_assigned_to_the_whole_mesh() {
    Plate plate;
    plate.invoke(
        "mesh.assign_shell_section",
        Value(Value::Object{{"mesh_id", Value(plate.mesh)}, {"section_id", Value(plate.section)}}),
        "assign");
    std::size_t assigned{};
    plate.view().visit(RecordTraits<records::Tri3>::type_id, [&](const Record& record) {
        assigned += record->get<records::Tri3>().section == EntityId(plate.section);
    });
    check(assigned == 5, "Every triangle of the mesh must get the section");
    const auto info = good(plate.app.current_document(), "current");
    good(plate.app.undo(plate.caller, {info.document, info.revision}, "undo"), "undo");
    plate.view().visit(RecordTraits<records::Tri3>::type_id, [&](const Record& record) {
        check(!record->get<records::Tri3>().section, "Undo must clear the assignment");
    });
    const auto empty =
        good(plate.write("empty",
                         [](EditSession& edit, const RecordIdentityAllocator& allocate) {
                             const records::MeshId id(allocate().value);
                             edit.put(records::Mesh{id, "Empty", "manual", std::nullopt, 0, false});
                             return EntityId(id.value);
                         }),
             "empty mesh");
    auto assign = [&](const std::string& mesh, const std::string& section, std::string key) {
        return plate.call(
            "mesh.assign_shell_section",
            Value(Value::Object{{"mesh_id", Value(mesh)}, {"section_id", Value(section)}}),
            std::move(key));
    };
    refused(assign("no-mesh", plate.section, "missing-mesh"), "mesh_id", "A missing mesh");
    refused(assign(plate.mesh, "no-section", "missing-section"), "section_id", "A missing section");
    refused(assign(empty.primary_entity.value, plate.section, "empty"),
            "mesh_id",
            "A mesh without triangles must be refused");
}

void an_edge_force_is_spread_by_node_spacing() {
    Plate plate;
    // Edge 0 of the outer loop runs from (0, 0) to (200, 0).
    plate.invoke("load_case.add_edge_force", plate.edge_force(0, 0, quantity(1, "kN")), "force");
    const auto view = plate.view();
    const auto& forces = plate.load_case_record(view).forces;
    check(forces.size() == 4, "The force must be spread over the four nodes of the edge");
    const std::array expected{125., 375., 375., 125.};
    double total = 0;
    for (std::size_t i = 0; i < forces.size(); ++i) {
        const auto& force = view.find<records::NodalForce>(forces[i])->get<records::NodalForce>();
        check(force.node == plate.bottom[i], "Forces must follow the nodes along the edge");
        check(std::abs(force.force_n[0] - expected[i]) < 1e-9 && force.force_n[1] == 0 &&
                  force.force_n[2] == 0,
              "Each node must carry the force times its share of the edge");
        total += force.force_n[0];
    }
    check(std::abs(total - 1000) < 1e-9, "The nodal forces must add up to the edge force");

    const auto info = good(plate.app.current_document(), "current");
    good(plate.app.undo(plate.caller, {info.document, info.revision}, "undo"), "undo");
    check(plate.load_case_record(plate.view()).forces.empty() &&
              plate.view().count(RecordTraits<records::NodalForce>::type_id) == 0,
          "Undo must remove the edge forces and restore the load case");
}

void an_edge_constraint_holds_the_nodes_of_the_edge() {
    Plate plate;
    // Edge 3 of the outer loop runs from (0, 100) to (0, 0).
    plate.invoke(
        "load_case.add_edge_constraint",
        [&] {
            auto input = std::get<Value::Object>(plate.edge_input(0, 3).data);
            input.emplace("dofs", Value("1"));
            return Value(std::move(input));
        }(),
        "support");
    const auto view = plate.view();
    const auto& constraints = plate.load_case_record(view).constraints;
    check(constraints.size() == 1, "The load case must gain one constraint");
    const auto& constraint =
        view.find<records::Constraint>(constraints[0])->get<records::Constraint>();
    check(constraint.dofs == "1" &&
              constraint.nodes == std::vector<EntityId>{plate.top[0], plate.bottom[0]},
          "The constraint must hold the edge nodes, ordered along the edge");
    auto bad_dofs = std::get<Value::Object>(plate.edge_input(0, 3).data);
    bad_dofs.emplace("dofs", Value("17"));
    check(!plate.call("load_case.add_edge_constraint", Value(std::move(bad_dofs)), "bad").ok(),
          "Invalid degrees of freedom must be refused");
}

void edge_requests_must_name_a_real_edge_of_a_region_mesh() {
    Plate plate;
    refused(
        plate.call("load_case.add_edge_force", plate.edge_force(0, 0, quantity(0, "N")), "zero"),
        "x",
        "A zero edge force must be refused");
    refused(
        plate.call("load_case.add_edge_force", plate.edge_force(1, 0, quantity(1, "N")), "loop"),
        "loop",
        "A loop the region does not have must be refused");
    refused(
        plate.call("load_case.add_edge_force", plate.edge_force(0, 4, quantity(1, "N")), "edge"),
        "edge",
        "An edge the loop does not have must be refused");
    auto no_case = std::get<Value::Object>(plate.edge_force(0, 0, quantity(1, "N")).data);
    no_case["load_case_id"] = Value("no-case");
    refused(plate.call("load_case.add_edge_force", Value(std::move(no_case)), "no-case"),
            "load_case_id",
            "A missing load case must be refused");
    const auto manual = good(
        plate.write("manual",
                    [](EditSession& edit, const RecordIdentityAllocator& allocate) {
                        const records::MeshId id(allocate().value);
                        edit.put(records::Mesh{id, "Manual", "manual", std::nullopt, 0, false});
                        return EntityId(id.value);
                    }),
        "manual mesh");
    auto not_region = std::get<Value::Object>(plate.edge_force(0, 0, quantity(1, "N")).data);
    not_region["mesh_id"] = Value(manual.primary_entity.value);
    refused(plate.call("load_case.add_edge_force", Value(std::move(not_region)), "not-region"),
            "mesh_id",
            "A mesh not derived from a region must be refused");
    check(plate.view().count(RecordTraits<records::NodalForce>::type_id) == 0,
          "Refused edge requests must not create forces");
}
} // namespace

int main() {
    try {
        a_shell_section_gives_the_plate_its_thickness();
        a_shell_section_is_assigned_to_the_whole_mesh();
        an_edge_force_is_spread_by_node_spacing();
        an_edge_constraint_holds_the_nodes_of_the_edge();
        edge_requests_must_name_a_real_edge_of_a_region_mesh();
        std::cout
            << "PASS: plate model: shell section and its assignment, edge force spread by node "
               "spacing, edge constraint, refusals and undo\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
