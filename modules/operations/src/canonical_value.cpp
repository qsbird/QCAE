#include "qcae/operation_registry.hpp"
#include "qcae/operation_ledger.hpp"
#include <charconv>
#include <cmath>
#include <limits>

namespace qcae::operations {
namespace {
void append(std::string& output, std::string_view value) {
    const auto size = output.size();
    const auto capacity = output.capacity();
    output.append(value);
    ledger::add(ledger::Stage::application, ledger::Metric::metadata_copy_bytes, value.size());
    if (output.capacity() != capacity)
        ledger::add(ledger::Stage::application, ledger::Metric::metadata_copy_bytes, size);
}
template <class Integer> std::string decimal(Integer value) {
    auto output = std::to_string(value);
    ledger::add(ledger::Stage::application, ledger::Metric::metadata_copy_bytes, output.size());
    return output;
}
void text(std::string& output, char tag, std::string_view value) {
    append(output, std::string_view(&tag, 1));
    append(output, decimal(value.size()));
    append(output, ":");
    append(output, value);
}
std::optional<Diagnostic> encode(const Value& value, std::string& output, std::size_t depth) {
    if (depth > 64) {
        return Diagnostic{ErrorCode::resource_limit, "Value nesting exceeds 64 levels.", "value"};
    }
    if (const auto* boolean = std::get_if<bool>(&value.data)) {
        append(output, *boolean ? "b1;" : "b0;");
    } else if (const auto* integer = std::get_if<std::int64_t>(&value.data)) {
        text(output, 'n', decimal(*integer));
    } else if (const auto* real = std::get_if<double>(&value.data)) {
        if (!std::isfinite(*real)) {
            return Diagnostic{
                ErrorCode::invalid_input, "Canonical numbers must be finite.", "value"};
        }
        // 2^63 is exactly representable; unlike rounded INT64_MAX it is a safe exclusive bound.
        constexpr double signed_limit = 9223372036854775808.0;
        if (std::trunc(*real) == *real && *real >= -signed_limit && *real < signed_limit) {
            text(output, 'n', decimal(static_cast<std::int64_t>(*real)));
        } else {
            std::array<char, 64> buffer{};
            ledger::add(
                ledger::Stage::application, ledger::Metric::metadata_copy_bytes, buffer.size());
            const auto encoded = std::to_chars(
                buffer.data(), buffer.data() + buffer.size(), *real, std::chars_format::general);
            if (encoded.ec != std::errc{}) {
                return Diagnostic{ErrorCode::invalid_input, "Number encoding failed.", "value"};
            }
            ledger::add(ledger::Stage::application,
                        ledger::Metric::metadata_copy_bytes,
                        static_cast<std::size_t>(encoded.ptr - buffer.data()));
            text(output, 'n', std::string_view(buffer.data(), encoded.ptr));
        }
    } else if (const auto* string = std::get_if<std::string>(&value.data)) {
        text(output, 's', *string);
    } else if (const auto* array = std::get_if<Value::Array>(&value.data)) {
        append(output, "a");
        append(output, decimal(array->size()));
        append(output, ":");
        for (const auto& item : *array) {
            if (const auto error = encode(item, output, depth + 1)) {
                return error;
            }
        }
    } else {
        const auto& object = std::get<Value::Object>(value.data);
        append(output, "o");
        append(output, decimal(object.size()));
        append(output, ":");
        for (const auto& [key, item] : object) {
            text(output, 's', key);
            if (const auto error = encode(item, output, depth + 1)) {
                return error;
            }
        }
    }
    return std::nullopt;
}
} // namespace

Result<std::string> canonical_value(const Value& value) {
    try {
        std::string output{"QCV1:"};
        ledger::add(ledger::Stage::application, ledger::Metric::metadata_copy_bytes, output.size());
        if (auto error = encode(value, output, 0)) {
            return {Status::failed, std::nullopt, std::move(error)};
        }
        return {Status::success, std::move(output), std::nullopt};
    } catch (...) {
        ledger::unknown(ledger::Stage::application, ledger::Metric::metadata_copy_bytes);
        throw;
    }
}
} // namespace qcae::operations
