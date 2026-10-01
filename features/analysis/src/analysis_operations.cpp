#include "qcae/analysis_features.hpp"
#include "qcae/quantities.hpp"

namespace qcae::features::analysis {
namespace {
using namespace operations;
template <class T> Result<T> success(T value) {
    return {Status::success, std::move(value), {}};
}
template <class T> Result<T> failed(const RecordError& error) {
    return {Status::failed, {}, Diagnostic{error.code(), error.what(), error.field()}};
}
using Edit = std::function<EntityId(EditSession&, const RecordIdentityAllocator&)>;
template <class Input> Result<Input> normalized_force_components(Input input) {
    for (const auto& [component, name] :
         {std::pair{&input.x, "x"}, {&input.y, "y"}, {&input.z, "z"}}) {
        const auto value = parameters::canonical_quantity(*component, parameters::Dimension::force);
        if (!value.ok()) {
            auto error = value.error;
            if (error)
                error->field = std::string(name) + "." + error->field;
            return {value.status, {}, std::move(error)};
        }
        *component = *value.value;
    }
    return success(std::move(input));
}
Result<OperationPlan> plan(const Value& input, std::string label, bool creates, Edit action) {
    const auto signature = canonical_value(input);
    if (!signature.ok())
        return {signature.status, {}, signature.error};
    RecordPrepare prepare = [signature = *signature.value,
                             label = std::move(label),
                             creates,
                             action = std::move(action)](const DocumentView& view,
                                                         const RecordIdentityAllocator& allocate) {
        try {
            EditSession edit(view);
            const auto identity = action(edit, allocate);
            return success(
                RecordPreparedOperation{edit.prepare(), label, identity, signature, 0, creates});
        } catch (const RecordError& error) {
            return failed<RecordPreparedOperation>(error);
        }
    };
    return success(OperationPlan{*signature.value, std::move(prepare)});
}
Value profile_value(const ProfileRef& value) {
    return Value(Value::Object{{"profile_id", Value(value.profile_id)},
                               {"profile_version", Value(value.profile_version)},
                               {"definition_digest", Value(value.definition_digest)}});
}
template <class Input, class Factory>
Result<Value> execute(RecordApplication& app,
                      const OperationContext& context,
                      const Input& input,
                      Factory factory) {
    const auto operation = factory(input);
    if (!operation.ok())
        return {operation.status, {}, operation.error};
    const auto receipt = app.execute(context.caller,
                                     {*context.document, *context.expected_revision},
                                     std::string(InputTraits<Input>::operation_id),
                                     operation.value->signature,
                                     operation.value->prepare,
                                     context.idempotency_key);
    if (!receipt.ok())
        return {receipt.status, {}, receipt.error};
    return success(change_receipt_value(*receipt.value));
}
} // namespace

Result<OperationPlan> prepare_create_force(const ForceCreateInput& input) {
    const auto normalized = normalized_force_components(input);
    if (!normalized.ok())
        return {normalized.status, {}, normalized.error};
    return plan(
        InputTraits<ForceCreateInput>::to_value(*normalized.value),
        "Create nodal force",
        true,
        [value = *normalized.value](EditSession& edit, const RecordIdentityAllocator& allocate) {
            const auto id = allocate();
            edit.put(records::NodalForce{
                id, value.node_id, {value.x.value, value.y.value, value.z.value}});
            return id;
        });
}
Result<OperationPlan> prepare_set_force_vector(const ForceSetVectorInput& input) {
    const auto normalized = normalized_force_components(input);
    if (!normalized.ok())
        return {normalized.status, {}, normalized.error};
    return plan(InputTraits<ForceSetVectorInput>::to_value(*normalized.value),
                "Set nodal force vector",
                false,
                [value = *normalized.value](EditSession& edit, const RecordIdentityAllocator&) {
                    edit.update<records::NodalForce>(value.force_id, [&](auto& force) {
                        force.force_n = {value.x.value, value.y.value, value.z.value};
                    });
                    return value.force_id;
                });
}
Result<OperationPlan> prepare_create_constraint(const ConstraintCreateInput& input) {
    return plan(InputTraits<ConstraintCreateInput>::to_value(input),
                "Create constraint",
                true,
                [input](EditSession& edit, const RecordIdentityAllocator& allocate) {
                    const auto id = allocate();
                    edit.put(records::Constraint{id, input.node_ids, input.dofs});
                    return id;
                });
}
Result<OperationPlan> prepare_create_load_case(const LoadCaseCreateInput& input) {
    return plan(InputTraits<LoadCaseCreateInput>::to_value(input),
                "Create load case",
                true,
                [input](EditSession& edit, const RecordIdentityAllocator& allocate) {
                    const auto id = allocate();
                    edit.put(
                        records::LoadCase{id, input.name, input.force_ids, input.constraint_ids});
                    return id;
                });
}
Result<OperationPlan> prepare_set_references(const LoadCaseSetReferencesInput& input) {
    return plan(InputTraits<LoadCaseSetReferencesInput>::to_value(input),
                "Set load case references",
                false,
                [input](EditSession& edit, const RecordIdentityAllocator&) {
                    edit.update<records::LoadCase>(input.load_case_id, [&](auto& value) {
                        value.forces = input.force_ids;
                        value.constraints = input.constraint_ids;
                    });
                    return input.load_case_id;
                });
}
Result<OperationPlan> prepare_create_analysis(const AnalysisCreateInput& input,
                                              const ProfileRef& profile) {
    auto signature =
        std::get<Value::Object>(InputTraits<AnalysisCreateInput>::to_value(input).data);
    signature.emplace("profile", profile_value(profile));
    return plan(Value(std::move(signature)),
                "Create linear static analysis",
                true,
                [input, profile](EditSession& edit, const RecordIdentityAllocator& allocate) {
                    const auto id = allocate();
                    edit.put(records::AnalysisDefinition{
                        id, input.name, {profile, "linear_static"}, {}, {}, input.load_case_ids});
                    return id;
                });
}

Result<bool> register_handlers(OperationRegistry& registry, RecordApplication& app) {
    auto result = registry.register_typed<ForceCreateInput>(
        InputTraits<ForceCreateInput>::definition(),
        [&app](const OperationContext& context, const ForceCreateInput& input) {
            return execute(app, context, input, prepare_create_force);
        });
    if (!result.ok())
        return result;
    result = registry.register_typed<ForceSetVectorInput>(
        InputTraits<ForceSetVectorInput>::definition(),
        [&app](const OperationContext& context, const ForceSetVectorInput& input) {
            return execute(app, context, input, prepare_set_force_vector);
        });
    if (!result.ok())
        return result;
    result = registry.register_typed<ForcePreviewVectorInput>(
        InputTraits<ForcePreviewVectorInput>::definition(),
        [&app](const OperationContext& context,
               const ForcePreviewVectorInput& input) -> Result<Value> {
            const auto operation =
                prepare_set_force_vector({input.force_id, input.x, input.y, input.z});
            if (!operation.ok())
                return {operation.status, {}, operation.error};
            const auto preview = app.preview(context.caller,
                                             {*context.document, *context.expected_revision},
                                             operation.value->prepare);
            if (!preview.ok())
                return {preview.status, {}, preview.error};
            return success(Value(Value::Object{
                {"preview_id", Value(preview.value->id.value)},
                {"affected_entity_id", Value(preview.value->affected_entity.value)},
                {"creates_entity", Value(false)},
                {"revision", Value(std::to_string(preview.value->context.expected_revision))}}));
        });
    if (!result.ok())
        return result;
    result = registry.register_typed<ConstraintCreateInput>(
        InputTraits<ConstraintCreateInput>::definition(),
        [&app](const OperationContext& context, const ConstraintCreateInput& input) {
            return execute(app, context, input, prepare_create_constraint);
        });
    if (!result.ok())
        return result;
    result = registry.register_typed<LoadCaseCreateInput>(
        InputTraits<LoadCaseCreateInput>::definition(),
        [&app](const OperationContext& context, const LoadCaseCreateInput& input) {
            return execute(app, context, input, prepare_create_load_case);
        });
    if (!result.ok())
        return result;
    result = registry.register_typed<LoadCaseSetReferencesInput>(
        InputTraits<LoadCaseSetReferencesInput>::definition(),
        [&app](const OperationContext& context, const LoadCaseSetReferencesInput& input) {
            return execute(app, context, input, prepare_set_references);
        });
    if (!result.ok())
        return result;
    result = registry.register_typed<AnalysisCreateInput>(
        InputTraits<AnalysisCreateInput>::definition(),
        [&app](const OperationContext& context, const AnalysisCreateInput& input) {
            return execute(app, context, input, [&](const auto& value) {
                return prepare_create_analysis(value, *context.expected_profile);
            });
        });
    if (!result.ok())
        return result;
    const auto checks = std::make_shared<CheckService>(app);
    auto response = [&app](const OperationContext& context,
                           const Result<CheckReport>& report) -> Result<Value> {
        if (!report.value)
            return {report.status, {}, report.error};
        const auto current = app.snapshot(*context.document);
        if (!current.ok())
            return {current.status, {}, current.error};
        return {
            report.status, check_report_value(*report.value, current.value->records), report.error};
    };
    result = registry.register_typed<AnalysisCheckInput>(
        InputTraits<AnalysisCheckInput>::definition(),
        [checks, response](const OperationContext& context, const AnalysisCheckInput& input) {
            return response(context,
                            checks->run(context.caller,
                                        {*context.document, *context.expected_revision},
                                        input.analysis_id,
                                        context.idempotency_key));
        });
    if (!result.ok())
        return result;
    return registry.register_typed<AnalysisGetIssuesInput>(
        InputTraits<AnalysisGetIssuesInput>::definition(),
        [checks, response](const OperationContext& context, const AnalysisGetIssuesInput& input) {
            return response(context,
                            checks->get(context.caller, *context.document, input.check_id));
        });
}
} // namespace qcae::features::analysis
