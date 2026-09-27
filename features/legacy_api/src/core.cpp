#include "qcae/core.hpp"
#include "qcae/legacy_migration.hpp"
#include "qcae/quantities.hpp"
#include "qcae/records.hpp"
#include "qcae/records_model_bridge.hpp"
#include "legacy_state.hpp"
#include <algorithm>
#include <cmath>

namespace qcae {
namespace {
template <class T> Result<T> success(T value) {
    return {Status::success, std::move(value), {}};
}
template <class T>
Result<T> failure(Status status, ErrorCode code, const char* message, const char* field = "") {
    return {status, {}, Diagnostic{code, message, field}};
}
bool blank(const std::string& value) {
    return value.find_first_not_of(" \t\r\n") == std::string::npos;
}
Result<double> normalize(Quantity value) {
    // Validate the supplied numeric range before scaling, matching the typed
    // input contract even when a larger unit could amplify a subnormal value.
    const auto checked =
        parameters::convert_quantity(value, parameters::Dimension::pressure, value.unit);
    auto normalized = checked.ok() ? parameters::canonical_quantity(*checked.value,
                                                                    parameters::Dimension::pressure)
                                   : checked;
    if (!normalized.ok()) {
        if (normalized.error)
            normalized.error->field = "young_modulus." + normalized.error->field;
        return {normalized.status, {}, std::move(normalized.error)};
    }
    if (normalized.value->value <= 0)
        return failure<double>(Status::failed,
                               ErrorCode::invalid_input,
                               "Young modulus must be finite and positive",
                               "young_modulus.value");
    return success(normalized.value->value);
}
// Compatibility for embedders supplying only the old blob port (including test
// fakes). Production SQLite implements IRecordStore and never uses this adapter.
class BlobRecordStore final : public IRecordStore {
  public:
    explicit BlobRecordStore(std::shared_ptr<IWorkspaceStore> store) : store_(std::move(store)) {}
    LoadedRows load_rows() override {
        auto loaded = store_->load();
        if (!loaded) {
            rows_.clear();
            return {};
        }
        try {
            auto rows = decode_record_rows(loaded->payload);
            rows_.clear();
            for (const auto& row : rows)
                rows_.emplace(row.key, row.value);
            return {loaded->generation, std::move(rows), {}};
        } catch (const RecordError&) {
            return {loaded->generation, {}, *loaded};
        }
    }
    BatchReceipt commit_rows(const StoreBatch& batch) override {
        auto next = rows_;
        for (const auto& mutation : batch.mutations)
            if (mutation.after)
                next[mutation.key] = mutation.after;
            else
                next.erase(mutation.key);
        std::vector<StoredRow> values;
        for (const auto& [key, value] : next)
            values.push_back({key, value});
        note_whole_model_serialization();
        auto payload = encode_record_rows(values);
        auto generation = store_->commit(batch.expected_generation, payload);
        rows_.swap(next);
        return {generation, batch.mutations.size(), payload.size()};
    }

