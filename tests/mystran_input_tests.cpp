#include "mystran_input.hpp"
#include "qcae/nastran_codec.hpp"
#include "qcae/record_registry.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <tuple>

namespace {
using namespace qcae;
unsigned checks{};
void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
    ++checks;
}
std::string replace(std::string value, std::string_view before, std::string_view after) {
    const auto position = value.find(before);
    check(position != value.npos, "Input rejection mutation did not apply");
    value.replace(position, before.size(), after);
    return value;
}
void rejected(std::string_view root, const std::vector<TextResource>& resources) {
    try {
        ipc::detail::validate_mystran_input(root, resources);
    } catch (const RecordError& error) {
        check(error.code() == ErrorCode::invalid_input && error.field() == "solver_input",
              "MYSTRAN input rejection lost its structured field/code");
        return;
    }
    throw std::runtime_error("Unsupported MYSTRAN input was accepted");
}
std::vector<TextResource> benchmark(const NastranCodec& codec) {
    ImportRequest input;
    input.root_resource = "cantilever.bdf";
    input.source_profile = codec.definition().reference;
    input.source_model_id = "mystran-input-gate-benchmark";
    input.unit_system = "mm-N-MPa";
    for (const char* name : {"cantilever.bdf", "nodes.bdf", "properties.bdf", "beams.bdf"}) {
        std::ifstream stream(std::string(QCAE_SOURCE_DIR) +
                             "/tests/fixtures/nastran-real-benchmark-v3/" + name);
        check(stream.good(), "Preregistered v3 input is missing");
        input.resources.push_back({name, {std::istreambuf_iterator<char>(stream), {}}});
    }
    const auto imported = codec.decode(input);
    check(imported.candidate && imported.report.complete && imported.report.issues.empty(),
          "The unchanged v3 input no longer passes the shared production codec");
    rejected(input.root_resource, input.resources);
    const auto exported = codec.encode(
        *imported.candidate, imported.candidate->analyses.front().id, input.source_profile);
    check(exported.artifact && exported.report.complete &&
              std::none_of(exported.report.issues.begin(),
                           exported.report.issues.end(),
                           [](const auto& issue) { return issue.blocking; }),
          "The production codec did not produce the benchmark artifact");
    ipc::detail::validate_mystran_input(exported.artifact->root_resource,
                                        exported.artifact->resources);
    check(exported.artifact->profile == input.source_profile,
          "MYSTRAN admission changed the shared immutable ProfileRef");
    for (const double coordinate : {12345678.0, 1.0e20}) {
        auto model = *imported.candidate;
        model.nodes.back().position.x = coordinate;
        const auto compact = codec.encode(model, model.analyses.front().id, input.source_profile);
        check(compact.artifact && compact.report.complete,
              "Shared codec no longer emits its exact eight-character integer/exponent fallback");
        rejected(compact.artifact->root_resource, compact.artifact->resources);
    }
    return exported.artifact->resources;
}
} // namespace

