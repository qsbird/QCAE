#include "qcae/static_validation.hpp"
#include "qcae/nastran_codec.hpp"
#include "qcae/records.hpp"
#include "qcae/records_model_bridge.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>

namespace {
using namespace qcae;
namespace analysis = features::analysis;
namespace results = features::results;
void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
template <class T> T good(Result<T> result) {
    if (!result.ok())
        throw std::runtime_error(result.error ? result.error->message : "Missing result");
    return std::move(*result.value);
}
analysis::FrozenAnalysisInput frozen_benchmark() {
    NastranCodec codec;
    ImportRequest request;
    request.root_resource = "cantilever.bdf";
    request.source_profile = codec.definition().reference;
    request.source_model_id = "synthetic-comparison-input";
    request.unit_system = "mm-N-MPa";
    for (const char* name : {"cantilever.bdf", "nodes.bdf", "properties.bdf", "beams.bdf"}) {
        std::ifstream stream(std::string(QCAE_SOURCE_DIR) +
                             "/tests/fixtures/nastran-real-benchmark-v3/" + name);
        check(stream.good(), "Preregistered resource is missing");
        request.resources.push_back({name, {std::istreambuf_iterator<char>(stream), {}}});
    }
    const auto imported = codec.decode(request);
    check(imported.candidate && imported.report.complete && imported.report.issues.empty(),
          "Preregistered input must pass the production codec");
    const auto& model = *imported.candidate;
    check(model.analyses.size() == 1, "Reference has one analysis");
    const auto exported = codec.encode(model, model.analyses.front().id, request.source_profile);
    if (!exported.artifact || !exported.report.complete)
        for (const auto& issue : exported.report.issues)
            std::cerr << issue.code << ": " << issue.message << '\n';
    check(exported.artifact && exported.report.complete &&
              std::none_of(exported.report.issues.begin(),
                           exported.report.issues.end(),
                           [](const auto& issue) { return issue.blocking; }),
          "The reference must produce a complete frozen export map");
    const auto view = records_from_model(
        model,
        make_record_registry(),
        {{DocumentId("original-document"), DocumentEpoch("original-epoch")}, 20});
    return good(analysis::freeze_analysis_input(
        view, model.analyses.front().id, request.source_profile, exported.artifact->identities));
}
results::StaticFields synthetic_fields(const analysis::FrozenAnalysisInput& frozen) {
    results::StaticFields fields;
    fields.subcase = 1;
    fields.coordinate_basis = "basic";
    fields.displacement_units = {"mm", "mm", "mm", "rad", "rad", "rad"};
    fields.reaction_units = {"N", "N", "N", "N*mm", "N*mm", "N*mm"};
    const auto signature = record_wire::read_strings(frozen.input_signature);
    for (std::size_t index = 8; index < signature.size(); ++index) {
        const auto row = record_wire::read_strings(signature[index]);
        if (row[0] != "GRID")
            continue;
        const double x = record_wire::read_vector3(row[2])[0];
        const auto number =
            std::find_if(frozen.identities.begin(), frozen.identities.end(), [&](const auto& item) {
                return item.entity.value == row[1];
            });
        check(number != frozen.identities.end(), "Every synthetic GRID has a frozen mapping");
        // Synthetic analytic fields only. These are never an actual solver result.
        const double uy = -x * x * (3000.0 - x) / (6.0 * 210000.0 * 833.333);
        const double rz = -x * (2000.0 - x) / (2.0 * 210000.0 * 833.333);
        fields.displacements.push_back({number->entity, number->number, {0, uy, 0, 0, 0, rz}});
        if (x == 0)
            fields.spc_reactions.push_back({number->entity, number->number, {0, 1, 0, 0, 0, 1000}});
    }
    return fields;
}
template <class Change>
analysis::FrozenAnalysisInput
alter_source(analysis::FrozenAnalysisInput input, std::string_view kind, Change change) {
    auto fields = record_wire::read_strings(input.input_signature);
    for (std::size_t index = 8; index < fields.size(); ++index) {
        auto row = record_wire::read_strings(fields[index]);
        if (row[0] == kind) {
            change(row);
            fields[index] = record_wire::strings(row);
            input.input_signature = record_wire::strings(fields);
            return input;
        }
    }
    throw std::runtime_error("The source mutation did not apply");
}
} // namespace
int main() {
    try {
        const auto input = frozen_benchmark();
        const auto fields = synthetic_fields(input);
        const auto compare = [&](const auto& source, const auto& values) {
            return results::compare_preregistered_cantilever(source, values, input.target.profile);
        };
        const auto report = good(compare(input, fields));
        check(report.matched && report.components.size() == 132,
              "All 132 preregistered components must be compared");
        const auto tip =
            std::max_element(fields.displacements.begin(),
                             fields.displacements.end(),
                             [](const auto& a, const auto& b) {
                                 return std::abs(a.components[1]) < std::abs(b.components[1]);
                             });
        check(std::abs(tip->components[1] + 1.9047626666669714) < 1e-14 &&
                  std::abs(tip->components[5] + .002857144000000457) < 1e-17,
              "Reference formulas agree with frozen decimal tip values");
        auto shuffled = fields;
        std::reverse(shuffled.displacements.begin(), shuffled.displacements.end());
        check(good(compare(input, shuffled)).matched,
              "Parser row ordering cannot change a comparison");
        auto wrong = fields;
        wrong.displacements[10].components[1] *= -1;
        check(!good(compare(input, wrong)).matched,
              "A wrong internal-node direction cannot pass a tip-only comparison");
        wrong = fields;
        wrong.displacements.front().components[2] = .00001;
        check(good(compare(input, wrong)).matched,
              "Zero components accept the preregistered inclusive absolute boundary");
        wrong.displacements.front().components[2] =
            std::nextafter(.00001, std::numeric_limits<double>::infinity());
        check(!good(compare(input, wrong)).matched,
              "A component just above the absolute tolerance must fail");
        wrong = fields;
        wrong.spc_reactions.front().components[5] = -1000;
        check(!good(compare(input, wrong)).matched,
              "Equal reaction norm with opposite sign cannot pass");
        for (unsigned variant = 0; variant < 8; ++variant) {
            wrong = fields;
            switch (variant) {
            case 0:
                wrong.subcase = 2;
                break;
            case 1:
                wrong.coordinate_basis = "local";
                break;
            case 2:
                wrong.displacement_units[3] = "mm";
                break;
            case 3:
                wrong.reaction_units[5] = "N";
                break;
            case 4:
                wrong.displacements.pop_back();
                break;
            case 5:
                wrong.displacements[1] = wrong.displacements[0];
                break;
            case 6:
                ++wrong.displacements[0].solver_number;
                break;
            case 7:
                wrong.displacements[0].components[0] = std::numeric_limits<double>::quiet_NaN();
                break;
            }
            check(!compare(input, wrong).ok(),
                  "Malformed metadata, mapping or components cannot produce a comparison");
        }
        const auto changed_modulus =
            alter_source(input, "MAT1", [](auto& row) { row[2] = record_wire::real(210001); });
        const auto changed_force = alter_source(
            input, "FORCE", [](auto& row) { row[3] = record_wire::vector3({0, 1, 0}); });
        const auto changed_constraint =
            alter_source(input, "SPC1", [](auto& row) { row[3] = "123"; });
        const auto changed_axis = alter_source(
            input, "CBAR", [](auto& row) { row[5] = record_wire::vector3({0, 0, 1}); });
        for (const auto& changed :
             {changed_modulus, changed_force, changed_constraint, changed_axis})
            check(!compare(changed, fields).ok(),
                  "The reference must reject different original physics");
        auto damaged = input;
        damaged.input_signature = "bad";
        check(!compare(damaged, fields).ok(),
              "Malformed frozen source must fail without a partial report");
        auto foreign_profile = input;
        foreign_profile.target.profile.definition_digest = "unregistered-same-id-version";
        auto signature = record_wire::read_strings(foreign_profile.input_signature);
        signature[4] = record_wire::profile(foreign_profile.target.profile);
        foreign_profile.input_signature = record_wire::strings(signature);
        check(!compare(foreign_profile, fields).ok(),
              "A consistent foreign profile digest cannot reuse the registered reference");
        auto renumbered = input;
        for (auto& item : renumbered.identities)
            if (item.name_space == "GRID")
                item.number += 100;
        auto remapped = fields;
        for (auto& row : remapped.displacements)
            row.solver_number += 100;
        for (auto& row : remapped.spc_reactions)
            row.solver_number += 100;
        check(good(compare(renumbered, remapped)).matched,
              "Frozen entity identity and solver numbering remain separate");
        check(!compare(renumbered, fields).ok(),
              "Original result numbers cannot be reused against another export map");
        std::cout << "PASS: synthetic 132-component comparison, frozen input and negative cases; "
                     "no actual solver or numerical acceptance\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