  private:
    std::shared_ptr<IWorkspaceStore> store_;
    std::map<StoreKey, SharedStoreBytes> rows_;
};
std::optional<Diagnostic> validate_records(const DocumentView& view, const Limits& limits) {
    if (view.count(RecordTraits<records::Material>::type_id) > limits.max_materials)
        return Diagnostic{ErrorCode::resource_limit, "Material limit reached", "materials"};
    std::size_t entities{}, relations{};
    std::optional<Diagnostic> error;
    view.visit([&](const Record& record) {
        if (record->key().type != RecordTraits<records::SourceIdentifier>::type_id &&
            record->key().type != RecordTraits<records::Mesh>::type_id)
            ++entities;
        record->descriptor().references(
            record->object(),
            [&](RecordFieldId, std::string_view, std::span<const RecordTypeId>) { ++relations; });
    });
    auto text_check = [&](const std::string& text, const EntityId& id) {
        if (text.size() > limits.max_name_bytes)
            error =
                Diagnostic{ErrorCode::resource_limit, "Entity text exceeds byte limit", id.value};
        else if (std::any_of(
                     text.begin(), text.end(), [](unsigned char c) { return c < 32 || c == 127; }))
            error = Diagnostic{
                ErrorCode::invalid_input, "Entity text contains control characters", id.value};
    };
    auto named = [&]<class T>() {
        view.visit(RecordTraits<T>::type_id, [&](const Record& record) {
            const auto& value = record->get<T>();
            text_check(value.name, value.id);
        });
    };
    named.template operator()<records::Material>();
    named.template operator()<records::BeamSection>();
    named.template operator()<records::Part>();
    named.template operator()<records::Assembly>();
    named.template operator()<records::EntitySet>();
    named.template operator()<records::AnalysisDefinition>();
    view.visit(RecordTraits<records::IncludeDocument>::type_id, [&](const Record& record) {
        const auto& value = record->get<records::IncludeDocument>();
        text_check(value.path, value.id);
    });
    if (entities > limits.max_entities || relations > limits.max_relations)
        return Diagnostic{ErrorCode::resource_limit, "Model limit reached", "model"};
    return error;
}
RecordApplicationOptions settings(Limits limits,
                                  std::shared_ptr<IWorkspaceStore> store,
                                  std::function<bool(const ProfileRef&)> supports,
                                  std::vector<OwnedRowHandler> owned_row_handlers,
                                  std::shared_ptr<const RecordRegistry> registry) {
    RecordApplicationOptions options;
    options.registry = registry ? std::move(registry) : make_record_registry();
    options.limits = limits;
    options.owned_row_handlers = std::move(owned_row_handlers);
    options.projects = store;
    options.records = std::dynamic_pointer_cast<IRecordStore>(store);
    if (store && !options.records)
        options.records = std::make_shared<BlobRecordStore>(store);
    options.material_count = [](const DocumentView& view) {
        return view.count(RecordTraits<records::Material>::type_id);
    };
    options.validate = validate_records;
    options.profiles_supported = [supports = std::move(supports)](const DocumentView& view) {
        if (!supports)
            return true;
        bool result = true;
        view.visit(RecordTraits<records::AnalysisDefinition>::type_id, [&](const Record& r) {
            result = result && supports(r->get<records::AnalysisDefinition>().target.profile);
        });
        view.visit(RecordTraits<records::SourceIdentifier>::type_id, [&](const Record& r) {
            result = result && supports(r->get<records::SourceIdentifier>().profile);
        });
        return result;
    };
    options.decode_legacy_project = [registry = options.registry, limits](std::string_view value) {
        try {
            return migrate_legacy_project(value, registry, limits);
        } catch (const state_codec::CodecError& error) {
            throw RecordError(ErrorCode::schema_unsupported, error.what());
        }
    };
    return options;
}
} // namespace
struct MemoryApplication::State {
    State(Limits limits,
          std::shared_ptr<IWorkspaceStore> store,
          std::function<bool(const ProfileRef&)> supports,
          std::vector<OwnedRowHandler> owned_row_handlers,
          std::shared_ptr<const RecordRegistry> registry)
        : app(settings(limits,
                       std::move(store),
                       std::move(supports),
                       std::move(owned_row_handlers),
                       std::move(registry))),
          limits(limits) {}
    RecordApplication app;
    Limits limits;
};
MemoryApplication::MemoryApplication(Limits limits,
                                     std::shared_ptr<IWorkspaceStore> store,
                                     std::function<bool(const ProfileRef&)> supported,
                                     std::vector<OwnedRowHandler> owned_row_handlers,
                                     std::shared_ptr<const RecordRegistry> registry) {
    try {
        state_ = std::make_unique<State>(limits,
                                         std::move(store),
                                         std::move(supported),
                                         std::move(owned_row_handlers),
                                         std::move(registry));
    } catch (const RecordError& error) {
        throw state_codec::CodecError(error.what());
    }
}
MemoryApplication::~MemoryApplication() = default;
RecordApplication& MemoryApplication::record_application() noexcept {
    return state_->app;
}
const RecordApplication& MemoryApplication::record_application() const noexcept {
    return state_->app;
}
bool MemoryApplication::durable() const noexcept {
    return state_->app.durable();
}
bool MemoryApplication::recovery_available() const {
    return state_->app.recovery_available();
}
Result<DocumentInfo>
MemoryApplication::create_document(const Caller& c, const std::string& n, const std::string& k) {
    return state_->app.create_document(c, n, k);
}
Result<DocumentInfo> MemoryApplication::current_document() const {
    return state_->app.current_document();
}
Result<DocumentInfo>
MemoryApplication::open_document(const Caller& c, const std::string& p, const std::string& k) {
    try {
        return state_->app.open_document(c, p, k);
    } catch (const state_codec::CodecError& e) {
        return failure<DocumentInfo>(Status::failed, ErrorCode::schema_unsupported, e.what());
    }
}
Result<DocumentInfo> MemoryApplication::recover_document(const Caller& c, const std::string& k) {
    return state_->app.recover_document(c, k);
}
Result<DocumentInfo> MemoryApplication::save_document(
    const Caller& c, const WriteContext& w, const std::string& p, bool a, const std::string& k) {
    return state_->app.save_document(c, w, p, a, k);
}
Result<DocumentInfo> MemoryApplication::close_document(const Caller& c,
                                                       const WriteContext& w,
                                                       ClosePolicy p,
                                                       const std::string& k) {
    return state_->app.close_document(c, w, p, k);
}
Result<DocumentInfo> MemoryApplication::host_operation(const Caller& c,
                                                       const std::string& op,
                                                       const std::string& k) const {
    return state_->app.host_operation(c, op, k);
}
Result<ModelSnapshot> MemoryApplication::snapshot(const DocumentRef& ref) const {
    auto value = state_->app.snapshot(ref);
    if (!value.ok())
        return {value.status, {}, value.error};
    ModelSnapshot result;
    static_cast<Model&>(result) = model_from_records(value.value->records);
    result.info = value.value->info;
    return success(std::move(result));
}
Result<ChangeReceipt> MemoryApplication::commit(const Caller& c,
                                                const WriteContext& w,
                                                const PreviewId& p,
                                                const std::string& k) {
    return state_->app.commit(c, w, p, k);
}
Result<ChangeReceipt>
MemoryApplication::undo(const Caller& c, const WriteContext& w, const std::string& k) {
    return state_->app.undo(c, w, k);
}
Result<ChangeReceipt>
MemoryApplication::redo(const Caller& c, const WriteContext& w, const std::string& k) {
    return state_->app.redo(c, w, k);
}
Result<HistorySnapshot> MemoryApplication::history(const DocumentRef& d) const {
    return state_->app.history(d);
}
Result<ChangeReceipt> MemoryApplication::operation(const Caller& c,
                                                   const DocumentRef& d,
                                                   const std::string& op,
                                                   const std::string& k) const {
    return state_->app.operation(c, d, op, k);
}
Result<ChangePreview> MemoryApplication::preview(const Caller& caller,
                                                 const WriteContext& context,
                                                 const MaterialCommand& command) {
    return state_->app.preview(
        caller,
        context,
        [&](const DocumentView& view,
            const RecordIdentityAllocator& allocate) -> Result<RecordPreparedOperation> {
            EditSession edit(view);
            records::Material material;
            bool create = false;
            Result<double> value;
            if (const auto* c = std::get_if<CreateMaterial>(&command)) {
                if (blank(c->name))
                    return failure<RecordPreparedOperation>(Status::needs_input,
                                                            ErrorCode::missing_input,
                                                            "Material name is required",
                                                            "name");
                if (c->name.size() > state_->limits.max_name_bytes)
                    return failure<RecordPreparedOperation>(Status::failed,
                                                            ErrorCode::resource_limit,
                                                            "Material name exceeds byte limit",
                                                            "name");
                if (view.count(RecordTraits<records::Material>::type_id) >=
                    state_->limits.max_materials)
                    return failure<RecordPreparedOperation>(
                        Status::failed, ErrorCode::resource_limit, "Material limit reached");
                value = normalize(c->young_modulus);
                material.name = c->name;
                create = true;
            } else {
                const auto& change = std::get<SetYoungModulus>(command);
                auto record = view.find<records::Material>(change.id);
                if (!record)
                    return failure<RecordPreparedOperation>(Status::failed,
                                                            ErrorCode::entity_not_found,
                                                            "Material does not exist",
                                                            "id");
                material = record->get<records::Material>();
                value = normalize(change.young_modulus);
            }
            if (!value.ok())
                return {value.status, {}, value.error};
            if (create)
                material.id = allocate();
            material.young_modulus_mpa = *value.value;
            edit.put(material);
            return success(RecordPreparedOperation{edit.prepare(),
                                                   create ? "Create material" : "Set Young modulus",
                                                   material.id,
                                                   material.name,
                                                   *value.value,
                                                   create});
        });
}
Result<ChangePreview> MemoryApplication::preview_import(const Caller& caller,
                                                        const WriteContext& context,
                                                        const Model& model) {
    return state_->app.preview(
        caller,
        context,
        [&](const DocumentView& view,
            const RecordIdentityAllocator&) -> Result<RecordPreparedOperation> {
            if (view.size())
                return failure<RecordPreparedOperation>(
                    Status::conflict, ErrorCode::invalid_input, "Import requires an empty model");
            if (auto error = legacy_detail::validate_candidate(model, state_->limits))
                return {Status::failed, {}, error};
            RecordStats stats;
            auto imported = records_from_model(model, view.registry(), view.version(), &stats);
            auto changes = diff_record_views(view, imported);
            return success(RecordPreparedOperation{
                {view.version(), std::move(imported), std::move(changes), stats},
                "Import model",
                EntityId{},
                "",
                0,
                false});
        });
}
Result<ChangePreview> MemoryApplication::preview_edit(const Caller& caller,
                                                      const WriteContext& context,
                                                      const ModelEdit& command) {
    return state_->app.preview(
        caller,
        context,
        [&](const DocumentView& view,
            const RecordIdentityAllocator& allocate) -> Result<RecordPreparedOperation> {
            EditSession edit(view);
            EntityId affected;
            std::string label;
            bool create = false, valid = true;
            auto upsert = [&]<class T>(T value) {
                if (value.id.value.empty()) {
                    value.id = allocate();
                    create = true;
                } else if (!view.find<T>(value.id)) {
                    valid = false;
                    return;
                }
                affected = value.id;
                edit.put(std::move(value));
            };
            std::visit(
                [&](const auto& change) {
                    using T = std::decay_t<decltype(change)>;
                    if constexpr (std::is_same_v<T, UpsertPart>) {
                        upsert(records::Part{
                            change.value.id, change.value.name, change.value.members});
                        label = "Upsert part";
                    } else if constexpr (std::is_same_v<T, UpsertAssembly>) {
                        upsert(records::Assembly{
                            change.value.id, change.value.name, change.value.children});
                        label = "Upsert assembly";
                    } else if constexpr (std::is_same_v<T, UpsertSet>) {
                        upsert(records::EntitySet{
                            change.value.id, change.value.name, change.value.members});
                        label = "Upsert set";
                    } else if constexpr (std::is_same_v<T, MoveNode>) {
                        affected = change.id;
                        label = "Move node";
                        if (!view.find<records::Node>(change.id)) {
                            valid = false;
                            return;
                        }
                        edit.update<records::Node>(change.id, [&](auto& node) {
                            node.position = {
                                change.position.x, change.position.y, change.position.z};
                        });
                    } else if constexpr (std::is_same_v<T, DeleteEntity>) {
                        affected = change.id;
                        label = "Delete entity";
                        auto target = view.find_identity(change.id.value);
                        if (!target) {
                            valid = false;
                            return;
                        }
                        view.visit([&](const Record& record) {
                            if (record->key() == target->key())
                                return;
                            if (record->key().type ==
                                RecordTraits<records::IncludeDocument>::type_id) {
                                auto include = record->get<records::IncludeDocument>();
                                if (include.parent && *include.parent == change.id)
                                    valid = false;
                                auto old = include.members.size();
                                std::erase(include.members, change.id);
                                if (old != include.members.size())
                                    edit.put(std::move(include));
                                return;
                            }
                            if (record->key().type ==
                                    RecordTraits<records::SourceIdentifier>::type_id &&
                                record->get<records::SourceIdentifier>().entity == change.id) {
                                edit.erase(record->key());
                                return;
                            }
                            record->descriptor().references(record->object(),
                                                            [&](RecordFieldId,
                                                                std::string_view id,
                                                                std::span<const RecordTypeId>) {
                                                                if (id == change.id.value)
                                                                    valid = false;
                                                            });
                        });
                        if (valid)
                            edit.erase(target->key());
                    }
                },
                command);
            if (!valid)
                return failure<RecordPreparedOperation>(
                    Status::failed,
                    ErrorCode::invalid_input,
                    "Edit entity is missing, has the wrong kind, or is referenced",
                    "id");
            return success(RecordPreparedOperation{edit.prepare(), label, affected, "", 0, create});
        });
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
