#include "qcae/nastran_static_result.hpp"
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace {
void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
std::string replace(std::string value, const std::string& from, const std::string& to) {
    const auto at = value.find(from);
    check(at != value.npos, "The negative fixture mutation did not apply");
    value.replace(at, from.size(), to);
    return value;
}
const std::string columns = "(in global coordinate system at each grid)\n"
                            " GRID COORD T1 T2 T3 R1 R2 R3\n SYS\n";
const std::string displacement = "OUTPUT FOR SUBCASE 1\nD I S P L A C E M E N T S\n" + columns;
const std::string reaction = "OUTPUT FOR SUBCASE 1\nS P C F O R C E S\n" + columns;
// Synthetic continuation/mutation cases exercise format guards, never numerical acceptance.
const std::string synthetic = "MYSTRAN Version 19.0.0\n"
                              "*INFORMATION: EPSILON ERROR ESTIMATE = 0.0\n" +
                              displacement + "91 0 0.0 0.0 0.0 0.0 0.0 0.0\n" + displacement +
                              "7 0 +0.0 -1.904763D+00 0.0 0.0 0.0 -2.857144E-03\n" + reaction +
                              "91 0 0.0 +1.0 0.0 0.0 0.0 1.0E+03\n"
                              ">> LINK 9 END\n* * * END OF JOB * * *\n";
} // namespace

