#include "qcae/static_validation.hpp"
#include "qcae/records.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace qcae::features::results {
namespace {
using EncodedRecord = std::vector<std::string>;
constexpr double modulus = 210000.0, inertia = 833.333, length = 1000.0, force = 1.0;
[[noreturn]] void invalid(const char* message, std::string field) {
    throw RecordError(ErrorCode::invalid_input, message, std::move(field));
}
[[noreturn]] void unsupported() {
    throw RecordError(ErrorCode::unsupported_capability,
                      "Frozen physics differs from the preregistered v3 cantilever reference.",
                      "source_input");
}
void require_reference(bool condition) {
    if (!condition)
        unsupported();
}
struct Reference {
    std::map<EntityId, double> nodes;
    std::map<EntityId, std::uint64_t> numbers;
    EntityId support;
};
Reference reference(const analysis::FrozenAnalysisInput& input, const ProfileRef& registered) {
    require_reference(input.input_signature.size() <= 65536 && input.identities.size() == 45);
    // Reuse the frozen-input codec's complete header, namespace and bijection validation.
    (void)analysis::encode_frozen_analysis_input(input);
    require_reference(input.target.profile == registered &&
                      input.target.profile.profile_id == "qcae.nastran.linear-static" &&
                      input.target.profile.profile_version == "0.2.0");
    const auto fields = record_wire::read_strings(input.input_signature);
    std::map<std::string, std::vector<EncodedRecord>> records;
    for (std::size_t index = 8; index < fields.size(); ++index) {
        auto row = record_wire::read_strings(fields[index]);
        records[row[0]].push_back(std::move(row));
    }
    require_reference(records["GRID"].size() == 21 && records["CBAR"].size() == 20 &&
                      records["MAT1"].size() == 1 && records["PBAR"].size() == 1 &&
                      records["FORCE"].size() == 1 && records["SPC1"].size() == 1);
    const auto& material = records["MAT1"].front();
    const auto& section = records["PBAR"].front();
    require_reference(record_wire::read_real(material[2]) == modulus && material[3] == "present" &&
                      record_wire::read_real(material[4]) == .3 && section[2] == material[1] &&
                      record_wire::read_real(section[3]) == 100.0 &&
                      record_wire::read_real(section[4]) == inertia &&
                      record_wire::read_real(section[5]) == inertia &&
                      record_wire::read_real(section[6]) == 1400.0);
    Reference result;
    std::map<double, EntityId> positions;
    for (const auto& row : records["GRID"]) {
        const auto position = record_wire::read_vector3(row[2]);
        const double x = position[0];
        require_reference(std::isfinite(x) && x >= 0 && x <= length && std::fmod(x, 50.0) == 0 &&
                          position[1] == 0 && position[2] == 0 &&
                          positions.emplace(x, EntityId(row[1])).second);
        result.nodes.emplace(EntityId(row[1]), x);
    }
    std::set<std::pair<double, double>> edges;
    for (const auto& row : records["CBAR"]) {
        const auto first = result.nodes.find(EntityId(row[3]));
        const auto second = result.nodes.find(EntityId(row[4]));
        require_reference(row[2] == section[1] && first != result.nodes.end() &&
                          second != result.nodes.end() && second->second - first->second == 50.0 &&
                          record_wire::read_vector3(row[5]) == std::array<double, 3>{0, 1, 0} &&
                          edges.emplace(first->second, second->second).second);
    }
    result.support = positions.at(0);
    const auto& load = records["FORCE"].front();
    const auto& constraint = records["SPC1"].front();
    require_reference(load[2] == positions.at(length).value &&
                      record_wire::read_vector3(load[3]) == std::array<double, 3>{0, -force, 0} &&
                      record_wire::read_strings(constraint[2]) ==
                          std::vector<std::string>{result.support.value} &&
                      constraint[3] == "123456" &&
                      record_wire::read_strings(fields[6]) == std::vector<std::string>{load[1]} &&
                      record_wire::read_strings(fields[7]) ==
                          std::vector<std::string>{constraint[1]});
    for (const auto& identity : input.identities)
        if (identity.name_space == "GRID")
            result.numbers.emplace(identity.entity, identity.number);
    return result;
}
void validate_fields(const StaticFields& fields, const Reference& expected) {
    if (fields.subcase != 1 || fields.coordinate_basis != "basic" ||
        fields.displacement_units !=
            std::array<std::string, 6>{"mm", "mm", "mm", "rad", "rad", "rad"} ||
        fields.reaction_units != std::array<std::string, 6>{"N", "N", "N", "N*mm", "N*mm", "N*mm"})
        invalid("Result case, coordinates or six-component units differ from the reference.",
                "result_metadata");
    if (fields.displacements.size() != expected.nodes.size() || fields.spc_reactions.size() != 1)
        invalid("The reference requires all 21 GRID displacements and one support reaction.",
                "result_fields");
    auto validate_rows = [&](const auto& rows, bool reaction) {
        std::set<EntityId> seen;
        for (const auto& row : rows) {
            const auto number = expected.numbers.find(row.entity);
            if (number == expected.numbers.end() || number->second != row.solver_number ||
                !seen.insert(row.entity).second || (reaction && row.entity != expected.support) ||
                !std::all_of(row.components.begin(), row.components.end(), [](double value) {
                    return std::isfinite(value);
                }))
                invalid("Result row has an invalid frozen identity, number or component.",
                        "result_fields");
        }
    };
    validate_rows(fields.displacements, false);
    validate_rows(fields.spc_reactions, true);
}
void append(StaticComparisonReport& report,
            const StaticGridValue& row,
            bool reaction,
            const std::array<double, 6>& expected) {
    constexpr std::array<const char*, 6> names{"T1", "T2", "T3", "R1", "R2", "R3"};
    for (std::size_t index = 0; index < 6; ++index) {
        const bool rotational = index >= 3;
        const double absolute =
            reaction ? (rotational ? .001 : .000001) : (rotational ? .00000001 : .00001);
        const double error = std::abs(row.components[index] - expected[index]);
        const double tolerance = absolute + .0001 * std::abs(expected[index]);
        const bool matched = error <= tolerance;
        report.matched = report.matched && matched;
        report.components.push_back(
            {row.entity,
             row.solver_number,
             reaction ? "spc_reaction" : "displacement",
             names[index],
             reaction ? (rotational ? "N*mm" : "N") : (rotational ? "rad" : "mm"),
             row.components[index],
             expected[index],
             error,
             tolerance,
             matched});
    }
}
} // namespace
Result<StaticComparisonReport>
compare_preregistered_cantilever(const analysis::FrozenAnalysisInput& input,
                                 const StaticFields& fields,
                                 const ProfileRef& registered_reference_profile) {
    try {
        const auto expected = reference(input, registered_reference_profile);
        validate_fields(fields, expected);
        StaticComparisonReport report;
        report.matched = true;
        report.components.reserve(132);
        // Sorting gives a deterministic report independent of parser row order.
        std::map<EntityId, const StaticGridValue*> rows;
        for (const auto& row : fields.displacements)
            rows.emplace(row.entity, &row);
        for (const auto& [entity, row] : rows) {
            const double x = expected.nodes.at(entity);
            append(report,
                   *row,
                   false,
                   {0,
                    -force * x * x * (3 * length - x) / (6 * modulus * inertia),
                    0,
                    0,
                    0,
                    -force * x * (2 * length - x) / (2 * modulus * inertia)});
        }
        append(report, fields.spc_reactions.front(), true, {0, force, 0, 0, 0, force * length});
        return {Status::success, std::move(report), {}};
    } catch (const RecordError& error) {
        return {Status::failed, {}, Diagnostic{error.code(), error.what(), error.field()}};
    }
}
} // namespace qcae::features::results
