#pragma once
#include "qcae/immutable_fact_map.hpp"
#include "qcae/application_types.hpp"
#include "qcae/edit_session.hpp"
#include "qcae/record_store.hpp"
#include <functional>
#include <map>
#include <memory>

namespace qcae {
struct RecordSnapshot {
    DocumentView records;
    DocumentInfo info;
};
// Notifications and render deltas borrow immutable records from committed history.
// A reverse entry applies its changes' after->before images without copying records.
struct CommittedRecordChange {
    DocumentRef document;
    Revision base_revision{}, revision{};
    TransactionId transaction;
    std::shared_ptr<const RecordChangeSet> changes;
    RecordDirection direction{RecordDirection::forward};
};
struct RecordChangeBatch {
    DocumentRef document;
    Revision base_revision{}, current_revision{};
    bool resync_required{};
    std::vector<CommittedRecordChange> changes;
};
struct ChangeJournalStats {
    std::uint64_t metadata_bytes_copied{}, entries_published{}, records_referenced{};
};
struct RecordPreparedOperation {
    PreparedRecordChange change;
    std::string label;
    EntityId affected_entity;
    std::string signature;
    double normalized_value{};
    bool creates_entity{};
};
using RecordIdentityAllocator = std::function<EntityId()>;
// Runs inside the single-writer critical section. The callback edits only its
// supplied immutable view and must not re-enter this application.
using RecordPrepare = std::function<Result<RecordPreparedOperation>(
    const DocumentView&, const RecordIdentityAllocator&)>;
struct ProjectRecordMigration {
    // Identifies the explicit source/target semantics for host-operation replay.
    std::string signature;
    std::function<PreparedRecordChange(const DocumentView&)> prepare;
};

// Restoration images are values passed at the migration boundary, never mutable
// handles into an active application. Ordinary writes accept prepared changes.
struct RecordHistoryImage {
    TransactionId transaction;
    std::string label;
    RecordChangeSet changes;
    std::string content_state;
    SharedStoreBytes encoded{};
};
struct RecordOperationImage {
    std::string signature;
    ChangeReceipt receipt;
    SharedStoreBytes encoded{};
};
struct RecordHostImage {
    std::string signature;
    DocumentInfo result;
    SharedStoreBytes encoded{};
};
struct RecordSaveImage {
    std::string host_key, signature, path, token, project_id, snapshot;
    bool save_as{};
};
struct OwnedRowImage {
    StoreKey key;
    std::string owner;
    std::uint32_t schema_version{1};
    SharedStoreBytes payload;
    SharedStoreBytes encoded{};
};
using OwnedRowTable = std::map<StoreKey, std::shared_ptr<const OwnedRowImage>>;
struct OwnedRowPage {
    std::vector<std::shared_ptr<const OwnedRowImage>> rows;
    bool overflow{};
};
struct OwnedRowUpdate {
    StoreKey key;
    SharedStoreBytes expected; // Null requires absence; otherwise compare exact payload bytes.
    std::shared_ptr<const OwnedRowImage> after; // Null deletes an existing row.
};
struct OwnedRowHandler {
    StoreSpace space;
    std::string owner;
    std::function<void(const OwnedRowImage&)> validate;
    // Invoked during explicit recovery/open, never during read-only decoding.
    std::function<std::shared_ptr<const OwnedRowImage>(const OwnedRowImage&)> recover;
    std::function<bool(const OwnedRowImage&)> blocks_close;
};
using CommitOwnedRows = std::function<std::vector<OwnedRowUpdate>(const ChangeReceipt&)>;
struct RecordStateImage {
    explicit RecordStateImage(std::shared_ptr<const RecordRegistry> registry)
        : records(std::move(registry)) {}
    DocumentView records;
    std::string application_nonce;
    std::uint64_t next_id{1};
    std::optional<DocumentInfo> document;
    std::string initial_content_state;
    std::vector<std::shared_ptr<const RecordHistoryImage>> history;
    std::size_t cursor{};
    ImmutableFactMap<RecordOperationImage> operations;
    ImmutableFactMap<RecordHostImage> host_operations;
    std::shared_ptr<const RecordSaveImage> save_intent;
    bool recoverable{};
    std::shared_ptr<const OwnedRowTable> owned_rows = std::make_shared<const OwnedRowTable>();
};
struct RecordProjectImage {
    DocumentView records;
    std::string project_id, name, content_state;
    std::shared_ptr<const OwnedRowTable> owned_rows = std::make_shared<const OwnedRowTable>();
};
struct RecordApplicationOptions {
    std::shared_ptr<const RecordRegistry> registry;
    std::shared_ptr<IRecordStore> records;
    std::shared_ptr<IWorkspaceStore> projects;
    Limits limits;
    std::function<std::size_t(const DocumentView&)> material_count;
    std::function<std::optional<Diagnostic>(const DocumentView&, const Limits&)> validate;
    std::function<bool(const DocumentView&)> profiles_supported;
    std::function<RecordProjectImage(std::string_view)> decode_legacy_project;
    std::vector<OwnedRowHandler> owned_row_handlers;
    std::size_t max_owned_rows{4096};
    std::size_t max_owned_row_bytes{65536};
    std::size_t max_change_journal_entries{128};
};
std::vector<StoredRow> encode_record_state_image(const RecordStateImage&);
RecordStateImage decode_record_state_image(std::span<const StoredRow>,
                                           std::shared_ptr<const RecordRegistry>,
                                           Limits = {},
                                           std::span<const OwnedRowHandler> = {});
std::string encode_record_rows(std::span<const StoredRow>);
std::vector<StoredRow> decode_record_rows(std::string_view);

class RecordApplication {
  public:
    explicit RecordApplication(RecordApplicationOptions);
    ~RecordApplication();
    RecordApplication(const RecordApplication&) = delete;
    RecordApplication& operator=(const RecordApplication&) = delete;
    bool durable() const noexcept;
    bool recovery_available() const;
    Result<DocumentInfo> create_document(const Caller&, const std::string&, const std::string&);
    Result<DocumentInfo> current_document() const;
    Result<DocumentInfo> open_document(const Caller&, const std::string&, const std::string&);
    Result<DocumentInfo> open_migrated_document(const Caller&,
                                                const std::string& source_path,
                                                const std::string& idempotency_key,
                                                const ProjectRecordMigration&);
    Result<DocumentInfo> recover_document(const Caller&, const std::string&);
    Result<DocumentInfo>
    save_document(const Caller&, const WriteContext&, const std::string&, bool, const std::string&);
    Result<DocumentInfo>
    close_document(const Caller&, const WriteContext&, ClosePolicy, const std::string&);
    Result<DocumentInfo>
    host_operation(const Caller&, const std::string&, const std::string&) const;
    Result<RecordSnapshot> snapshot(const DocumentRef&) const;
    Result<RecordChangeBatch> changes_since(const DocumentRef&, Revision) const;
    ChangeJournalStats change_journal_stats() const;
    Result<ChangePreview> preview(const Caller&, const WriteContext&, const RecordPrepare&);
    Result<ChangeReceipt>
    commit(const Caller&, const WriteContext&, const PreviewId&, const std::string&);
    Result<ChangeReceipt> commit_with_rows(const Caller&,
                                           const WriteContext&,
                                           const PreviewId&,
                                           const std::string&,
                                           const CommitOwnedRows&);
    Result<ChangeReceipt> execute(const Caller&,
                                  const WriteContext&,
                                  const std::string& operation_name,
                                  const std::string& normalized_signature,
                                  const RecordPrepare&,
                                  const std::string& idempotency_key,
                                  const CommitOwnedRows& = {});
    Result<ChangeReceipt> action_outcome(const Caller&,
                                         const DocumentRef&,
                                         const std::string& operation_name,
                                         const std::string& idempotency_key) const;
    Result<bool>
    update_owned_rows(const Caller&, const DocumentRef&, std::span<const OwnedRowUpdate>);
    // User-initiated auxiliary writes must admit the revision and CAS together.
    Result<bool>
    update_owned_rows(const Caller&, const WriteContext&, std::span<const OwnedRowUpdate>);
    Result<std::vector<std::shared_ptr<const OwnedRowImage>>>
    owned_rows(const DocumentRef&, StoreSpace, std::string_view owner) const;
    // Reads only the indexed identity-prefix range. A bounded page borrows the
    // immutable row payloads; callers still validate ownership and row schemas.
    Result<OwnedRowPage> owned_rows_with_prefix(const DocumentRef&,
                                                StoreSpace,
                                                std::string_view identity_prefix,
                                                std::size_t limit) const;
    Result<ChangeReceipt> undo(const Caller&, const WriteContext&, const std::string&);
    Result<ChangeReceipt> redo(const Caller&, const WriteContext&, const std::string&);
    Result<HistorySnapshot> history(const DocumentRef&) const;
    Result<ChangeReceipt>
    operation(const Caller&, const DocumentRef&, const std::string&, const std::string&) const;
    RecordStats stats() const;

  private:
    Result<bool> update_owned_rows_checked(const Caller&,
                                           const DocumentRef&,
                                           std::optional<Revision>,
                                           std::span<const OwnedRowUpdate>);
    Result<DocumentInfo> open_document_impl(const Caller&,
                                            const std::string&,
                                            const std::string&,
                                            const ProjectRecordMigration*);
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace qcae
