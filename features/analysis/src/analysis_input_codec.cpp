#include "qcae/analysis_input.hpp"
#include "qcae/records.hpp"
#include <charconv>
#include <map>
#include <set>

namespace qcae::features::analysis {
namespace {
[[noreturn]] void invalid() {
    throw RecordError(ErrorCode::schema_unsupported, "Malformed frozen analysis input");
}
std::uint64_t number(std::string_view text) {
    std::uint64_t value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        invalid();
    return value;
}
void validate(const FrozenAnalysisInput& input) {
    if (input.version.document.id.value.empty() || input.version.document.epoch.value.empty() ||
        input.analysis.value.empty() || input.target.analysis_kind != "linear_static" ||
        input.target.profile.profile_id.empty() || input.target.profile.profile_version.empty() ||
        input.target.profile.definition_digest.empty() || input.case_label != "LC1" ||
        input.identities.empty() || input.identities.size() > 500000)
        invalid();
    const auto fields = record_wire::read_strings(input.input_signature);
    if (fields.size() < 8 || fields[0] != "QCAE-PHYSICAL-INPUT-1" ||
        fields[1] != input.analysis.value || fields[2] != input.load_case.value ||
        fields[3] != input.target.analysis_kind ||
        fields[4] != record_wire::profile(input.target.profile) || fields[5] != "mm-N-MPa")
        invalid();
    std::map<EntityId, std::string> physical;
    for (std::size_t index = 8; index < fields.size(); ++index) {
        const auto record = record_wire::read_strings(fields[index]);
        if (record.size() < 2 || record[1].empty())
            invalid();
        const auto& kind = record[0];
        const std::size_t expected = kind == "GRID"                      ? 3
                                     : kind == "MAT1"                    ? 5
                                     : kind == "PBAR"                    ? 7
                                     : kind == "CBAR"                    ? 6
                                     : kind == "FORCE" || kind == "SPC1" ? 4
                                                                         : 0;
        if (!expected || record.size() != expected ||
            !physical.emplace(EntityId(record[1]), kind).second)
            invalid();
    }
    if (physical.size() != input.identities.size())
        invalid();
    std::set<EntityId> entities;
    std::set<std::pair<std::string_view, std::uint64_t>> numbers;
    for (const auto& item : input.identities) {
        const auto expected = physical.find(item.entity);
        if (item.entity.value.empty() || !item.number ||
            (item.name_space != "GRID" && item.name_space != "MAT1" && item.name_space != "PBAR" &&
             item.name_space != "CBAR" && item.name_space != "FORCE" &&
             item.name_space != "SPC1") ||
            expected == physical.end() || expected->second != item.name_space ||
            !entities.insert(item.entity).second ||
            !numbers.emplace(item.name_space, item.number).second)
            invalid();
    }
}
} // namespace
std::string physical_signature_hex(const FrozenAnalysisInput& input) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(input.input_signature.size() * 2);
    for (unsigned char byte : input.input_signature) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 15]);
    }
    return result;
}
std::string encode_frozen_analysis_input(const FrozenAnalysisInput& input) {
    validate(input);
    std::vector<std::string> fields{"QCAE-FROZEN-STATIC-1",
                                    input.version.document.id.value,
                                    input.version.document.epoch.value,
                                    std::to_string(input.version.revision),
                                    input.analysis.value,
                                    input.target.profile.profile_id,
                                    input.target.profile.profile_version,
                                    input.target.profile.definition_digest,
                                    input.target.analysis_kind,
                                    input.load_case.value,
                                    input.case_label,
                                    input.input_signature,
                                    std::to_string(input.identities.size())};
    for (const auto& item : input.identities)
        fields.insert(fields.end(),
                      {item.entity.value, item.name_space, std::to_string(item.number)});
    return record_wire::strings(fields);
}
FrozenAnalysisInput decode_frozen_analysis_input(std::string_view bytes) {
    const auto fields = record_wire::read_strings(bytes);
    if (fields.size() < 13 || fields[0] != "QCAE-FROZEN-STATIC-1")
        invalid();
    const auto count = number(fields[12]);
    if (count > 500000 || fields.size() != 13 + count * 3)
        invalid();
    FrozenAnalysisInput input;
    input.version = {{DocumentId(fields[1]), DocumentEpoch(fields[2])}, number(fields[3])};
    input.analysis = EntityId(fields[4]);
    input.target = {{fields[5], fields[6], fields[7]}, fields[8]};
    input.load_case = EntityId(fields[9]);
    input.case_label = fields[10];
    input.input_signature = fields[11];
    for (std::size_t index = 13; index < fields.size(); index += 3)
        input.identities.push_back(
            {EntityId(fields[index]), fields[index + 1], number(fields[index + 2])});
    validate(input);
    return input;
}
} // namespace qcae::features::analysis
