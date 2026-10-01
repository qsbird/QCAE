#pragma once
#include "qcae/operation_registry.hpp"
#include <QJsonArray>
#include <cstddef>

namespace qcae::ipc::detail {
// Both response encoding and quota accounting use the same typed value writer.
QJsonArray typed_json_array(const operations::Value::Array&);
std::size_t typed_json_array_bytes(const operations::Value::Array&);
} // namespace qcae::ipc::detail
