#include "qcae/result_fixture.hpp"
#include "qcae/quantities.hpp"
#include <algorithm>
#include <map>
#include <set>

namespace qcae::features::results {
namespace {
Result<ResultBundle> bad(ErrorCode code, const char* message, const char* field) {
    return {code == ErrorCode::missing_input ? Status::needs_input : Status::failed,
            {},
            Diagnostic{code, message, field}};
}
bool same_map(std::span<const ExportIdentifier> left, std::span<const ExportIdentifier> right) {
    if (left.size() != right.size())
        return false;
    std::map<EntityId, std::pair<std::string_view, std::uint64_t>> values;
    for (const auto& item : left)
        if (!values
                 .emplace(item.entity,
                          std::pair<std::string_view, std::uint64_t>{item.name_space, item.number})
                 .second)
            return false;
    std::set<EntityId> seen;
    for (const auto& item : right) {
        const auto found = values.find(item.entity);
        if (!seen.insert(item.entity).second || found == values.end() ||
            found->second !=
                std::pair<std::string_view, std::uint64_t>{item.name_space, item.number})
            return false;
    }
    return true;
}
} // namespace
Result<ResultBundle> FixtureResultReader::read(const analysis::FrozenAnalysisInput& input,
                                               const ResultFixture& fixture) const {
    try {
        (void)analysis::encode_frozen_analysis_input(input);
    } catch (const RecordError& error) {
        return {Status::failed, {}, Diagnostic{error.code(), error.what(), error.field()}};
    }
    if (fixture.input_fingerprint.empty() ||
        fixture.input_fingerprint != analysis::physical_signature_hex(input))
        return bad(ErrorCode::invalid_input,
                   "Result input fingerprint does not match the frozen input",
                   "input_fingerprint");
    if (fixture.quantity != "displacement" ||
        fixture.components != std::vector<std::string>{"X", "Y", "Z"})
        return bad(ErrorCode::unsupported_capability,
                   "Fixture reader requires displacement components X/Y/Z",
                   "quantity");
    if (fixture.unit != "mm")
        return bad(
            ErrorCode::invalid_unit, "Fixture displacement must declare millimeters", "unit");
    if (fixture.location != "node" || fixture.coordinate_basis != "global")
        return bad(
            ErrorCode::invalid_input, "Fixture reader requires global nodal values", "location");
    if (fixture.case_label != input.case_label || !fixture.frame || *fixture.frame != 0)
        return bad(ErrorCode::invalid_input,
                   "Fixture case/frame does not match the frozen static case",
                   "case");
    if (fixture.source_kind != "fixture")
        return bad(ErrorCode::invalid_input,
                   "Fixture source must remain explicitly marked fixture",
                   "source_kind");
    if (!same_map(input.identities, fixture.identities))
        return bad(ErrorCode::invalid_input,
                   "Result identity map differs from the frozen export map",
                   "identities");
    std::map<std::uint64_t, EntityId> nodes;
    for (const auto& item : input.identities)
        if (item.name_space == "GRID")
            nodes.emplace(item.number, item.entity);
    if (fixture.values.empty())
        return bad(
            ErrorCode::missing_input, "Fixture contains no node displacement values", "values");
    ResultBundle bundle;
    bundle.input = input;
    bundle.field = {fixture.quantity,
                    fixture.unit,
                    fixture.components,
                    fixture.location,
                    fixture.coordinate_basis,
                    fixture.case_label,
                    *fixture.frame,
                    {}};
    std::set<std::uint64_t> seen;
    for (const auto& value : fixture.values) {
        const auto found = nodes.find(value.solver_number);
        if (found == nodes.end() || !seen.insert(value.solver_number).second)
            return bad(ErrorCode::invalid_input,
                       "Result GRID number is absent or duplicated",
                       "solver_number");
        for (double component : value.value) {
            const auto checked = parameters::canonical_quantity({component, fixture.unit},
                                                                parameters::Dimension::length);
            if (!checked.ok())
                return {checked.status, {}, checked.error};
        }
        bundle.field.values.push_back({found->second, value.value});
    }
    return {Status::success, std::move(bundle), {}};
}
ResultState result_state(const ResultBundle& bundle, const DocumentView& current) {
    if (bundle.source_kind != "fixture" ||
        bundle.reader_version != "qcae.fixture.displacement.v1" ||
        bundle.input.version.document.id != current.version().document.id)
        return ResultState::stale;
    const auto input = analysis::freeze_analysis_input(
        current, bundle.input.analysis, bundle.input.target.profile, bundle.input.identities);
    return input.ok() && input.value->input_signature == bundle.input.input_signature
               ? ResultState::current
               : ResultState::stale;
}
} // namespace qcae::features::results
