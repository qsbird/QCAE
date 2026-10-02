#include "qcae/engine_contributions.hpp"
#include "qcae/json_ledger.hpp"
#include "qcae/analysis_features.hpp"
#include "qcae/records.hpp"
#include "qcae/geometry_features.hpp"
#include "qcae/line_mesh_task.hpp"
#include "qcae/material_operations.hpp"
#include "qcae/mesh_editing_operations.hpp"
#include "qcae/nastran_contribution.hpp"
#ifdef QCAE_HAS_SOLVER_LOCAL
#include "qcae/solver_contribution.hpp"
#endif
#include "typed_values.hpp"
#include <algorithm>
#include <iterator>
#include <set>

namespace qcae::ipc {
using namespace operations;
using namespace detail;
TypedHost::OperationContributor TypedHost::default_operations() {
    return [](OperationRegistry& registry,
              RecordApplication& app,
              std::function<TaskService&()> task_service) -> Result<bool> {
        const auto materials = features::materials::register_handlers(registry, app);
        if (!materials.ok())
            return materials;
        const auto mesh_editing = features::mesh_editing::register_handlers(registry, app);
        if (!mesh_editing.ok())
            return mesh_editing;
        const auto analysis = features::analysis::register_handlers(registry, app);
        if (!analysis.ok())
            return analysis;
        const auto geometry = registry.register_typed<GeometryCreateLineInput>(
            InputTraits<GeometryCreateLineInput>::definition(),
            [&app](const OperationContext& ctx, const GeometryCreateLineInput& input) {
                const LineGeometryInput line{input.start_mm, input.end_mm};
                return converted(app.execute(ctx.caller,
                                             {*ctx.document, *ctx.expected_revision},
                                             "geometry.create_line",
                                             line_geometry_signature(line),
                                             create_line_handler(line),
                                             ctx.idempotency_key),
                                 change_receipt_value);
            });
        if (!geometry.ok())
            return geometry;
        const auto evaluate = registry.register_typed<GeometryEvaluateLineInput>(
            InputTraits<GeometryEvaluateLineInput>::definition(),
            [&app](const OperationContext& ctx,
                   const GeometryEvaluateLineInput& input) -> Result<Value> {
                const auto snapshot = app.snapshot(*ctx.document);
                if (!snapshot.ok())
                    return {snapshot.status, {}, snapshot.error};
                try {
                    const auto position =
                        evaluate_line(snapshot.value->records,
                                      records::GeometryId(input.geometry_id.value),
                                      input.u);
                    return {
                        Status::success,
                        Value(Value::Object{
                            {"geometry_id", Value(input.geometry_id.value)},
                            {"position_mm",
                             Value(Value::Array{
                                 Value(position[0]), Value(position[1]), Value(position[2])})},
                            {"revision", Value(std::to_string(snapshot.value->info.revision))}}),
                        {}};
                } catch (const RecordError& error) {
                    return {
                        Status::failed, {}, Diagnostic{error.code(), error.what(), error.field()}};
                }
            });
        if (!evaluate.ok())
            return evaluate;
        const auto endpoint = registry.register_typed<GeometryMoveEndpointInput>(
            InputTraits<GeometryMoveEndpointInput>::definition(),
            [&app](const OperationContext& ctx, const GeometryMoveEndpointInput& input) {
                const records::GeometryId geometry(input.geometry_id.value);
                return converted(app.execute(ctx.caller,
                                             {*ctx.document, *ctx.expected_revision},
                                             "geometry.move_endpoint",
                                             line_endpoint_signature(geometry, input.end_mm),
                                             move_line_endpoint_handler(geometry, input.end_mm),
                                             ctx.idempotency_key),
                                 change_receipt_value);
            });
        if (!endpoint.ok())
            return endpoint;
        const auto regenerate = registry.register_typed<MeshRegenerateLineInput>(
            InputTraits<MeshRegenerateLineInput>::definition(),
            [&app, task_service](const OperationContext& ctx,
                                 const MeshRegenerateLineInput& input) {
                const auto snapshot = app.snapshot(*ctx.document);
                if (!snapshot.ok())
                    return Result<Value>{snapshot.status, {}, snapshot.error};
                try {
                    auto request = regenerate_line_mesh_task(*snapshot.value,
                                                             ctx.caller,
                                                             records::MeshId(input.mesh_id.value),
                                                             input.segments,
                                                             input.replacement_policy,
                                                             ctx.idempotency_key);
                    request.input.revision = *ctx.expected_revision;
                    return converted(task_service().start(std::move(request)), task_value);
                } catch (const RecordError& error) {
                    return Result<Value>{
                        Status::failed, {}, Diagnostic{error.code(), error.what(), error.field()}};
                }
            });
        if (!regenerate.ok())
            return regenerate;
        return registry.register_typed<MeshGenerateLineInput>(
            InputTraits<MeshGenerateLineInput>::definition(),
            [&app, task_service = std::move(task_service)](
                const OperationContext& ctx, const MeshGenerateLineInput& input) -> Result<Value> {
                const auto snapshot = app.snapshot(*ctx.document);
                if (!snapshot.ok())
                    return {snapshot.status, {}, snapshot.error};
                try {
                    auto request = line_mesh_task(
                        *snapshot.value,
                        ctx.caller,
                        {},
                        {records::GeometryId(input.geometry_id.value), input.segments, {}},
                        ctx.idempotency_key);
                    // Deduplication compares the original submitted context before freshness
                    // checks.
                    request.input.revision = *ctx.expected_revision;
                    return converted(task_service().start(std::move(request)), task_value);
                } catch (const RecordError& error) {
                    return {
                        Status::failed, {}, Diagnostic{error.code(), error.what(), error.field()}};
                }
            });
    };
}

namespace {
EngineContribution disabled_solver_contribution() {
    return {"qcae.solver.disabled",
            {},
            [](OperationRegistry& registry,
               RecordApplication&,
               std::function<TaskService&()>) -> Result<bool> {
                for (auto definition : {InputTraits<AnalysisStartInput>::definition(),
                                        InputTraits<AnalysisGetRunInput>::definition(),
                                        InputTraits<AnalysisGetResultInput>::definition(),
                                        InputTraits<AnalysisReconcileResultInput>::definition(),
                                        InputTraits<AnalysisValidateResultInput>::definition(),
                                        InputTraits<SolverConfigurationInput>::definition()}) {
                    const auto declared = registry.declare_unavailable(
                        std::move(definition),
                        "Local solver support or the Nastran package is disabled");
                    if (!declared.ok())
                        return declared;
                }
                return {Status::success, true, {}};
            },
            {}};
}
std::vector<EngineContribution> default_contributions(const LocalSolverConfiguration* config) {
    std::vector<EngineContribution> result{
        {"qcae.model",
         [](RecordRegistry& registry) {
             for (auto descriptor : generated_record_descriptors())
                 registry.add(std::move(descriptor));
             registry.add_rule(records::validate_relations);
         },
         TypedHost::default_operations(),
         [] { return std::vector<OwnedRowHandler>{features::analysis::check_row_handler()}; }}};
    result.front().core_rule_ids = {"qcae.model.relations"};
    if (nastran_package_enabled()) {
        const auto nastran = std::make_shared<NastranArtifactCoordinator>();
        auto contribution = nastran_engine_contribution(nastran);
#ifdef QCAE_HAS_SOLVER_LOCAL
        const auto solver = std::make_shared<SolverCoordinator>(
            config ? std::optional<LocalSolverConfiguration>(*config) : std::nullopt,
            [nastran](const Caller& caller, const DocumentRef& document, std::string_view id) {
                return nastran->verified_artifact(caller, document, id);
            });
        const auto base = std::move(contribution.publisher_factory);
        contribution.publisher_factory =
            [base, solver](RecordApplication& app,
                           std::function<bool(const ProfileRef&)> supported) {
                return solver->wrap_publisher(app, base(app, std::move(supported)));
            };
        result.push_back(std::move(contribution));
        result.push_back(solver_engine_contribution(solver));
#else
        if (config)
            throw RecordError(
                ErrorCode::unsupported_capability, "Local solver support is disabled", "solver");
        result.push_back(std::move(contribution));
        result.push_back(disabled_solver_contribution());
#endif
    } else if (config) {
        throw RecordError(
            ErrorCode::unsupported_capability, "The Nastran package is disabled", "solver");
    } else {
        result.push_back(disabled_solver_contribution());
    }
    return result;
}
} // namespace
std::vector<EngineContribution> default_engine_contributions() {
    return default_contributions(nullptr);
}
std::vector<EngineContribution>
default_engine_contributions(const LocalSolverConfiguration& config) {
    return default_contributions(&config);
}
bool nastran_package_enabled() noexcept {
    return QCAE_NASTRAN_PACKAGE_ENABLED != 0;
}
const IModelCodec* EngineAssembly::model_codec() const noexcept {
    return profile_codec ? profile_codec->codec.get() : nullptr;
}
const ProfileDefinition* EngineAssembly::profile_definition() const noexcept {
    return profile_codec && profile_codec->profile ? &profile_codec->profile->definition()
                                                   : nullptr;
}
const std::vector<EngineContributionRegistration>& EngineContributionCatalog::entries() const {
    if (!operations_registered_)
        throw std::logic_error("Engine contribution operations have not finished registering");
    return entries_;
}
QJsonArray EngineContributionCatalog::describe() const {
    QJsonArray result;
    for (const auto& entry : entries()) {
        QJsonArray core, registered_operations, codecs, validation, ui, render;
        for (const auto type : entry.record_types) {
            const auto* descriptor = records_->find(type);
            core.append(QJsonObject{{"kind", "record"},
                                    {"record_type", static_cast<qint64>(type.value)},
                                    {"name", transport::json_ledger::from_utf8(descriptor->name)},
                                    {"version", static_cast<qint64>(descriptor->current_version)}});
        }
        for (const auto& rule : entry.core_rules)
            core.append(
                QJsonObject{{"kind", "rule"}, {"id", transport::json_ledger::from_utf8(rule.id)}});
        for (const auto& operation : entry.operations) {
            const auto& definition = operation.definition;
            registered_operations.append(
                QJsonObject{{"name", transport::json_ledger::from_utf8(definition.operation_id)},
                            {"version", static_cast<qint64>(definition.version)},
                            {"schema_id", transport::json_ledger::from_utf8(definition.schema_id)},
                            {"available", operation.available}});
        }
        if (entry.profile_codec) {
            const auto& profile = entry.profile_codec->profile->definition().reference;
            codecs.append(QJsonObject{
                {"profile_ref",
                 QJsonObject{{"profile_id", transport::json_ledger::from_utf8(profile.profile_id)},
                             {"profile_version",
                              transport::json_ledger::from_utf8(profile.profile_version)},
                             {"definition_digest",
                              transport::json_ledger::from_utf8(profile.definition_digest)}}}});
        }
        for (const auto& validator : entry.validators)
            validation.append(QJsonObject{{"id", transport::json_ledger::from_utf8(validator.id)},
                                          {"version", static_cast<qint64>(validator.version)}});
        for (const auto& operation_id : entry.ui_operations) {
            const auto operation = std::find_if(
                entry.operations.begin(), entry.operations.end(), [&](const auto& item) {
                    return item.definition.operation_id == operation_id;
                });
            // Registration validated this reference against its actual available handler.
            ui.append(QJsonObject{{"operation", transport::json_ledger::from_utf8(operation_id)},
                                  {"version", static_cast<qint64>(operation->definition.version)}});
        }
        for (const auto& projection : entry.render)
            render.append(
                QJsonObject{{"record_type", static_cast<qint64>(projection.record_type.value)},
                            {"topology", transport::json_ledger::from_utf8(projection.topology)}});
        result.append(QJsonObject{{"contribution_id", transport::json_ledger::from_utf8(entry.id)},
                                  {"core", core},
                                  {"operations", registered_operations},
                                  {"codecs", codecs},
                                  {"validation", validation},
                                  {"ui", ui},
                                  {"render", render}});
    }
    return result;
}
EngineAssembly assemble_engine(std::span<const EngineContribution> contributions) {
    auto registry = std::make_shared<RecordRegistry>();
    std::set<std::string> identities;
    struct Registration {
        std::size_t index;
        TypedHost::OperationContributor operations;
    };
    auto catalog = std::make_shared<EngineContributionCatalog>();
    catalog->records_ = registry;
    std::vector<Registration> contributors;
    std::set<std::string> rule_ids, validator_ids;
    std::vector<OwnedRowHandler> owned_rows;
    TypedHost::TaskPublisherFactory publisher_factory;
    std::optional<EngineCodecBinding> profile_codec;
    std::string profile_codec_owner;
    std::optional<RenderContributions> render;
    std::string render_owner;
    for (const auto& contribution : contributions) {
        if (contribution.id.empty() || !identities.insert(contribution.id).second ||
            (!contribution.records && !contribution.operations && !contribution.profile_codec &&
             !contribution.render_factory && contribution.validators.empty()))
            throw RecordError(
                ErrorCode::invalid_input,
                "Engine contribution identity must be unique and nonempty, with a registration",
                "contribution");
        EngineContributionRegistration registration;
        registration.id = contribution.id;
        registration.ui_operations = contribution.ui_operations;
        for (const auto& validator : contribution.validators) {
            if (validator.id.empty() || validator.version == 0 || !validator.validate_export ||
                !validator_ids.insert(validator.id).second)
                throw RecordError(ErrorCode::invalid_input,
                                  "Engine validator identity, version and callable must be valid",
                                  "contribution");
            registration.validators.push_back(validator);
        }
        if (contribution.profile_codec) {
            const auto& binding = *contribution.profile_codec;
            if (profile_codec)
                throw RecordError(ErrorCode::invalid_input,
                                  "Only one profile/codec binding may own the engine",
                                  "contribution");
            if (!binding.codec || !binding.profile || binding.codec.use_count() == 0 ||
                binding.profile.use_count() == 0 || binding.codec.owner_before(binding.profile) ||
                binding.profile.owner_before(binding.codec))
                throw RecordError(ErrorCode::invalid_input,
                                  "Profile and codec ports must share an owned lifetime",
                                  "contribution");
            const auto& definition = binding.profile->definition();
            if (definition.reference.profile_id.empty() ||
                definition.reference.profile_version.empty() ||
                definition.reference.definition_digest.empty() ||
                definition.solver_family.empty() || definition.analysis_kind.empty())
                throw RecordError(ErrorCode::invalid_input,
                                  "Profile/codec binding requires a complete definition",
                                  "contribution");
            profile_codec = binding;
            profile_codec_owner = contribution.id;
            registration.profile_codec = binding;
        }
        if (contribution.render_factory) {
            if (render)
                throw RecordError(ErrorCode::invalid_input,
                                  "Only one render factory may own the engine",
                                  "contribution");
            auto selected = contribution.render_factory();
            if (!selected.frozen() || selected.entries().empty())
                throw RecordError(ErrorCode::invalid_input,
                                  "Engine render contributions must be frozen and nonempty",
                                  "contribution");
            for (const auto& [type, projection] : selected.entries()) {
                (void)type;
                registration.render.push_back(projection);
            }
            render = std::move(selected);
            render_owner = contribution.id;
        }
        const auto previous_types = registry->types();
        const auto previous_rules = registry->rules().size();
        if (contribution.records)
            contribution.records(*registry);
        for (const auto type : registry->types())
            if (std::find(previous_types.begin(), previous_types.end(), type) ==
                previous_types.end())
                registration.record_types.push_back(type);
        if (!contribution.core_rule_ids.empty()) {
            if (contribution.core_rule_ids.size() != registry->rules().size() - previous_rules)
                throw RecordError(ErrorCode::invalid_input,
                                  "Named core rules must match the actual registered rules",
                                  "contribution");
            for (std::size_t index = 0; index < contribution.core_rule_ids.size(); ++index) {
                const auto& id = contribution.core_rule_ids[index];
                if (id.empty() || !rule_ids.insert(id).second)
                    throw RecordError(ErrorCode::invalid_input,
                                      "Core rule identities must be unique and nonempty",
                                      "contribution");
                registration.core_rules.push_back({id, previous_rules + index});
            }
        }
        if (!contribution.operations && !contribution.ui_operations.empty())
            throw RecordError(ErrorCode::invalid_input,
                              "UI contributions must reference their own registered operations",
                              "contribution");
        if (contribution.operations)
            contributors.push_back({catalog->entries_.size(), contribution.operations});
        catalog->entries_.push_back(std::move(registration));
        if (contribution.owned_rows) {
            auto rows = contribution.owned_rows();
            owned_rows.insert(owned_rows.end(),
                              std::make_move_iterator(rows.begin()),
                              std::make_move_iterator(rows.end()));
        }
        if (contribution.publisher_factory) {
            if (publisher_factory)
                throw RecordError(ErrorCode::invalid_input,
                                  "Only one task publisher factory may own the engine",
                                  "contribution");
            publisher_factory = contribution.publisher_factory;
        }
    }
    registry->freeze();
    return {std::move(registry),
            [contributors = std::move(contributors), catalog](operations::OperationRegistry& target,
                                                              RecordApplication& app,
                                                              std::function<TaskService&()> tasks) {
                catalog->operations_registered_ = false;
                std::vector<std::vector<OperationDescriptor>> registered(catalog->entries_.size());
                for (const auto& contribute : contributors) {
                    std::set<std::string> previous;
                    for (const auto& descriptor : target.descriptors())
                        previous.insert(descriptor.definition.operation_id);
                    auto result = contribute.operations(target, app, tasks);
                    if (!result.ok())
                        return result;
                    auto& installed = registered[contribute.index];
                    for (const auto& descriptor : target.descriptors())
                        if (!previous.contains(descriptor.definition.operation_id))
                            installed.push_back(descriptor);
                    std::set<std::string> ui_ids;
                    for (const auto& id : catalog->entries_[contribute.index].ui_operations) {
                        const auto handler = std::find_if(
                            installed.begin(), installed.end(), [&](const auto& descriptor) {
                                return descriptor.definition.operation_id == id;
                            });
                        if (!ui_ids.insert(id).second || handler == installed.end() ||
                            !handler->available ||
                            handler->definition.effect != OperationEffect::read_only)
                            throw RecordError(
                                ErrorCode::invalid_input,
                                "UI reference must name its own available read-only operation",
                                "contribution");
                    }
                }
                for (std::size_t index = 0; index < registered.size(); ++index)
                    catalog->entries_[index].operations = std::move(registered[index]);
                catalog->operations_registered_ = true;
                return Result<bool>{Status::success, true, {}};
            },
            std::move(owned_rows),
            std::move(publisher_factory),
            std::move(profile_codec),
            std::move(profile_codec_owner),
            render ? std::move(*render) : default_render_contributions(),
            std::move(render_owner),
            std::move(catalog)};
}
} // namespace qcae::ipc
