#include "qcae/record_registry.hpp"
#include "qcae/operation_ledger.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <set>

namespace qcae {
namespace {
thread_local RecordActivityCounters activity_counters;
[[noreturn]] void malformed(const std::string& message) {
    throw RecordError(ErrorCode::invalid_input, message);
}
void append_number(std::string& bytes, std::uint64_t value, unsigned width) {
    for (unsigned index = 0; index < width; ++index) {
        const auto previous_size = bytes.size();
        const auto previous_capacity = bytes.capacity();
        bytes.push_back(static_cast<char>((value >> (index * 8)) & 255));
        if (bytes.capacity() != previous_capacity)
            ledger::add(ledger::Stage::records, ledger::Metric::model_copy_bytes, previous_size);
    }
}
void append_text(std::string& bytes, std::string_view value) {
    if (value.size() > record_wire::maximum_record_bytes)
        malformed("Record text exceeds size limit");
    append_number(bytes, value.size(), 4);
    const auto previous_size = bytes.size();
    const auto previous_capacity = bytes.capacity();
    bytes.append(value);
    ledger::add(ledger::Stage::records, ledger::Metric::model_copy_bytes, value.size());
    if (bytes.capacity() != previous_capacity)
        ledger::add(ledger::Stage::records, ledger::Metric::model_copy_bytes, previous_size);
}
class Reader {
  public:
    explicit Reader(std::string_view bytes) : bytes_(bytes) {
        if (bytes.size() > record_wire::maximum_batch_bytes)
            malformed("Encoded data exceeds size limit");
    }
    std::uint64_t number(unsigned width) {
        const auto bytes = take(width);
        std::uint64_t value = 0;
        for (unsigned index = 0; index < width; ++index)
            value |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[index]))
                     << (index * 8);
        return value;
    }
    std::string_view take(std::size_t count) {
        if (count > bytes_.size() - offset_)
            malformed("Truncated record");
        const auto value = bytes_.substr(offset_, count);
        offset_ += count;
        return value;
    }
    std::string text() {
        const auto count = number(4);
        ledger::add(ledger::Stage::records, ledger::Metric::model_copy_bytes, count);
        return std::string(take(static_cast<std::size_t>(count)));
    }
    std::size_t remaining() const noexcept {
        return bytes_.size() - offset_;
    }
    void finish() const {
        if (remaining())
            malformed("Trailing record bytes");
    }

  private:
    std::string_view bytes_;
    std::size_t offset_{};
};

void check_input(const RecordInput& input, const RecordDescriptor& descriptor) {
    if (input.key.type != descriptor.type || input.key.identity.empty() ||
        input.key.identity.size() > 1024 || input.key.identity.find('\0') != std::string::npos)
        malformed("Invalid record identity");
    if (input.version == 0 || input.version > descriptor.current_version)
        throw RecordError(ErrorCode::schema_unsupported, "Unsupported future record version");
    std::set<RecordFieldId> seen;
    for (const auto& field : input.fields) {
        const auto found = std::find_if(descriptor.fields.begin(),
                                        descriptor.fields.end(),
                                        [&](const auto& value) { return value.id == field.id; });
        if (found == descriptor.fields.end() || !seen.insert(field.id).second)
            malformed("Unknown or duplicate record field");
        if (field.kind != found->kind)
            throw RecordError(ErrorCode::invalid_input, "Wrong record field type", found->name);
        if (field.unit != found->unit)
            throw RecordError(ErrorCode::invalid_unit,
                              field.unit.empty() ? "Required field unit is missing"
                                                 : "Unknown or incompatible field unit",
                              found->name);
        if (found->introduced_version > input.version)
            malformed("Field predates its declared record version");
    }
    for (const auto& field : descriptor.fields)
        if (!field.optional && !seen.contains(field.id))
            throw RecordError(
                ErrorCode::missing_input, "Required record field is missing", field.name);
}
} // namespace

bool same_record_version(const RecordVersion& left, const RecordVersion& right) noexcept {
    return left.document.id == right.document.id && left.document.epoch == right.document.epoch &&
           left.revision == right.revision;
}
RecordError::RecordError(ErrorCode code, std::string message, std::string field)
    : std::runtime_error(std::move(message)), code_(code), field_(std::move(field)) {}
