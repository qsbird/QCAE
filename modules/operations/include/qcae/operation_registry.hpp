#pragma once
#include "qcae/types.hpp"
#include "qcae/application_types.hpp"
#include "qcae/operation_ledger.hpp"
#include <array>
#include <concepts>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace qcae::operations {
namespace detail {
ledger::Metric value_copy_metric() noexcept;
class MetadataValueCopies {
  public:
    MetadataValueCopies() noexcept;
    ~MetadataValueCopies();
    MetadataValueCopies(const MetadataValueCopies&) = delete;
    MetadataValueCopies& operator=(const MetadataValueCopies&) = delete;

  private:
    ledger::Metric previous_;
};
} // namespace detail

// Wire DTO only. This tree does not store or mutate authoritative engineering entities.
struct Value {
    using Array = std::vector<Value>;
    using Object = std::map<std::string, Value, std::less<>>;
    using Storage = std::variant<bool, double, std::int64_t, std::string, Array, Object>;
    Storage data{Object{}};

    Value() = default;
    explicit Value(bool value);
    explicit Value(double value);
    template <std::signed_integral Integer>
    explicit Value(Integer value) : data(static_cast<std::int64_t>(value)) {
        static_assert(sizeof(Integer) <= sizeof(std::int64_t));
        ledger::add(ledger::Stage::application, detail::value_copy_metric(), sizeof(std::int64_t));
    }
    explicit Value(const std::string& value);
    explicit Value(std::string&& value);
    explicit Value(std::string_view value);
    explicit Value(const char* value);
    explicit Value(const Array& value);
    explicit Value(Array&& value);
    explicit Value(const Object& value);
    explicit Value(Object&& value);
    Value(const Value&);
    Value& operator=(const Value&);
    Value(Value&&) noexcept;
    Value& operator=(Value&&) noexcept;
    bool operator==(const Value&) const = default;
};

[[nodiscard]] Result<std::string> canonical_value(const Value& value);
// Shared wire projection. Revisions remain exact across JSON transports as decimal strings.
[[nodiscard]] Value change_receipt_value(const ChangeReceipt& receipt);

enum class OperationEffect { read_only, document_write, preview, background_task, auxiliary_write };
struct ContextRequirements {
    bool document{};
    bool epoch{};
    bool expected_revision{};
    bool idempotency_key{};
    bool expected_profile{};
    bool operator==(const ContextRequirements&) const = default;
};
struct OperationContext {
    Caller caller;
    std::optional<DocumentRef> document;
    std::optional<Revision> expected_revision;
    std::string idempotency_key;
    std::optional<ProfileRef> expected_profile;
    std::string request_id;
    // Omitted versions preserve trusted local/legacy callers using the installed definition.
    std::optional<std::uint32_t> requested_version{};
};
struct InputFieldDescriptor {
    std::uint32_t field_id{};
    std::string name;
    std::string wire_type;
    std::vector<std::string> units;
    bool required{true};
    bool allow_empty{};
    bool operator==(const InputFieldDescriptor&) const = default;
};
struct OperationDefinition {
    std::string operation_id;
    std::uint32_t version{};
    std::string schema_id;
    OperationEffect effect{};
    ContextRequirements context;
    std::vector<InputFieldDescriptor> fields;
    bool operator==(const OperationDefinition&) const = default;
};
struct OperationDescriptor {
    OperationDefinition definition;
    bool available{};
    std::string unavailable_reason;
};

template <class Input> struct InputTraits;

