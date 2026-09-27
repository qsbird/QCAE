#pragma once

#include "qcae/types.hpp"
#include <array>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace qcae {
struct RecordTypeId {
    std::uint32_t value{};
    auto operator<=>(const RecordTypeId&) const = default;
};
struct RecordFieldId {
    std::uint32_t value{};
    auto operator<=>(const RecordFieldId&) const = default;
};
struct RecordKey {
    RecordTypeId type;
    std::string identity;
    auto operator<=>(const RecordKey&) const = default;
};
struct RecordVersion {
    DocumentRef document{DocumentId{}, DocumentEpoch{}};
    Revision revision{};
};
bool same_record_version(const RecordVersion&, const RecordVersion&) noexcept;

class RecordError : public std::runtime_error {
  public:
    RecordError(ErrorCode code, std::string message, std::string field = {});
    ErrorCode code() const noexcept;
    const std::string& field() const noexcept;

  private:
    ErrorCode code_;
    std::string field_;
};

// Counts cumulative work. Shared immutable references are not model-byte copies.
struct RecordStats {
    std::uint64_t model_bytes_copied{};
    std::uint64_t model_bytes_encoded{};
    std::uint64_t metadata_bytes_copied{};
    std::uint64_t changed_records{};
    std::uint64_t dirty_pages{};
    std::uint64_t whole_model_serializations{};
    std::uint64_t whole_model_materializations{};
    RecordStats& operator+=(const RecordStats&) noexcept;
};

struct RecordActivityCounters {
    std::uint64_t whole_model_serializations{};
    std::uint64_t whole_model_materializations{};
};
// Monotonic per-thread counters also catch full work when a caller omits its optional ledger.
RecordActivityCounters record_activity_counters() noexcept;
void note_whole_model_serialization(RecordStats* = nullptr) noexcept;
void note_whole_model_materialization(RecordStats* = nullptr) noexcept;

enum class RecordFieldKind : std::uint8_t {
    text = 1,
    real,
    unsigned_integer,
    boolean,
    vector3,
    reference,
    references,
    profile,
    target
};

struct RecordFieldDescriptor {
    RecordFieldId id;
    std::string name;
    RecordFieldKind kind;
    bool optional{};
    std::string unit;
    std::vector<RecordTypeId> reference_types;
    std::uint32_t introduced_version{1};
    bool operator==(const RecordFieldDescriptor&) const = default;
};

// Temporary decoding input only; authoritative records are generated C++ values.
struct RecordFieldInput {
    RecordFieldId id;
    RecordFieldKind kind;
    std::string unit;
    std::string payload;
};
struct RecordInput {
    RecordKey key;
    std::uint32_t version{1};
    std::vector<RecordFieldInput> fields;
};

class DocumentView;
class RecordImage;
using Record = std::shared_ptr<const RecordImage>;
using RecordReferenceVisitor =
    std::function<void(RecordFieldId, std::string_view, std::span<const RecordTypeId>)>;
using RecordRule = std::function<void(const DocumentView&)>;

struct RecordDescriptor {
    RecordTypeId type;
    std::string name;
    std::uint32_t current_version{1};
    std::string query_kind;
    std::function<std::string_view(const void*)> display_name;
    std::size_t maximum_encoded_bytes{16u * 1024u * 1024u};
    std::vector<RecordFieldDescriptor> fields;
    const void* cpp_type_token{};
    std::function<RecordInput(const void*)> encode;
    std::function<std::shared_ptr<const void>(const RecordInput&)> decode;
    std::function<std::size_t(const void*)> owned_bytes;
    std::function<void(const void*, const RecordReferenceVisitor&)> references;
    std::function<void(const void*, const DocumentView&)> validate;
};

template <class T> struct RecordTraits;

class RecordImage {
  public:
    const RecordKey& key() const noexcept;
    std::uint32_t version() const noexcept;
    const std::string& encoded() const noexcept;
    const RecordDescriptor& descriptor() const noexcept;
    const void* object() const noexcept;

    template <class T> const T& get() const {
        if (descriptor_->cpp_type_token != RecordTraits<T>::token())
            throw RecordError(ErrorCode::invalid_input, "Record has the wrong C++ type");
        return *static_cast<const T*>(object_.get());
    }

  private:
    friend class RecordRegistry;
    RecordKey key_;
    std::uint32_t version_{};
    std::shared_ptr<const RecordDescriptor> descriptor_;
    std::shared_ptr<const void> object_;
    std::string encoded_;
};

class RecordRegistry {
  public:
    void add(RecordDescriptor);
    void add_rule(RecordRule);
    void freeze();
    bool frozen() const noexcept;
    const RecordDescriptor* find(RecordTypeId) const noexcept;
    std::vector<RecordTypeId> types() const;
    const std::vector<RecordRule>& rules() const noexcept;

    template <class T> Record make(T value, RecordStats* stats = nullptr) const {
        const auto* descriptor = find(RecordTraits<T>::type_id);
        if (!descriptor || descriptor->cpp_type_token != RecordTraits<T>::token())
            throw RecordError(ErrorCode::schema_unsupported, "Unregistered C++ record type");
        return make_erased(
            RecordTraits<T>::type_id, std::make_shared<const T>(std::move(value)), stats);
    }

    Record decode(std::string_view, RecordStats* stats = nullptr) const;
    Record from_input(const RecordInput&, RecordStats* stats = nullptr) const;

  private:
    Record make_erased(RecordTypeId, std::shared_ptr<const void>, RecordStats*) const;
    std::map<RecordTypeId, std::shared_ptr<const RecordDescriptor>> descriptors_;
    std::vector<RecordRule> rules_;
    bool frozen_{};
};

namespace record_wire {
inline constexpr std::size_t maximum_record_bytes = 16u * 1024u * 1024u;
inline constexpr std::size_t maximum_batch_bytes = 256u * 1024u * 1024u;
std::string encode(const RecordInput&, RecordStats* = nullptr);
RecordInput decode(std::string_view);
const RecordFieldInput* find(const RecordInput&, RecordFieldId) noexcept;
const RecordFieldInput& require(const RecordInput&, RecordFieldId);
std::string text(std::string_view);
std::string real(double);
std::string number(std::uint64_t);
std::string boolean(bool);
std::string vector3(const std::array<double, 3>&);
std::string strings(std::span<const std::string>);
std::string profile(const ProfileRef&);
std::string target(const TargetBinding&);
std::string read_text(std::string_view);
double read_real(std::string_view);
std::uint64_t read_number(std::string_view);
bool read_boolean(std::string_view);
std::array<double, 3> read_vector3(std::string_view);
std::vector<std::string> read_strings(std::string_view);
ProfileRef read_profile(std::string_view);
TargetBinding read_target(std::string_view);
} // namespace record_wire
} // namespace qcae
