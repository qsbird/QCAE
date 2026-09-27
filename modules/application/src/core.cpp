#include "qcae/record_application.hpp"
#include "application_state.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <iomanip>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <type_traits>

namespace qcae {
namespace {
using namespace record_detail;

template <class T> Result<T> success(T value) {
    return {Status::success, std::move(value), std::nullopt};
}

template <class T>
Result<T> failure(Status status, ErrorCode code, const char* message, const char* field = "") {
    return {status, std::nullopt, Diagnostic{code, message, field}};
}

std::string scoped_key(const Caller& caller, const std::string& operation, const std::string& key) {
    return std::to_string(caller.principal.size()) + ":" + caller.principal +
           std::to_string(operation.size()) + ":" + operation + std::to_string(key.size()) + ":" +
           key;
}

void append_part(std::string& to, const std::string& part) {
    to += std::to_string(part.size()) + ":" + part;
}

std::string context_signature(const WriteContext& context) {
    std::string result;
    append_part(result, context.document.id.value);
    append_part(result, context.document.epoch.value);
    append_part(result, std::to_string(context.expected_revision));
    return result;
}

bool blank(const std::string& text) {
    return text.find_first_not_of(" \t\r\n") == std::string::npos;
}
bool profiles_supported(const DocumentView& view, const RecordApplicationOptions& options) {
    return !options.profiles_supported || options.profiles_supported(view);
}

std::string
commit_signature(const WriteContext& context, const PreviewId& preview, const Prepared& prepared) {
    std::string result = context_signature(context);
    append_part(result, preview.value);
    append_part(result, prepared.create ? "create" : "set");
    append_part(result, prepared.entity.value);
    append_part(result, prepared.name);
    append_part(result, std::to_string(std::bit_cast<std::uint64_t>(prepared.modulus)));
    return result;
}

struct ProjectLeaseGuard {
    IWorkspaceStore* store;
    std::string keep_path;
    bool release{true};
    ~ProjectLeaseGuard() {
        if (release)
            store->release_projects_except(keep_path);
    }
};

std::string new_id(Data& data, const char* prefix) {
    return std::string(prefix) + "-" + data.application_nonce + "-" +
           std::to_string(data.next_id++);
}

bool same_document(const DocumentRef& a, const DocumentRef& b) {
    return a.id == b.id && a.epoch == b.epoch;
}

template <class T>
std::optional<Result<T>> check_document(const Data& data, const DocumentRef& ref) {
    if (!data.document || data.recoverable || data.document->document.id != ref.id)
        return failure<T>(
            Status::failed, ErrorCode::document_not_found, "Document is not active", "document_id");
    if (data.document->document.epoch != ref.epoch)
        return failure<T>(Status::conflict,
                          ErrorCode::document_epoch_expired,
                          "Document epoch has expired",
                          "document_epoch");
    return std::nullopt;
}

template <class T>
std::optional<Result<T>> check_revision(const Data& data, const WriteContext& context) {
    if (context.expected_revision != data.document->revision)
        return failure<T>(Status::conflict,
                          ErrorCode::revision_conflict,
                          "Document revision has changed",
                          "expected_revision");
    return std::nullopt;
}

ChangeReceipt current_receipt(const Data& data, ChangeReceipt recorded, bool replayed) {
    recorded.current_revision = data.document->revision;
    recorded.current_content_state = data.document->content_state;
    recorded.replayed = replayed;
    return recorded;
}

Result<ChangePreview> stage_preview(Data& data, Prepared prepared) {
    Data candidate = data;
    PreviewId id(new_id(candidate, "preview"));
    ChangePreview response{
        id, prepared.context, prepared.entity, prepared.modulus, prepared.create};
    candidate.previews.emplace(id.value, std::move(prepared));
    auto result = success(std::move(response));
    using std::swap;
    swap(data, candidate);
    return result;
}

void drop_expired_previews(Data& data) {
    for (auto it = data.previews.begin(); it != data.previews.end();) {
        if (it->second.context.expected_revision != data.document->revision)
            it = data.previews.erase(it);
        else
            ++it;
    }
}

} // namespace

struct RecordApplication::State {
    explicit State(std::shared_ptr<const RecordApplicationOptions> settings)
        : data(settings), store(settings->projects), records(settings->records) {}
    mutable std::mutex mutex;
    Data data;
    std::shared_ptr<IWorkspaceStore> store;
    std::shared_ptr<IRecordStore> records;
    std::uint64_t generation{};
    bool poisoned{};
    Result<ChangePreview> prepare(const Caller&, const WriteContext&, const RecordPrepare&);
    Result<ChangeReceipt> commit(const Caller&,
                                 const WriteContext&,
                                 const PreviewId&,
                                 const std::string&,
                                 const CommitOwnedRows&,
                                 const std::string& operation_name = "commit",
                                 const std::optional<std::string>& direct_signature = {});
};

RecordApplication::RecordApplication(RecordApplicationOptions options)
    : state_(std::make_unique<State>(
          std::make_shared<const RecordApplicationOptions>(std::move(options)))) {
    state_->data.application_nonce = nonce();
    if (state_->records) {
        auto loaded = state_->records->load_rows();
        state_->data = decode_data(loaded, state_->data.options);
        state_->generation = loaded.generation;
        state_->data.recoverable = state_->data.document.has_value();
    }
}
RecordApplication::~RecordApplication() = default;

static_assert(std::is_nothrow_move_constructible_v<Result<DocumentInfo>>);
static_assert(std::is_nothrow_move_constructible_v<Result<ChangePreview>>);
static_assert(std::is_nothrow_move_constructible_v<Result<ChangeReceipt>>);

Result<DocumentInfo> RecordApplication::create_document(const Caller& caller,
                                                        const std::string& name,
                                                        const std::string& idempotency_key) {
    std::lock_guard lock(state_->mutex);
    const Data& current = state_->data;
    if (blank(caller.principal) || blank(idempotency_key) || blank(name))
        return failure<DocumentInfo>(Status::needs_input,
                                     ErrorCode::missing_input,
                                     "Caller, document name and idempotency key are required");
    if (name.size() > current.limits.max_name_bytes)
        return failure<DocumentInfo>(
            Status::failed, ErrorCode::resource_limit, "Document name exceeds byte limit", "name");
    const auto key = scoped_key(caller, "create_document", idempotency_key);
    if (const auto found = current.host_operations.find(key);
        found != current.host_operations.end()) {
        if (found->second.signature != name)
            return failure<DocumentInfo>(Status::conflict,
                                         ErrorCode::idempotency_key_conflict,
                                         "Key was used with another document name",
                                         "idempotency_key");
        return success(found->second.result);
    }
    if (current.document)
        return failure<DocumentInfo>(Status::conflict,
                                     ErrorCode::document_already_open,
                                     "An active document already exists");
    if (current.host_operations.size() + current.operations.size() >=
        current.limits.max_idempotency_records)
        return failure<DocumentInfo>(
            Status::failed, ErrorCode::resource_limit, "Idempotency record limit reached");
    Data candidate = current;
    DocumentInfo info;
    info.document.id = DocumentId(new_id(candidate, "doc"));
    info.document.epoch = DocumentEpoch(new_id(candidate, "epoch"));
    info.content_state = new_id(candidate, "state");
    info.name = name;
    info.durable = static_cast<bool>(state_->records);
    candidate.initial_content_state = info.content_state;
    candidate.document = info;
    update_document(candidate);
    candidate.host_operations.emplace(key, HostOperation{name, info});
    if (auto error =
            persist(candidate, state_->records.get(), state_->generation, state_->poisoned))
        return {Status::failed, std::nullopt, std::move(error)};
    auto result = success(std::move(info));
    static_assert(std::is_nothrow_swappable_v<Data>);
    using std::swap;
    swap(state_->data, candidate);
    return result;
}

Result<RecordSnapshot> RecordApplication::snapshot(const DocumentRef& ref) const {
    std::lock_guard lock(state_->mutex);
    if (auto error = check_document<RecordSnapshot>(state_->data, ref))
        return *error;
    return success(RecordSnapshot{state_->data.records, *state_->data.document});
}
RecordStats RecordApplication::stats() const {
    std::lock_guard lock(state_->mutex);
    return state_->data.stats;
}
Result<ChangePreview> RecordApplication::preview(const Caller& caller,
                                                 const WriteContext& context,
                                                 const RecordPrepare& handler) {
    std::lock_guard lock(state_->mutex);
    return state_->prepare(caller, context, handler);
}
Result<ChangePreview> RecordApplication::State::prepare(const Caller& caller,
                                                        const WriteContext& context,
                                                        const RecordPrepare& handler) {
    auto* state_ = this;
    const Data& current = state_->data;
    if (blank(caller.principal))
        return failure<ChangePreview>(
            Status::needs_input, ErrorCode::missing_input, "Caller is required", "caller");
    if (auto error = check_document<ChangePreview>(current, context.document))
        return *error;
    if (auto error = check_revision<ChangePreview>(current, context))
        return *error;
    if (current.previews.size() >= current.limits.max_previews)
        return failure<ChangePreview>(
            Status::failed, ErrorCode::resource_limit, "Preview limit reached");
    Data candidate = current;
    try {
        auto result = handler(current.records, [&] {
            EntityId id;
            do {
                id = EntityId(new_id(candidate, "entity"));
            } while (candidate.records.find_identity(id.value));
            return id;
        });
        if (!result.ok())
            return {result.status, {}, result.error};
        auto& operation = *result.value;
        if (!same_record_version(operation.change.base, current.records.version()))
            return failure<ChangePreview>(Status::conflict,
                                          ErrorCode::revision_conflict,
                                          "Prepared change base does not match");
        if (auto error = validate_candidate(operation.change.candidate, candidate))
            return {Status::failed, {}, error};
        if (!profiles_supported(operation.change.candidate, *candidate.options))
            return failure<ChangePreview>(Status::failed,
                                          ErrorCode::schema_unsupported,
                                          "Prepared change requires an unavailable solver profile");
        candidate.stats += operation.change.stats;
        Prepared prepared{caller,
                          context,
                          operation.creates_entity,
                          operation.affected_entity,
                          operation.signature,
                          operation.normalized_value,
                          std::make_shared<const PreparedRecordChange>(std::move(operation.change)),
                          operation.label};
        auto response = stage_preview(candidate, std::move(prepared));
        using std::swap;
        swap(state_->data, candidate);
        return response;
    } catch (const RecordError& error) {
        return {Status::failed, {}, Diagnostic{error.code(), error.what(), error.field()}};
    }
}

Result<ChangeReceipt> RecordApplication::commit(const Caller& caller,
                                                const WriteContext& context,
                                                const PreviewId& preview_id,
                                                const std::string& idempotency_key) {
    return commit_with_rows(caller, context, preview_id, idempotency_key, {});
}
Result<ChangeReceipt> RecordApplication::commit_with_rows(const Caller& caller,
                                                          const WriteContext& context,
                                                          const PreviewId& preview_id,
                                                          const std::string& idempotency_key,
                                                          const CommitOwnedRows& owned) {
    std::lock_guard lock(state_->mutex);
    return state_->commit(caller, context, preview_id, idempotency_key, owned);
}
Result<ChangeReceipt>
RecordApplication::State::commit(const Caller& caller,
                                 const WriteContext& context,
                                 const PreviewId& preview_id,
                                 const std::string& idempotency_key,
                                 const CommitOwnedRows& owned,
                                 const std::string& operation_name,
                                 const std::optional<std::string>& direct_signature) {
    auto* state_ = this;
    const Data& current = state_->data;
    if (blank(caller.principal) || blank(idempotency_key))
        return failure<ChangeReceipt>(Status::needs_input,
                                      ErrorCode::missing_input,
                                      "Caller and idempotency key are required");
    if (auto error = check_document<ChangeReceipt>(current, context.document))
        return *error;
    const auto key = scoped_key(caller, operation_name, idempotency_key);
    const auto existing = current.operations.find(key);
    if (existing != current.operations.end()) {
        // A completed operation survives preview eviction and undo. Its stored signature
        // starts with the caller-supplied context and preview ID.
        std::string prefix = context_signature(context);
        append_part(prefix, preview_id.value);
        if (direct_signature ? existing->second.signature != *direct_signature
                             : existing->second.signature.compare(0, prefix.size(), prefix) != 0)
            return failure<ChangeReceipt>(Status::conflict,
                                          ErrorCode::idempotency_key_conflict,
                                          "Key was used with other commit parameters",
                                          "idempotency_key");
        return success(current_receipt(current, existing->second.receipt, true));
    }
    if (auto error = check_revision<ChangeReceipt>(current, context))
        return *error;
    if (current.save_intent)
        return failure<ChangeReceipt>(
            Status::conflict, ErrorCode::storage_uncertain, "Save intent needs reconciliation");
    const auto preview = current.previews.find(preview_id.value);
    if (preview == current.previews.end())
        return failure<ChangeReceipt>(Status::conflict,
                                      ErrorCode::preview_expired,
                                      "Preview is unavailable or expired",
                                      "preview_id");
    const Prepared& prepared = preview->second;
    if (prepared.caller.principal != caller.principal)
        return failure<ChangeReceipt>(Status::conflict,
                                      ErrorCode::preview_expired,
                                      "Preview belongs to another caller",
                                      "preview_id");
    if (!same_document(prepared.context.document, context.document) ||
        prepared.context.expected_revision != context.expected_revision)
        return failure<ChangeReceipt>(Status::conflict,
                                      ErrorCode::preview_expired,
                                      "Preview context does not match",
                                      "preview_id");
    if (current.host_operations.size() + current.operations.size() >=
        current.limits.max_idempotency_records)
        return failure<ChangeReceipt>(
            Status::failed, ErrorCode::resource_limit, "Idempotency record limit reached");
    if (current.cursor >= current.limits.max_history_entries)
        return failure<ChangeReceipt>(
            Status::failed, ErrorCode::resource_limit, "History entry limit reached");
    Data candidate = current;
    for (std::size_t i = candidate.cursor; i < candidate.history.size(); ++i)
        candidate.pending.push_back(
            {{StoreSpace::history_entry, candidate.history[i]->transaction.value}, {}});
    candidate.history.resize(candidate.cursor);
    if (!prepared.after)
        return failure<ChangeReceipt>(
            Status::failed, ErrorCode::invalid_input, "Preview has no candidate model");
    if (!same_record_version(prepared.after->base, current.records.version()))
        return failure<ChangeReceipt>(
            Status::conflict, ErrorCode::revision_conflict, "Prepared change base changed");
    try {
        candidate.records = apply_record_changes(
            current.records, prepared.after->changes, RecordDirection::forward, &candidate.stats);
    } catch (const RecordError& error) {
        return {Status::failed, {}, Diagnostic{error.code(), error.what(), error.field()}};
    }
    if (auto diagnostic = validate_candidate(candidate.records, candidate))
        return {Status::failed, {}, diagnostic};
    if (!profiles_supported(candidate.records, *candidate.options))
        return failure<ChangeReceipt>(Status::failed,
                                      ErrorCode::schema_unsupported,
                                      "Commit requires an unavailable solver profile");
    queue_changes(candidate, prepared.after->changes);
    TransactionId transaction(new_id(candidate, "transaction"));
    candidate.document->content_state = new_id(candidate, "state");
    ++candidate.document->revision;
    update_document(candidate);
    candidate.history.push_back(make_history(transaction,
                                             prepared.label,
                                             prepared.after->changes,
                                             candidate.document->content_state,
                                             &candidate.stats));
    candidate.pending.push_back(
        {{StoreSpace::history_entry, transaction.value}, candidate.history.back()->encoded});
    candidate.cursor = candidate.history.size();
    ChangeReceipt receipt{transaction,
                          candidate.document->revision,
                          candidate.document->revision,
                          candidate.document->content_state,
                          false};
    receipt.primary_entity = prepared.entity;
    try {
        if (owned)
            queue_owned_rows(candidate, owned(receipt));
    } catch (const RecordError& error) {
        return {Status::failed, {}, Diagnostic{error.code(), error.what(), error.field()}};
    }
    candidate.operations.emplace(
        key,
        RecordedOperation{
            direct_signature.value_or(commit_signature(context, preview_id, prepared)), receipt});
    drop_expired_previews(candidate);
    auto result = success(std::move(receipt));
    if (auto error =
            persist(candidate, state_->records.get(), state_->generation, state_->poisoned))
        return {Status::failed, std::nullopt, std::move(error)};
    using std::swap;
    swap(state_->data, candidate);
    return result;
}

Result<ChangeReceipt> RecordApplication::execute(const Caller& caller,
                                                 const WriteContext& context,
                                                 const std::string& operation_name,
                                                 const std::string& normalized_signature,
                                                 const RecordPrepare& handler,
                                                 const std::string& idempotency_key,
                                                 const CommitOwnedRows& owned) {
    std::lock_guard lock(state_->mutex);
    if (blank(caller.principal) || blank(operation_name) || blank(normalized_signature) ||
        blank(idempotency_key) || !handler)
        return failure<ChangeReceipt>(
            Status::needs_input,
            ErrorCode::missing_input,
            "Caller, operation, input signature, handler and key are required");
    if (auto error = check_document<ChangeReceipt>(state_->data, context.document))
        return *error;
    const auto scope = "execute:" + operation_name;
    auto signature = context_signature(context);
    append_part(signature, normalized_signature);
    const auto key = scoped_key(caller, scope, idempotency_key);
    if (const auto found = state_->data.operations.find(key);
        found != state_->data.operations.end()) {
        if (found->second.signature != signature)
            return failure<ChangeReceipt>(Status::conflict,
                                          ErrorCode::idempotency_key_conflict,
                                          "Operation key was used with different normalized input");
        return success(current_receipt(state_->data, found->second.receipt, true));
    }
    auto preview = state_->prepare(
        caller, context, [&](const DocumentView& view, const RecordIdentityAllocator& allocate) {
            auto prepared = handler(view, allocate);
            if (prepared.ok() && prepared.value->signature != normalized_signature)
                return failure<RecordPreparedOperation>(
                    Status::failed,
                    ErrorCode::invalid_input,
                    "Prepared operation signature does not match normalized input");
            return prepared;
        });
    if (!preview.ok())
        return {preview.status, {}, preview.error};
    struct ReleasePrivatePreview {
        Data& data;
        const std::string& identity;
        ~ReleasePrivatePreview() {
            data.previews.erase(identity);
        }
    } release{state_->data, preview.value->id.value};
    return state_->commit(
        caller, context, preview.value->id, idempotency_key, owned, scope, signature);
}
Result<ChangeReceipt> RecordApplication::action_outcome(const Caller& caller,
                                                        const DocumentRef& document,
                                                        const std::string& operation_name,
                                                        const std::string& idempotency_key) const {
    std::lock_guard lock(state_->mutex);
    if (auto error = check_document<ChangeReceipt>(state_->data, document))
        return *error;
    const auto key = scoped_key(caller, "execute:" + operation_name, idempotency_key);
    const auto found = state_->data.operations.find(key);
    if (found == state_->data.operations.end())
        return failure<ChangeReceipt>(
            Status::failed, ErrorCode::entity_not_found, "Operation is not recorded");
    return success(current_receipt(state_->data, found->second.receipt, true));
}

Result<bool> RecordApplication::update_owned_rows(const Caller& caller,
                                                  const DocumentRef& document,
                                                  std::span<const OwnedRowUpdate> updates) {
    std::lock_guard lock(state_->mutex);
    if (blank(caller.principal))
        return failure<bool>(Status::needs_input, ErrorCode::missing_input, "Caller is required");
    if (auto error = check_document<bool>(state_->data, document))
        return *error;
    if (state_->data.save_intent)
        return failure<bool>(Status::conflict,
                             ErrorCode::storage_uncertain,
                             "Save intent needs reconciliation before side-row writes");
    Data candidate = state_->data;
    try {
        queue_owned_rows(candidate, updates);
        if (auto error =
                persist(candidate, state_->records.get(), state_->generation, state_->poisoned))
            return {Status::failed, {}, std::move(error)};
        using std::swap;
        swap(state_->data, candidate);
        return success(true);
    } catch (const RecordError& error) {
        const auto status =
            error.code() == ErrorCode::revision_conflict ? Status::conflict : Status::failed;
        return {status, {}, Diagnostic{error.code(), error.what(), error.field()}};
    }
}
Result<std::vector<std::shared_ptr<const OwnedRowImage>>> RecordApplication::owned_rows(
    const DocumentRef& document, StoreSpace space, std::string_view owner) const {
    std::lock_guard lock(state_->mutex);
    using Rows = std::vector<std::shared_ptr<const OwnedRowImage>>;
    if (auto error = check_document<Rows>(state_->data, document))
        return *error;
    Rows result;
    for (const auto& [key, row] : *state_->data.owned_rows)
        if (key.space == space && row->owner == owner)
            result.push_back(row);
    return success(std::move(result));
}

namespace {
Result<ChangeReceipt> move_history(Data& data,
                                   IRecordStore* store,
                                   std::uint64_t& generation,
                                   bool& poisoned,
                                   const Caller& caller,
                                   const WriteContext& context,
                                   const std::string& idempotency_key,
                                   bool undo) {
    const char* operation = undo ? "undo" : "redo";
    if (blank(caller.principal) || blank(idempotency_key))
        return failure<ChangeReceipt>(Status::needs_input,
                                      ErrorCode::missing_input,
                                      "Caller and idempotency key are required");
    if (auto error = check_document<ChangeReceipt>(data, context.document))
        return *error;
    const auto key = scoped_key(caller, operation, idempotency_key);
    const auto signature = context_signature(context);
    if (const auto existing = data.operations.find(key); existing != data.operations.end()) {
        if (existing->second.signature != signature)
            return failure<ChangeReceipt>(Status::conflict,
                                          ErrorCode::idempotency_key_conflict,
                                          "Key was used with other history parameters",
                                          "idempotency_key");
        return success(current_receipt(data, existing->second.receipt, true));
    }
    if (auto error = check_revision<ChangeReceipt>(data, context))
        return *error;
    if (data.save_intent)
        return failure<ChangeReceipt>(
            Status::conflict, ErrorCode::storage_uncertain, "Save intent needs reconciliation");
    if (undo ? data.cursor == 0 : data.cursor == data.history.size())
        return failure<ChangeReceipt>(Status::conflict,
                                      undo ? ErrorCode::nothing_to_undo
                                           : ErrorCode::nothing_to_redo,
                                      undo ? "Nothing to undo" : "Nothing to redo");
    if (data.host_operations.size() + data.operations.size() >= data.limits.max_idempotency_records)
        return failure<ChangeReceipt>(
            Status::failed, ErrorCode::resource_limit, "Idempotency record limit reached");
    Data candidate = data;
    if (undo) {
        queue_changes(
            candidate, candidate.history[candidate.cursor - 1]->changes, RecordDirection::reverse);
        candidate.records = apply_record_changes(candidate.records,
                                                 candidate.history[candidate.cursor - 1]->changes,
                                                 RecordDirection::reverse,
                                                 &candidate.stats);
        --candidate.cursor;
    } else {
        queue_changes(candidate, candidate.history[candidate.cursor]->changes);
        candidate.records = apply_record_changes(candidate.records,
                                                 candidate.history[candidate.cursor]->changes,
                                                 RecordDirection::forward,
                                                 &candidate.stats);
        ++candidate.cursor;
    }
    if (auto diagnostic = validate_candidate(candidate.records, candidate))
        return {Status::failed, std::nullopt, std::move(diagnostic)};
    if (!profiles_supported(candidate.records, *candidate.options))
        return failure<ChangeReceipt>(Status::failed,
                                      ErrorCode::schema_unsupported,
                                      "History requires an unavailable solver profile");
    if (candidate.cursor == 0) {
        candidate.document->content_state = candidate.initial_content_state;
    } else {
        const auto& entry = candidate.history[candidate.cursor - 1];
        candidate.document->content_state = entry->content_state;
    }
    ++candidate.document->revision;
    update_document(candidate);
    TransactionId transaction(new_id(candidate, "transaction"));
    ChangeReceipt receipt{transaction,
                          candidate.document->revision,
                          candidate.document->revision,
                          candidate.document->content_state,
                          false};
    candidate.operations.emplace(key, RecordedOperation{signature, receipt});
    drop_expired_previews(candidate);
    auto result = success(std::move(receipt));
    if (auto error = persist(candidate, store, generation, poisoned))
        return {Status::failed, std::nullopt, std::move(error)};
    using std::swap;
    swap(data, candidate);
    return result;
}
} // namespace

Result<ChangeReceipt> RecordApplication::undo(const Caller& caller,
                                              const WriteContext& context,
                                              const std::string& idempotency_key) {
    std::lock_guard lock(state_->mutex);
    return move_history(state_->data,
                        state_->records.get(),
                        state_->generation,
                        state_->poisoned,
                        caller,
                        context,
                        idempotency_key,
                        true);
}

Result<ChangeReceipt> RecordApplication::redo(const Caller& caller,
                                              const WriteContext& context,
                                              const std::string& idempotency_key) {
    std::lock_guard lock(state_->mutex);
    return move_history(state_->data,
                        state_->records.get(),
                        state_->generation,
                        state_->poisoned,
                        caller,
                        context,
                        idempotency_key,
                        false);
}

Result<HistorySnapshot> RecordApplication::history(const DocumentRef& ref) const {
    std::lock_guard lock(state_->mutex);
    const Data& data = state_->data;
    if (auto error = check_document<HistorySnapshot>(data, ref))
        return *error;
    HistorySnapshot snapshot;
    snapshot.cursor = data.cursor;
    snapshot.revision = data.document->revision;
    for (std::size_t i = 0; i < data.history.size(); ++i)
        snapshot.items.push_back(
            HistoryItem{data.history[i]->transaction, data.history[i]->label, i < data.cursor});
    return success(std::move(snapshot));
}

Result<ChangeReceipt> RecordApplication::operation(const Caller& caller,
                                                   const DocumentRef& ref,
                                                   const std::string& operation_name,
                                                   const std::string& idempotency_key) const {
    std::lock_guard lock(state_->mutex);
    const Data& data = state_->data;
    if (blank(caller.principal) || blank(idempotency_key))
        return failure<ChangeReceipt>(Status::needs_input,
                                      ErrorCode::missing_input,
                                      "Caller and idempotency key are required");
    if (auto error = check_document<ChangeReceipt>(data, ref))
        return *error;
    if (operation_name != "commit" && operation_name != "undo" && operation_name != "redo")
        return failure<ChangeReceipt>(
            Status::failed, ErrorCode::invalid_input, "Unknown operation", "operation_name");
    const auto key = scoped_key(caller, operation_name, idempotency_key);
    const auto found = data.operations.find(key);
    if (found == data.operations.end())
        return failure<ChangeReceipt>(Status::failed,
                                      ErrorCode::entity_not_found,
                                      "Operation is not recorded",
                                      "idempotency_key");
    return success(current_receipt(data, found->second.receipt, true));
}

bool RecordApplication::durable() const noexcept {
    return static_cast<bool>(state_->records);
}
bool RecordApplication::recovery_available() const {
    std::lock_guard lock(state_->mutex);
    return state_->data.recoverable || state_->poisoned;
}
Result<DocumentInfo> RecordApplication::current_document() const {
    std::lock_guard lock(state_->mutex);
    const auto& data = state_->data;
    if (!data.document || data.recoverable)
        return failure<DocumentInfo>(
            Status::failed, ErrorCode::document_not_found, "No active document");
    return success(*data.document);
}
Result<DocumentInfo> RecordApplication::host_operation(const Caller& caller,
                                                       const std::string& operation_name,
                                                       const std::string& idempotency_key) const {
    std::lock_guard lock(state_->mutex);
    if (blank(caller.principal) || blank(idempotency_key))
        return failure<DocumentInfo>(
            Status::needs_input, ErrorCode::missing_input, "Caller and key are required");
    if (operation_name != "create_document" && operation_name != "open_document" &&
        operation_name != "recover_document" && operation_name != "save_document" &&
        operation_name != "close_document")
        return failure<DocumentInfo>(
            Status::failed, ErrorCode::invalid_input, "Unknown host operation");
    const auto key = scoped_key(caller, operation_name, idempotency_key);
    const auto found = state_->data.host_operations.find(key);
    if (found == state_->data.host_operations.end())
        return failure<DocumentInfo>(
            Status::failed, ErrorCode::entity_not_found, "Operation is not recorded");
    return success(found->second.result);
}
Result<DocumentInfo> RecordApplication::open_document(const Caller& caller,
                                                      const std::string& path,
                                                      const std::string& idempotency_key) {
    std::lock_guard lock(state_->mutex);
    if (blank(caller.principal) || blank(path) || blank(idempotency_key))
        return failure<DocumentInfo>(
            Status::needs_input, ErrorCode::missing_input, "Caller, path and key are required");
    const auto key = scoped_key(caller, "open_document", idempotency_key);
    if (const auto existing = state_->data.host_operations.find(key);
        existing != state_->data.host_operations.end()) {
        if (existing->second.signature != path)
            return failure<DocumentInfo>(Status::conflict,
                                         ErrorCode::idempotency_key_conflict,
                                         "Key was used with another path");
        return success(existing->second.result);
    }
    if (state_->data.document)
        return failure<DocumentInfo>(
            Status::conflict, ErrorCode::document_already_open, "A document already exists");
    if (!state_->store)
        return failure<DocumentInfo>(
            Status::failed, ErrorCode::unsupported_capability, "Project storage unavailable");
    if (state_->data.host_operations.size() + state_->data.operations.size() >=
        state_->data.limits.max_idempotency_records)
        return failure<DocumentInfo>(
            Status::failed, ErrorCode::resource_limit, "Operation record limit reached");
    try {
        const auto canonical = state_->store->acquire_project(path);
        ProjectLeaseGuard lease{state_->store.get(), ""};
        const auto project = state_->store->read_project(canonical);
        Data candidate = state_->data;
        Data opened = decode_project(project.payload, candidate.options);
        if (!profiles_supported(opened.records, *state_->data.options))
            return failure<DocumentInfo>(Status::failed,
                                         ErrorCode::schema_unsupported,
                                         "Project requires an unavailable solver profile");
        replace_records(candidate, std::move(opened.records));
        candidate.owned_rows = std::move(opened.owned_rows);
        for (const auto& [row_key, row] : *candidate.owned_rows)
            candidate.pending.push_back({row_key, row->encoded});
        recover_owned_rows(candidate);
        candidate.history.clear();
        candidate.cursor = 0;
        candidate.operations.clear();
        candidate.save_intent.reset();
        candidate.recoverable = false;
        auto info = *opened.document;
        info.document.id = DocumentId(new_id(candidate, "doc"));
        info.document.epoch = DocumentEpoch(new_id(candidate, "epoch"));
        info.saved_path = canonical;
        info.saved_content_state = info.content_state;
        info.durable = true;
        candidate.document = info;
        candidate.initial_content_state = info.content_state;
        update_document(candidate);
        candidate.host_operations.emplace(key, HostOperation{path, *candidate.document});
        if (auto error =
                persist(candidate, state_->records.get(), state_->generation, state_->poisoned))
            return {Status::failed, std::nullopt, std::move(error)};
        using std::swap;
        swap(state_->data, candidate);
        state_->store->release_projects_except(canonical);
        lease.release = false;
        return success(*state_->data.document);
    } catch (const RecordError& error) {
        return failure<DocumentInfo>(Status::failed, ErrorCode::schema_unsupported, error.what());
    } catch (const StorageError& error) {
        return failure<DocumentInfo>(Status::failed,
                                     error.uncertain() ? ErrorCode::storage_uncertain
                                                       : ErrorCode::storage_failure,
                                     error.what());
    }
}
Result<DocumentInfo> RecordApplication::recover_document(const Caller& caller,
                                                         const std::string& idempotency_key) {
    std::lock_guard lock(state_->mutex);
    if (blank(caller.principal) || blank(idempotency_key))
        return failure<DocumentInfo>(
            Status::needs_input, ErrorCode::missing_input, "Caller and key are required");
    if (state_->poisoned && state_->records) {
        try {
            const auto loaded = state_->records->load_rows();
            if (loaded.rows.empty() && !loaded.legacy)
                return failure<DocumentInfo>(
                    Status::failed, ErrorCode::storage_failure, "No durable workspace to recover");
            Data restored = decode_data(loaded, state_->data.options);
            restored.recoverable = restored.document.has_value();
            state_->data = std::move(restored);
            state_->generation = loaded.generation;
            state_->poisoned = false;
        } catch (const RecordError& error) {
            return failure<DocumentInfo>(
                Status::failed, ErrorCode::schema_unsupported, error.what());
        } catch (const StorageError& error) {
            return failure<DocumentInfo>(Status::failed,
                                         error.uncertain() ? ErrorCode::storage_uncertain
                                                           : ErrorCode::storage_failure,
                                         error.what());
        }
    }
    const auto key = scoped_key(caller, "recover_document", idempotency_key);
    if (const auto existing = state_->data.host_operations.find(key);
        existing != state_->data.host_operations.end())
        return success(existing->second.result);
    if (!state_->records || !state_->data.document || !state_->data.recoverable)
        return failure<DocumentInfo>(
            Status::failed, ErrorCode::document_not_found, "No retained document to recover");
    if (state_->data.host_operations.size() + state_->data.operations.size() >=
        state_->data.limits.max_idempotency_records)
        return failure<DocumentInfo>(
            Status::failed, ErrorCode::resource_limit, "Operation record limit reached");
    try {
        Data candidate = state_->data;
        if (!profiles_supported(candidate.records, *state_->data.options))
            return failure<DocumentInfo>(Status::failed,
                                         ErrorCode::schema_unsupported,
                                         "Recovery requires an unavailable solver profile");
        if (state_->store && !candidate.document->saved_path.empty())
            candidate.document->saved_path =
                state_->store->acquire_project(candidate.document->saved_path);
        if (candidate.save_intent && !state_->store)
            return failure<DocumentInfo>(Status::failed,
                                         ErrorCode::unsupported_capability,
                                         "Pending save requires project storage");
        if (candidate.save_intent) {
            const auto& intent = *candidate.save_intent;
            const auto canonical = state_->store->acquire_project(intent.path);
            try {
                const auto project = state_->store->read_project(canonical);
                if (project.save_token == intent.token && project.payload == intent.snapshot) {
                    candidate.document->saved_path = canonical;
                    candidate.document->project_id = intent.project_id;
                    candidate.document->saved_content_state = candidate.document->content_state;
                    update_document(candidate);
                    candidate.host_operations.emplace(
                        intent.host_key, HostOperation{intent.signature, *candidate.document});
                    candidate.save_intent.reset();
                } else
                    candidate.save_intent.reset(); // Target changed; preserve the working model.
            } catch (const StorageError& error) {
                if (std::string_view(error.what()) != "project_not_found")
                    throw;
            }
        }
        candidate.document->document.epoch = DocumentEpoch(new_id(candidate, "epoch"));
        candidate.document->durable = true;
        candidate.recoverable = false;
        candidate.previews.clear();
        recover_owned_rows(candidate);
        update_document(candidate);
        candidate.host_operations.emplace(key, HostOperation{"", *candidate.document});
        if (auto error =
                persist(candidate, state_->records.get(), state_->generation, state_->poisoned))
            return {Status::failed, std::nullopt, std::move(error)};
        using std::swap;
        swap(state_->data, candidate);
        if (state_->store)
            state_->store->release_projects_except(state_->data.document->saved_path);
        return success(*state_->data.document);
    } catch (const RecordError& error) {
        return failure<DocumentInfo>(Status::failed, error.code(), error.what());
    } catch (const StorageError& error) {
        return failure<DocumentInfo>(Status::failed,
                                     error.uncertain() ? ErrorCode::storage_uncertain
                                                       : ErrorCode::storage_failure,
                                     error.what());
    }
}
Result<DocumentInfo> RecordApplication::save_document(const Caller& caller,
                                                      const WriteContext& context,
                                                      const std::string& path,
                                                      bool save_as,
                                                      const std::string& idempotency_key) {
    std::lock_guard lock(state_->mutex);
    if (blank(caller.principal) || blank(idempotency_key))
        return failure<DocumentInfo>(
            Status::needs_input, ErrorCode::missing_input, "Caller and key are required");
    const auto key = scoped_key(caller, "save_document", idempotency_key);
    std::string signature = context_signature(context);
    append_part(signature, path);
    append_part(signature, save_as ? "save_as" : "save");
    if (const auto existing = state_->data.host_operations.find(key);
        existing != state_->data.host_operations.end()) {
        if (existing->second.signature != signature)
            return failure<DocumentInfo>(Status::conflict,
                                         ErrorCode::idempotency_key_conflict,
                                         "Key was used with other save parameters");
        return success(existing->second.result);
    }
    const bool retry_pending = state_->data.save_intent &&
                               state_->data.save_intent->host_key == key &&
                               state_->data.save_intent->signature == signature;
    if (!retry_pending) {
        if (auto error = check_document<DocumentInfo>(state_->data, context.document))
            return *error;
        if (auto error = check_revision<DocumentInfo>(state_->data, context))
            return *error;
    }
    if (!state_->store)
        return failure<DocumentInfo>(
            Status::failed, ErrorCode::unsupported_capability, "Project storage unavailable");
    if (state_->data.host_operations.size() + state_->data.operations.size() >=
        state_->data.limits.max_idempotency_records)
        return failure<DocumentInfo>(
            Status::failed, ErrorCode::resource_limit, "Operation record limit reached");
    const std::string target = path.empty() ? state_->data.document->saved_path : path;
    if (blank(target))
        return failure<DocumentInfo>(
            Status::needs_input, ErrorCode::missing_input, "Save path required");
    if (!save_as && !state_->data.document->saved_path.empty() &&
        target != state_->data.document->saved_path)
        return failure<DocumentInfo>(
            Status::conflict, ErrorCode::invalid_input, "Use save-as for a new path");
    try {
        const auto canonical = state_->store->acquire_project(target);
        ProjectLeaseGuard lease{state_->store.get(),
                                state_->data.save_intent ? state_->data.save_intent->path
                                                         : state_->data.document->saved_path};
        Data candidate = state_->data;
        if (candidate.save_intent)
            lease.release = false; // Retain both source and pending target leases.
        if (candidate.save_intent && (candidate.save_intent->host_key != key ||
                                      candidate.save_intent->signature != signature))
            return failure<DocumentInfo>(
                Status::conflict, ErrorCode::idempotency_key_conflict, "Another save is pending");
        if (candidate.save_intent && candidate.save_intent->path != canonical)
            return failure<DocumentInfo>(Status::conflict,
                                         ErrorCode::idempotency_key_conflict,
                                         "Save target resolved to a different path");
        if (!candidate.save_intent) {
            // Reject an unrelated or unreadable target before recording a durable
            // intent. Otherwise a definite conflict would strand the document.
            try {
                const auto old = state_->store->read_project(canonical);
                if (save_as)
                    return failure<DocumentInfo>(Status::conflict,
                                                 ErrorCode::invalid_input,
                                                 "Save-as target already exists");
                const auto old_data = decode_project(old.payload, candidate.options);
                if (candidate.document->project_id.empty() ||
                    old_data.document->project_id != candidate.document->project_id)
                    return failure<DocumentInfo>(Status::conflict,
                                                 ErrorCode::invalid_input,
                                                 "Save target belongs to another project");
            } catch (const StorageError& error) {
                if (std::string_view(error.what()) != "project_not_found")
                    throw;
            }
            SaveIntent intent;
            intent.host_key = key;
            intent.signature = signature;
            intent.path = canonical;
            intent.token = new_id(candidate, "save");
            intent.project_id = save_as || candidate.document->project_id.empty()
                                    ? new_id(candidate, "project")
                                    : candidate.document->project_id;
            intent.snapshot = encode_project(candidate, intent.project_id);
            intent.save_as = save_as;
            candidate.save_intent = std::make_shared<const SaveIntent>(std::move(intent));
            if (auto error =
                    persist(candidate, state_->records.get(), state_->generation, state_->poisoned))
                return {Status::failed, std::nullopt, std::move(error)};
            using std::swap;
            swap(state_->data, candidate);
        }
        lease.release = false; // The durable intent retains its target lease.
        const auto intent = *state_->data.save_intent;
        bool published = false;
        try {
            const auto old = state_->store->read_project(canonical);
            if (old.save_token == intent.token && old.payload == intent.snapshot)
                published = true;
            else if (old.save_token == intent.token || (save_as && !old.payload.empty()))
                return failure<DocumentInfo>(Status::conflict,
                                             ErrorCode::idempotency_key_conflict,
                                             "Save target contains another snapshot");
            else {
                const auto old_data = decode_project(old.payload, state_->data.options);
                if (old_data.document->project_id != state_->data.document->project_id)
                    return failure<DocumentInfo>(Status::conflict,
                                                 ErrorCode::invalid_input,
                                                 "Save target belongs to another project");
            }
        } catch (const StorageError& error) {
            if (std::string_view(error.what()) != "project_not_found")
                throw;
        }
        if (!published)
            state_->store->publish_project(canonical, StoredProject{intent.token, intent.snapshot});
        Data finished = state_->data;
        finished.document->saved_path = canonical;
        finished.document->project_id = intent.project_id;
        finished.document->saved_content_state = finished.document->content_state;
        update_document(finished);
        finished.host_operations.emplace(key, HostOperation{signature, *finished.document});
        finished.save_intent.reset();
        if (auto error =
                persist(finished, state_->records.get(), state_->generation, state_->poisoned))
            return {Status::failed, std::nullopt, std::move(error)};
        using std::swap;
        swap(state_->data, finished);
        state_->store->release_projects_except(canonical);
        return success(*state_->data.document);
    } catch (const RecordError& error) {
        return failure<DocumentInfo>(Status::failed, ErrorCode::schema_unsupported, error.what());
    } catch (const StorageError& error) {
        if (error.uncertain())
            state_->poisoned = true;
        return failure<DocumentInfo>(Status::failed,
                                     error.uncertain() ? ErrorCode::storage_uncertain
                                                       : ErrorCode::storage_failure,
                                     error.what());
    }
}
Result<DocumentInfo> RecordApplication::close_document(const Caller& caller,
                                                       const WriteContext& context,
                                                       ClosePolicy policy,
                                                       const std::string& idempotency_key) {
    std::lock_guard lock(state_->mutex);
    if (blank(caller.principal) || blank(idempotency_key))
        return failure<DocumentInfo>(
            Status::needs_input, ErrorCode::missing_input, "Caller and key are required");
    const auto key = scoped_key(caller, "close_document", idempotency_key);
    auto signature = context_signature(context);
    append_part(signature, policy == ClosePolicy::discard ? "discard" : "keep");
    if (const auto existing = state_->data.host_operations.find(key);
        existing != state_->data.host_operations.end()) {
        if (existing->second.signature != signature)
            return failure<DocumentInfo>(Status::conflict,
                                         ErrorCode::idempotency_key_conflict,
                                         "Key was used with other close parameters");
        return success(existing->second.result);
    }
    if (auto error = check_document<DocumentInfo>(state_->data, context.document))
        return *error;
    if (auto error = check_revision<DocumentInfo>(state_->data, context))
        return *error;
    if (owned_rows_block_close(state_->data))
        return failure<DocumentInfo>(
            Status::conflict,
            ErrorCode::invalid_input,
            "Active background work must finish or be cancelled before closing");
    if (state_->data.save_intent)
        return failure<DocumentInfo>(
            Status::conflict, ErrorCode::storage_uncertain, "Save intent needs reconciliation");
    Data candidate = state_->data;
    const auto result = *candidate.document;
    candidate.host_operations.emplace(key, HostOperation{signature, result});
    candidate.previews.clear();
    if (policy == ClosePolicy::keep_recovery && state_->records)
        candidate.recoverable = true;
    else {
        candidate.document.reset();
        clear_document_rows(candidate);
        candidate.history.clear();
        candidate.cursor = 0;
        candidate.operations.clear();
        candidate.initial_content_state.clear();
        candidate.recoverable = false;
    }
    if (auto error =
            persist(candidate, state_->records.get(), state_->generation, state_->poisoned))
        return {Status::failed, std::nullopt, std::move(error)};
    using std::swap;
    swap(state_->data, candidate);
    if (state_->store)
        state_->store->release_projects_except("");
    return success(result);
}
} // namespace qcae
