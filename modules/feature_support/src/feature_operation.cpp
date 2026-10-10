#include "qcae/feature_operation.hpp"

namespace qcae::features {
namespace {
template <class T> Result<bool> field_failure(Result<T> result, std::string_view field) {
    if (result.error)
        result.error->field = std::string(field) + "." + result.error->field;
    return forward_failure<bool>(result);
}
} // namespace

Result<bool> normalize(Quantity& value, parameters::Dimension dimension, std::string_view field) {
    auto canonical = parameters::canonical_quantity(value, dimension);
    if (!canonical.ok())
        return field_failure(std::move(canonical), field);
    operations::wire::observe_input_copy(*canonical.value, *canonical.value);
    value = std::move(*canonical.value);
    operations::wire::observe_input_copy(value, value);
    return success(true);
}

Result<bool> normalize(double& value,
                       std::string_view unit,
                       parameters::Dimension dimension,
                       std::string_view field) {
    const Quantity given{value, std::string(unit)};
    ledger::add(ledger::Stage::application,
                ledger::Metric::model_copy_bytes,
                sizeof(Quantity) + unit.size());
    auto canonical = parameters::canonical_quantity(given, dimension);
    if (!canonical.ok())
        return field_failure(std::move(canonical), field);
    operations::wire::observe_input_copy(*canonical.value, *canonical.value);
    value = canonical.value->value;
    ledger::add(ledger::Stage::application, ledger::Metric::model_copy_bytes, sizeof(double));
    return success(true);
}

namespace detail {
void observe_capture(std::size_t moved_input_bytes, const std::string& signature) noexcept {
    ledger::add(ledger::Stage::application, ledger::Metric::model_copy_bytes, moved_input_bytes);
    // One string clone into the plan callback, then a possible short-rep move into std::function.
    ledger::add(ledger::Stage::application,
                ledger::Metric::metadata_copy_bytes,
                signature.size() + operations::wire::moved_bytes(signature));
}

void observe_signature_move(const std::string& signature) noexcept {
    ledger::add(ledger::Stage::application,
                ledger::Metric::metadata_copy_bytes,
                operations::wire::moved_bytes(signature));
}

Result<RecordPreparedOperation>
prepared(PreparedRecordChange change, ChangeSummary summary, const std::string& signature) {
    ledger::add(ledger::Stage::application,
                ledger::Metric::metadata_copy_bytes,
                3 * sizeof(std::string) + summary.label.size() +
                    summary.affected_entity.value.size() + signature.size() +
                    sizeof(summary.creates_entity));
    ledger::add(ledger::Stage::application,
                ledger::Metric::model_copy_bytes,
                sizeof(summary.normalized_value));
    return success(RecordPreparedOperation{std::move(change),
                                           std::move(summary.label),
                                           std::move(summary.affected_entity),
                                           signature,
                                           summary.normalized_value,
                                           summary.creates_entity});
}

void observe_write(const WriteContext& write, const std::string& operation) noexcept {
    ledger::add(ledger::Stage::application,
                ledger::Metric::metadata_copy_bytes,
                sizeof(WriteContext) + write.document.id.value.size() +
                    write.document.epoch.value.size() + operation.size());
}
} // namespace detail
} // namespace qcae::features
