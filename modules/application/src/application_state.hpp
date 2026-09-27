#pragma once
#include "qcae/record_application.hpp"

namespace qcae::record_detail {
struct Prepared {
    Caller caller;
    WriteContext context;
    bool create{};
    EntityId entity;
    std::string name;
    double modulus{};
    std::shared_ptr<const PreparedRecordChange> after;
    std::string label;
};
using HistoryEntry = RecordHistoryImage;
using RecordedOperation = RecordOperationImage;
using HostOperation = RecordHostImage;
using SaveIntent = RecordSaveImage;
struct Data : RecordStateImage {
    explicit Data(std::shared_ptr<const RecordApplicationOptions> settings)
        : RecordStateImage(settings->registry), limits(settings->limits),
          options(std::move(settings)) {}
    Limits limits;
    std::shared_ptr<const RecordApplicationOptions> options;
    std::map<std::string, Prepared> previews;
    std::vector<RowMutation> pending;
    RecordStats stats;
    // Transient, bounded and never written to a workspace/project snapshot.
    std::shared_ptr<const std::vector<std::shared_ptr<const CommittedRecordChange>>> change_journal;
    ChangeJournalStats journal_stats;
};
std::string nonce();
void update_document(Data&);
std::optional<Diagnostic> validate_candidate(const DocumentView&, const Data&);
std::string encode_project(Data&, const std::string&);
Data decode_project(std::string_view, std::shared_ptr<const RecordApplicationOptions>);
Data decode_data(const LoadedRows&, std::shared_ptr<const RecordApplicationOptions>);
void replace_records(Data&, DocumentView);
void clear_document_rows(Data&);
void queue_changes(Data&, const RecordChangeSet&, RecordDirection = RecordDirection::forward);
void queue_owned_rows(Data&, std::span<const OwnedRowUpdate>);
void recover_owned_rows(Data&);
bool owned_rows_block_close(const Data&);
std::shared_ptr<const HistoryEntry>
make_history(TransactionId, std::string, RecordChangeSet, std::string, RecordStats*);
std::optional<Diagnostic> persist(Data&, IRecordStore*, std::uint64_t&, bool&);
} // namespace qcae::record_detail
