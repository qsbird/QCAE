#pragma once
#include "qcae/operation_registry.hpp"
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <stdexcept>
#include <cstddef>

namespace qcae::ipc::detail {
// Both response encoding and quota accounting use the same typed value writer.
QJsonArray typed_json_array(const operations::Value::Array&);
std::size_t typed_json_array_bytes(const operations::Value::Array&);

// Request parameters as a generated-input value; throws std::invalid_argument on JSON null.
operations::Value typed_value(const QJsonValue&);
// The response for parameters a generated contract rejected, shaped like typed-host failures.
QJsonObject typed_input_failure(const QString& id, Status status, const Diagnostic& error);
// Decodes IPC parameters through the operation's generated contract.
template <class Input> Result<Input> typed_input(const QJsonObject& parameters) {
    try {
        return operations::InputTraits<Input>::from_value(typed_value(parameters));
    } catch (const std::invalid_argument& error) {
        return {Status::failed,
                std::nullopt,
                Diagnostic{ErrorCode::invalid_input, error.what(), "input"}};
    }
}
} // namespace qcae::ipc::detail
