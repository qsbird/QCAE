#pragma once

#include "qcae/record_registry.hpp"

namespace qcae {
inline constexpr std::size_t record_page_capacity = 1024;

struct RecordLimits {
    std::size_t max_records{500000};
    std::size_t max_references{2000000};
    std::size_t max_record_bytes{record_wire::maximum_record_bytes};
};

struct RecordChange {
    RecordKey key;
    std::optional<Record> before;
    std::optional<Record> after;
    std::vector<RecordFieldId> fields;
};
struct RecordChangeSet {
    std::vector<RecordChange> records;
    bool empty() const noexcept;
};
enum class RecordDirection { forward, reverse };

struct RecordDocumentState;

class DocumentView {
  public:
    explicit DocumentView(std::shared_ptr<const RecordRegistry>, RecordVersion = {});
    RecordVersion version() const;
    std::shared_ptr<const RecordRegistry> registry() const noexcept;
    std::size_t size() const noexcept;
    std::size_t count(RecordTypeId) const noexcept;
    Record find(RecordTypeId, std::string_view identity) const noexcept;
    Record find(const RecordKey&) const noexcept;
    Record find_identity(std::string_view identity) const noexcept;
    void visit(RecordTypeId, const std::function<void(const Record&)>&) const;
    void visit(const std::function<void(const Record&)>&) const;
    void validate(RecordLimits = {}) const;
    DocumentView with_version(RecordVersion) const;

    template <class T, class IdType> Record find(const IdType& id) const noexcept {
        return find(RecordTraits<T>::type_id, id.value);
    }

  private:
    friend DocumentView apply_record_changes(const DocumentView&,
                                             const RecordChangeSet&,
                                             RecordDirection,
                                             RecordStats*);
    std::shared_ptr<const RecordDocumentState> state_;
    RecordVersion version_;
};

DocumentView apply_record_changes(const DocumentView&,
                                  const RecordChangeSet&,
                                  RecordDirection = RecordDirection::forward,
                                  RecordStats* = nullptr);
RecordChangeSet diff_record_views(const DocumentView&, const DocumentView&);
std::string encode_record_changes(const RecordChangeSet&, RecordStats* = nullptr);
RecordChangeSet decode_record_changes(const RecordRegistry&, std::string_view);
} // namespace qcae
