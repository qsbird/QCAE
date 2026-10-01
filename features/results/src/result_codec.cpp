#include "qcae/result_fixture.hpp"
#include "qcae/records.hpp"
#include <charconv>
#include <map>

namespace qcae::features::results {
namespace {
[[noreturn]] void invalid() {
    throw RecordError(ErrorCode::schema_unsupported, "Malformed fixture result bundle");
}
std::uint64_t number(std::string_view text) {
    std::uint64_t value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        invalid();
    return value;
}
void validate(const ResultBundle& bundle) {
    if (bundle.source_kind != "fixture" || bundle.reader_version != "qcae.fixture.displacement.v1")
        invalid();
    ResultFixture fixture;
    fixture.input_fingerprint = analysis::physical_signature_hex(bundle.input);
    fixture.identities = bundle.input.identities;
    fixture.quantity = bundle.field.quantity;
    fixture.unit = bundle.field.unit;
    fixture.components = bundle.field.components;
    fixture.location = bundle.field.location;
    fixture.coordinate_basis = bundle.field.coordinate_basis;
    fixture.case_label = bundle.field.case_label;
    fixture.source_kind = bundle.source_kind;
    fixture.frame = bundle.field.frame;
    std::map<EntityId, std::uint64_t> numbers;
    for (const auto& identity : bundle.input.identities)
        if (identity.name_space == "GRID")
            numbers.emplace(identity.entity, identity.number);
    for (const auto& value : bundle.field.values) {
        const auto found = numbers.find(value.entity);
        if (found == numbers.end())
            invalid();
        fixture.values.push_back({found->second, value.value});
    }
    if (!FixtureResultReader{}.read(bundle.input, fixture).ok())
        invalid();
}
} // namespace
std::string encode_result_bundle(const ResultBundle& bundle) {
    validate(bundle);
    std::vector<std::string> fields{"QCAE-FIXTURE-RESULT-1",
                                    analysis::encode_frozen_analysis_input(bundle.input),
                                    bundle.source_kind,
                                    bundle.reader_version,
                                    bundle.field.quantity,
                                    bundle.field.unit,
                                    record_wire::strings(bundle.field.components),
                                    bundle.field.location,
                                    bundle.field.coordinate_basis,
                                    bundle.field.case_label,
                                    std::to_string(bundle.field.frame),
                                    std::to_string(bundle.field.values.size())};
    for (const auto& value : bundle.field.values)
        fields.insert(fields.end(), {value.entity.value, record_wire::vector3(value.value)});
    return record_wire::strings(fields);
}
ResultBundle decode_result_bundle(std::string_view bytes) {
    const auto fields = record_wire::read_strings(bytes);
    if (fields.size() < 12 || fields[0] != "QCAE-FIXTURE-RESULT-1")
        invalid();
    const auto count = number(fields[11]);
    if (count > 500000 || fields.size() != 12 + count * 2)
        invalid();
    ResultBundle bundle;
    bundle.input = analysis::decode_frozen_analysis_input(fields[1]);
    bundle.source_kind = fields[2];
    bundle.reader_version = fields[3];
    bundle.field = {fields[4],
                    fields[5],
                    record_wire::read_strings(fields[6]),
                    fields[7],
                    fields[8],
                    fields[9],
                    number(fields[10]),
                    {}};
    for (std::size_t index = 12; index < fields.size(); index += 2)
        bundle.field.values.push_back(
            {EntityId(fields[index]), record_wire::read_vector3(fields[index + 1])});
    validate(bundle);
    return bundle;
}
} // namespace qcae::features::results
