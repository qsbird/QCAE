#include "qcae/nastran_static_result.hpp"
#include "qcae/nastran_codec.hpp"
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace {
void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
const std::string header = "      POINT ID. TYPE T1 T2 T3 R1 R2 R3\n";
const std::string synthetic = "1 SYNTHETIC PARSER FIXTURE PAGE 1\nSUBCASE 1\n"
                              "D I S P L A C E M E N T V E C T O R\n" +
                              header +
                              " 91 G 0.0 0.0 0.0 0.0 0.0 0.0\n"
                              "1 SYNTHETIC PARSER FIXTURE PAGE 2\nSUBCASE 1\n"
                              "D I S P L A C E M E N T V E C T O R\n" +
                              header +
                              " 7 G +0.0 -1.904763D+00 0.0 0.0 0.0 -2.857144E-03\n"
                              "F O R C E S O F S I N G L E - P O I N T C O N S T R A I N T\n" +
                              header + " 91 G 0.0 +1.0 0.0 0.0 0.0 1.0E+03\n";
std::string replace(std::string value, const std::string& from, const std::string& to) {
    const auto at = value.find(from);
    check(at != value.npos, "The negative fixture mutation did not apply");
    value.replace(at, from.size(), to);
    return value;
}
} // namespace
int main() {
    try {
        const qcae::NastranStaticReadContext context{1, "mm-N-MPa", "basic"};
        const std::vector<qcae::ExportIdentifier> identities{
            {qcae::EntityId("tip-stable-id"), "GRID", 7},
            {qcae::EntityId("fixed-stable-id"), "GRID", 91}};
        const auto parsed = qcae::read_nastran_static_f06(synthetic, context, identities);
        check(parsed.ok(), "Synthetic paged F06 did not parse");
        check(parsed.value->displacements.size() == 2 && parsed.value->spc_reactions.size() == 1 &&
                  parsed.value->displacements[1].entity.value == "tip-stable-id" &&
                  parsed.value->displacements[1].components[1] == -1.904763 &&
                  parsed.value->spc_reactions[0].components[5] == 1000.0,
              "The frozen mapping, component direction or D exponent was lost");
        check(parsed.value->displacement_units[3] == "rad" &&
                  parsed.value->reaction_units[5] == "N*mm" &&
                  parsed.value->coordinate_basis == "basic",
              "Translation, rotation, force and moment units were conflated");
        for (const auto& input : std::vector<std::string>{
                 replace(synthetic, " 7 G", " 92 G"),
                 replace(synthetic, " 7 G", " 91 G"),
                 replace(synthetic, "-1.904763D+00", "NaN"),
                 replace(synthetic, "-1.904763D+00", "1.0E+999"),
                 replace(synthetic, "-1.904763D+00", "1.9-3"),
                 replace(synthetic, " 7 G", " 7 S"),
                 replace(synthetic, "R1 R2 R3", "R1 R3 R2"),
                 replace(synthetic, "SUBCASE 1", "SUBCASE 2"),
                 replace(synthetic, "SUBCASE 1", "SUBCASE = bad"),
                 replace(synthetic, "-2.857144E-03", ""),
                 synthetic + "USER FATAL MESSAGE 100\n",
                 synthetic + " 91 G 0 1 0 0 0 1000\n",
                 synthetic + " +92 G NaN 0 0 0 0 0\n",
                 synthetic + " -92 G NaN 0 0 0 0 0\n",
                 synthetic + "F O R C E S O F M U L T I - P O I N T C O N S T R A I N T\n" +
                     header + " 7 G 0 99 0 0 0 0\n",
                 replace(synthetic,
                         " 91 G 0.0 +1.0 0.0 0.0 0.0 1.0E+03\n",
                         "F O R C E S O F M U L T I - P O I N T C O N S T R A I N T\n" + header +
                             " 91 G 0 99 0 0 0 0\n"),
                 replace(synthetic, "SUBCASE 1\nD I", "SUBCASE 1\nEND OF JOB\nD I"),
                 replace(synthetic, "SUBCASE 1\nD I", "3 SUBCASE 1\nD I"),
                 replace(synthetic, "SUBCASE 1\nD I", "PAGE 100\nD I"),
                 synthetic + "PAGE 99\n92 G NaN 1 0 0 0 0\n",
                 std::string("SUBCASE 1\n") + std::string(4097, 'x'),
                 synthetic + std::string(1, '\0')})
            check(!qcae::read_nastran_static_f06(input, context, identities).ok(),
                  "Malformed, wrong-case, duplicate or nonfinite F06 was accepted");
        const auto no_reaction = synthetic.substr(0, synthetic.find("F O R C E S"));
        check(!qcae::read_nastran_static_f06(no_reaction, context, identities).ok(),
              "Missing SPC reactions were accepted");
        check(!qcae::read_nastran_static_f06(synthetic, {1, "", "basic"}, identities).ok(),
              "Undeclared units were assumed");
        check(!qcae::read_nastran_static_f06(synthetic, {1, "m-N-Pa", "basic"}, identities).ok(),
              "Unsupported units were accepted");
        check(!qcae::read_nastran_static_f06(synthetic, {1, "mm-N-MPa", "local"}, identities).ok(),
              "Local coordinates were relabeled basic");
        auto duplicate = identities;
        duplicate.push_back(identities.front());
        check(!qcae::read_nastran_static_f06(synthetic, context, duplicate).ok(),
              "Ambiguous export mapping was accepted");
        check(!qcae::read_nastran_static_f06(synthetic, context, {}).ok(),
              "Missing export mapping was accepted");
        const qcae::NastranCodec codec;
        qcae::ImportRequest request;
        request.root_resource = "cantilever.bdf";
        request.source_profile = codec.definition().reference;
        request.source_model_id = "preregistered-real-cantilever-v3";
        request.unit_system = "mm-N-MPa";
        for (const char* name : {"cantilever.bdf", "nodes.bdf", "properties.bdf", "beams.bdf"}) {
            std::ifstream source(std::string(QCAE_SOURCE_DIR) +
                                 "/tests/fixtures/nastran-real-benchmark-v3/" + name);
            check(source.good(), "The preregistered benchmark resource is missing");
            request.resources.push_back({name, {std::istreambuf_iterator<char>(source), {}}});
        }
        const auto imported = codec.decode(request);
        check(imported.candidate && imported.report.complete && imported.report.issues.empty(),
              "The preregistered real benchmark is unsupported by the production codec");
        std::cout << "PASS: synthetic F06 parsing and negative cases; no actual solver or "
                     "numerical acceptance\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
