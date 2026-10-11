#pragma once
#include "qcae/edit_session.hpp"
#include "qcae/operation_registry.hpp"
#include "qcae/quantities.hpp"
#include "qcae/record_application.hpp"
#include "qcae/records.hpp"

// Building blocks for typed document-write operations. A feature canonicalizes its input and
// describes the record edit; these helpers own the operation signature, the single
// RecordApplication write path and the copy accounting of every step.
namespace qcae::features {
struct OperationPlan {
    std::string signature;
    RecordPrepare prepare;
};

// History label and receipt facts of one prepared change.
struct ChangeSummary {
    std::string label;
    EntityId affected_entity;
    // Legacy changes.preview field: canonical Young's modulus in MPa for material edits,
    // 0 otherwise.
    // It is part of the wire response and of persisted commit signatures; keep it.
    double normalized_value{};
    bool creates_entity{false};
};

template <class T> Result<T> failed(const RecordError& error) {
    return {Status::failed, std::nullopt, Diagnostic{error.code(), error.what(), error.field()}};
}

// The copy a feature canonicalizes before computing the operation signature.
template <class Input> Input canonical_copy(const Input& input) {
    Input copy = input;
    operations::InputTraits<Input>::observe_copy(copy);
    return copy;
}

// Converts a quantity to the canonical unit of its dimension. Diagnostics name `field`.
[[nodiscard]] Result<bool>
normalize(Quantity& value, parameters::Dimension dimension, std::string_view field);
// Converts a bare number given in `unit` to the canonical unit of its dimension.
[[nodiscard]] Result<bool> normalize(double& value,
                                     std::string_view unit,
                                     parameters::Dimension dimension,
                                     std::string_view field);

// Owned bytes of a generated record, as declared by its generated descriptor.
template <class Record> std::size_t record_bytes(const Record& record) {
    static const auto owned_bytes = [] {
        for (auto& descriptor : generated_record_descriptors())
            if (descriptor.type == RecordTraits<Record>::type_id)
                return descriptor.owned_bytes;
        throw RecordError(ErrorCode::schema_unsupported, "Record type is not generated");
    }();
    return owned_bytes(&record);
}

// Applies the generated field rules to a record built only for checking.
template <class Record> std::optional<RecordError> check_record_fields(const Record& probe) {
    ledger::add(ledger::Stage::application, ledger::Metric::model_copy_bytes, record_bytes(probe));
    try {
        RecordTraits<Record>::validate_fields(probe);
    } catch (const RecordError& error) {
        return error;
    }
    return std::nullopt;
}

// Copies a value that owns heap memory into a record field; scalars are assigned directly.
template <class T> void copy_into(T& target, const T& source) {
    target = source;
    ledger::add(ledger::Stage::application,
                ledger::Metric::model_copy_bytes,
                sizeof(target) + operations::wire::dynamic_bytes(target));
}

// Record edits available while a plan prepares its change. Invalid edits throw RecordError.
class PlanEdit {
  public:
    PlanEdit(const DocumentView& view, const RecordIdentityAllocator& allocate)
        : session_(view), allocate_(allocate) {}
    EntityId allocate() const {
        return allocate_();
    }
    template <class Record> void put(Record record) {
        ledger::add(
            ledger::Stage::application, ledger::Metric::model_copy_bytes, record_bytes(record));
        session_.put(std::move(record));
    }
    template <class Record, class Id, class Change> void update(const Id& id, Change change) {
        session_.template update<Record>(id, std::move(change));
    }
    PreparedRecordChange prepare() const {
        return session_.prepare();
    }

  private:
    EditSession session_;
    const RecordIdentityAllocator& allocate_;
};

namespace detail {
void observe_capture(std::size_t moved_input_bytes, const std::string& signature) noexcept;
void observe_signature_move(const std::string& signature) noexcept;
Result<RecordPreparedOperation>
prepared(PreparedRecordChange change, ChangeSummary summary, const std::string& signature);
void observe_write(const WriteContext& write, const std::string& operation) noexcept;
} // namespace detail

// Plans a document write. `change(canonical, edit)` runs inside the single-writer section of
// RecordApplication and returns the ChangeSummary of the edits it made. The retry signature
// covers `signature_source`; use the overload below when it is the canonical input alone.
template <class Input, class Change>
Result<OperationPlan>
record_plan(Input canonical, const operations::Value& signature_source, Change change) {
    auto signature = operations::canonical_value(signature_source);
    if (!signature.ok())
        return forward_failure<OperationPlan>(signature);
    detail::observe_capture(2 * operations::InputTraits<Input>::moved_bytes(canonical),
                            *signature.value);
    RecordPrepare prepare =
        [canonical = std::move(canonical),
         signature = *signature.value,
         change = std::move(change)](
            const DocumentView& view,
            const RecordIdentityAllocator& allocate) -> Result<RecordPreparedOperation> {
        try {
            PlanEdit edit(view, allocate);
            auto summary = change(canonical, edit);
            return detail::prepared(edit.prepare(), std::move(summary), signature);
        } catch (const RecordError& error) {
            return failed<RecordPreparedOperation>(error);
        }
    };
    detail::observe_signature_move(*signature.value);
    return success(OperationPlan{std::move(*signature.value), std::move(prepare)});
}
template <class Input, class Change>
Result<OperationPlan> record_plan(Input canonical, Change change) {
    const auto signature_source = operations::InputTraits<Input>::to_value(canonical);
    return record_plan(std::move(canonical), signature_source, std::move(change));
}

// Commits a plan through RecordApplication and returns the change receipt.
template <class Input>
Result<operations::Value> execute_plan(RecordApplication& app,
                                       const operations::OperationContext& context,
                                       const Result<OperationPlan>& plan) {
    if (!plan.ok())
        return forward_failure<operations::Value>(plan);
    // Presence is validated by registry metadata; identity, revision and authorization stay in
    // the application.
    const WriteContext write{*context.document, *context.expected_revision};
    const std::string operation(operations::InputTraits<Input>::operation_id);
    detail::observe_write(write, operation);
    const auto receipt = app.execute(context.caller,
                                     write,
                                     operation,
                                     plan.value->signature,
                                     plan.value->prepare,
                                     context.idempotency_key);
    if (!receipt.ok())
        return forward_failure<operations::Value>(receipt);
    return success(operations::change_receipt_value(*receipt.value));
}

// Prepares a plan against the current revision without committing it.
inline Result<ChangePreview> preview_plan(RecordApplication& app,
                                          const operations::OperationContext& context,
                                          const Result<OperationPlan>& plan) {
    if (!plan.ok())
        return forward_failure<ChangePreview>(plan);
    return app.preview(
        context.caller, {*context.document, *context.expected_revision}, plan.value->prepare);
}

// Registers a typed document-write operation whose plan comes from `prepare`.
template <class Input>
Result<bool> register_plan(operations::OperationRegistry& registry,
                           RecordApplication& app,
                           Result<OperationPlan> (*prepare)(const Input&)) {
    return registry.register_typed<Input>(
        operations::InputTraits<Input>::definition(),
        [&app, prepare](const operations::OperationContext& context, const Input& input) {
            return execute_plan<Input>(app, context, prepare(input));
        });
}

// Lists a scaffolded operation as unavailable, so discovery never offers it before its plan exists.
template <class Input> Result<bool> declare_unimplemented(operations::OperationRegistry& registry) {
    return registry.declare_unavailable(operations::InputTraits<Input>::definition(),
                                        std::string(operations::InputTraits<Input>::operation_id) +
                                            " is not implemented");
}
} // namespace qcae::features
