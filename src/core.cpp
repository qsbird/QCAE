#include "qcae/core.hpp"

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

template <class T> Result<T> success(T value) {
    return {Status::success, std::move(value), std::nullopt};
}

template <class T>
Result<T> failure(Status status, ErrorCode code, const char* message, const char* field = "") {
    return {status, std::nullopt, Diagnostic{code, message, field}};
}

std::string nonce() {
    std::random_device source;
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (int i = 0; i != 4; ++i)
        out << std::setw(8) << source();
    return out.str();
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

struct Prepared {
    Caller caller;
    WriteContext context;
    bool create{};
    EntityId entity;
    std::string name;
    double modulus{};
    std::optional<Model> after;
    std::string label;
};

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

struct HistoryEntry {
    TransactionId transaction;
    std::string label;
    Model after;
    std::string content_state;
};

struct RecordedOperation {
    std::string signature;
    ChangeReceipt receipt;
};

struct Data {
    Limits limits;
    std::string application_nonce;
    std::uint64_t next_id{1};
    std::optional<DocumentInfo> document;
    Model model;
    std::string initial_content_state;
    std::vector<HistoryEntry> history;
    std::size_t cursor{};
    std::map<std::string, Prepared> previews;
    std::map<std::string, RecordedOperation> operations;
    std::map<std::string, std::pair<std::string, DocumentInfo>> creates;
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
    if (!data.document || data.document->document.id != ref.id)
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

void update_document(Data& data) {
    data.document->material_count = data.model.materials.size();
    data.document->dirty = data.document->content_state != data.initial_content_state;
}

std::size_t entity_count(const Model& model) {
    return model.materials.size() + model.nodes.size() + model.sections.size() +
           model.beams.size() + model.parts.size() + model.assemblies.size() + model.sets.size() +
           model.includes.size() + model.forces.size() + model.constraints.size() +
           model.analyses.size();
}

std::size_t relation_count(const Model& model) {
    std::size_t count = model.sources.size() * 2;
    for (const auto& beam : model.beams) {
        (void)beam;
        count += 3;
    }
    for (const auto& section : model.sections) {
        (void)section;
        ++count;
    }
    for (const auto& force : model.forces) {
        (void)force;
        ++count;
    }
    for (const auto& part : model.parts)
        count += part.members.size();
    for (const auto& assembly : model.assemblies)
        count += assembly.children.size();
    for (const auto& set : model.sets)
        count += set.members.size();
    for (const auto& include : model.includes)
        count += include.members.size() + (include.parent ? 1 : 0);
    for (const auto& constraint : model.constraints)
        count += constraint.nodes.size();
    for (const auto& analysis : model.analyses)
        count += analysis.forces.size() + analysis.constraints.size();
    return count;
}

bool has_control(const std::string& value) {
    return std::any_of(
        value.begin(), value.end(), [](unsigned char c) { return c < 32 || c == 127; });
}

std::optional<Diagnostic> validate_candidate(const Model& model, const Limits& limits) {
    if (model.materials.size() > limits.max_materials)
        return Diagnostic{ErrorCode::resource_limit, "Material limit reached", "materials"};
    if (entity_count(model) > limits.max_entities)
        return Diagnostic{ErrorCode::resource_limit, "Entity limit reached", "model"};
    if (relation_count(model) > limits.max_relations)
        return Diagnostic{ErrorCode::resource_limit, "Relation limit reached", "model"};
    for (const auto& entity : model_entities(model)) {
        if (entity.name.size() > limits.max_name_bytes)
            return Diagnostic{
                ErrorCode::resource_limit, "Entity name exceeds byte limit", entity.id.value};
        if (has_control(entity.name))
            return Diagnostic{ErrorCode::invalid_input,
                              "Entity name contains control characters",
                              entity.id.value};
    }
    for (const auto& include : model.includes)
        if (include.path.size() > limits.max_name_bytes)
            return Diagnostic{
                ErrorCode::resource_limit, "Include path exceeds byte limit", include.id.value};
        else if (has_control(include.path))
            return Diagnostic{ErrorCode::invalid_input,
                              "Include path contains control characters",
                              include.id.value};
    for (const auto& diagnostic : validate_model(model))
        return diagnostic;
    return std::nullopt;
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
};

MemoryApplication::MemoryApplication(Limits limits) : state_(std::make_unique<State>()) {
    state_->data.limits = limits;
    state_->data.application_nonce = nonce();
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
    if (const auto found = current.creates.find(key); found != current.creates.end()) {
        if (found->second.first != name)
            return failure<DocumentInfo>(Status::conflict,
                                         ErrorCode::idempotency_key_conflict,
                                         "Key was used with another document name",
                                         "idempotency_key");
        return success(found->second.second);
    }
    if (current.document)
        return failure<DocumentInfo>(Status::conflict,
                                     ErrorCode::document_already_open,
                                     "An active document already exists");
    if (current.creates.size() + current.operations.size() >=
        current.limits.max_idempotency_records)
        return failure<DocumentInfo>(
            Status::failed, ErrorCode::resource_limit, "Idempotency record limit reached");
    Data candidate = current;
    DocumentInfo info;
    info.document.id = DocumentId(new_id(candidate, "doc"));
    info.document.epoch = DocumentEpoch(new_id(candidate, "epoch"));
    info.content_state = new_id(candidate, "state");
    info.name = name;
    candidate.initial_content_state = info.content_state;
    candidate.document = info;
    candidate.creates.emplace(key, std::make_pair(name, info));
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
    if (current.creates.size() + current.operations.size() >=
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
    candidate.history.push_back(HistoryEntry{
        transaction, prepared.label, candidate.model, candidate.document->content_state});
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
    using std::swap;
    swap(state_->data, candidate);
    return result;
}

namespace {
Result<ChangeReceipt> move_history(Data& data,
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
    if (undo ? data.cursor == 0 : data.cursor == data.history.size())
        return failure<ChangeReceipt>(Status::conflict,
                                      undo ? ErrorCode::nothing_to_undo
                                           : ErrorCode::nothing_to_redo,
                                      undo ? "Nothing to undo" : "Nothing to redo");
    if (data.creates.size() + data.operations.size() >= data.limits.max_idempotency_records)
        return failure<ChangeReceipt>(
            Status::failed, ErrorCode::resource_limit, "Idempotency record limit reached");
    Data candidate = data;
    if (undo)
        --candidate.cursor;
    else
        ++candidate.cursor;
    if (candidate.cursor == 0) {
        candidate.model = {};
        candidate.document->content_state = candidate.initial_content_state;
    } else {
        const auto& entry = candidate.history[candidate.cursor - 1];
        candidate.model = entry.after;
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
    using std::swap;
    swap(data, candidate);
    return result;
}
} // namespace

Result<ChangeReceipt> MemoryApplication::undo(const Caller& caller,
                                              const WriteContext& context,
                                              const std::string& idempotency_key) {
    std::lock_guard lock(state_->mutex);
    return move_history(state_->data, caller, context, idempotency_key, true);
}

Result<ChangeReceipt> MemoryApplication::redo(const Caller& caller,
                                              const WriteContext& context,
                                              const std::string& idempotency_key) {
    std::lock_guard lock(state_->mutex);
    return move_history(state_->data, caller, context, idempotency_key, false);
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
    }
    return "UNKNOWN";
}

} // namespace qcae
