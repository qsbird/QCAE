#include "qcae/records.hpp"

#include <map>
#include <tuple>

namespace qcae::records {
namespace {
template <class T, class Visitor> void visit(const DocumentView& view, Visitor visitor) {
    view.visit(RecordTraits<T>::type_id, [&](const Record& record) { visitor(record->get<T>()); });
}
[[noreturn]] void invalid(const char* message, std::string_view field) {
    throw RecordError(ErrorCode::invalid_input, message, std::string(field));
}
double length_squared(const std::array<double, 3>& value) {
    return value[0] * value[0] + value[1] * value[1] + value[2] * value[2];
}
using Graph = std::map<std::string_view, std::vector<std::string_view>, std::less<>>;
void require_acyclic(const Graph& graph, const char* field) {
    std::map<std::string_view, unsigned, std::less<>> colors;
    for (const auto& [root, unused] : graph) {
        (void)unused;
        if (colors[root])
            continue;
        std::vector<std::pair<std::string_view, std::size_t>> stack{{root, 0}};
        colors[root] = 1;
        while (!stack.empty()) {
            auto& [identity, next] = stack.back();
            const auto found = graph.find(identity);
            if (found == graph.end() || next == found->second.size()) {
                colors[identity] = 2;
                stack.pop_back();
                continue;
            }
            const auto child = found->second[next++];
            if (colors[child] == 1)
                invalid("Reference graph contains a cycle", field);
            if (!colors[child]) {
                colors[child] = 1;
                stack.emplace_back(child, 0);
            }
        }
    }
}
} // namespace

void validate_record(const Beam& value, const DocumentView& view) {
    const auto a = view.find<Node>(value.nodes[0]);
    const auto b = view.find<Node>(value.nodes[1]);
    if (!a || !b)
        invalid("Beam nodes are missing", value.id.value);
    const auto& left = a->get<Node>();
    const auto& right = b->get<Node>();
    std::array<double, 3> axis{};
    for (std::size_t index = 0; index < axis.size(); ++index)
        axis[index] = right.position[index] - left.position[index];
    const auto& orientation = value.orientation;
    const std::array<double, 3> normal{axis[1] * orientation[2] - axis[2] * orientation[1],
                                       axis[2] * orientation[0] - axis[0] * orientation[2],
                                       axis[0] * orientation[1] - axis[1] * orientation[0]};
    const double axis2 = length_squared(axis);
    const double orientation2 = length_squared(orientation);
    const double normal2 = length_squared(normal);
    if (!std::isfinite(axis2) || axis2 <= 0 || !std::isfinite(orientation2) || orientation2 <= 0 ||
        !std::isfinite(normal2) || normal2 <= 1e-24 * axis2 * orientation2)
        invalid("Beam requires finite length and nonparallel orientation", value.id.value);
    if (left.mesh != right.mesh || value.mesh != left.mesh)
        invalid("Beam and its nodes must belong to the same mesh", value.id.value);
}
void validate_record(const Constraint& value, const DocumentView&) {
    if (value.dofs.find_first_not_of("123456") != std::string::npos ||
        std::set<char>(value.dofs.begin(), value.dofs.end()).size() != value.dofs.size())
        invalid("Constraint DOFs must be unique digits 1 through 6", value.id.value);
}
void validate_record(const GeometryLine& value, const DocumentView&) {
    std::array<double, 3> axis{};
    for (std::size_t index = 0; index < axis.size(); ++index)
        axis[index] = value.end[index] - value.start[index];
    const double length2 = length_squared(axis);
    if (!std::isfinite(length2) || length2 <= 0)
        invalid("Geometry line requires two distinct finite endpoints", value.id.value);
}
void validate_record(const Mesh& value, const DocumentView& view) {
    if (value.origin == "geometry") {
        if (!value.geometry || !value.geometry_revision)
            invalid("Derived mesh requires geometry provenance", value.id.value);
        const auto geometry = view.find<GeometryLine>(*value.geometry);
        if (!geometry)
            invalid("Mesh geometry is missing", value.id.value);
        if (!value.stale &&
            geometry->get<GeometryLine>().geometry_revision != value.geometry_revision)
            invalid("Outdated geometry binding must be marked stale", value.id.value);
    } else if (value.geometry || value.geometry_revision || value.stale) {
        invalid("Non-derived mesh cannot claim geometry provenance", value.id.value);
    }
}

void validate_relations(const DocumentView& view) {
    // These indexes borrow strings from the immutable view; they do not copy model records.
    std::map<std::string_view, std::string_view, std::less<>> beam_owners;
    visit<Part>(view, [&](const auto& part) {
        for (const auto& member : part.members) {
            const auto record = view.find_identity(member.value);
            if (record && record->key().type == RecordTraits<Beam>::type_id &&
                !beam_owners.emplace(member.value, part.id.value).second)
                invalid("Beam belongs to more than one part", member.value);
        }
    });
    Graph assemblies;
    std::set<std::string_view> assembly_children;
    visit<Assembly>(view, [&](const auto& assembly) {
        for (const auto& child : assembly.children) {
            if (!assembly_children.insert(child.value).second)
                invalid("Assembly child has more than one parent", child.value);
            const auto record = view.find_identity(child.value);
            if (record && record->key().type == RecordTraits<Assembly>::type_id)
                assemblies[assembly.id.value].push_back(child.value);
        }
    });
    require_acyclic(assemblies, "assemblies");
    visit<EntitySet>(view, [&](const auto& set) {
        std::optional<RecordTypeId> member_type;
        for (const auto& member : set.members) {
            const auto record = view.find_identity(member.value);
            if (!record)
                invalid("Set member is missing", member.value);
            if (member_type && *member_type != record->key().type)
                invalid("Entity set mixes record types", set.id.value);
            member_type = record->key().type;
        }
    });
    Graph includes;
    std::set<std::string_view> paths;
    std::map<std::string_view, std::string_view, std::less<>> include_owners;
    std::size_t roots = 0;
    visit<IncludeDocument>(view, [&](const auto& include) {
        if (!paths.insert(include.path).second)
            invalid("Include path is duplicated", include.id.value);
        if (include.parent)
            includes[include.parent->value].push_back(include.id.value);
        else
            ++roots;
        for (const auto& member : include.members)
            if (!include_owners.emplace(member.value, include.id.value).second)
                invalid("Entity belongs to more than one include", member.value);
    });
    if (view.count(RecordTraits<IncludeDocument>::type_id) && roots != 1)
        invalid("Include tree requires exactly one root", "includes");
    require_acyclic(includes, "includes");
    std::set<std::tuple<std::string_view, std::string_view, std::uint64_t>> source_numbers;
    std::set<std::pair<std::string_view, std::string_view>> source_entities;
    visit<SourceIdentifier>(view, [&](const auto& source) {
        if (!source_numbers.emplace(source.source_model_id, source.name_space, source.number)
                 .second)
            invalid("Duplicate source number within namespace", source.id.value);
        if (!source_entities.emplace(source.source_model_id, source.entity.value).second)
            invalid("Entity has duplicate source identifiers", source.id.value);
        const auto owner = include_owners.find(source.entity.value);
        if (owner == include_owners.end() || owner->second != source.include.value)
            invalid("Source include does not own the entity", source.id.value);
    });
}
} // namespace qcae::records

namespace qcae {
std::shared_ptr<const RecordRegistry> make_record_registry() {
    auto registry = std::make_shared<RecordRegistry>();
    for (auto descriptor : generated_record_descriptors())
        registry->add(std::move(descriptor));
    registry->add_rule(records::validate_relations);
    registry->freeze();
    return registry;
}
} // namespace qcae
