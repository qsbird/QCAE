#include "qcae/operation_registry.hpp"

namespace qcae::operations {
Value change_receipt_value(const ChangeReceipt& receipt) {
    detail::MetadataValueCopies metadata;
    const auto revision = [](Revision value) {
        auto result = std::to_string(value);
        ledger::add(ledger::Stage::application, ledger::Metric::metadata_copy_bytes, result.size());
        return result;
    };
    Value::Object output;
    output.emplace("transaction_id", Value(receipt.transaction.value));
    wire::observe_key("transaction_id");
    output.emplace("committed_revision", Value(revision(receipt.committed_revision)));
    wire::observe_key("committed_revision");
    output.emplace("current_revision", Value(revision(receipt.current_revision)));
    wire::observe_key("current_revision");
    output.emplace("replayed", Value(receipt.replayed));
    wire::observe_key("replayed");
    if (!receipt.primary_entity.value.empty()) {
        output.emplace("entity_id", Value(receipt.primary_entity.value));
        wire::observe_key("entity_id");
    }
    return Value(std::move(output));
}
} // namespace qcae::operations
