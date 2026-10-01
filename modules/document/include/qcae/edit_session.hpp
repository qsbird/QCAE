#pragma once

#include "qcae/document_view.hpp"
#include "qcae/operation_ledger.hpp"

namespace qcae {
struct PreparedRecordChange {
    RecordVersion base;
    DocumentView candidate;
    RecordChangeSet changes;
    RecordStats stats;
};

// A private overlay. Preparing does not publish or advance the document revision.
class EditSession {
  public:
    explicit EditSession(DocumentView base, RecordLimits = {});

    template <class T> void put(T value) {
        auto record = base_.registry()->make(std::move(value), &stats_);
        put(std::move(record));
    }

    template <class T, class IdType, class Function> void update(const IdType& id, Function edit) {
        const auto record = find(RecordTraits<T>::type_id, id.value);
        if (!record)
            throw RecordError(ErrorCode::entity_not_found, "Record does not exist", id.value);
        T value = record->template get<T>();
        ledger::add(ledger::Stage::records,
                    ledger::Metric::model_copy_bytes,
                    record->descriptor().owned_bytes(record->object()));
        edit(value);
        if (RecordTraits<T>::identity(value) != id.value)
            throw RecordError(ErrorCode::invalid_input, "Updating a record cannot change its ID");
        put(std::move(value));
    }

    void put(Record);
    void erase(const RecordKey&);
    Record find(RecordTypeId, std::string_view) const;
    PreparedRecordChange prepare() const;

  private:
    DocumentView base_;
    RecordLimits limits_;
    std::map<RecordKey, RecordChange> pending_;
    RecordStats stats_;
};
} // namespace qcae