int main() {
    try {
        const qcae::NastranStaticReadContext context{1, "mm-N-MPa", "basic"};
        const std::vector<qcae::ExportIdentifier> identities{
            {qcae::EntityId("tip-stable-id"), "GRID", 7},
            {qcae::EntityId("fixed-stable-id"), "GRID", 91}};
        const auto parsed = qcae::read_mystran_static_f06(synthetic, context, identities);
        check(parsed.ok(), "Synthetic MYSTRAN continuation tables did not parse");
        check(parsed.value->reader_version == "qcae.mystran.static-f06.v1" &&
                  parsed.value->displacements.size() == 2 &&
                  parsed.value->spc_reactions.size() == 1 &&
                  parsed.value->displacements[1].entity.value == "tip-stable-id" &&
                  parsed.value->displacements[1].components[1] == -1.904763 &&
                  parsed.value->spc_reactions[0].components[5] == 1000.0,
              "The frozen mapping, reader version, component order or D exponent was lost");
        check(parsed.value->displacement_units[0] == "mm" &&
                  parsed.value->displacement_units[3] == "rad" &&
                  parsed.value->reaction_units[0] == "N" &&
                  parsed.value->reaction_units[5] == "N*mm" &&
                  parsed.value->coordinate_basis == "basic",
              "Translation, rotation, force and moment units were conflated");

        const std::vector<std::string> malformed{
            "",
            replace(synthetic, "MYSTRAN Version 19.0.0\n", ""),
            replace(synthetic, "MYSTRAN Version 19.0.0", "MYSTRAN Version 19.0.0-extra"),
            replace(synthetic, "MYSTRAN Version 19.0.0", "MYSTRAN Version 18.2.0"),
            "MYSTRAN Version 19.0.0\n" + synthetic,
            "MSC Nastran 2024.1\n" + synthetic,
            replace(synthetic, "OUTPUT FOR SUBCASE 1", "OUTPUT FOR SUBCASE 2"),
            replace(synthetic, "OUTPUT FOR SUBCASE 1", "OUTPUT FOR SUBCASE 1 extra"),
            replace(synthetic, "OUTPUT FOR SUBCASE 1", "ECHO: OUTPUT FOR SUBCASE 1"),
            replace(synthetic, "OUTPUT FOR SUBCASE 1\n", ""),
            replace(synthetic, "OUTPUT FOR SUBCASE 1\n", "OUTPUT FOR SUBCASE 1\nEND OF JOB\n"),
            replace(synthetic, "D I S P L A C E M E N T S", "A P P L I E D F O R C E S"),
            replace(
                synthetic, "(in global coordinate system at each grid)", "(in local coordinates)"),
            replace(synthetic, "GRID COORD T1 T2 T3 R1 R2 R3", "GRID COORD T1 T3 T2 R1 R2 R3"),
            replace(synthetic, "GRID COORD", "POINT ID. TYPE"),
            replace(synthetic, "\n SYS\n", "\n"),
            replace(synthetic, "91 0 0.0", "91 42 0.0"),
            replace(synthetic, "91 0 0.0", "91 G 0.0"),
            replace(synthetic, "7 0 +0.0", "92 0 +0.0"),
            replace(synthetic, "7 0 +0.0", "91 0 +0.0"),
            replace(synthetic, "7 0 +0.0", "0 0 +0.0"),
            replace(synthetic, "7 0 +0.0", "+7 0 +0.0"),
            replace(synthetic, "-1.904763D+00", "NaN"),
            replace(synthetic, "-1.904763D+00", "Inf"),
            replace(synthetic, "-1.904763D+00", "1.0E+999"),
            replace(synthetic, "-1.904763D+00", "1.9-3"),
            replace(synthetic, "-2.857144E-03", ""),
            replace(synthetic, "91 0 0.0 0.0 0.0 0.0 0.0 0.0\n", ""),
            replace(synthetic, "7 0 +0.0 -1.904763D+00 0.0 0.0 0.0 -2.857144E-03\n", ""),
            replace(synthetic, "91 0 0.0 +1.0", "92 0 0.0 +1.0"),
            replace(synthetic,
                    "91 0 0.0 +1.0 0.0 0.0 0.0 1.0E+03\n",
                    "91 0 0.0 +1.0 0.0 0.0 0.0 1.0E+03\n91 0 0 1 0 0 0 1000\n"),
            replace(synthetic, reaction, ""),
            replace(synthetic, "* * * END OF JOB * * *\n", ""),
            replace(synthetic, "* * * END OF JOB * * *", "ECHO: END OF JOB"),
            replace(synthetic, "* * * END OF JOB * * *", reaction + "* * * END OF JOB * * *"),
            replace(synthetic, ">> LINK 9 END", ">> LINK 9 END\n7 0 0 0 0 0 0 0"),
            synthetic + "*ERROR 1701: bad input\n",
            synthetic + "USER FATAL MESSAGE\n",
            synthetic + "OUTPUT FOR SUBCASE 1\n",
            synthetic + std::string(1, '\0'),
            "MYSTRAN Version 19.0.0\n" + std::string(4097, 'x')};
        for (std::size_t index = 0; index < malformed.size(); ++index)
            if (qcae::read_mystran_static_f06(malformed[index], context, identities).ok())
                throw std::runtime_error("Malformed MYSTRAN fixture accepted: " +
                                         std::to_string(index));
        for (const auto& invalid_context :
             std::vector<qcae::NastranStaticReadContext>{{0, "mm-N-MPa", "basic"},
                                                         {2, "mm-N-MPa", "basic"},
                                                         {1, "", "basic"},
                                                         {1, "m-N-Pa", "basic"},
                                                         {1, "mm-N-MPa", ""},
                                                         {1, "mm-N-MPa", "local"}})
            check(!qcae::read_mystran_static_f06(synthetic, invalid_context, identities).ok(),
                  "An undeclared or unsupported result context was accepted");
        auto ambiguous = identities;
        ambiguous.push_back(identities.front());
        check(!qcae::read_mystran_static_f06(synthetic, context, ambiguous).ok(),
              "An ambiguous GRID number was accepted");
        ambiguous.back().number = 92;
        check(!qcae::read_mystran_static_f06(synthetic, context, ambiguous).ok(),
              "One entity mapped to two GRID numbers was accepted");
        check(!qcae::read_mystran_static_f06(synthetic, context, {}).ok(),
              "An absent frozen GRID mapping was accepted");
        ambiguous = identities;
        ambiguous.push_back({qcae::EntityId("material"), "MAT1", 7});
        check(qcae::read_mystran_static_f06(synthetic, context, ambiguous).ok(),
              "An unrelated numbering namespace was conflated with GRID");

        std::ifstream source(std::string(QCAE_SOURCE_DIR) +
                             "/tests/fixtures/mystran/cantilever-19.0.0.F06");
        check(source.good(), "The sanitized real MYSTRAN parser reference is missing");
        const std::string real{std::istreambuf_iterator<char>(source), {}};
        std::vector<qcae::ExportIdentifier> real_identities;
        for (std::uint64_t number = 21; number; --number)
            real_identities.push_back(
                {qcae::EntityId("frozen-grid-" + std::to_string(number)), "GRID", number});
        const auto real_parsed = qcae::read_mystran_static_f06(real, context, real_identities);
        check(real_parsed.ok() && real_parsed.value->displacements.size() == 21 &&
                  real_parsed.value->spc_reactions.size() == 1 &&
                  real_parsed.value->displacements.back().entity.value == "frozen-grid-21" &&
                  real_parsed.value->displacements.back().components[1] == -1.904763 &&
                  real_parsed.value->spc_reactions.front().entity.value == "frozen-grid-1",
              "The captured MYSTRAN layout or frozen identity mapping was lost");
        const auto missing_tip = replace(
            real,
            "             21        0  0.0          -1.904763E+00  0.0           0.0           0.0 "
            "         -2.857144E-03\n",
            "");
        check(!qcae::read_mystran_static_f06(missing_tip, context, real_identities).ok(),
              "An incomplete displacement table was accepted");
        check(!qcae::read_mystran_static_f06(
                   replace(real, "MIN* :", "NaN* :"), context, real_identities)
                   .ok(),
              "An unrecognized summary was accepted");
        std::cout << "PASS: MYSTRAN captured-output parsing, synthetic continuations and "
                     "negative guards; no new solver execution or numerical acceptance\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
