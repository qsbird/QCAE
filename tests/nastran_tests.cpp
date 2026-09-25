#include "qcae/nastran_codec.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace qcae;
namespace {
void check(bool value, const std::string& reason) {
    if (!value)
        throw std::runtime_error(reason);
}
bool has(const std::vector<FormatIssue>& issues, const std::string& code) {
    for (const auto& issue : issues)
        if (issue.code == code)
            return true;
    return false;
}
std::string fixture(const std::string& name) {
    const auto path = std::filesystem::path(__FILE__).parent_path() / "fixtures" / "nastran" / name;
    std::ifstream stream(path);
    check(stream.good(), "fixture unavailable: " + path.string());
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
ImportRequest sample(const NastranCodec& codec) {
    return {"cantilever.bdf",
            {{"cantilever.bdf", fixture("cantilever.bdf")},
             {"mesh/nodes.bdf", fixture("mesh/nodes.bdf")},
             {"mesh/beams.bdf", fixture("mesh/beams.bdf")},
             {"properties.bdf", fixture("properties.bdf")}},
            codec.definition().reference,
            "example-model",
            "mm-N-MPa"};
}
void round_trip() {
    NastranCodec codec;
    check(codec.definition().solver_family == "Nastran" &&
              codec.definition().analysis_kind == "linear_static" &&
              !codec.definition().configured && !codec.definition().validated,
          "profile metadata");
    const auto imported = codec.decode(sample(codec));
    if (!imported.candidate) {
        for (const auto& issue : imported.report.issues)
            std::cerr << issue.code << " " << issue.resource << ":" << issue.line << " "
                      << issue.message << "\n";
    }
    check(imported.candidate && imported.report.complete, "sample import");
    const auto& model = *imported.candidate;
    check(model.includes.size() == 4 && model.parts.empty(), "include organization only");
    check(model.nodes.size() == 2 && model.materials.size() == 1 && model.beams.size() == 1,
          "physical entities");
    check(model.nodes.front().id != model.materials.front().id, "independent number namespaces");
    check(model.sources.size() == 7, "source mapping retained");
    const auto exported =
        codec.encode(model, model.analyses.front().id, codec.definition().reference);
    check(exported.artifact && exported.report.complete, "artifact generation");
    check(exported.artifact->resources.size() == 4 && exported.artifact->identities.size() == 7,
          "resources and frozen identity map");
    for (const auto& resource : exported.artifact->resources)
        if (resource.path == "mesh/beams.bdf")
            check(resource.text.find("CBAR,1,1,1,2,0.0,") != std::string::npos,
                  "CBAR orientation exported as floating vector");
    ImportRequest again{exported.artifact->root_resource,
                        exported.artifact->resources,
                        codec.definition().reference,
                        "example-model",
                        "mm-N-MPa"};
    const auto decoded = codec.decode(again);
    check(decoded.candidate && decoded.report.complete, "exported deck reimports");
    check(decoded.candidate->nodes == model.nodes &&
              decoded.candidate->sections == model.sections &&
              decoded.candidate->beams == model.beams &&
              decoded.candidate->materials == model.materials,
          "structural semantics survive round trip");
    check(decoded.candidate->forces.front().force_n == model.forces.front().force_n,
          "force semantics survive round trip");
}
void rejection() {
    NastranCodec codec;
    auto base = sample(codec);
    auto bad = base;
    bad.unit_system.clear();
    check(!codec.decode(bad).candidate && has(codec.decode(bad).report.issues, "unit_system"),
          "units required");
    bad = base;
    bad.source_profile.profile_version = "9";
    check(!codec.decode(bad).candidate && has(codec.decode(bad).report.issues, "profile_mismatch"),
          "profile mismatch");
    bad = base;
    bad.resources[0].text.insert(bad.resources[0].text.find("ENDDATA"), "FOO,1,2\n");
    check(!codec.decode(bad).candidate && has(codec.decode(bad).report.issues, "unsupported_card"),
          "unknown card rejected");
    bad = base;
    bad.resources[0].text.insert(bad.resources[0].text.find("BEGIN BULK"), "SPC = 20\n");
    check(!codec.decode(bad).candidate &&
              has(codec.decode(bad).report.issues, "unsupported_control"),
          "repeated SPC rejected");
    bad = base;
    bad.resources[1].text += "GRID,1,,9,9,9\n";
    check(!codec.decode(bad).candidate && has(codec.decode(bad).report.issues, "duplicate_number"),
          "cross-resource duplicates");
    bad = base;
    bad.resources[2].text = "INCLUDE 'nodes.bdf'\n";
    check(!codec.decode(bad).candidate && has(codec.decode(bad).report.issues, "include_cycle"),
          "include cycle");
    bad = base;
    bad.resources[0].text.insert(bad.resources[0].text.find("ENDDATA"),
                                 "INCLUDE '../outside.bdf'\n");
    check(!codec.decode(bad).candidate && has(codec.decode(bad).report.issues, "include_path"),
          "traversal rejected");
    bad = base;
    bad.resources[2].text = "CBAR,1,1,1,999,0,0,1\n";
    check(!codec.decode(bad).candidate &&
              has(codec.decode(bad).report.issues, "unsupported_orientation"),
          "integer G0 rejected");
    bad = base;
    bad.resources[2].text = "CBAR,1,1,1,999,0.0,0,1\n";
    check(!codec.decode(bad).candidate && has(codec.decode(bad).report.issues, "invalid_model"),
          "missing reference rejected");
    bad = base;
    bad.resources.push_back({"unreferenced.bdf", "GRID,99,,0,0,0\n"});
    check(!codec.decode(bad).candidate && has(codec.decode(bad).report.issues, "unused_resource"),
          "unreferenced bundle rejected");
    bad = base;
    bad.resources[1].path = "mesh/unsafe\t.bdf";
    check(!codec.decode(bad).candidate && has(codec.decode(bad).report.issues, "resource_path"),
          "control-character path rejected");
    bad = base;
    bad.resources[3].text = "MAT1,1,210000,,0.3\nPBAR,1,1,100,833.333,1000,2000,0,1\n";
    check(!codec.decode(bad).candidate && has(codec.decode(bad).report.issues, "unsupported_field"),
          "PBAR extra fields rejected");
    bad = base;
    bad.resources[3].text = "MAT1,1,210000,,0.3\nPBAR,1,1,100,833.333333,1000,2000\n";
    check(!codec.decode(bad).candidate &&
              has(codec.decode(bad).report.issues, "unsupported_precision"),
          "long free-field real rejected");
    bad = base;
    bad.resources[3].text = "MAT1,1,210000,80769.2,0.3\nPBAR,1,1,100,833.333,1000,2000\n";
    check(!codec.decode(bad).candidate && has(codec.decode(bad).report.issues, "inconsistent_mat1"),
          "rounded explicit G rejected");
    bad = base;
    bad.resources[3].text = "MAT1,1,208000,80000,0.3\nPBAR,1,1,100,833.333,1000,2000\n";
    check(codec.decode(bad).candidate.has_value(), "exact explicit G accepted");
    bad = base;
    bad.resources[3].text = "MAT1,1,1e308,,-.99\nPBAR,1,1,100,833.333,1000,2000\n";
    check(!codec.decode(bad).candidate && has(codec.decode(bad).report.issues, "invalid_physics"),
          "derived G overflow rejected");
    bad = base;
    const std::string force_line = "FORCE,10,2,0,100,0,-1,0";
    bad.resources[0].text.replace(bad.resources[0].text.find(force_line),
                                  force_line.size(),
                                  "FORCE,10,2,0,1e-200,0,1e-150,0");
    check(!codec.decode(bad).candidate && has(codec.decode(bad).report.issues, "invalid_physics"),
          "force underflow rejected");
    auto imported = codec.decode(base);
    check(imported.candidate.has_value(), "baseline needed");
    auto model = *imported.candidate;
    model.parts.push_back({EntityId{"part"}, "Part", {model.beams.front().id}});
    auto result = codec.encode(model, model.analyses.front().id, codec.definition().reference);
    check(result.artifact && has(result.report.issues, "organization_loss"),
          "organization loss reported");
    model.materials.front().poisson_ratio.reset();
    result = codec.encode(model, model.analyses.front().id, codec.definition().reference);
    check(!result.artifact && has(result.report.issues, "material_incomplete"),
          "incomplete material blocks artifact");
    model = *imported.candidate;
    model.constraints.front().dofs = "7";
    result = codec.encode(model, model.analyses.front().id, codec.definition().reference);
    check(!result.artifact && has(result.report.issues, "invalid_dofs"),
          "unsupported constraint blocks artifact");
    model = *imported.candidate;
    model.sections.front().i1_mm4 = 833.333333;
    result = codec.encode(model, model.analyses.front().id, codec.definition().reference);
    check(!result.artifact && has(result.report.issues, "precision_unrepresentable"),
          "unrepresentable real blocks artifact");
    model = *imported.candidate;
    model.nodes.front().position.x = 1234567;
    result = codec.encode(model, model.analyses.front().id, codec.definition().reference);
    if (!result.artifact)
        for (const auto& issue : result.report.issues)
            std::cerr << issue.code << " " << issue.message << "\n";
    check(result.artifact.has_value(), "seven-digit integer real can use trailing decimal point");
    ImportRequest wide_coordinate{result.artifact->root_resource,
                                  result.artifact->resources,
                                  codec.definition().reference,
                                  "example-model",
                                  "mm-N-MPa"};
    check(codec.decode(wide_coordinate).candidate.has_value(), "trailing-point real reimports");
    model = *imported.candidate;
    model.nodes.front().position.x = 12345678;
    result = codec.encode(model, model.analyses.front().id, codec.definition().reference);
    check(result.artifact.has_value(), "eight-digit real uses integer spelling outside CBAR X1");
    model = *imported.candidate;
    model.nodes.front().position.x = 1.234e-4;
    result = codec.encode(model, model.analyses.front().id, codec.definition().reference);
    check(result.artifact.has_value(), "compact scientific exponent exports exactly");
    model = *imported.candidate;
    model.materials.front().young_modulus_mpa = 1e308;
    model.materials.front().poisson_ratio = -0.99;
    result = codec.encode(model, model.analyses.front().id, codec.definition().reference);
    check(!result.artifact && has(result.report.issues, "invalid_physics"),
          "export derived G overflow rejected");
    model = *imported.candidate;
    model.sources.front().name_space = "FOO";
    result = codec.encode(model, model.analyses.front().id, codec.definition().reference);
    check(!result.artifact && has(result.report.issues, "source_namespace"),
          "mislabeled source cannot silently renumber");
    model = *imported.candidate;
    const auto original_owner = model.includes[1].id;
    const EntityId added{"added-node"};
    model.nodes.push_back({added, {150, 0, 0}});
    for (auto& inc : model.includes)
        if (inc.id == original_owner)
            inc.members.push_back(added);
    result = codec.encode(model, model.analyses.front().id, codec.definition().reference);
    check(result.artifact.has_value(), "new member exports");
    for (const auto& resource : result.artifact->resources)
        if (resource.path == "mesh/nodes.bdf")
            check(resource.text.find("GRID,3,,150.0,0.0,0.0") != std::string::npos,
                  "new include membership determines output resource");
    model = *imported.candidate;
    model.includes[1].path = "bad'path.bdf";
    result = codec.encode(model, model.analyses.front().id, codec.definition().reference);
    check(!result.artifact && has(result.report.issues, "include_path"),
          "unsafe export include blocked");
    model = *imported.candidate;
    EntityId parent = model.includes.front().id;
    for (int i = 1; i < 11; ++i) {
        EntityId next{"nested-" + std::to_string(i)};
        model.includes.push_back({next, "nested-" + std::to_string(i) + ".bdf", parent, {}});
        parent = next;
    }
    result = codec.encode(model, model.analyses.front().id, codec.definition().reference);
    check(!result.artifact && has(result.report.issues, "resource_limit"),
          "export include depth guarded");
}
} // namespace
int main() {
    try {
        round_trip();
        rejection();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
