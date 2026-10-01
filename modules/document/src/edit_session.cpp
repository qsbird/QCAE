#include "qcae/edit_session.hpp"

#include <algorithm>

namespace qcae {
namespace {
void observe_key_copy(const RecordKey& key) noexcept {
    ledger::add(ledger::Stage::records,
                ledger::Metric::metadata_copy_bytes,
                sizeof(RecordKey) + key.identity.size());
}
void append_field(std::vector<RecordFieldId>& fields, const RecordFieldId& field) {
    const auto size = fields.size();
    const auto capacity = fields.capacity();
    fields.push_back(field);
    ledger::add(ledger::Stage::records, ledger::Metric::metadata_copy_bytes, sizeof(field));
    if (fields.capacity() != capacity)
        ledger::add(
            ledger::Stage::records, ledger::Metric::metadata_copy_bytes, size * sizeof(field));
}
} // namespace
EditSession::EditSession(DocumentView base, RecordLimits limits)
    : base_(std::move(base)), limits_(limits) {}

void EditSession::put(Record record) {
    if (!record || base_.registry()->find(record->key().type) != &record->descriptor())
        throw RecordError(ErrorCode::invalid_input, "Record does not belong to this registry");
    if (record->encoded().size() > limits_.max_record_bytes)
        throw RecordError(ErrorCode::resource_limit, "Record byte quota exceeded");
    const auto key = record->key();
    observe_key_copy(key);
    auto [found, inserted] = pending_.try_emplace(key);
    if (inserted) {
        observe_key_copy(key);
        found->second.key = key;
        observe_key_copy(key);
        if (const auto before = base_.find(key))
            found->second.before = before;
    }
    found->second.after = std::move(record);
    if (found->second.before &&
        (*found->second.before)->encoded() == (*found->second.after)->encoded())
        pending_.erase(found);
}
void EditSession::erase(const RecordKey& key) {
    if (!find(key.type, key.identity))
        throw RecordError(ErrorCode::entity_not_found, "Record does not exist", key.identity);
    auto [found, inserted] = pending_.try_emplace(key);
    if (inserted) {
        observe_key_copy(key);
        found->second.key = key;
        observe_key_copy(key);
        found->second.before = base_.find(key);
    }
    found->second.after.reset();
    if (!found->second.before)
        pending_.erase(found);
}
Record EditSession::find(RecordTypeId type, std::string_view identity) const {
    const RecordKey key{type, std::string(identity)};
    observe_key_copy(key);
    const auto found = pending_.find(key);
    if (found == pending_.end())
        return base_.find(type, identity);
    return found->second.after ? *found->second.after : Record{};
}
PreparedRecordChange EditSession::prepare() const {
    RecordChangeSet changes;
    changes.records.reserve(pending_.size());
    for (const auto& [unused, value] : pending_) {
        (void)unused;
        auto change = value;
        ledger::add(ledger::Stage::records,
                    ledger::Metric::metadata_copy_bytes,
                    sizeof(RecordChange) + value.key.identity.size() +
                        value.fields.size() * sizeof(RecordFieldId));
        if (change.before && change.after) {
            const auto before = record_wire::decode((*change.before)->encoded());
            const auto after = record_wire::decode((*change.after)->encoded());
            for (const auto& field : (*change.after)->descriptor().fields) {
                const auto* left = record_wire::find(before, field.id);
                const auto* right = record_wire::find(after, field.id);
                if ((!left != !right) || (left && right && left->payload != right->payload))
                    append_field(change.fields, field.id);
            }
        } else {
            const auto& image = change.after ? *change.after : *change.before;
            for (const auto& field : image->descriptor().fields)
                append_field(change.fields, field.id);
        }
        changes.records.push_back(std::move(change));
    }
    RecordStats stats = stats_;
    for (const auto& change : changes.records)
        if (change.before && change.after)
            stats.model_bytes_copied +=
                (*change.before)->encoded().size() + (*change.after)->encoded().size();
    auto candidate = apply_record_changes(base_, changes, RecordDirection::forward, &stats);
    candidate.validate(limits_);
    return {base_.version(), std::move(candidate), std::move(changes), stats};
}
} // namespace qcae
