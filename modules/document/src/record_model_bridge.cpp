#include "qcae/records_model_bridge.hpp"
#include "qcae/records.hpp"

#include <iomanip>
#include <set>
#include <sstream>

namespace qcae {
namespace {
std::array<double, 3> coordinates(const Vec3& value) {
    return {value.x, value.y, value.z};
}
Vec3 coordinates(const std::array<double, 3>& value) {
    return {value[0], value[1], value[2]};
}
std::string source_identity(const SourceIdentifier& value) {
    // Source-model/entity is the existing unique logical identity, independent of numbering.
    const std::array<std::string, 2> identity{value.source_model_id, value.entity.value};
    const auto bytes = record_wire::strings(identity);
    std::uint64_t hash = 14695981039346656037ull;
    for (unsigned char byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    std::ostringstream output;
    output << "qcae:legacy-source:" << std::hex << std::setfill('0') << std::setw(16) << hash;
    return output.str();
}
template <class T, class Visitor> void visit_typed(const DocumentView& view, Visitor visitor) {
    view.visit(RecordTraits<T>::type_id, [&](const Record& record) { visitor(record->get<T>()); });
}
} // namespace

DocumentView records_from_model(const Model& model,
                                std::shared_ptr<const RecordRegistry> registry,
                                RecordVersion version,
                                RecordStats* stats) {
    note_whole_model_serialization(stats);
    DocumentView empty(registry, std::move(version));
    RecordChangeSet changes;
    std::set<std::string> identities;
    for (const auto& item : model_entities(model))
        identities.insert(item.id.value);
    std::string mesh_identity = "qcae:legacy-mesh";
    while (identities.contains(mesh_identity))
        mesh_identity += ":m";
    const records::MeshId mesh(mesh_identity);
    auto add = [&](auto value) {
        auto record = registry->make(std::move(value), stats);
        changes.records.push_back({record->key(), std::nullopt, std::move(record), {}});
    };
    for (const auto& value : model.materials)
        add(records::Material{value.id, value.name, value.young_modulus_mpa, value.poisson_ratio});
    if (!model.nodes.empty() || !model.beams.empty())
        add(records::Mesh{mesh, "Imported mesh", "imported", std::nullopt, 0, false});
    for (const auto& value : model.nodes)
        add(records::Node{value.id, coordinates(value.position), mesh});
    for (const auto& value : model.sections)
        add(records::BeamSection{value.id,
                                 value.name,
                                 value.material,
                                 value.area_mm2,
                                 value.i1_mm4,
                                 value.i2_mm4,
                                 value.torsion_mm4});
    for (const auto& value : model.beams)
        add(records::Beam{
            value.id, value.section, value.nodes, coordinates(value.orientation), mesh});
    for (const auto& value : model.parts)
        add(records::Part{value.id, value.name, value.members});
    for (const auto& value : model.assemblies)
        add(records::Assembly{value.id, value.name, value.children});
    for (const auto& value : model.sets)
        add(records::EntitySet{value.id, value.name, value.members});
    for (const auto& value : model.includes)
        add(records::IncludeDocument{value.id, value.path, value.parent, value.members});
    for (const auto& value : model.forces)
        add(records::NodalForce{value.id, value.node, coordinates(value.force_n)});
    for (const auto& value : model.constraints)
        add(records::Constraint{value.id, value.nodes, value.dofs});
    for (const auto& value : model.analyses)
        add(records::AnalysisDefinition{
            value.id, value.name, value.target, value.forces, value.constraints});
    std::set<std::string> source_keys;
    for (const auto& value : model.sources) {
        auto identity = source_identity(value);
        if (identities.contains(identity) || !source_keys.insert(identity).second)
            throw RecordError(
                ErrorCode::invalid_input, "Legacy source identity collision", identity);
        add(records::SourceIdentifier{EntityId(std::move(identity)),
                                      value.entity,
                                      value.source_model_id,
                                      value.include,
                                      value.profile,
                                      value.name_space,
                                      value.number});
    }
    auto result = apply_record_changes(empty, changes, RecordDirection::forward, stats);
    result.validate();
    return result;
}

Model model_from_records(const DocumentView& view, RecordStats* stats) {
    note_whole_model_materialization(stats);
    if (stats) {
        view.visit([&](const Record& record) {
            stats->model_bytes_copied += record->descriptor().owned_bytes(record->object());
        });
    }
    Model result;
    visit_typed<records::Material>(view, [&](const auto& value) {
        result.materials.push_back(
            {value.id, value.name, value.young_modulus_mpa, value.poisson_ratio});
    });
    visit_typed<records::Node>(view, [&](const auto& value) {
        result.nodes.push_back({value.id, coordinates(value.position)});
    });
    visit_typed<records::BeamSection>(view, [&](const auto& value) {
        result.sections.push_back({value.id,
                                   value.name,
                                   value.material,
                                   value.area_mm2,
                                   value.i1_mm4,
                                   value.i2_mm4,
                                   value.torsion_mm4});
    });
    visit_typed<records::Beam>(view, [&](const auto& value) {
        result.beams.push_back({value.id,
                                value.section.value_or(EntityId{}),
                                value.nodes,
                                coordinates(value.orientation)});
    });
    visit_typed<records::Part>(view, [&](const auto& value) {
        result.parts.push_back({value.id, value.name, value.members});
    });
    visit_typed<records::Assembly>(view, [&](const auto& value) {
        result.assemblies.push_back({value.id, value.name, value.children});
    });
    visit_typed<records::EntitySet>(view, [&](const auto& value) {
        result.sets.push_back({value.id, value.name, value.members});
    });
    visit_typed<records::IncludeDocument>(view, [&](const auto& value) {
        result.includes.push_back({value.id, value.path, value.parent, value.members});
    });
    visit_typed<records::NodalForce>(view, [&](const auto& value) {
        result.forces.push_back({value.id, value.node, coordinates(value.force_n)});
    });
    visit_typed<records::Constraint>(view, [&](const auto& value) {
        result.constraints.push_back({value.id, value.nodes, value.dofs});
    });
    visit_typed<records::AnalysisDefinition>(view, [&](const auto& value) {
        result.analyses.push_back(
            {value.id, value.name, value.target, value.forces, value.constraints});
    });
    visit_typed<records::SourceIdentifier>(view, [&](const auto& value) {
        result.sources.push_back({value.entity,
                                  value.source_model_id,
                                  value.include,
                                  value.profile,
                                  value.name_space,
                                  value.number});
    });
    return result;
}

RecordChangeSet record_changes_from_models(const Model& before,
                                           const Model& after,
                                           std::shared_ptr<const RecordRegistry> registry,
                                           RecordStats* stats) {
    const auto old = records_from_model(before, registry, {}, stats);
    const auto current = records_from_model(after, std::move(registry), {}, stats);
    return diff_record_views(old, current);
}
} // namespace qcae
