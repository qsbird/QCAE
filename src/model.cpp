#include "qcae/model.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>

namespace qcae {
namespace {

bool finite(Vec3 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

double dot(Vec3 a, Vec3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x};
}

template <class T>
void append_entities(std::vector<EntitySummary>& out, const std::vector<T>& values,
                     const char* kind) {
    for (const auto& value : values) {
        if constexpr (requires { value.name; }) out.push_back({value.id, kind, value.name});
        else if constexpr (requires { value.path; }) out.push_back({value.id, kind, value.path});
        else out.push_back({value.id, kind, {}});
    }
}

void append(std::vector<Reference>& refs, const EntityId& from, const EntityId& to,
            const char* role) {
    refs.push_back({from, to, role});
}

bool has_cycle(const std::map<std::string, std::vector<std::string>>& children) {
    std::map<std::string, int> colors;
    for (const auto& [root, unused] : children) {
        (void)unused;
        if (colors[root]) continue;
        std::vector<std::pair<std::string, std::size_t>> stack{{root, 0}};
        colors[root] = 1;
        while (!stack.empty()) {
            auto& [id, next] = stack.back();
            const auto found = children.find(id);
            if (found == children.end() || next == found->second.size()) {
                colors[id] = 2;
                stack.pop_back();
                continue;
            }
            const auto& child = found->second[next++];
            if (colors[child] == 1) return true;
            if (colors[child] == 0) {
                colors[child] = 1;
                stack.emplace_back(child, 0);
            }
        }
    }
    return false;
}

} // namespace

std::vector<EntitySummary> model_entities(const Model& model) {
    std::vector<EntitySummary> result;
    result.reserve(model.materials.size() + model.nodes.size() + model.sections.size() +
                   model.beams.size() + model.parts.size() + model.assemblies.size() +
                   model.sets.size() + model.includes.size() + model.forces.size() +
                   model.constraints.size() + model.analyses.size());
    append_entities(result, model.materials, "material");
    append_entities(result, model.nodes, "node");
    append_entities(result, model.sections, "section");
    append_entities(result, model.beams, "beam");
    append_entities(result, model.parts, "part");
    append_entities(result, model.assemblies, "assembly");
    append_entities(result, model.sets, "set");
    append_entities(result, model.includes, "include");
    append_entities(result, model.forces, "force");
    append_entities(result, model.constraints, "constraint");
    append_entities(result, model.analyses, "analysis");
    return result;
}

std::vector<Reference> model_references(const Model& model) {
    std::vector<Reference> refs;
    for (const auto& section : model.sections) append(refs, section.id, section.material, "section.material");
    for (const auto& beam : model.beams) {
        append(refs, beam.id, beam.section, "beam.section");
        for (const auto& node : beam.nodes) append(refs, beam.id, node, "beam.node");
    }
    for (const auto& part : model.parts)
        for (const auto& member : part.members) append(refs, part.id, member, "part.member");
    for (const auto& assembly : model.assemblies)
        for (const auto& child : assembly.children) append(refs, assembly.id, child, "assembly.child");
    for (const auto& set : model.sets)
        for (const auto& member : set.members) append(refs, set.id, member, "set.member");
    for (const auto& include : model.includes) {
        if (include.parent) append(refs, include.id, *include.parent, "include.parent");
        for (const auto& member : include.members) append(refs, include.id, member, "include.member");
    }
    for (const auto& force : model.forces) append(refs, force.id, force.node, "force.node");
    for (const auto& constraint : model.constraints)
        for (const auto& node : constraint.nodes) append(refs, constraint.id, node, "constraint.node");
    for (const auto& analysis : model.analyses) {
        for (const auto& force : analysis.forces) append(refs, analysis.id, force, "analysis.force");
        for (const auto& constraint : analysis.constraints)
            append(refs, analysis.id, constraint, "analysis.constraint");
    }
    for (const auto& source : model.sources) {
        append(refs, source.entity, source.include, "source.include");
    }
    return refs;
}

std::vector<Diagnostic> validate_model(const Model& model) {
    std::vector<Diagnostic> errors;
    auto error = [&](const char* message, const std::string& field) {
        errors.push_back({ErrorCode::invalid_input, message, field});
    };
    std::map<std::string, std::string> kinds;
    for (const auto& item : model_entities(model)) {
        if (item.id.value.empty()) error("Entity ID is required", item.kind + ".id");
        else if (!kinds.emplace(item.id.value, item.kind).second)
            error("Entity ID is duplicated", item.id.value);
    }
    auto require = [&](const EntityId& id, const char* kind, const std::string& field) {
        const auto found = kinds.find(id.value);
        if (found == kinds.end() || found->second != kind)
            error("Reference is missing or has the wrong entity kind", field);
    };
    for (const auto& value : model.materials) {
        if (value.name.empty() || !std::isfinite(value.young_modulus_mpa) || value.young_modulus_mpa <= 0)
            error("Material requires a name and finite positive modulus", value.id.value);
        if (value.poisson_ratio && (!std::isfinite(*value.poisson_ratio) ||
                                     *value.poisson_ratio <= -1 || *value.poisson_ratio >= .5))
            error("Poisson ratio must be between -1 and 0.5", value.id.value);
    }
    std::map<std::string, Vec3> node_positions;
    for (const auto& value : model.nodes) {
        if (!finite(value.position)) error("Node position must be finite", value.id.value);
        node_positions.emplace(value.id.value, value.position);
    }
    for (const auto& value : model.sections) {
        require(value.material, "material", value.id.value + ".material");
        if (value.name.empty() || !std::isfinite(value.area_mm2) || value.area_mm2 <= 0 ||
            !std::isfinite(value.i1_mm4) || value.i1_mm4 <= 0 ||
            !std::isfinite(value.i2_mm4) || value.i2_mm4 <= 0 ||
            !std::isfinite(value.torsion_mm4) || value.torsion_mm4 <= 0)
            error("Section requires a name and finite positive properties", value.id.value);
    }
    for (const auto& value : model.beams) {
        require(value.section, "section", value.id.value + ".section");
        require(value.nodes[0], "node", value.id.value + ".node0");
        require(value.nodes[1], "node", value.id.value + ".node1");
        const auto a = node_positions.find(value.nodes[0].value);
        const auto b = node_positions.find(value.nodes[1].value);
        if (a != node_positions.end() && b != node_positions.end() && finite(value.orientation)) {
            Vec3 axis{b->second.x-a->second.x, b->second.y-a->second.y, b->second.z-a->second.z};
            const double axis2 = dot(axis, axis);
            const double orient2 = dot(value.orientation, value.orientation);
            const Vec3 normal = cross(axis, value.orientation);
            const double normal2 = dot(normal, normal);
            if (!std::isfinite(axis2) || axis2 <= 0 || !std::isfinite(orient2) || orient2 <= 0 ||
                !std::isfinite(normal2) || normal2 <= 1e-24 * axis2 * orient2)
                error("Beam length and nonparallel orientation are required", value.id.value);
        } else if (!finite(value.orientation)) {
            error("Beam orientation must be finite", value.id.value);
        }
    }
    std::map<std::string, std::string> part_owner;
    for (const auto& value : model.parts) {
        std::set<std::string> local;
        for (const auto& member : value.members) {
            const auto kind = kinds.find(member.value);
            if (kind == kinds.end() || (kind->second != "node" && kind->second != "beam"))
                error("Part member must be a node or beam", value.id.value);
            if (!local.insert(member.value).second) error("Duplicate part member", value.id.value);
            if (kind != kinds.end() && kind->second == "beam" &&
                !part_owner.emplace(member.value, value.id.value).second)
                error("Beam belongs to more than one part", member.value);
        }
    }
    std::map<std::string, std::string> assembly_parent;
    std::map<std::string, std::vector<std::string>> assembly_children;
    for (const auto& value : model.assemblies) {
        std::set<std::string> local;
        for (const auto& child : value.children) {
            const auto kind = kinds.find(child.value);
            if (kind == kinds.end() || (kind->second != "part" && kind->second != "assembly"))
                error("Assembly child must be a part or assembly", value.id.value);
            if (!local.insert(child.value).second) error("Duplicate assembly child", value.id.value);
            if (!assembly_parent.emplace(child.value, value.id.value).second)
                error("Assembly child has more than one parent", child.value);
            if (kind != kinds.end() && kind->second == "assembly")
                assembly_children[value.id.value].push_back(child.value);
        }
    }
    if (has_cycle(assembly_children)) error("Assembly cycle", "assemblies");
    for (const auto& value : model.sets) {
        std::set<std::string> local;
        std::string member_kind;
        for (const auto& member : value.members) {
            const auto found = kinds.find(member.value);
            if (found == kinds.end() || (found->second != "node" && found->second != "beam"))
                error("Set member must be a node or beam", value.id.value);
            else if (member_kind.empty()) member_kind = found->second;
            else if (member_kind != found->second) error("Set mixes node and beam members", value.id.value);
            if (!local.insert(member.value).second) error("Duplicate set member", value.id.value);
        }
    }
    std::map<std::string, std::vector<std::string>> include_children;
    std::map<std::string, std::string> member_owner;
    std::set<std::string> paths;
    std::size_t roots = 0;
    for (const auto& value : model.includes) {
        if (value.path.empty() || !paths.insert(value.path).second)
            error("Include path is empty or duplicated", value.id.value);
        if (value.parent) {
            require(*value.parent, "include", value.id.value + ".parent");
            include_children[value.parent->value].push_back(value.id.value);
        } else ++roots;
        std::set<std::string> local;
        for (const auto& member : value.members) {
            const auto found = kinds.find(member.value);
            if (found == kinds.end() || found->second == "include")
                error("Include member is missing or is an include", value.id.value);
            if (!local.insert(member.value).second) error("Duplicate include member", value.id.value);
            if (!member_owner.emplace(member.value, value.id.value).second)
                error("Entity belongs to more than one include", member.value);
        }
    }
    if (!model.includes.empty() && roots != 1) error("Include tree requires one root", "includes");
    if (has_cycle(include_children)) error("Include cycle", "includes");
    for (const auto& value : model.forces) {
        require(value.node, "node", value.id.value + ".node");
        if (!finite(value.force_n)) error("Force must be finite", value.id.value);
    }
    for (const auto& value : model.constraints) {
        if (value.nodes.empty() || value.dofs.empty()) error("Constraint requires nodes and DOFs", value.id.value);
        if (value.dofs.find_first_not_of("123456") != std::string::npos ||
            std::set<char>(value.dofs.begin(), value.dofs.end()).size() != value.dofs.size())
            error("Constraint DOFs must be unique digits 1 through 6", value.id.value);
        std::set<std::string> local;
        for (const auto& node : value.nodes) {
            require(node, "node", value.id.value + ".node");
            if (!local.insert(node.value).second) error("Duplicate constraint node", value.id.value);
        }
    }
    for (const auto& value : model.analyses) {
        if (value.name.empty() || value.target.profile.profile_id.empty() ||
            value.target.profile.profile_version.empty() ||
            value.target.profile.definition_digest.empty() || value.target.analysis_kind.empty())
            error("Analysis requires a name and target binding", value.id.value);
        std::set<std::string> local_forces, local_constraints;
        for (const auto& force : value.forces) {
            require(force, "force", value.id.value + ".force");
            if (!local_forces.insert(force.value).second) error("Duplicate analysis force", value.id.value);
        }
        for (const auto& constraint : value.constraints) {
            require(constraint, "constraint", value.id.value + ".constraint");
            if (!local_constraints.insert(constraint.value).second)
                error("Duplicate analysis constraint", value.id.value);
        }
    }
    std::set<std::tuple<std::string, std::string, std::uint64_t>> source_numbers;
    std::set<std::pair<std::string, std::string>> source_entities;
    for (const auto& value : model.sources) {
        if (!kinds.contains(value.entity.value)) error("Source entity is missing", value.entity.value);
        require(value.include, "include", value.entity.value + ".source.include");
        if (value.source_model_id.empty() || value.name_space.empty() || value.number == 0 ||
            value.profile.profile_id.empty() || value.profile.profile_version.empty() ||
            value.profile.definition_digest.empty())
            error("Source identifier is incomplete", value.entity.value);
        if (!source_numbers.emplace(value.source_model_id, value.name_space, value.number).second)
            error("Duplicate source number within namespace", value.entity.value);
        if (!source_entities.emplace(value.source_model_id, value.entity.value).second)
            error("Entity has duplicate source identifiers", value.entity.value);
        const auto owner = member_owner.find(value.entity.value);
        if (owner == member_owner.end() || owner->second != value.include.value)
            error("Source include does not own entity", value.entity.value);
    }
    return errors;
}

std::vector<EntityId> affected_analyses(const Model& model, const EntityId& entity) {
    std::map<std::string, std::vector<std::string>> reverse;
    for (const auto& ref : model_references(model))
        if (ref.role != "source.include") reverse[ref.to.value].push_back(ref.from.value);
    std::set<std::string> seen{entity.value};
    std::vector<std::string> pending{entity.value};
    for (std::size_t i = 0; i < pending.size(); ++i)
        for (const auto& from : reverse[pending[i]])
            if (seen.insert(from).second) pending.push_back(from);
    bool affects_geometry = false;
    for (const auto& beam : model.beams) if (seen.contains(beam.id.value)) affects_geometry = true;
    std::vector<EntityId> result;
    for (const auto& analysis : model.analyses)
        if (seen.contains(analysis.id.value) || affects_geometry) result.push_back(analysis.id);
    return result;
}

} // namespace qcae
