#include "qcae/material_operations.hpp"
#include "qcae/quantities.hpp"
#include "qcae/records.hpp"

namespace qcae::features::materials {
namespace {
using namespace operations;

template <class T> Result<T> success(T value) {
    return {Status::success, std::move(value), std::nullopt};
}

template <class T> Result<T> failed(const RecordError& error) {
    return {Status::failed, std::nullopt, Diagnostic{error.code(), error.what(), error.field()}};
}

Result<Quantity>
normalized(const Quantity& input, parameters::Dimension dimension, std::string_view field) {
    auto value = parameters::canonical_quantity(input, dimension);
    if (value.error) {
        value.error->field = std::string(field) + "." + value.error->field;
    }
    return value;
}

template <class Input, class Factory>
Result<Value> execute(RecordApplication& app,
                      const OperationContext& context,
                      const Input& input,
                      Factory factory) {
    const auto plan = factory(input);
    if (!plan.ok()) {
        return {plan.status, std::nullopt, plan.error};
    }
    // Presence is validated by registry metadata; actual identity/revision/authorization stays in
    // app.
    const auto receipt = app.execute(context.caller,
                                     WriteContext{*context.document, *context.expected_revision},
                                     std::string(InputTraits<Input>::operation_id),
                                     plan.value->signature,
                                     plan.value->prepare,
                                     context.idempotency_key);
    if (!receipt.ok()) {
        return {receipt.status, std::nullopt, receipt.error};
    }
    return success(change_receipt_value(*receipt.value));
}
} // namespace

Result<OperationPlan> prepare_create_material(const operations::MaterialCreateInput& input) {
    const auto young =
        normalized(input.young_modulus, parameters::Dimension::pressure, "young_modulus");
    if (!young.ok()) {
        return {young.status, std::nullopt, young.error};
    }
    auto canonical = input;
    canonical.young_modulus = *young.value;
    try {
        // Reuse authoritative generated field rules; this probe is never published or allocated.
        RecordTraits<records::Material>::validate_fields({EntityId{"material-field-check"},
                                                          canonical.name,
                                                          young.value->value,
                                                          canonical.poisson_ratio});
    } catch (const RecordError& error) {
        return failed<OperationPlan>(error);
    }
    const auto signature = canonical_value(InputTraits<MaterialCreateInput>::to_value(canonical));
    if (!signature.ok()) {
        return {signature.status, std::nullopt, signature.error};
    }
    RecordPrepare prepare = [canonical, signature = *signature.value](
                                const DocumentView& view, const RecordIdentityAllocator& allocate) {
        try {
            const auto identity = allocate();
            EditSession edit(view);
            edit.put(records::Material{
                identity, canonical.name, canonical.young_modulus.value, canonical.poisson_ratio});
            return success(RecordPreparedOperation{edit.prepare(),
                                                   "Create material",
                                                   identity,
                                                   signature,
                                                   canonical.young_modulus.value,
                                                   true});
        } catch (const RecordError& error) {
            return failed<RecordPreparedOperation>(error);
        }
    };
    return success(OperationPlan{*signature.value, std::move(prepare)});
}

Result<OperationPlan>
prepare_set_young_modulus(const operations::MaterialSetYoungModulusInput& input) {
    const auto young =
        normalized(input.young_modulus, parameters::Dimension::pressure, "young_modulus");
    if (!young.ok()) {
        return {young.status, std::nullopt, young.error};
    }
    auto canonical = input;
    canonical.young_modulus = *young.value;
    const auto signature =
        canonical_value(InputTraits<MaterialSetYoungModulusInput>::to_value(canonical));
    if (!signature.ok()) {
        return {signature.status, std::nullopt, signature.error};
    }
    RecordPrepare prepare = [canonical, signature = *signature.value](
                                const DocumentView& view, const RecordIdentityAllocator&) {
        try {
            EditSession edit(view);
            edit.update<records::Material>(canonical.entity_id, [&](auto& material) {
                material.young_modulus_mpa = canonical.young_modulus.value;
            });
            return success(RecordPreparedOperation{edit.prepare(),
                                                   "Set Young modulus",
                                                   canonical.entity_id,
                                                   signature,
                                                   canonical.young_modulus.value,
                                                   false});
        } catch (const RecordError& error) {
            return failed<RecordPreparedOperation>(error);
        }
    };
    return success(OperationPlan{*signature.value, std::move(prepare)});
}

Result<OperationPlan> prepare_create_section(const operations::SectionCreateInput& input) {
    auto canonical = input;
    const auto area = normalized({input.area_mm2, "mm2"}, parameters::Dimension::area, "area_mm2");
    const auto i1 = normalized({input.i1_mm4, "mm4"}, parameters::Dimension::inertia, "i1_mm4");
    const auto i2 = normalized({input.i2_mm4, "mm4"}, parameters::Dimension::inertia, "i2_mm4");
    const auto torsion =
        normalized({input.torsion_mm4, "mm4"}, parameters::Dimension::inertia, "torsion_mm4");
    for (const auto* field : {&area, &i1, &i2, &torsion}) {
        if (!field->ok()) {
            return {field->status, std::nullopt, field->error};
        }
    }
    canonical.area_mm2 = area.value->value;
    canonical.i1_mm4 = i1.value->value;
    canonical.i2_mm4 = i2.value->value;
    canonical.torsion_mm4 = torsion.value->value;
    try {
        RecordTraits<records::BeamSection>::validate_fields({EntityId{"section-field-check"},
                                                             canonical.name,
                                                             canonical.material_id,
                                                             canonical.area_mm2,
                                                             canonical.i1_mm4,
                                                             canonical.i2_mm4,
                                                             canonical.torsion_mm4});
    } catch (const RecordError& error) {
        return failed<OperationPlan>(error);
    }
    const auto signature = canonical_value(InputTraits<SectionCreateInput>::to_value(canonical));
    if (!signature.ok()) {
        return {signature.status, std::nullopt, signature.error};
    }
    RecordPrepare prepare = [canonical, signature = *signature.value](
                                const DocumentView& view, const RecordIdentityAllocator& allocate) {
        try {
            const auto identity = allocate();
            EditSession edit(view);
            edit.put(records::BeamSection{identity,
                                          canonical.name,
                                          canonical.material_id,
                                          canonical.area_mm2,
                                          canonical.i1_mm4,
                                          canonical.i2_mm4,
                                          canonical.torsion_mm4});
            return success(RecordPreparedOperation{
                edit.prepare(), "Create section", identity, signature, 0, true});
        } catch (const RecordError& error) {
            return failed<RecordPreparedOperation>(error);
        }
    };
    return success(OperationPlan{*signature.value, std::move(prepare)});
}

Result<bool> register_handlers(operations::OperationRegistry& registry, RecordApplication& app) {
    auto registered = registry.register_typed<MaterialCreateInput>(
        InputTraits<MaterialCreateInput>::definition(),
        [&app](const OperationContext& context, const MaterialCreateInput& input) {
            return execute(app, context, input, prepare_create_material);
        });
    if (!registered.ok())
        return registered;
    registered = registry.register_typed<MaterialSetYoungModulusInput>(
        InputTraits<MaterialSetYoungModulusInput>::definition(),
        [&app](const OperationContext& context, const MaterialSetYoungModulusInput& input) {
            return execute(app, context, input, prepare_set_young_modulus);
        });
    if (!registered.ok())
        return registered;
    return registry.register_typed<SectionCreateInput>(
        InputTraits<SectionCreateInput>::definition(),
        [&app](const OperationContext& context, const SectionCreateInput& input) {
            return execute(app, context, input, prepare_create_section);
        });
}
} // namespace qcae::features::materials
