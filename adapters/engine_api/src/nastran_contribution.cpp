#include "qcae/nastran_contribution.hpp"
#include "qcae/nastran_package.hpp"
#include "qcae/operation_inputs.hpp"
#include "qcae/records.hpp"
#include "qcae/result_api.hpp"
#include "typed_values.hpp"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <charconv>
#include <mutex>

namespace qcae::ipc {
namespace {
using namespace operations;
using FrozenInput = features::analysis::FrozenAnalysisInput;
constexpr std::string_view owner = "qcae.nastran.artifact";
constexpr std::string_view task_owner = "qcae.runtime.task";
constexpr std::string_view reconcile_owner = "qcae.nastran.reconcile";
template <class T> Result<T> good(T value) {
    return {Status::success, std::move(value), {}};
}
template <class T> Result<T> bad(ErrorCode code, std::string message) {
    return {code == ErrorCode::revision_conflict || code == ErrorCode::idempotency_key_conflict ||
                    code == ErrorCode::document_epoch_expired
                ? Status::conflict
                : Status::failed,
            {},
            Diagnostic{code, std::move(message), "artifact"}};
}
std::uint64_t number(std::string_view text) {
    std::uint64_t value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        throw RecordError(ErrorCode::schema_unsupported, "Malformed artifact integer");
    return value;
}
struct ArtifactRecord {
    std::string principal;
    FrozenInput frozen;
    LocalArtifactIntent intent;
    bool published{};
};
std::string encode(const ArtifactRecord& record) {
    std::vector<std::string> fields{"QCAE-NASTRAN-ARTIFACT-1",
                                    record.principal,
                                    features::analysis::encode_frozen_analysis_input(record.frozen),
                                    record.intent.artifact_id,
                                    record.intent.task_id,
                                    record.intent.directory.string(),
                                    record.intent.root_resource,
                                    record.intent.manifest,
                                    record.published ? "published" : "intent",
                                    std::to_string(record.intent.files.size())};
    for (const auto& file : record.intent.files)
        fields.insert(fields.end(), {file.path, std::to_string(file.byte_length), file.sha256});
    return record_wire::strings(fields);
}
ArtifactRecord decode(const OwnedRowImage& image) {
    if (image.owner != owner || image.schema_version != 1 || !image.payload ||
        image.key.space != StoreSpace::artifact_record)
        throw RecordError(ErrorCode::schema_unsupported, "Invalid Nastran artifact row");
    const auto fields = record_wire::read_strings(*image.payload);
    if (fields.size() < 10 || fields[0] != "QCAE-NASTRAN-ARTIFACT-1" || fields[1].empty() ||
        fields[3] != image.key.identity || fields[4].empty() || fields[7].empty() ||
        (fields[8] != "published" && fields[8] != "intent"))
        throw RecordError(ErrorCode::schema_unsupported, "Malformed Nastran artifact fact");
    const auto count = number(fields[9]);
    if (count == 0 || count > 1000 || fields.size() != 10 + count * 3)
        throw RecordError(ErrorCode::schema_unsupported, "Malformed artifact file manifest");
    ArtifactRecord record{fields[1],
                          features::analysis::decode_frozen_analysis_input(fields[2]),
                          {fields[3], fields[4], fields[5], fields[6], {}, fields[7]},
                          fields[8] == "published"};
    for (std::size_t index = 10; index < fields.size(); index += 3)
        record.intent.files.push_back(
            {fields[index], number(fields[index + 1]), fields[index + 2]});
    return record;
}
std::shared_ptr<const OwnedRowImage> row(const ArtifactRecord& record) {
    return std::make_shared<const OwnedRowImage>(
        OwnedRowImage{{StoreSpace::artifact_record, record.intent.artifact_id},
                      std::string(owner),
                      1,
                      std::make_shared<const std::string>(encode(record)),
                      {}});
}
std::shared_ptr<const OwnedRowImage> task_row(const TaskRecord& task) {
    return std::make_shared<const OwnedRowImage>(
        OwnedRowImage{{StoreSpace::task_record, task.id},
                      std::string(task_owner),
                      1,
                      std::make_shared<const std::string>(encode_task_record(task)),
                      {}});
}
QJsonObject profile_json(const ProfileRef& profile) {
    return {{"profile_id", QString::fromStdString(profile.profile_id)},
            {"profile_version", QString::fromStdString(profile.profile_version)},
            {"definition_digest", QString::fromStdString(profile.definition_digest)}};
}
std::string manifest(const ArtifactRecord& record) {
    const auto& frozen = record.frozen;
    QJsonArray files, identifiers;
    for (const auto& file : record.intent.files)
        files.append(QJsonObject{{"path", QString::fromStdString(file.path)},
                                 {"byte_length", QString::number(file.byte_length)},
                                 {"sha256", QString::fromStdString(file.sha256)}});
    for (const auto& item : frozen.identities)
        identifiers.append(QJsonObject{{"entity_id", QString::fromStdString(item.entity.value)},
                                       {"namespace", QString::fromStdString(item.name_space)},
                                       {"number", QString::number(item.number)}});
    return QJsonDocument(
               QJsonObject{
                   {"schema_version", "qcae.nastran.artifact.v1"},
                   {"complete", true},
                   {"artifact_id", QString::fromStdString(record.intent.artifact_id)},
                   {"task_id", QString::fromStdString(record.intent.task_id)},
                   {"analysis_id", QString::fromStdString(frozen.analysis.value)},
                   {"document_id", QString::fromStdString(frozen.version.document.id.value)},
                   {"document_epoch", QString::fromStdString(frozen.version.document.epoch.value)},
                   {"revision", QString::number(frozen.version.revision)},
                   {"target_profile", profile_json(frozen.target.profile)},
                   {"analysis_kind", QString::fromStdString(frozen.target.analysis_kind)},
                   {"unit_system", "mm-N-MPa"},
                   {"case_label", QString::fromStdString(frozen.case_label)},
                   {"input_sha256",
                    QString::fromStdString(artifact_sha256(frozen.input_signature))},
                   {"physical_signature_hex",
                    QString::fromStdString(features::analysis::physical_signature_hex(frozen))},
                   {"export_id_map", identifiers},
                   {"root_resource", QString::fromStdString(record.intent.root_resource)},
                   {"files", files}})
        .toJson(QJsonDocument::Indented)
        .toStdString();
}
void validate_artifact_task(const ArtifactRecord& record, const TaskRecord& task) {
    const auto& input = record.frozen;
    if (record.intent.manifest != manifest(record))
        throw RecordError(ErrorCode::schema_unsupported,
                          "Artifact manifest differs from its frozen input and file map");
    if (task.id != record.intent.task_id || task.caller.principal != record.principal ||
        task.operation != "model.export" || task.input.document.id != input.version.document.id ||
        task.input.document.epoch != input.version.document.epoch ||
        task.input.revision != input.version.revision ||
        task.input.profile != input.target.profile || task.receipt ||
        record.published != (task.state == TaskState::succeeded))
        throw RecordError(ErrorCode::schema_unsupported,
                          "Artifact and task have inconsistent publication provenance");
    const TaskArtifactReceipt expected{
        record.intent.artifact_id, input.version.revision, artifact_sha256(record.intent.manifest)};
    if ((task.state == TaskState::succeeded && task.artifact_receipt != expected) ||
        (task.state != TaskState::succeeded && task.artifact_receipt))
        throw RecordError(ErrorCode::schema_unsupported,
                          "Artifact task receipt differs from the frozen manifest");
}
Value artifact_value(const ArtifactRecord& record, bool verified) {
    return Value(Value::Object{
        {"artifact_id", Value(record.intent.artifact_id)},
        {"task_id", Value(record.intent.task_id)},
        {"state", Value(record.published ? "published" : "unverified")},
        {"verified", Value(verified)},
        {"output_directory", Value(record.intent.directory.string())},
        {"manifest_path", Value((record.intent.directory / "manifest.json").string())},
        {"analysis_id", Value(record.frozen.analysis.value)},
        {"input_revision", Value(std::to_string(record.frozen.version.revision))},
        {"input_sha256", Value(artifact_sha256(record.frozen.input_signature))},
        {"physical_signature_hex",
         Value(features::analysis::physical_signature_hex(record.frozen))}});
}
struct FrozenExport {
    ArtifactPlan plan;
    ArtifactRecord record;
};
struct ArtifactPayload final : TaskPayload {
    explicit ArtifactPayload(std::shared_ptr<const FrozenExport> value)
        : frozen(std::move(value)) {}
    std::shared_ptr<const FrozenExport> frozen;
};
std::string pending_key(const Caller& caller, std::string_view key) {
    return caller.principal + '\0' + std::string(key);
}
} // namespace

struct NastranArtifactCoordinator::State {
    NastranCodec codec;
    LocalArtifactStore files;
    RecordApplication* app{};
    std::mutex mutex;
    std::map<std::string, std::shared_ptr<FrozenExport>> pending;
    Result<std::pair<ArtifactRecord, std::shared_ptr<const OwnedRowImage>>>
    find(const DocumentRef& document, std::string_view id) const {
        if (!app)
            return bad<std::pair<ArtifactRecord, std::shared_ptr<const OwnedRowImage>>>(
                ErrorCode::invalid_input, "Artifact coordinator has no application");
        const auto rows = app->owned_rows(document, StoreSpace::artifact_record, owner);
        if (!rows.ok())
            return {rows.status, {}, rows.error};
        for (const auto& image : *rows.value)
            if (image->key.identity == id)
                return good(std::make_pair(decode(*image), image));
        return bad<std::pair<ArtifactRecord, std::shared_ptr<const OwnedRowImage>>>(
            ErrorCode::entity_not_found, "Artifact does not exist");
    }
    Result<std::pair<TaskRecord, std::shared_ptr<const OwnedRowImage>>>
    find_task(const DocumentRef& document, const ArtifactRecord& record) const {
        const auto rows = app->owned_rows(document, StoreSpace::task_record, task_owner);
        if (!rows.ok())
            return {rows.status, {}, rows.error};
        for (const auto& image : *rows.value)
            if (image->key.identity == record.intent.task_id) {
                auto task = decode_task_record(*image->payload);
                validate_artifact_task(record, task);
                return good(std::make_pair(std::move(task), image));
            }
        return bad<std::pair<TaskRecord, std::shared_ptr<const OwnedRowImage>>>(
            ErrorCode::schema_unsupported, "Artifact task fact is missing");
    }
};
NastranArtifactCoordinator::NastranArtifactCoordinator() : state_(std::make_shared<State>()) {}
NastranArtifactCoordinator::~NastranArtifactCoordinator() = default;
const NastranCodec& NastranArtifactCoordinator::codec() const noexcept {
    return state_->codec;
}
OwnedRowHandler NastranArtifactCoordinator::row_handler() {
    return {StoreSpace::artifact_record,
            std::string(owner),
            [](const OwnedRowImage& image) { (void)decode(image); },
            [](const OwnedRowImage&) -> std::shared_ptr<const OwnedRowImage> { return {}; },
            [](const OwnedRowImage&) { return false; }};
}
OwnedRowHandler NastranArtifactCoordinator::reconcile_row_handler() {
    return {StoreSpace::artifact_record,
            std::string(reconcile_owner),
            [](const OwnedRowImage& image) {
                if (image.owner != reconcile_owner || image.schema_version != 1 || !image.payload)
                    throw RecordError(ErrorCode::schema_unsupported,
                                      "Invalid reconcile fact owner/schema");
                const auto fields = record_wire::read_strings(*image.payload);
                if (fields.size() != 5 || fields[0] != "QCAE-ARTIFACT-RECONCILE-1" ||
                    fields[1].empty() || fields[2].empty() || fields[3].empty() ||
                    fields[4].empty() ||
                    image.key.identity !=
                        "reconcile-" + artifact_sha256(record_wire::strings(
                                           std::array<std::string, 2>{fields[1], fields[2]})))
                    throw RecordError(ErrorCode::schema_unsupported,
                                      "Malformed reconcile request fact");
            },
            [](const OwnedRowImage&) -> std::shared_ptr<const OwnedRowImage> { return {}; },
            [](const OwnedRowImage&) { return false; }};
}
TaskPublisher
NastranArtifactCoordinator::publisher(RecordApplication& app,
                                      std::function<bool(const ProfileRef&)> supported) {
    state_->app = &app;
    auto base = record_task_publisher(app, std::move(supported));
    auto publisher = base;
    const auto state = state_;
    publisher.persist = [state, base, &app](const TaskRecord& task,
                                            const std::optional<TaskRecord>& previous) {
        if (task.operation != "model.export" || previous)
            return base.persist(task, previous);
        try {
            std::shared_ptr<FrozenExport> frozen;
            {
                std::lock_guard lock(state->mutex);
                const auto found =
                    state->pending.find(pending_key(task.caller, task.idempotency_key));
                if (found == state->pending.end())
                    return bad<bool>(ErrorCode::invalid_input, "Export frozen intent is missing");
                frozen = found->second;
            }
            frozen->record.intent.task_id = task.id;
            frozen->record.intent.artifact_id = "artifact-" + task.id;
            frozen->record.intent.manifest = manifest(frozen->record);
            validate_artifact_task(frozen->record, task);
            const auto artifact = row(frozen->record), next = task_row(task);
            const std::array<OwnedRowUpdate, 2> updates{
                {{next->key, {}, next}, {artifact->key, {}, artifact}}};
            const auto result = app.update_owned_rows(task.caller, task.input.document, updates);
            if (result.ok()) {
                std::lock_guard lock(state->mutex);
                state->pending.erase(pending_key(task.caller, task.idempotency_key));
            }
            return result;
        } catch (const std::exception& error) {
            return bad<bool>(ErrorCode::storage_failure, error.what());
        }
    };
    publisher.publish = [state, base, &app](const std::shared_ptr<const TaskPayload>& payload,
                                            const TaskRecord& previous,
                                            TaskRecord complete) -> Result<TaskRecord> {
        const auto artifact = std::dynamic_pointer_cast<const ArtifactPayload>(payload);
        if (!artifact)
            return base.publish(payload, previous, std::move(complete));
        try {
            const auto current = base.validate_input(previous.input);
            if (!current.ok())
                return {current.status, {}, current.error};
            const auto saved =
                state->find(previous.input.document, artifact->frozen->record.intent.artifact_id);
            if (!saved.ok())
                return {saved.status, {}, saved.error};
            validate_artifact_task(saved.value->first, previous);
            if (encode(saved.value->first) != encode(artifact->frozen->record))
                return bad<TaskRecord>(ErrorCode::schema_unsupported,
                                       "Persisted artifact differs from the admitted frozen input");
            state->files.publish(artifact->frozen->record.intent);
            auto verified = artifact->frozen->record;
            verify_nastran_readback(
                artifact->frozen->plan, state->files.verify(verified.intent), state->codec);
            verified.published = true;
            complete.artifact_receipt =
                TaskArtifactReceipt{verified.intent.artifact_id,
                                    verified.frozen.version.revision,
                                    artifact_sha256(verified.intent.manifest)};
            validate_artifact_task(verified, complete);
            const auto next_artifact = row(verified), next_task = task_row(complete);
            const std::array<OwnedRowUpdate, 2> updates{
                {{next_task->key, task_row(previous)->payload, next_task},
                 {next_artifact->key, saved.value->second->payload, next_artifact}}};
            const auto result =
                app.update_owned_rows(previous.caller, previous.input.document, updates);
            if (!result.ok())
                return {result.status, {}, result.error};
            return good(std::move(complete));
        } catch (const std::exception& error) {
            return bad<TaskRecord>(ErrorCode::storage_failure, error.what());
        }
    };
    return publisher;
}
Result<FrozenInput> NastranArtifactCoordinator::frozen_input(const Caller& caller,
                                                             const DocumentRef& document,
                                                             std::string_view artifact_id) const {
    auto artifact = verified_artifact(caller, document, artifact_id);
    if (!artifact.ok())
        return {artifact.status, {}, artifact.error};
    return good(std::move(artifact.value->first));
}
Result<std::pair<FrozenInput, LocalArtifactIntent>> NastranArtifactCoordinator::verified_artifact(
    const Caller& caller, const DocumentRef& document, std::string_view artifact_id) const {
    using VerifiedArtifact = std::pair<FrozenInput, LocalArtifactIntent>;
    try {
        const auto found = state_->find(document, artifact_id);
        if (!found.ok())
            return {found.status, {}, found.error};
        if (found.value->first.principal != caller.principal)
            return bad<VerifiedArtifact>(ErrorCode::invalid_input,
                                         "Artifact belongs to a different principal");
        if (!found.value->first.published)
            return bad<VerifiedArtifact>(ErrorCode::invalid_input,
                                         "Artifact has no published database fact");
        const auto task = state_->find_task(document, found.value->first);
        if (!task.ok())
            return {task.status, {}, task.error};
        (void)state_->files.verify(found.value->first.intent);
        return good(std::make_pair(found.value->first.frozen, found.value->first.intent));
    } catch (const std::exception& error) {
        return bad<VerifiedArtifact>(ErrorCode::storage_failure, error.what());
    }
}

Result<bool> NastranArtifactCoordinator::register_operations(OperationRegistry& registry,
                                                             RecordApplication& app,
                                                             std::function<TaskService&()> tasks) {
    state_->app = &app;
    const auto state = state_;
    auto result = registry.register_typed<ProjectMigrateProfileInput>(
        InputTraits<ProjectMigrateProfileInput>::definition(),
        [state, &app](const OperationContext& context,
                      const ProjectMigrateProfileInput& input) -> Result<Value> {
            const auto installed = state->codec.definition().reference;
            if (*context.expected_profile != installed)
                return bad<Value>(ErrorCode::schema_unsupported,
                                  "Migration target must match installed profile semantics");
            const ProfileRef old{
                input.old_profile_id, input.old_profile_version, input.old_definition_digest};
            const auto signature =
                record_wire::strings(std::array<std::string, 4>{"qcae.nastran.profile-migration.v1",
                                                                input.source_path,
                                                                record_wire::profile(old),
                                                                record_wire::profile(installed)});
            const ProjectRecordMigration migration{
                signature, [old, installed](const DocumentView& view) {
                    const auto prepared = migrate_nastran_profile(view, old, installed);
                    if (!prepared.ok())
                        throw RecordError(prepared.error->code, prepared.error->message);
                    return *prepared.value;
                }};
            const auto document = app.open_migrated_document(
                context.caller, input.source_path, context.idempotency_key, migration);
            if (!document.ok())
                return {document.status, {}, document.error};
            return good(
                Value(Value::Object{{"document_id", Value(document.value->document.id.value)},
                                    {"document_epoch", Value(document.value->document.epoch.value)},
                                    {"revision", Value(std::to_string(document.value->revision))},
                                    {"name", Value(document.value->name)},
                                    {"dirty", Value(document.value->dirty)}}));
        });
    if (!result.ok())
        return result;
    result = registry.register_typed<ModelExportInput>(
        InputTraits<ModelExportInput>::definition(),
        [state, &app, tasks](const OperationContext& context,
                             const ModelExportInput& input) -> Result<Value> {
            try {
                if (*context.expected_profile != state->codec.definition().reference)
                    return bad<Value>(ErrorCode::schema_unsupported,
                                      "Export profile semantics are incompatible");
                const auto signature =
                    canonical_value(InputTraits<ModelExportInput>::to_value(input));
                if (!signature.ok())
                    return {signature.status, {}, signature.error};
                const auto saved_tasks =
                    app.owned_rows(*context.document, StoreSpace::task_record, task_owner);
                if (!saved_tasks.ok())
                    return {saved_tasks.status, {}, saved_tasks.error};
                for (const auto& image : *saved_tasks.value) {
                    const auto previous = decode_task_record(*image->payload);
                    if (previous.caller.principal == context.caller.principal &&
                        previous.idempotency_key == context.idempotency_key) {
                        if (previous.operation != "model.export" ||
                            previous.signature != *signature.value ||
                            previous.input.document.id != context.document->id ||
                            previous.input.document.epoch != context.document->epoch ||
                            previous.input.revision != *context.expected_revision ||
                            previous.input.profile != *context.expected_profile)
                            return bad<Value>(ErrorCode::idempotency_key_conflict,
                                              "Export key belongs to a different input");
                        const auto artifact =
                            state->find(*context.document, "artifact-" + previous.id);
                        if (!artifact.ok())
                            return {artifact.status, {}, artifact.error};
                        validate_artifact_task(artifact.value->first, previous);
                        return detail::converted(tasks().query(context.caller, previous.id),
                                                 detail::task_value);
                    }
                }
                const auto snapshot = app.snapshot(*context.document);
                if (!snapshot.ok())
                    return {snapshot.status, {}, snapshot.error};
                if (snapshot.value->info.revision != *context.expected_revision)
                    return bad<Value>(ErrorCode::revision_conflict,
                                      "Export input revision is stale");
                const auto plan = validate_nastran_export(
                    snapshot.value->records, input.analysis_id, state->codec);
                if (!plan.ok())
                    return {plan.status, {}, plan.error};
                const auto frozen_input =
                    features::analysis::freeze_analysis_input(snapshot.value->records,
                                                              input.analysis_id,
                                                              *context.expected_profile,
                                                              plan.value->identities);
                if (!frozen_input.ok())
                    return {frozen_input.status, {}, frozen_input.error};
                const std::filesystem::path directory(input.output_directory);
                if (!directory.is_absolute() || !directory.has_filename() ||
                    std::filesystem::exists(directory) ||
                    !std::filesystem::is_directory(directory.parent_path()))
                    return bad<Value>(
                        ErrorCode::invalid_input,
                        "Export requires a new absolute directory in an existing parent");
                const auto frozen = std::make_shared<FrozenExport>(
                    FrozenExport{*plan.value,
                                 {context.caller.principal,
                                  *frozen_input.value,
                                  {{},
                                   {},
                                   directory,
                                   plan.value->root_resource,
                                   artifact_file_digests(*plan.value),
                                   {}},
                                  false}});
                {
                    std::lock_guard lock(state->mutex);
                    state->pending[pending_key(context.caller, context.idempotency_key)] = frozen;
                }
                TaskRequest request{
                    context.caller,
                    context.idempotency_key,
                    "model.export",
                    *signature.value,
                    {*context.document, *context.expected_revision, *context.expected_profile},
                    [state,
                     frozen](const TaskControl& control) -> std::shared_ptr<const TaskPayload> {
                        control.checkpoint();
                        const auto resources = state->files.stage(frozen->record.intent,
                                                                  frozen->plan.resources,
                                                                  [&] { control.checkpoint(); });
                        verify_nastran_readback(frozen->plan, resources, state->codec);
                        control.checkpoint();
                        return std::make_shared<const ArtifactPayload>(frozen);
                    }};
                return detail::converted(tasks().start(std::move(request)), detail::task_value);
            } catch (const std::exception& error) {
                return bad<Value>(ErrorCode::invalid_input, error.what());
            }
        });
    if (!result.ok())
        return result;
    result = registry.register_typed<ArtifactGetInput>(
        InputTraits<ArtifactGetInput>::definition(),
        [state](const OperationContext& context, const ArtifactGetInput& input) -> Result<Value> {
            try {
                const auto found = state->find(*context.document, input.artifact_id);
                if (!found.ok())
                    return {found.status, {}, found.error};
                if (found.value->first.principal != context.caller.principal)
                    return bad<Value>(ErrorCode::invalid_input,
                                      "Artifact belongs to a different principal");
                bool verified = false;
                if (found.value->first.published) {
                    const auto task = state->find_task(*context.document, found.value->first);
                    if (!task.ok())
                        return {task.status, {}, task.error};
                    (void)state->files.verify(found.value->first.intent);
                    verified = true;
                }
                return good(artifact_value(found.value->first, verified));
            } catch (const std::exception& error) {
                return bad<Value>(ErrorCode::storage_failure, error.what());
            }
        });
    if (!result.ok())
        return result;
    result = registry.register_typed<ArtifactReconcileInput>(
        InputTraits<ArtifactReconcileInput>::definition(),
        [state, &app, tasks](const OperationContext& context,
                             const ArtifactReconcileInput& input) -> Result<Value> {
            try {
                const auto signature = record_wire::strings(
                    std::array<std::string, 5>{context.document->id.value,
                                               context.document->epoch.value,
                                               std::to_string(*context.expected_revision),
                                               input.artifact_id,
                                               context.caller.principal});
                const auto fact_id =
                    "reconcile-" + artifact_sha256(record_wire::strings(std::array<std::string, 2>{
                                       context.caller.principal, context.idempotency_key}));
                const auto facts =
                    app.owned_rows(*context.document, StoreSpace::artifact_record, reconcile_owner);
                if (!facts.ok())
                    return {facts.status, {}, facts.error};
                const auto response = [&](const ArtifactRecord& record, bool replayed) {
                    bool currently_verified = false;
                    try {
                        (void)state->files.verify(record.intent);
                        currently_verified = true;
                    } catch (const std::exception&) {
                    }
                    auto value = artifact_value(record, currently_verified);
                    auto& fields = std::get<Value::Object>(value.data);
                    fields.emplace("replayed", Value(replayed));
                    fields.emplace("verified_at_reconcile", Value(true));
                    fields.emplace("current_files_verified", Value(currently_verified));
                    return good(std::move(value));
                };
                for (const auto& fact : *facts.value)
                    if (fact->key.identity == fact_id) {
                        const auto saved = record_wire::read_strings(*fact->payload);
                        if (saved[3] != signature)
                            return bad<Value>(
                                ErrorCode::idempotency_key_conflict,
                                "Reconcile key belongs to different request semantics");
                        const auto record = state->find(*context.document, saved[4]);
                        if (!record.ok())
                            return {record.status, {}, record.error};
                        if (record.value->first.principal != context.caller.principal)
                            return bad<Value>(ErrorCode::invalid_input,
                                              "Artifact belongs to a different principal");
                        if (!record.value->first.published)
                            return bad<Value>(ErrorCode::schema_unsupported,
                                              "Reconcile fact has no completed publication");
                        const auto task = state->find_task(*context.document, record.value->first);
                        if (!task.ok())
                            return {task.status, {}, task.error};
                        return response(record.value->first, true);
                    }
                const auto snapshot = app.snapshot(*context.document);
                if (!snapshot.ok())
                    return {snapshot.status, {}, snapshot.error};
                if (snapshot.value->info.revision != *context.expected_revision)
                    return bad<Value>(ErrorCode::revision_conflict, "Reconcile revision is stale");
                if (tasks().active_workers() || tasks().queued_tasks())
                    return bad<Value>(ErrorCode::invalid_input,
                                      "Artifact reconcile requires quiescent tasks");
                const auto found = state->find(*context.document, input.artifact_id);
                if (!found.ok())
                    return {found.status, {}, found.error};
                auto record = found.value->first;
                if (record.principal != context.caller.principal)
                    return bad<Value>(ErrorCode::invalid_input,
                                      "Artifact belongs to a different principal");
                (void)state->files.verify(record.intent);
                const auto linked = state->find_task(*context.document, record);
                if (!linked.ok())
                    return {linked.status, {}, linked.error};
                const auto& image = linked.value->second;
                auto task = linked.value->first;
                if (task.state != TaskState::succeeded && task.state != TaskState::interrupted &&
                    task.state != TaskState::outcome_unknown)
                    return bad<Value>(
                        ErrorCode::invalid_input,
                        "Failed or cancelled tasks cannot be reconciled as successful");
                record.published = true;
                const bool already_succeeded = task.state == TaskState::succeeded;
                if (!already_succeeded) {
                    task.state = TaskState::succeeded;
                    task.progress = 1;
                    task.diagnostic.reset();
                    task.artifact_receipt =
                        TaskArtifactReceipt{record.intent.artifact_id,
                                            record.frozen.version.revision,
                                            artifact_sha256(record.intent.manifest)};
                    const auto sequence = task.events.back().sequence;
                    if (sequence == UINT64_MAX || task.events.size() >= 128)
                        return bad<Value>(ErrorCode::resource_limit,
                                          "Reconcile task event limit exceeded");
                    task.events.push_back({sequence + 1, task.state, task.progress});
                }
                validate_artifact_task(record, task);
                const auto artifact = row(record), complete = task_row(task);
                const auto fact = std::make_shared<const OwnedRowImage>(
                    OwnedRowImage{{StoreSpace::artifact_record, fact_id},
                                  std::string(reconcile_owner),
                                  1,
                                  std::make_shared<const std::string>(record_wire::strings(
                                      std::array<std::string, 5>{"QCAE-ARTIFACT-RECONCILE-1",
                                                                 context.caller.principal,
                                                                 context.idempotency_key,
                                                                 signature,
                                                                 input.artifact_id})),
                                  {}});
                std::vector<OwnedRowUpdate> updates{{fact->key, {}, fact}};
                if (!found.value->first.published)
                    updates.push_back({artifact->key, found.value->second->payload, artifact});
                if (!already_succeeded)
                    updates.push_back({complete->key, image->payload, complete});
                const auto updated =
                    app.update_owned_rows(context.caller, *context.document, updates);
                if (!updated.ok())
                    return {updated.status, {}, updated.error};
                const auto reloaded = tasks().reconcile();
                if (!reloaded.ok())
                    return {reloaded.status, {}, reloaded.error};
                return response(record, false);
            } catch (const std::exception& error) {
                return bad<Value>(ErrorCode::storage_failure, error.what());
            }
        });
    if (!result.ok())
        return result;
    return registry.register_typed<NastranUiInput>(
        InputTraits<NastranUiInput>::definition(),
        [state](const OperationContext&, const NastranUiInput&) -> Result<Value> {
            const auto ui = nastran_ui_contribution(state->codec);
            Value::Array fields;
            for (const auto& field : ui.required_fields)
                fields.emplace_back(field);
            return good(Value(
                Value::Object{{"action_id", Value(ui.action_id)},
                              {"label", Value(ui.label)},
                              {"operation", Value(ui.operation)},
                              {"units", Value(ui.units)},
                              {"required_fields", Value(std::move(fields))},
                              {"profile",
                               Value(Value::Object{
                                   {"profile_id", Value(ui.profile.profile_id)},
                                   {"profile_version", Value(ui.profile.profile_version)},
                                   {"definition_digest", Value(ui.profile.definition_digest)}})}}));
        });
}
EngineContribution
nastran_engine_contribution(const std::shared_ptr<NastranArtifactCoordinator>& coordinator) {
    if (!coordinator)
        throw std::invalid_argument("Nastran package coordinator is required");
    return {
        "qcae.nastran",
        [coordinator](RecordRegistry& registry) {
            register_nastran_core_rules(registry, coordinator->codec().definition().reference);
        },
        [coordinator](OperationRegistry& registry,
                      RecordApplication& app,
                      std::function<TaskService&()> tasks) {
            const auto registered =
                coordinator->register_operations(registry, app, std::move(tasks));
            if (!registered.ok())
                return registered;
            return register_fixture_result_handlers(
                registry,
                app,
                [coordinator](
                    const Caller& caller, const DocumentRef& document, std::string_view id) {
                    return coordinator->frozen_input(caller, document, id);
                });
        },
        [] {
            return std::vector<OwnedRowHandler>{NastranArtifactCoordinator::row_handler(),
                                                NastranArtifactCoordinator::reconcile_row_handler(),
                                                features::results::result_row_handler()};
        },
        [coordinator](RecordApplication& app, std::function<bool(const ProfileRef&)> supported) {
            return coordinator->publisher(app, std::move(supported));
        }};
}
RenderContributions nastran_render_contributions() {
    RenderContributions result;
    result.add(
        {RecordTraits<records::Node>::type_id, "point", [](const Record& image) -> RenderItem {
             const auto& node = image->get<records::Node>();
             return RenderPoint{node.id, node.position, true};
         }});
    result.add({RecordTraits<records::GeometryLine>::type_id,
                "geometry_line",
                [](const Record& image) -> RenderItem {
                    const auto& line = image->get<records::GeometryLine>();
                    return RenderGeometryLine{EntityId(line.id.value), line.start, line.end};
                }});
    result.add(
        {RecordTraits<records::Beam>::type_id, "line2", [](const Record& image) -> RenderItem {
             const auto& beam = image->get<records::Beam>();
             return RenderLine2{beam.id, beam.nodes};
         }});
    result.freeze();
    return result;
}
} // namespace qcae::ipc