ErrorCode RecordError::code() const noexcept {
    return code_;
}
const std::string& RecordError::field() const noexcept {
    return field_;
}
RecordStats& RecordStats::operator+=(const RecordStats& value) noexcept {
    model_bytes_copied += value.model_bytes_copied;
    model_bytes_encoded += value.model_bytes_encoded;
    metadata_bytes_copied += value.metadata_bytes_copied;
    changed_records += value.changed_records;
    dirty_pages += value.dirty_pages;
    whole_model_serializations += value.whole_model_serializations;
    whole_model_materializations += value.whole_model_materializations;
    return *this;
}
RecordActivityCounters record_activity_counters() noexcept {
    return activity_counters;
}
void note_whole_model_serialization(RecordStats* stats) noexcept {
    ledger::add(ledger::Stage::records, ledger::Metric::full_model_serializations, 1);
    ++activity_counters.whole_model_serializations;
    if (stats)
        ++stats->whole_model_serializations;
}
void note_whole_model_materialization(RecordStats* stats) noexcept {
    ledger::add(ledger::Stage::records, ledger::Metric::full_model_materializations, 1);
    ++activity_counters.whole_model_materializations;
    if (stats)
        ++stats->whole_model_materializations;
}
const RecordKey& RecordImage::key() const noexcept {
    return key_;
}
std::uint32_t RecordImage::version() const noexcept {
    return version_;
}
const std::string& RecordImage::encoded() const noexcept {
    return encoded_;
}
const RecordDescriptor& RecordImage::descriptor() const noexcept {
    return *descriptor_;
}
const void* RecordImage::object() const noexcept {
    return object_.get();
}

void RecordRegistry::add(RecordDescriptor descriptor) {
    if (frozen_)
        malformed("Record registry is frozen");
    if (!descriptor.type.value || descriptor.name.empty() || !descriptor.current_version ||
        !descriptor.cpp_type_token || !descriptor.encode || !descriptor.decode ||
        !descriptor.owned_bytes || !descriptor.references || !descriptor.validate ||
        !descriptor.maximum_encoded_bytes ||
        descriptor.maximum_encoded_bytes > record_wire::maximum_record_bytes)
        malformed("Incomplete record descriptor");
    std::set<RecordFieldId> ids;
    std::set<std::string> names;
    for (const auto& field : descriptor.fields) {
        if (!field.id.value || field.name.empty() || !ids.insert(field.id).second ||
            !names.insert(field.name).second || !field.introduced_version ||
            field.introduced_version > descriptor.current_version)
            malformed("Invalid or duplicate field declaration");
    }
    std::sort(descriptor.fields.begin(),
              descriptor.fields.end(),
              [](const auto& left, const auto& right) { return left.id < right.id; });
    const auto type = descriptor.type;
    for (const auto& [unused, existing] : descriptors_) {
        (void)unused;
        if (existing->cpp_type_token == descriptor.cpp_type_token ||
            existing->name == descriptor.name)
            malformed("Duplicate record type registration");
    }
    if (!descriptors_.emplace(type, std::make_shared<const RecordDescriptor>(std::move(descriptor)))
             .second)
        malformed("Duplicate persistent record type ID");
}
void RecordRegistry::add_rule(RecordRule rule) {
    if (frozen_ || !rule)
        malformed("Cannot add record validation rule");
    rules_.push_back(std::move(rule));
}
void RecordRegistry::freeze() {
    for (const auto& [unused, descriptor] : descriptors_) {
        (void)unused;
        for (const auto& field : descriptor->fields)
            for (const auto type : field.reference_types)
                if (!find(type))
                    malformed("Reference declaration names an unregistered type");
    }
    frozen_ = true;
}
bool RecordRegistry::frozen() const noexcept {
    return frozen_;
}
const RecordDescriptor* RecordRegistry::find(RecordTypeId type) const noexcept {
    const auto found = descriptors_.find(type);
    return found == descriptors_.end() ? nullptr : found->second.get();
}
std::vector<RecordTypeId> RecordRegistry::types() const {
    std::vector<RecordTypeId> result;
    result.reserve(descriptors_.size());
    for (const auto& [type, unused] : descriptors_) {
        (void)unused;
        result.push_back(type);
    }
    return result;
}
const std::vector<RecordRule>& RecordRegistry::rules() const noexcept {
    return rules_;
}
Record RecordRegistry::make_erased(RecordTypeId type,
                                   std::shared_ptr<const void> object,
                                   RecordStats* stats) const {
    if (!frozen_)
        malformed("Record registry must be frozen before use");
    const auto found = descriptors_.find(type);
    if (found == descriptors_.end() || !object)
        throw RecordError(ErrorCode::schema_unsupported, "Unknown record type");
    const auto& descriptor = *found->second;
    auto input = descriptor.encode(object.get());
    ledger::add(ledger::Stage::records,
                ledger::Metric::model_copy_bytes,
                descriptor.owned_bytes(object.get()));
    check_input(input, descriptor);
    auto result = std::make_shared<RecordImage>();
    result->key_ = input.key;
    result->version_ = input.version;
    result->descriptor_ = found->second;
    result->object_ = std::move(object);
    result->encoded_ = record_wire::encode(input, stats);
    ledger::cover(ledger::Stage::records);
    ledger::add(ledger::Stage::records,
                ledger::Metric::metadata_copy_bytes,
                sizeof(RecordImage) + input.key.identity.size());
    if (result->encoded_.size() > descriptor.maximum_encoded_bytes)
        throw RecordError(ErrorCode::resource_limit,
                          "Encoded record exceeds its schema byte limit");
    if (stats) {
        // Account one owned input value and the actual intermediate payload copies.
        stats->model_bytes_copied += descriptor.owned_bytes(result->object_.get());
        for (const auto& field : input.fields)
            stats->model_bytes_copied += field.payload.size();
    }
    return result;
}
Record RecordRegistry::decode(std::string_view bytes, RecordStats* stats) const {
    if (stats)
        stats->model_bytes_copied += bytes.size();
    return from_input(record_wire::decode(bytes), stats);
}
Record RecordRegistry::from_input(const RecordInput& input, RecordStats* stats) const {
    const auto* descriptor = find(input.key.type);
    if (!descriptor)
        throw RecordError(ErrorCode::schema_unsupported, "Unknown persistent record type");
    check_input(input, *descriptor);
    return make_erased(input.key.type, descriptor->decode(input), stats);
}

