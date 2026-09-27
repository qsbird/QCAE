#include "qcae/operation_registry.hpp"

namespace qcae::operations {
Value change_receipt_value(const ChangeReceipt& receipt) {
    Value::Object output{{"transaction_id", Value(receipt.transaction.value)},
                         {"committed_revision", Value(std::to_string(receipt.committed_revision))},
                         {"current_revision", Value(std::to_string(receipt.current_revision))},
                         {"replayed", Value(receipt.replayed)}};
    if (!receipt.primary_entity.value.empty())
        output.emplace("entity_id", Value(receipt.primary_entity.value));
    return Value(std::move(output));
}
} // namespace qcae::operations