// Generated input adapters reuse only these transport-neutral mechanical checks.
namespace wire {
inline std::size_t dynamic_bytes(const std::string& value) noexcept {
    return value.size();
}
inline std::size_t dynamic_bytes(const EntityId& value) noexcept {
    return value.value.size();
}
inline std::size_t dynamic_bytes(const Quantity& value) noexcept {
    return value.unit.size();
}
inline std::size_t dynamic_bytes(const std::vector<EntityId>& values) noexcept {
    std::size_t bytes = values.size() * sizeof(EntityId);
    for (const auto& value : values)
        bytes += value.value.size();
    return bytes;
}
template <class T> std::size_t dynamic_bytes(const T&) noexcept {
    return 0;
}
template <class T> std::size_t dynamic_bytes(const std::optional<T>& value) noexcept {
    return value ? dynamic_bytes(*value) : 0;
}
// Generated aggregate copies include their inline fields once, plus owned dynamic payload.
template <class Input, class... Fields>
void observe_input_copy(const Input&, const Fields&... fields) noexcept {
    ledger::add(ledger::Stage::application,
                ledger::Metric::model_copy_bytes,
                sizeof(Input) + (std::size_t{} + ... + dynamic_bytes(fields)));
}
inline void observe_key(std::string_view key) noexcept {
    ledger::add(ledger::Stage::application,
                ledger::Metric::metadata_copy_bytes,
                sizeof(std::string) + key.size());
}
[[nodiscard]] Result<const Value::Object*> object_fields(const Value& value,
                                                         std::span<const std::string_view> allowed,
                                                         std::string_view field);
[[nodiscard]] Result<const Value::Object*> object_fields(const Value& value,
                                                         std::span<const std::string_view> allowed,
                                                         std::span<const std::string_view> required,
                                                         std::string_view field);
[[nodiscard]] Result<std::string>
string_value(const Value& value, std::string_view field, bool allow_empty = false);
[[nodiscard]] Result<double> finite_number(const Value& value, std::string_view field);
[[nodiscard]] Result<EntityId> entity_id(const Value& value, std::string_view field);
[[nodiscard]] Result<std::vector<EntityId>>
entity_id_array(const Value& value, std::string_view field, bool allow_empty = false);
[[nodiscard]] Result<Quantity> quantity(const Value& value,
                                        std::span<const std::string_view> allowed_units,
                                        std::string_view field);
[[nodiscard]] Result<std::array<double, 3>> vector3(const Value& value, std::string_view field);
[[nodiscard]] Result<std::uint32_t> positive_uint32(const Value& value, std::string_view field);
[[nodiscard]] Value to_value(const Quantity& quantity);
[[nodiscard]] Value to_value(const std::vector<EntityId>& identities);
[[nodiscard]] Value to_value(const std::array<double, 3>& vector);
} // namespace wire

class OperationRegistry {
  public:
    template <class Input, class Handler>
    [[nodiscard]] Result<bool> register_typed(OperationDefinition definition, Handler handler) {
        std::function<Result<Value>(const OperationContext&, const Input&)> typed_handler(
            std::move(handler));
        if (!typed_handler) {
            return {Status::failed,
                    std::nullopt,
                    Diagnostic{ErrorCode::invalid_input, "Operation handler is empty.", "handler"}};
        }
        if (definition != InputTraits<Input>::definition()) {
            return {Status::failed,
                    std::nullopt,
                    Diagnostic{ErrorCode::invalid_input,
                               "Registered metadata does not match the generated input contract.",
                               "definition"}};
        }
        return register_handler(
            std::move(definition),
            std::string(InputTraits<Input>::schema_id),
            [callback = std::move(typed_handler)](const OperationContext& context,
                                                  const Value& value) -> Result<Value> {
                const auto decoded = InputTraits<Input>::from_value(value);
                if (!decoded.ok()) {
                    return {decoded.status, std::nullopt, decoded.error};
                }
                return callback(context, *decoded.value);
            });
    }

    [[nodiscard]] Result<bool> declare_unavailable(OperationDefinition definition,
                                                   std::string reason);
    [[nodiscard]] Result<Value> invoke(std::string_view operation_id,
                                       const OperationContext& context,
                                       const Value& input) const;
    [[nodiscard]] Result<Value> invoke(std::string_view operation_id,
                                       std::uint32_t requested_version,
                                       const OperationContext& context,
                                       const Value& input) const;
    [[nodiscard]] std::vector<OperationDescriptor> descriptors() const;

  private:
    using Handler = std::function<Result<Value>(const OperationContext&, const Value&)>;
    struct Entry {
        OperationDefinition definition;
        Handler handler;
        std::string unavailable_reason;
    };
    [[nodiscard]] Result<bool>
    register_handler(OperationDefinition definition, std::string schema_token, Handler handler);
    std::map<std::string, Entry, std::less<>> entries_;
};

} // namespace qcae::operations
