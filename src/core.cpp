#include "qcae/core.hpp"
#include "application_state.hpp"
#include "qcae/model_delta.hpp"
#include "qcae/state_codec.hpp"

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
using namespace detail;

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
bool profiles_supported(const Model& model,
                        const std::function<bool(const ProfileRef&)>& supports) {
    if (!supports)
        return true;
    for (const auto& analysis : model.analyses)
        if (!supports(analysis.target.profile))
            return false;
    for (const auto& source : model.sources)
        if (!supports(source.profile))
            return false;
    return true;
}

Result<double> normalize(Quantity value) {
    if (value.unit.empty())
        return failure<double>(Status::needs_input,
                               ErrorCode::missing_input,
                               "A Young modulus unit is required",
                               "young_modulus.unit");
    double factor = 0.0;
    if (value.unit == "Pa")
        factor = 1e-6;
    else if (value.unit == "kPa")
        factor = 1e-3;
    else if (value.unit == "MPa")
        factor = 1.0;
    else if (value.unit == "GPa")
        factor = 1e3;
    else
        return failure<double>(Status::failed,
                               ErrorCode::invalid_unit,
                               "Unsupported Young modulus unit",
                               "young_modulus.unit");
    if (!std::isfinite(value.value) || value.value <= 0.0)
        return failure<double>(Status::failed,
                               ErrorCode::invalid_input,
                               "Young modulus must be finite and positive",
                               "young_modulus.value");
    const double normalized = value.value * factor;
    if (!std::isfinite(normalized) || normalized <= 0.0)
        return failure<double>(Status::failed,
                               ErrorCode::invalid_input,
                               "Normalized Young modulus is out of range",
                               "young_modulus.value");
    return success(normalized);
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

struct MemoryApplication::State {
    mutable std::mutex mutex;
    Data data;
    std::shared_ptr<IWorkspaceStore> store;
    std::function<bool(const ProfileRef&)> profile_supported;
    std::uint64_t generation{};
    bool poisoned{};
};

MemoryApplication::MemoryApplication(Limits limits,
                                     std::shared_ptr<IWorkspaceStore> store,
                                     std::function<bool(const ProfileRef&)> profile_supported)
    : state_(std::make_unique<State>()) {
    state_->data.limits = limits;
    state_->data.application_nonce = nonce();
    state_->store = std::move(store);
    state_->profile_supported = std::move(profile_supported);
    if (state_->store) {
        if (auto loaded = state_->store->load()) {
            state_->data = decode_data(loaded->payload, limits);
            state_->generation = loaded->generation;
            state_->data.recoverable = state_->data.document.has_value();
        }
    }
}
MemoryApplication::~MemoryApplication() = default;

static_assert(std::is_nothrow_move_constructible_v<Result<DocumentInfo>>);
static_assert(std::is_nothrow_move_constructible_v<Result<ChangePreview>>);
static_assert(std::is_nothrow_move_constructible_v<Result<ChangeReceipt>>);

Result<DocumentInfo> MemoryApplication::create_document(const Caller& caller,
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
    info.durable = static_cast<bool>(state_->store);
    candidate.initial_content_state = info.content_state;
    candidate.document = info;
    candidate.host_operations.emplace(key, HostOperation{name, info});
    if (auto error = persist(candidate, state_->store.get(), state_->generation, state_->poisoned))
        return {Status::failed, std::nullopt, std::move(error)};
    auto result = success(std::move(info));
    static_assert(std::is_nothrow_swappable_v<Data>);
    using std::swap;
    swap(state_->data, candidate);
    return result;
}

Result<ModelSnapshot> MemoryApplication::snapshot(const DocumentRef& ref) const {
    std::lock_guard lock(state_->mutex);
    const Data& data = state_->data;
    if (auto error = check_document<ModelSnapshot>(data, ref))
        return *error;
    ModelSnapshot snapshot;
    static_cast<Model&>(snapshot) = data.model;
    snapshot.info = *data.document;
    return success(std::move(snapshot));
}

Result<ChangePreview> MemoryApplication::preview(const Caller& caller,
                                                 const WriteContext& context,
                                                 const MaterialCommand& command) {
    std::lock_guard lock(state_->mutex);
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

    Prepared prepared;
    prepared.caller = caller;
    prepared.context = context;
    Result<double> normalized;
    if (const auto* create = std::get_if<CreateMaterial>(&command)) {
        if (blank(create->name))
            return failure<ChangePreview>(
                Status::needs_input, ErrorCode::missing_input, "Material name is required", "name");
        if (create->name.size() > current.limits.max_name_bytes)
            return failure<ChangePreview>(Status::failed,
                                          ErrorCode::resource_limit,
                                          "Material name exceeds byte limit",
                                          "name");
        if (current.model.materials.size() >= current.limits.max_materials)
            return failure<ChangePreview>(
                Status::failed, ErrorCode::resource_limit, "Material limit reached");
        prepared.create = true;
        prepared.name = create->name;
        normalized = normalize(create->young_modulus);
    } else {
        const auto& change = std::get<SetYoungModulus>(command);
        const auto found =
            std::find_if(current.model.materials.begin(),
                         current.model.materials.end(),
                         [&](const Material& material) { return material.id == change.id; });
        if (found == current.model.materials.end())
            return failure<ChangePreview>(
                Status::failed, ErrorCode::entity_not_found, "Material does not exist", "id");
        prepared.entity = change.id;
        prepared.name = found->name;
        normalized = normalize(change.young_modulus);
    }
    if (!normalized.ok())
        return {normalized.status, std::nullopt, normalized.error};
    prepared.modulus = *normalized.value;
    Data candidate = current;
    if (prepared.create) {
        const auto entities = model_entities(candidate.model);
        do {
            prepared.entity = EntityId(new_id(candidate, "entity"));
        } while (std::any_of(entities.begin(), entities.end(), [&](const EntitySummary& value) {
            return value.id == prepared.entity;
        }));
    }
    Model after = candidate.model;
    if (prepared.create) {
        after.materials.push_back(
            Material{prepared.entity, prepared.name, prepared.modulus, std::nullopt});
        prepared.label = "Create material";
    } else {
        auto found =
            std::find_if(after.materials.begin(),
                         after.materials.end(),
                         [&](const Material& material) { return material.id == prepared.entity; });
        found->young_modulus_mpa = prepared.modulus;
        prepared.label = "Set Young modulus";
    }
    if (auto diagnostic = validate_candidate(after, current.limits))
        return {Status::failed, std::nullopt, std::move(diagnostic)};
    prepared.after = std::move(after);
    auto result = stage_preview(candidate, std::move(prepared));
    using std::swap;
    swap(state_->data, candidate);
    return result;
}

Result<ChangePreview> MemoryApplication::preview_import(const Caller& caller,
                                                        const WriteContext& context,
                                                        const Model& imported) {
    std::lock_guard lock(state_->mutex);
    Data& data = state_->data;
    if (blank(caller.principal))
        return failure<ChangePreview>(
            Status::needs_input, ErrorCode::missing_input, "Caller is required");
    if (auto error = check_document<ChangePreview>(data, context.document))
        return *error;
    if (auto error = check_revision<ChangePreview>(data, context))
        return *error;
    if (data.previews.size() >= data.limits.max_previews)
        return failure<ChangePreview>(
            Status::failed, ErrorCode::resource_limit, "Preview limit reached");
    if (entity_count(data.model) != 0 || !data.model.sources.empty())
        return failure<ChangePreview>(
            Status::conflict, ErrorCode::invalid_input, "Import requires an empty model");
    if (auto diagnostic = validate_candidate(imported, data.limits))
        return {Status::failed, std::nullopt, std::move(diagnostic)};
    Prepared prepared;
    prepared.caller = caller;
    prepared.context = context;
    prepared.after = imported;
    prepared.label = "Import model";
    return stage_preview(data, std::move(prepared));
}

Result<ChangePreview> MemoryApplication::preview_edit(const Caller& caller,
                                                      const WriteContext& context,
                                                      const ModelEdit& edit) {
    std::lock_guard lock(state_->mutex);
    Data& data = state_->data;
    if (blank(caller.principal))
        return failure<ChangePreview>(
            Status::needs_input, ErrorCode::missing_input, "Caller is required");
    if (auto error = check_document<ChangePreview>(data, context.document))
        return *error;
    if (auto error = check_revision<ChangePreview>(data, context))
        return *error;
    if (data.previews.size() >= data.limits.max_previews)
        return failure<ChangePreview>(
            Status::failed, ErrorCode::resource_limit, "Preview limit reached");
    Data candidate = data;
    Model after = data.model;
    Prepared prepared;
    prepared.caller = caller;
    prepared.context = context;
    auto all_entities = model_entities(after);
    auto kind_of = [&](const EntityId& id) -> std::string {
        const auto found = std::find_if(all_entities.begin(),
                                        all_entities.end(),
                                        [&](const EntitySummary& value) { return value.id == id; });
        return found == all_entities.end() ? "" : found->kind;
    };
    auto upsert = [&](auto& values, auto value, const char* kind) -> bool {
        if (value.id.value.empty()) {
            do {
                value.id = EntityId(new_id(candidate, "entity"));
            } while (!kind_of(value.id).empty());
            prepared.create = true;
        } else if (kind_of(value.id) != kind)
            return false;
        prepared.entity = value.id;
        const auto found = std::find_if(values.begin(), values.end(), [&](const auto& existing) {
            return existing.id == value.id;
        });
        if (found == values.end())
            values.push_back(std::move(value));
        else
            *found = std::move(value);
        return true;
    };
    bool valid = true;
    std::visit(
        [&](const auto& change) {
            using T = std::decay_t<decltype(change)>;
            if constexpr (std::is_same_v<T, UpsertPart>) {
                valid = upsert(after.parts, change.value, "part");
                prepared.label = "Upsert part";
            } else if constexpr (std::is_same_v<T, UpsertAssembly>) {
                valid = upsert(after.assemblies, change.value, "assembly");
                prepared.label = "Upsert assembly";
            } else if constexpr (std::is_same_v<T, UpsertSet>) {
                valid = upsert(after.sets, change.value, "set");
                prepared.label = "Upsert set";
            } else if constexpr (std::is_same_v<T, MoveNode>) {
                prepared.entity = change.id;
                prepared.label = "Move node";
                const auto found =
                    std::find_if(after.nodes.begin(), after.nodes.end(), [&](const Node& node) {
                        return node.id == change.id;
                    });
                if (found == after.nodes.end())
                    valid = false;
                else
                    found->position = change.position;
            } else if constexpr (std::is_same_v<T, DeleteEntity>) {
                prepared.entity = change.id;
                prepared.label = "Delete entity";
                if (kind_of(change.id).empty()) {
                    valid = false;
                    return;
                }
                for (const auto& ref : model_references(after)) {
                    if (ref.to == change.id && ref.from != change.id &&
                        ref.role != "include.member") {
                        valid = false;
                        return;
                    }
                }
                auto erase = [&](auto& values) {
                    values.erase(
                        std::remove_if(values.begin(),
                                       values.end(),
                                       [&](const auto& item) { return item.id == change.id; }),
                        values.end());
                };
                erase(after.materials);
                erase(after.nodes);
                erase(after.sections);
                erase(after.beams);
                erase(after.parts);
                erase(after.assemblies);
                erase(after.sets);
                erase(after.includes);
                erase(after.forces);
                erase(after.constraints);
                erase(after.analyses);
                for (auto& include : after.includes)
                    include.members.erase(
                        std::remove(include.members.begin(), include.members.end(), change.id),
                        include.members.end());
                after.sources.erase(std::remove_if(after.sources.begin(),
                                                   after.sources.end(),
                                                   [&](const SourceIdentifier& source) {
                                                       return source.entity == change.id;
                                                   }),
                                    after.sources.end());
            }
        },
        edit);
    if (!valid)
        return failure<ChangePreview>(
            Status::failed,
            ErrorCode::invalid_input,
            "Edit entity is missing, has the wrong kind, or is referenced",
            "id");
    if (auto diagnostic = validate_candidate(after, data.limits))
        return {Status::failed, std::nullopt, std::move(diagnostic)};
    prepared.after = std::move(after);
    auto result = stage_preview(candidate, std::move(prepared));
    using std::swap;
    swap(data, candidate);
    return result;
}

Result<ChangeReceipt> MemoryApplication::commit(const Caller& caller,
                                                const WriteContext& context,
                                                const PreviewId& preview_id,
                                                const std::string& idempotency_key) {
    std::lock_guard lock(state_->mutex);
    const Data& current = state_->data;
    if (blank(caller.principal) || blank(idempotency_key))
        return failure<ChangeReceipt>(Status::needs_input,
                                      ErrorCode::missing_input,
                                      "Caller and idempotency key are required");
    if (auto error = check_document<ChangeReceipt>(current, context.document))
        return *error;
    const auto key = scoped_key(caller, "commit", idempotency_key);
    const auto existing = current.operations.find(key);
    if (existing != current.operations.end()) {
        // A completed operation survives preview eviction and undo. Its stored signature
        // starts with the caller-supplied context and preview ID.
        std::string prefix = context_signature(context);
        append_part(prefix, preview_id.value);
        if (existing->second.signature.compare(0, prefix.size(), prefix) != 0)
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
    candidate.history.resize(candidate.cursor);
    if (!prepared.after)
        return failure<ChangeReceipt>(
            Status::failed, ErrorCode::invalid_input, "Preview has no candidate model");
    candidate.model = *prepared.after;
    TransactionId transaction(new_id(candidate, "transaction"));
    candidate.document->content_state = new_id(candidate, "state");
    ++candidate.document->revision;
    update_document(candidate);
    candidate.history.push_back(HistoryEntry{transaction,
                                             prepared.label,
                                             model_delta(current.model, candidate.model),
                                             candidate.document->content_state});
    candidate.cursor = candidate.history.size();
    ChangeReceipt receipt{transaction,
                          candidate.document->revision,
                          candidate.document->revision,
                          candidate.document->content_state,
                          false};
    candidate.operations.emplace(
        key, RecordedOperation{commit_signature(context, preview_id, prepared), receipt});
    drop_expired_previews(candidate);
    auto result = success(std::move(receipt));
    if (auto error = persist(candidate, state_->store.get(), state_->generation, state_->poisoned))
        return {Status::failed, std::nullopt, std::move(error)};
    using std::swap;
    swap(state_->data, candidate);
    return result;
}

namespace {
Result<ChangeReceipt> move_history(Data& data,
                                   IWorkspaceStore* store,
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
        candidate.model = apply_model_delta(
            candidate.model, candidate.history[candidate.cursor - 1].delta, false);
        --candidate.cursor;
    } else {
        candidate.model =
            apply_model_delta(candidate.model, candidate.history[candidate.cursor].delta, true);
        ++candidate.cursor;
    }
    if (auto diagnostic = validate_candidate(candidate.model, candidate.limits))
        return {Status::failed, std::nullopt, std::move(diagnostic)};
    if (candidate.cursor == 0) {
        candidate.document->content_state = candidate.initial_content_state;
    } else {
        const auto& entry = candidate.history[candidate.cursor - 1];
        candidate.document->content_state = entry.content_state;
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

Result<ChangeReceipt> MemoryApplication::undo(const Caller& caller,
                                              const WriteContext& context,
                                              const std::string& idempotency_key) {
    std::lock_guard lock(state_->mutex);
    return move_history(state_->data,
                        state_->store.get(),
                        state_->generation,
                        state_->poisoned,
                        caller,
                        context,
                        idempotency_key,
                        true);
}

Result<ChangeReceipt> MemoryApplication::redo(const Caller& caller,
                                              const WriteContext& context,
                                              const std::string& idempotency_key) {
    std::lock_guard lock(state_->mutex);
    return move_history(state_->data,
                        state_->store.get(),
                        state_->generation,
                        state_->poisoned,
                        caller,
                        context,
                        idempotency_key,
                        false);
}

Result<HistorySnapshot> MemoryApplication::history(const DocumentRef& ref) const {
    std::lock_guard lock(state_->mutex);
    const Data& data = state_->data;
    if (auto error = check_document<HistorySnapshot>(data, ref))
        return *error;
    HistorySnapshot snapshot;
    snapshot.cursor = data.cursor;
    snapshot.revision = data.document->revision;
    for (std::size_t i = 0; i < data.history.size(); ++i)
        snapshot.items.push_back(
            HistoryItem{data.history[i].transaction, data.history[i].label, i < data.cursor});
    return success(std::move(snapshot));
}

Result<ChangeReceipt> MemoryApplication::operation(const Caller& caller,
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

bool MemoryApplication::durable() const noexcept {
    return static_cast<bool>(state_->store);
}
bool MemoryApplication::recovery_available() const {
    std::lock_guard lock(state_->mutex);
    return state_->data.recoverable || state_->poisoned;
}
Result<DocumentInfo> MemoryApplication::current_document() const {
    std::lock_guard lock(state_->mutex);
    const auto& data = state_->data;
    if (!data.document || data.recoverable)
        return failure<DocumentInfo>(
            Status::failed, ErrorCode::document_not_found, "No active document");
    return success(*data.document);
}
Result<DocumentInfo> MemoryApplication::host_operation(const Caller& caller,
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
Result<DocumentInfo> MemoryApplication::open_document(const Caller& caller,
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
        Data opened = decode_project(project.payload, candidate.limits);
        if (!profiles_supported(opened.model, state_->profile_supported))
            return failure<DocumentInfo>(Status::failed,
                                         ErrorCode::schema_unsupported,
                                         "Project requires an unavailable solver profile");
        candidate.model = std::move(opened.model);
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
                persist(candidate, state_->store.get(), state_->generation, state_->poisoned))
            return {Status::failed, std::nullopt, std::move(error)};
        using std::swap;
        swap(state_->data, candidate);
        state_->store->release_projects_except(canonical);
        lease.release = false;
        return success(*state_->data.document);
    } catch (const state_codec::CodecError& error) {
        return failure<DocumentInfo>(Status::failed, ErrorCode::schema_unsupported, error.what());
    } catch (const StorageError& error) {
        return failure<DocumentInfo>(Status::failed,
                                     error.uncertain() ? ErrorCode::storage_uncertain
                                                       : ErrorCode::storage_failure,
                                     error.what());
    }
}
Result<DocumentInfo> MemoryApplication::recover_document(const Caller& caller,
                                                         const std::string& idempotency_key) {
    std::lock_guard lock(state_->mutex);
    if (blank(caller.principal) || blank(idempotency_key))
        return failure<DocumentInfo>(
            Status::needs_input, ErrorCode::missing_input, "Caller and key are required");
    if (state_->poisoned && state_->store) {
        try {
            const auto loaded = state_->store->load();
            if (!loaded)
                return failure<DocumentInfo>(
                    Status::failed, ErrorCode::storage_failure, "No durable workspace to recover");
            Data restored = decode_data(loaded->payload, state_->data.limits);
            restored.recoverable = restored.document.has_value();
            state_->data = std::move(restored);
            state_->generation = loaded->generation;
            state_->poisoned = false;
        } catch (const state_codec::CodecError& error) {
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
    if (!state_->store || !state_->data.document || !state_->data.recoverable)
        return failure<DocumentInfo>(
            Status::failed, ErrorCode::document_not_found, "No retained document to recover");
    if (state_->data.host_operations.size() + state_->data.operations.size() >=
        state_->data.limits.max_idempotency_records)
        return failure<DocumentInfo>(
            Status::failed, ErrorCode::resource_limit, "Operation record limit reached");
    try {
        Data candidate = state_->data;
        if (!profiles_supported(candidate.model, state_->profile_supported))
            return failure<DocumentInfo>(Status::failed,
                                         ErrorCode::schema_unsupported,
                                         "Recovery requires an unavailable solver profile");
        if (!candidate.document->saved_path.empty())
            candidate.document->saved_path =
                state_->store->acquire_project(candidate.document->saved_path);
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
        candidate.host_operations.emplace(key, HostOperation{"", *candidate.document});
        if (auto error =
                persist(candidate, state_->store.get(), state_->generation, state_->poisoned))
            return {Status::failed, std::nullopt, std::move(error)};
        using std::swap;
        swap(state_->data, candidate);
        state_->store->release_projects_except(state_->data.document->saved_path);
        return success(*state_->data.document);
    } catch (const StorageError& error) {
        return failure<DocumentInfo>(Status::failed,
                                     error.uncertain() ? ErrorCode::storage_uncertain
                                                       : ErrorCode::storage_failure,
                                     error.what());
    }
}
Result<DocumentInfo> MemoryApplication::save_document(const Caller& caller,
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
                const auto old_data = decode_project(old.payload, candidate.limits);
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
            candidate.save_intent = std::move(intent);
            if (auto error =
                    persist(candidate, state_->store.get(), state_->generation, state_->poisoned))
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
                const auto old_data = decode_project(old.payload, state_->data.limits);
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
                persist(finished, state_->store.get(), state_->generation, state_->poisoned))
            return {Status::failed, std::nullopt, std::move(error)};
        using std::swap;
        swap(state_->data, finished);
        state_->store->release_projects_except(canonical);
        return success(*state_->data.document);
    } catch (const state_codec::CodecError& error) {
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
Result<DocumentInfo> MemoryApplication::close_document(const Caller& caller,
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
    if (state_->data.save_intent)
        return failure<DocumentInfo>(
            Status::conflict, ErrorCode::storage_uncertain, "Save intent needs reconciliation");
    Data candidate = state_->data;
    const auto result = *candidate.document;
    candidate.host_operations.emplace(key, HostOperation{signature, result});
    candidate.previews.clear();
    if (policy == ClosePolicy::keep_recovery && state_->store)
        candidate.recoverable = true;
    else {
        candidate.document.reset();
        candidate.model = {};
        candidate.history.clear();
        candidate.cursor = 0;
        candidate.operations.clear();
        candidate.initial_content_state.clear();
        candidate.recoverable = false;
    }
    if (auto error = persist(candidate, state_->store.get(), state_->generation, state_->poisoned))
        return {Status::failed, std::nullopt, std::move(error)};
    using std::swap;
    swap(state_->data, candidate);
    if (state_->store)
        state_->store->release_projects_except("");
    return success(result);
}
const char* status_name(Status status) {
    switch (status) {
    case Status::success:
        return "success";
    case Status::needs_input:
        return "needs_input";
    case Status::conflict:
        return "conflict";
    case Status::failed:
        return "failed";
    }
    return "unknown";
}

const char* error_name(ErrorCode code) {
    switch (code) {
    case ErrorCode::missing_input:
        return "MISSING_INPUT";
    case ErrorCode::invalid_input:
        return "INVALID_INPUT";
    case ErrorCode::invalid_unit:
        return "INVALID_UNIT";
    case ErrorCode::entity_not_found:
        return "ENTITY_NOT_FOUND";
    case ErrorCode::document_not_found:
        return "DOCUMENT_NOT_FOUND";
    case ErrorCode::document_already_open:
        return "DOCUMENT_ALREADY_OPEN";
    case ErrorCode::document_epoch_expired:
        return "DOCUMENT_EPOCH_EXPIRED";
    case ErrorCode::revision_conflict:
        return "REVISION_CONFLICT";
    case ErrorCode::preview_expired:
        return "PREVIEW_EXPIRED";
    case ErrorCode::idempotency_key_conflict:
        return "IDEMPOTENCY_KEY_CONFLICT";
    case ErrorCode::nothing_to_undo:
        return "NOTHING_TO_UNDO";
    case ErrorCode::nothing_to_redo:
        return "NOTHING_TO_REDO";
    case ErrorCode::resource_limit:
        return "RESOURCE_LIMIT";
    case ErrorCode::unsupported_capability:
        return "UNSUPPORTED_CAPABILITY";
    case ErrorCode::storage_failure:
        return "STORAGE_FAILURE";
    case ErrorCode::storage_uncertain:
        return "STORAGE_UNCERTAIN";
    case ErrorCode::schema_unsupported:
        return "SCHEMA_UNSUPPORTED";
    }
    return "UNKNOWN";
}

} // namespace qcae