namespace record_wire {
std::string encode(const RecordInput& input, RecordStats* stats) {
    if (input.fields.size() > 256)
        malformed("Record has too many fields");
    std::vector<const RecordFieldInput*> fields;
    fields.reserve(input.fields.size());
    std::size_t expected = 20 + input.key.identity.size();
    for (const auto& field : input.fields) {
        if (field.payload.size() > maximum_record_bytes || field.unit.size() > 1024)
            malformed("Record field exceeds quota");
        expected += 13 + field.unit.size() + field.payload.size();
        fields.push_back(&field);
    }
    if (expected > maximum_record_bytes)
        malformed("Record exceeds size limit");
    std::sort(fields.begin(), fields.end(), [](const auto* left, const auto* right) {
        return left->id < right->id;
    });
    std::string result;
    result.reserve(expected);
    result.append("QCR1");
    append_number(result, input.key.type.value, 4);
    append_number(result, input.version, 4);
    append_text(result, input.key.identity);
    append_number(result, fields.size(), 4);
    for (const auto* field : fields) {
        append_number(result, field->id.value, 4);
        append_number(result, static_cast<unsigned>(field->kind), 1);
        append_text(result, field->unit);
        append_text(result, field->payload);
        if (stats)
            stats->model_bytes_encoded += field->payload.size();
    }
    if (stats) {
        stats->model_bytes_encoded += result.size();
        stats->model_bytes_copied += result.size();
    }
    ledger::add(ledger::Stage::records, ledger::Metric::encoded_bytes, result.size());
    return result;
}
RecordInput decode(std::string_view bytes) {
    if (bytes.size() > maximum_record_bytes)
        malformed("Record exceeds size limit");
    Reader reader(bytes);
    if (reader.take(4) != "QCR1")
        throw RecordError(ErrorCode::schema_unsupported, "Unsupported record encoding");
    RecordInput result;
    result.key.type.value = static_cast<std::uint32_t>(reader.number(4));
    result.version = static_cast<std::uint32_t>(reader.number(4));
    result.key.identity = reader.text();
    const auto count = reader.number(4);
    if (count > 256 || count > reader.remaining() / 13)
        malformed("Malformed record field count");
    result.fields.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t index = 0; index < count; ++index) {
        RecordFieldInput field;
        field.id.value = static_cast<std::uint32_t>(reader.number(4));
        const auto kind = reader.number(1);
        if (kind < 1 || kind > static_cast<unsigned>(RecordFieldKind::target))
            malformed("Unknown record field type");
        field.kind = static_cast<RecordFieldKind>(kind);
        field.unit = reader.text();
        field.payload = reader.text();
        result.fields.push_back(std::move(field));
    }
    reader.finish();
    return result;
}
const RecordFieldInput* find(const RecordInput& input, RecordFieldId id) noexcept {
    const auto found = std::find_if(input.fields.begin(),
                                    input.fields.end(),
                                    [&](const auto& value) { return value.id == id; });
    return found == input.fields.end() ? nullptr : &*found;
}
const RecordFieldInput& require(const RecordInput& input, RecordFieldId id) {
    const auto* found = find(input, id);
    if (!found)
        throw RecordError(ErrorCode::missing_input, "Required record field is missing");
    return *found;
}
std::string text(std::string_view value) {
    if (value.size() > maximum_record_bytes || value.find('\0') != std::string_view::npos)
        malformed("Invalid record text");
    ledger::add(ledger::Stage::records, ledger::Metric::model_copy_bytes, value.size());
    return std::string(value);
}
std::string number(std::uint64_t value) {
    std::string result;
    append_number(result, value, 8);
    return result;
}
std::string real(double value) {
    if (!std::isfinite(value))
        malformed("Record number must be finite");
    return number(std::bit_cast<std::uint64_t>(value == 0.0 ? 0.0 : value));
}
std::string boolean(bool value) {
    return std::string(1, value ? '\1' : '\0');
}
std::string vector3(const std::array<double, 3>& value) {
    std::string result;
    result.reserve(24);
    for (double component : value)
        result += real(component);
    ledger::add(ledger::Stage::records, ledger::Metric::model_copy_bytes, 24);
    return result;
}
std::string strings(std::span<const std::string> values) {
    if (values.size() > 5000000)
        malformed("Reference list exceeds quota");
    std::string result;
    append_number(result, values.size(), 4);
    for (const auto& value : values) {
        append_text(result, value);
        if (result.size() > maximum_batch_bytes)
            malformed("Reference list exceeds byte quota");
    }
    return result;
}
std::string profile(const ProfileRef& value) {
    std::string result;
    append_text(result, value.profile_id);
    append_text(result, value.profile_version);
    append_text(result, value.definition_digest);
    return result;
}
std::string target(const TargetBinding& value) {
    auto result = profile(value.profile);
    append_text(result, value.analysis_kind);
    return result;
}
std::string read_text(std::string_view bytes) {
    return text(bytes);
}
std::uint64_t read_number(std::string_view bytes) {
    Reader reader(bytes);
    auto result = reader.number(8);
    reader.finish();
    return result;
}
double read_real(std::string_view bytes) {
    const double result = std::bit_cast<double>(read_number(bytes));
    if (!std::isfinite(result))
        malformed("Record number must be finite");
    return result;
}
bool read_boolean(std::string_view bytes) {
    if (bytes.size() != 1 || static_cast<unsigned char>(bytes[0]) > 1)
        malformed("Invalid boolean record field");
    return bytes[0] != 0;
}
std::array<double, 3> read_vector3(std::string_view bytes) {
    Reader reader(bytes);
    std::array<double, 3> result{};
    for (auto& value : result)
        value = read_real(reader.take(8));
    reader.finish();
    return result;
}
std::vector<std::string> read_strings(std::string_view bytes) {
    Reader reader(bytes);
    const auto count = reader.number(4);
    if (count > 5000000 || count > reader.remaining() / 4)
        malformed("Malformed reference count");
    std::vector<std::string> result;
    result.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t index = 0; index < count; ++index)
        result.push_back(reader.text());
    reader.finish();
    return result;
}
ProfileRef read_profile(std::string_view bytes) {
    Reader reader(bytes);
    ProfileRef result{reader.text(), reader.text(), reader.text()};
    reader.finish();
    return result;
}
TargetBinding read_target(std::string_view bytes) {
    Reader reader(bytes);
    TargetBinding result{{reader.text(), reader.text(), reader.text()}, reader.text()};
    reader.finish();
    return result;
}
} // namespace record_wire
} // namespace qcae
