#include "qcae/analysis_features.hpp"

namespace qcae::features::analysis {
using namespace operations;
namespace {
template <class Input> Result<bool> normalize_force_components(Input& input) {
    for (const auto& [component, name] :
         {std::pair{&input.x, "x"}, {&input.y, "y"}, {&input.z, "z"}}) {
        if (auto normalized = normalize(*component, parameters::Dimension::force, name);
            !normalized.ok())
            return normalized;
    }
    return success(true);
}
Value profile_value(const ProfileRef& value) {
    return Value(Value::Object{{"profile_id", Value(value.profile_id)},
                               {"profile_version", Value(value.profile_version)},
                               {"definition_digest", Value(value.definition_digest)}});
}
} // namespace

Result<OperationPlan> prepare_create_force(const ForceCreateInput& input) {
    auto canonical = canonical_copy(input);
    if (auto normalized = normalize_force_components(canonical); !normalized.ok())
        return forward_failure<OperationPlan>(normalized);
    return record_plan(std::move(canonical), [](const ForceCreateInput& value, PlanEdit& edit) {
        const auto id = edit.allocate();
        edit.put(
            records::NodalForce{id, value.node_id, {value.x.value, value.y.value, value.z.value}});
        return ChangeSummary{"Create nodal force", id, 0, true};
    });
}
Result<OperationPlan> prepare_set_force_vector(const ForceSetVectorInput& input) {
    auto canonical = canonical_copy(input);
    if (auto normalized = normalize_force_components(canonical); !normalized.ok())
        return forward_failure<OperationPlan>(normalized);
    return record_plan(std::move(canonical), [](const ForceSetVectorInput& value, PlanEdit& edit) {
        edit.update<records::NodalForce>(value.force_id, [&](auto& force) {
            force.force_n = {value.x.value, value.y.value, value.z.value};
        });
        return ChangeSummary{"Set nodal force vector", value.force_id, 0, false};
    });
}
Result<OperationPlan> prepare_create_constraint(const ConstraintCreateInput& input) {
    return record_plan(canonical_copy(input),
                       [](const ConstraintCreateInput& value, PlanEdit& edit) {
                           const auto id = edit.allocate();
                           edit.put(records::Constraint{id, value.node_ids, value.dofs});
                           return ChangeSummary{"Create constraint", id, 0, true};
                       });
}
Result<OperationPlan> prepare_create_load_case(const LoadCaseCreateInput& input) {
    return record_plan(canonical_copy(input), [](const LoadCaseCreateInput& value, PlanEdit& edit) {
        const auto id = edit.allocate();
        edit.put(records::LoadCase{id, value.name, value.force_ids, value.constraint_ids});
        return ChangeSummary{"Create load case", id, 0, true};
    });
}
Result<OperationPlan> prepare_set_references(const LoadCaseSetReferencesInput& input) {
    return record_plan(
        canonical_copy(input), [](const LoadCaseSetReferencesInput& value, PlanEdit& edit) {
            edit.update<records::LoadCase>(value.load_case_id, [&](auto& load_case) {
                load_case.forces = value.force_ids;
                load_case.constraints = value.constraint_ids;
            });
            return ChangeSummary{"Set load case references", value.load_case_id, 0, false};
        });
}
Result<OperationPlan> prepare_create_analysis(const AnalysisCreateInput& input,
                                              const ProfileRef& profile) {
    // The bound profile is part of the retry signature, not only the input.
    auto signature =
        std::get<Value::Object>(InputTraits<AnalysisCreateInput>::to_value(input).data);
    signature.emplace("profile", profile_value(profile));
    return record_plan(
        canonical_copy(input),
        Value(std::move(signature)),
        [profile](const AnalysisCreateInput& value, PlanEdit& edit) {
            const auto id = edit.allocate();
            edit.put(records::AnalysisDefinition{
                id, value.name, {profile, "linear_static"}, {}, {}, value.load_case_ids});
            return ChangeSummary{"Create linear static analysis", id, 0, true};
        });
}

Result<bool> register_handlers(OperationRegistry& registry, RecordApplication& app) {
    auto registered = register_plan(registry, app, prepare_create_force);
    if (registered.ok())
        registered = register_plan(registry, app, prepare_set_force_vector);
    if (registered.ok())
        registered = registry.register_typed<ForcePreviewVectorInput>(
            InputTraits<ForcePreviewVectorInput>::definition(),
            [&app](const OperationContext& context,
                   const ForcePreviewVectorInput& input) -> Result<Value> {
                const auto plan =
                    prepare_set_force_vector({input.force_id, input.x, input.y, input.z});
                const auto preview = preview_plan(app, context, plan);
                if (!preview.ok())
                    return forward_failure<Value>(preview);
                const auto revision = std::to_string(preview.value->context.expected_revision);
                return success(Value(Value::Object{
                    {"preview_id", Value(preview.value->id.value)},
                    {"affected_entity_id", Value(preview.value->affected_entity.value)},
                    {"creates_entity", Value(false)},
                    {"revision", Value(revision)}}));
            });
    if (registered.ok())
        registered = register_plan(registry, app, prepare_create_constraint);
    if (registered.ok())
        registered = register_plan(registry, app, prepare_create_load_case);
    if (registered.ok())
        registered = register_plan(registry, app, prepare_set_references);
    if (registered.ok())
        registered = registry.register_typed<AnalysisCreateInput>(
            InputTraits<AnalysisCreateInput>::definition(),
            [&app](const OperationContext& context, const AnalysisCreateInput& input) {
                return execute_plan<AnalysisCreateInput>(
                    app, context, prepare_create_analysis(input, *context.expected_profile));
            });
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
    if (registered.ok())
        registered = registry.register_typed<AnalysisCheckInput>(
            InputTraits<AnalysisCheckInput>::definition(),
            [checks, response](const OperationContext& context, const AnalysisCheckInput& input) {
                return response(context,
                                checks->run(context.caller,
                                            {*context.document, *context.expected_revision},
                                            input.analysis_id,
                                            context.idempotency_key));
            });
    if (registered.ok())
        registered = registry.register_typed<AnalysisGetIssuesInput>(
            InputTraits<AnalysisGetIssuesInput>::definition(),
            [checks, response](const OperationContext& context,
                               const AnalysisGetIssuesInput& input) {
                return response(context,
                                checks->get(context.caller, *context.document, input.check_id));
            });
    return registered;
}
} // namespace qcae::features::analysis
