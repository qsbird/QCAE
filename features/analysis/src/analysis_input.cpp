#include "qcae/analysis_input.hpp"
#include "qcae/records.hpp"
#include <algorithm>
#include <map>
#include <set>
#include <tuple>

namespace qcae::features::analysis {
namespace {
[[noreturn]] void invalid(ErrorCode code, const char* message, std::string field = {}) {
    throw RecordError(code, message, std::move(field));
}
std::vector<std::string> identities(std::span<const EntityId> ids) {
    std::vector<std::string> result;
    result.reserve(ids.size());
    for (const auto& id : ids)
        result.push_back(id.value);
    std::sort(result.begin(), result.end());
    return result;
}
struct PhysicalInput {
    std::vector<std::string> fields;
    std::map<EntityId, std::string> namespaces;
    void add(const EntityId& id, std::string name_space, std::vector<std::string> values) {
        namespaces.emplace(id, name_space);
        fields.push_back(record_wire::strings(values));
    }
};
template <class T, class Function>
void visit(const DocumentView& view, PhysicalInput& input, Function function) {
    view.visit(RecordTraits<T>::type_id,
               [&](const Record& record) { function(record->get<T>(), input); });
}
void validate_map(const PhysicalInput& input, std::span<const ExportIdentifier> mapping) {
    if (mapping.size() != input.namespaces.size())
        invalid(ErrorCode::invalid_input,
                "Export identity map does not cover the physical input",
                "identities");
    std::set<EntityId> entities;
    std::set<std::pair<std::string, std::uint64_t>> numbers;
    for (const auto& item : mapping) {
        const auto expected = input.namespaces.find(item.entity);
        if (expected == input.namespaces.end() || expected->second != item.name_space ||
            !item.number || !entities.insert(item.entity).second ||
            !numbers.emplace(item.name_space, item.number).second)
            invalid(ErrorCode::invalid_input,
                    "Export identity map is not a typed number/entity bijection",
                    "identities");
    }
}
} // namespace
Result<FrozenAnalysisInput> freeze_analysis_input(const DocumentView& view,
                                                  const EntityId& identity,
                                                  const ProfileRef& profile,
                                                  std::span<const ExportIdentifier> mapping) {
    try {
        if (view.count(RecordTraits<records::Tri3>::type_id))
            invalid(ErrorCode::unsupported_capability,
                    "Tri3 shell analysis is not implemented",
                    "tri3");
        const auto record = view.find<records::AnalysisDefinition>(identity);
        if (!record)
            invalid(
                ErrorCode::entity_not_found, "Analysis definition does not exist", "analysis_id");
        const auto& analysis = record->get<records::AnalysisDefinition>();
        if (analysis.target.profile != profile || analysis.target.analysis_kind != "linear_static")
            invalid(ErrorCode::schema_unsupported,
                    "Frozen analysis target does not match the requested static profile",
                    "expected_profile");
        FrozenAnalysisInput frozen;
        frozen.version = view.version();
        frozen.analysis = identity;
        frozen.target = analysis.target;
        std::vector<EntityId> forces = analysis.forces, constraints = analysis.constraints;
        if (analysis.load_cases) {
            if (analysis.load_cases->size() != 1)
                invalid(ErrorCode::missing_input,
                        "Frozen static input requires exactly one load case",
                        "load_cases");
            frozen.load_case = analysis.load_cases->front();
            const auto load_case = view.find<records::LoadCase>(frozen.load_case);
            if (!load_case)
                invalid(
                    ErrorCode::invalid_input, "Frozen analysis load case is missing", "load_cases");
            forces = load_case->get<records::LoadCase>().forces;
            constraints = load_case->get<records::LoadCase>().constraints;
        }
        if (forces.size() != 1 || constraints.size() != 1)
            invalid(ErrorCode::missing_input,
                    "Frozen controlled input requires one load and one constraint",
                    "load_cases");
        PhysicalInput input;
        input.fields = {"QCAE-PHYSICAL-INPUT-1",
                        identity.value,
                        frozen.load_case.value,
                        analysis.target.analysis_kind,
                        record_wire::profile(profile),
                        "mm-N-MPa",
                        record_wire::strings(identities(forces)),
                        record_wire::strings(identities(constraints))};
        visit<records::Node>(view, input, [](const auto& value, auto& physical) {
            physical.add(
                value.id, "GRID", {"GRID", value.id.value, record_wire::vector3(value.position)});
        });
        visit<records::Material>(view, input, [](const auto& value, auto& physical) {
            physical.add(value.id,
                         "MAT1",
                         {"MAT1",
                          value.id.value,
                          record_wire::real(value.young_modulus_mpa),
                          value.poisson_ratio ? "present" : "absent",
                          value.poisson_ratio ? record_wire::real(*value.poisson_ratio) : ""});
        });
        visit<records::BeamSection>(view, input, [](const auto& value, auto& physical) {
            physical.add(value.id,
                         "PBAR",
                         {"PBAR",
                          value.id.value,
                          value.material.value,
                          record_wire::real(value.area_mm2),
                          record_wire::real(value.i1_mm4),
                          record_wire::real(value.i2_mm4),
                          record_wire::real(value.torsion_mm4)});
        });
        visit<records::Beam>(view, input, [](const auto& value, auto& physical) {
            if (!value.section)
                invalid(ErrorCode::missing_input, "Frozen beam requires a section", value.id.value);
            physical.add(value.id,
                         "CBAR",
                         {"CBAR",
                          value.id.value,
                          value.section->value,
                          value.nodes[0].value,
                          value.nodes[1].value,
                          record_wire::vector3(value.orientation)});
        });
        for (const auto& force : forces) {
            const auto image = view.find<records::NodalForce>(force);
            if (!image)
                invalid(ErrorCode::invalid_input, "Frozen load reference is missing", force.value);
            const auto& value = image->get<records::NodalForce>();
            input.add(
                value.id,
                "FORCE",
                {"FORCE", value.id.value, value.node.value, record_wire::vector3(value.force_n)});
        }
        for (const auto& constraint : constraints) {
            const auto image = view.find<records::Constraint>(constraint);
            if (!image)
                invalid(ErrorCode::invalid_input,
                        "Frozen constraint reference is missing",
                        constraint.value);
            const auto& value = image->get<records::Constraint>();
            input.add(value.id,
                      "SPC1",
                      {"SPC1",
                       value.id.value,
                       record_wire::strings(identities(value.nodes)),
                       value.dofs});
        }
        validate_map(input, mapping);
        // Sort record encodings separately from the fixed header. Neither storage
        // order nor a solver numbering permutation changes the physical signature.
        std::sort(input.fields.begin() + 8, input.fields.end());
        frozen.input_signature = record_wire::strings(input.fields);
        frozen.identities.assign(mapping.begin(), mapping.end());
        std::sort(frozen.identities.begin(),
                  frozen.identities.end(),
                  [](const auto& left, const auto& right) {
                      return std::tie(left.name_space, left.number, left.entity) <
                             std::tie(right.name_space, right.number, right.entity);
                  });
        return {Status::success, std::move(frozen), {}};
    } catch (const RecordError& error) {
        return {error.code() == ErrorCode::missing_input ? Status::needs_input : Status::failed,
                {},
                Diagnostic{error.code(), error.what(), error.field()}};
    }
}
} // namespace qcae::features::analysis