int main() {
    try {
        const NastranCodec codec;
        const auto exported = benchmark(codec);
        const auto before = exported;
        ipc::detail::validate_mystran_input("cantilever.bdf", exported);
        check(exported.size() == before.size() && std::equal(exported.begin(),
                                                             exported.end(),
                                                             before.begin(),
                                                             [](const auto& a, const auto& b) {
                                                                 return a.path == b.path &&
                                                                        a.text == b.text;
                                                             }),
              "MYSTRAN admission rewrote verified published bytes");
        const std::string root = "SOL 101\nCEND\nSUBCASE 1\nSPC = 20\nLOAD = 10\n"
                                 "DISPLACEMENT = ALL\nSPCFORCES = ALL\nBEGIN BULK\n"
                                 "INCLUDE 'nodes.bdf'\nINCLUDE 'physics.bdf'\nENDDATA\n";
        const std::vector<TextResource> valid{
            {"model.bdf", root},
            {"nodes.bdf", "GRID,1,,0.0,0.0,0.0\nGRID,2,,1000.0,0.0,0.0\n"},
            {"physics.bdf",
             "MAT1,1,210000.0,,0.3\nPBAR,1,1,100.0,833.333,833.333,1400.0\n"
             "CBAR,1,1,1,2,0.0,0.0,1.0\nFORCE,10,2,0,1.0,0.0,-1.0,0.0\n"
             "SPC1,20,123456,1\n"}};
        ipc::detail::validate_mystran_input("model.bdf", valid);
        auto scientific = valid;
        scientific[2].text = replace(scientific[2].text, "210000.0", "2.1e5");
        ipc::detail::validate_mystran_input("model.bdf", scientific);
        for (const auto& [index, from, to] :
             {std::tuple{1U, "0.0,0.0,0.0", "0,0.0,0.0"},
              {1U, "1000.0", "1e3"},
              {2U, "210000.0", "210000"},
              {2U, "210000.0", "2e5"},
              {2U, "210000.0", "NaN."},
              {2U, "210000.0", "1.e999"},
              {2U, "100.0", "100"},
              {2U, "0.0,0.0,1.0", "0.0,0,1.0"},
              {2U, "0.0,-1.0,0.0", "0.0,-1,0.0"},
              {2U, "MAT1,1,210000.0,,0.3", "MAT1,1,210000.0,,0.3,0"},
              {2U, "833.333,1400.0", "833.333,1400.0,0"}}) {
            auto input = valid;
            input[index].text = replace(input[index].text, from, to);
            rejected("model.bdf", input);
        }
        // Every controlled card's actual extra-coordinate/format boundary is
        // checked independently of the shared codec's model/reference validation.
        for (const auto& line : {"GRID,1,1,0.0,0.0,0.0\n",
                                 "GRID,1,,0.0,0.0,0.0,1\n",
                                 "FORCE,10,2,1,1.0,0.0,-1.0,0.0\n",
                                 "CQUAD4,3,1,1,2,3,4\n",
                                 "PARAM,POST,-1\n",
                                 "GRID    1               0.0     0.0     0.0\n",
                                 "+,0.0,0.0\n",
                                 "CBAR*,1,1,1,2,0.0,0.0,1.0\n",
                                 "GRID,1,,0.0,0.0,0.0,,,,+\n",
                                 "GRID,1,,0.0,0.0,0.0 $ inline\n",
                                 "SPC1,20,123456,1,THRU,2\n"}) {
            auto input = valid;
            input[1].text = line;
            rejected("model.bdf", input);
        }
        for (const auto target : {"parts/nodes.bdf", "../nodes.bdf", "/nodes.bdf", "missing.bdf"}) {
            auto input = valid;
            input[0].text = replace(input[0].text, "nodes.bdf", target);
            rejected("model.bdf", input);
        }
        auto nested = valid;
        nested[1].path = "parts/nodes.bdf";
        rejected("model.bdf", nested);
        rejected("dir/model.bdf", valid);
        rejected("model.txt", valid);
        rejected("missing.bdf", valid);
        auto uppercase_root = valid;
        uppercase_root[0].path = "model.BDF";
        rejected("model.BDF", uppercase_root);
        auto duplicate = valid;
        duplicate.push_back(valid[1]);
        rejected("model.bdf", duplicate);
        auto nul = valid;
        nul[1].text += '\0';
        rejected("model.bdf", nul);
        auto unknown_control = valid;
        unknown_control[0].text = replace(root, "SOL 101", "SOL 103");
        rejected("model.bdf", unknown_control);
        auto trailing = valid;
        trailing[0].text += "GRID,3,,0.0,0.0,0.0\n";
        rejected("model.bdf", trailing);
        auto comments = valid;
        comments[1].text = "$ full-line comment\r\n" + comments[1].text;
        ipc::detail::validate_mystran_input("model.bdf", comments);
        std::cout << "PASS: " << checks
                  << " MYSTRAN input compatibility checks; no solver execution\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
