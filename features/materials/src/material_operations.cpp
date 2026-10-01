#include "qcae/material_operations.hpp"
#include "qcae/quantities.hpp"
#include "qcae/records.hpp"

namespace qcae::features::materials {
namespace {
using namespace operations;

void observe_record_temporary(const records::Material& value) noexcept {
    ledger::add(ledger::Stage::application,
                ledger::Metric::model_copy_bytes,
                sizeof(value) + value.id.value.size() + value.name.size() +
                    (value.description ? value.description->size() : 0));
}
void observe_record_temporary(const records::BeamSection& value) noexcept {
    ledger::add(ledger::Stage::application,
                ledger::Metric::model_copy_bytes,
                sizeof(value) + value.id.value.size() + value.name.size() +
                    value.material.value.size());
}

std::size_t moved_string_bytes(const std::string& value) noexcept {
#if defined(_LIBCPP_VERSION)
    return value.capacity() <= std::string{}.capacity() ? value.size() : 0;
#else
    ledger::unknown(ledger::Stage::application, ledger::Metric::model_copy_bytes);
    return 0;
#endif
}
void observe_capture_move(const MaterialCreateInput& input) noexcept {
    ledger::add(ledger::Stage::application,
                ledger::Metric::model_copy_bytes,
                2 * (sizeof(input.young_modulus.value) + sizeof(input.poisson_ratio) +
                     moved_string_bytes(input.name) +
                     moved_string_bytes(input.young_modulus.unit)));
}
void observe_capture_move(const MaterialSetYoungModulusInput& input) noexcept {
    ledger::add(ledger::Stage::application,
                ledger::Metric::model_copy_bytes,
                2 * (sizeof(input.young_modulus.value) + moved_string_bytes(input.entity_id.value) +
                     moved_string_bytes(input.young_modulus.unit)));
}
void observe_capture_move(const MaterialSetDescriptionInput& input) noexcept {
    ledger::add(ledger::Stage::application,
                ledger::Metric::model_copy_bytes,
                2 * (sizeof(input.description) + moved_string_bytes(input.entity_id.value) +
                     (input.description ? moved_string_bytes(*input.description) : 0)));
}
void observe_capture_move(const SectionCreateInput& input) noexcept {
    ledger::add(ledger::Stage::application,
                ledger::Metric::model_copy_bytes,
                2 * (4 * sizeof(double) + moved_string_bytes(input.name) +
                     moved_string_bytes(input.material_id.value)));
}
void observe_signature_capture(const std::string& signature) noexcept {
    // One actual string clone into the lambda, then a possible short-rep move into std::function.
    ledger::add(ledger::Stage::application,
                ledger::Metric::metadata_copy_bytes,
                signature.size() + moved_string_bytes(signature));
}
Result<RecordPreparedOperation> prepared(PreparedRecordChange change,
                                         std::string_view label,
                                         const EntityId& entity,
                                         const std::string& signature,
                                         double value,
                                         bool creates) {
    RecordPreparedOperation result{
        std::move(change), std::string(label), entity, signature, value, creates};
    ledger::add(ledger::Stage::application,
                ledger::Metric::metadata_copy_bytes,
                3 * sizeof(std::string) + label.size() + entity.value.size() + signature.size() +
                    sizeof(creates));
    ledger::add(ledger::Stage::application, ledger::Metric::model_copy_bytes, sizeof(value));
    return {Status::success, std::move(result), std::nullopt};
}

template <class T> Result<T> success(T value) {
    return {Status::success, std::move(value), std::nullopt};
}

template <class T> Result<T> failed(const RecordError& error) {
    return {Status::failed, std::nullopt, Diagnostic{error.code(), error.what(), error.field()}};
}

Result<Quantity>
normalized(const Quantity& input, parameters::Dimension dimension, std::string_view field) {
    auto value = parameters::canonical_quantity(input, dimension);
    if (value.value)
        wire::observe_input_copy(*value.value, *value.value);
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
    WriteContext write{*context.document, *context.expected_revision};
    std::string operation(InputTraits<Input>::operation_id);
    ledger::add(ledger::Stage::application,
                ledger::Metric::metadata_copy_bytes,
                sizeof(WriteContext) + write.document.id.value.size() +
                    write.document.epoch.value.size() + operation.size());
    const auto receipt = app.execute(context.caller,
                                     write,
                                     operation,
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
    wire::observe_input_copy(canonical, canonical.name, canonical.young_modulus);
    canonical.young_modulus = *young.value;
    wire::observe_input_copy(canonical.young_modulus, canonical.young_modulus);
    try {
        // Reuse authoritative generated field rules; this probe is never published or allocated.
        const records::Material probe{EntityId{"material-field-check"},
                                      canonical.name,
                                      young.value->value,
                                      canonical.poisson_ratio};
        observe_record_temporary(probe);
        RecordTraits<records::Material>::validate_fields(probe);
    } catch (const RecordError& error) {
        return failed<OperationPlan>(error);
    }
    auto signature = canonical_value(InputTraits<MaterialCreateInput>::to_value(canonical));
    if (!signature.ok()) {
        return {signature.status, std::nullopt, signature.error};
    }
    observe_capture_move(canonical);
    observe_signature_capture(*signature.value);
    RecordPrepare prepare = [canonical = std::move(canonical), signature = *signature.value](
                                const DocumentView& view, const RecordIdentityAllocator& allocate) {
        try {
            const auto identity = allocate();
            EditSession edit(view);
            records::Material material{
                identity, canonical.name, canonical.young_modulus.value, canonical.poisson_ratio};
            observe_record_temporary(material);
            edit.put(std::move(material));
            return prepared(edit.prepare(),
                            "Create material",
                            identity,
                            signature,
                            canonical.young_modulus.value,
                            true);
        } catch (const RecordError& error) {
            return failed<RecordPreparedOperation>(error);
        }
    };
    ledger::add(ledger::Stage::application,
                ledger::Metric::metadata_copy_bytes,
                moved_string_bytes(*signature.value));
    return success(OperationPlan{std::move(*signature.value), std::move(prepare)});
}

Result<OperationPlan>
prepare_set_young_modulus(const operations::MaterialSetYoungModulusInput& input) {
    const auto young =
        normalized(input.young_modulus, parameters::Dimension::pressure, "young_modulus");
    if (!young.ok()) {
        return {young.status, std::nullopt, young.error};
    }
    auto canonical = input;
    wire::observe_input_copy(canonical, canonical.entity_id, canonical.young_modulus);
    canonical.young_modulus = *young.value;
    wire::observe_input_copy(canonical.young_modulus, canonical.young_modulus);
    auto signature =
        canonical_value(InputTraits<MaterialSetYoungModulusInput>::to_value(canonical));
    if (!signature.ok()) {
        return {signature.status, std::nullopt, signature.error};
    }
    observe_capture_move(canonical);
    observe_signature_capture(*signature.value);
    RecordPrepare prepare = [canonical = std::move(canonical), signature = *signature.value](
                                const DocumentView& view, const RecordIdentityAllocator&) {
        try {
            EditSession edit(view);
            edit.update<records::Material>(canonical.entity_id, [&](auto& material) {
                material.young_modulus_mpa = canonical.young_modulus.value;
            });
            return prepared(edit.prepare(),
                            "Set Young modulus",
                            canonical.entity_id,
                            signature,
                            canonical.young_modulus.value,
                            false);
        } catch (const RecordError& error) {
            return failed<RecordPreparedOperation>(error);
        }
    };
    ledger::add(ledger::Stage::application,
                ledger::Metric::metadata_copy_bytes,
                moved_string_bytes(*signature.value));
    return success(OperationPlan{std::move(*signature.value), std::move(prepare)});
}

Result<OperationPlan>
prepare_set_description(const operations::MaterialSetDescriptionInput& input) {
    auto signature = canonical_value(InputTraits<MaterialSetDescriptionInput>::to_value(input));
    if (!signature.ok()) {
        return {signature.status, std::nullopt, signature.error};
    }
    auto canonical = input;
    wire::observe_input_copy(canonical, canonical.entity_id, canonical.description);
    observe_capture_move(canonical);
    observe_signature_capture(*signature.value);
    RecordPrepare prepare = [canonical = std::move(canonical), signature = *signature.value](
                                const DocumentView& view, const RecordIdentityAllocator&) {
        try {
            EditSession edit(view);
            edit.update<records::Material>(canonical.entity_id, [&](auto& material) {
                material.description = canonical.description;
                ledger::add(ledger::Stage::application,
                            ledger::Metric::model_copy_bytes,
                            sizeof(material.description) +
                                (material.description ? material.description->size() : 0));
            });
            return prepared(edit.prepare(),
                            "Set material description",
                            canonical.entity_id,
                            signature,
                            0,
                            false);
        } catch (const RecordError& error) {
            return failed<RecordPreparedOperation>(error);
        }
    };
    ledger::add(ledger::Stage::application,
                ledger::Metric::metadata_copy_bytes,
                moved_string_bytes(*signature.value));
    return success(OperationPlan{std::move(*signature.value), std::move(prepare)});
}

Result<OperationPlan> prepare_create_section(const operations::SectionCreateInput& input) {
    auto canonical = input;
    wire::observe_input_copy(canonical, canonical.name, canonical.material_id);
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
    ledger::add(ledger::Stage::application,
                ledger::Metric::model_copy_bytes,
                4 * (sizeof(Quantity) + 3) + 4 * sizeof(double));
    try {
        const records::BeamSection probe{EntityId{"section-field-check"},
                                         canonical.name,
                                         canonical.material_id,
                                         canonical.area_mm2,
                                         canonical.i1_mm4,
                                         canonical.i2_mm4,
                                         canonical.torsion_mm4};
        observe_record_temporary(probe);
        RecordTraits<records::BeamSection>::validate_fields(probe);
    } catch (const RecordError& error) {
        return failed<OperationPlan>(error);
    }
    auto signature = canonical_value(InputTraits<SectionCreateInput>::to_value(canonical));
    if (!signature.ok()) {
        return {signature.status, std::nullopt, signature.error};
    }
    observe_capture_move(canonical);
    observe_signature_capture(*signature.value);
    RecordPrepare prepare = [canonical = std::move(canonical), signature = *signature.value](
                                const DocumentView& view, const RecordIdentityAllocator& allocate) {
        try {
            const auto identity = allocate();
            EditSession edit(view);
            records::BeamSection section{identity,
                                         canonical.name,
                                         canonical.material_id,
                                         canonical.area_mm2,
                                         canonical.i1_mm4,
                                         canonical.i2_mm4,
                                         canonical.torsion_mm4};
            observe_record_temporary(section);
            edit.put(std::move(section));
            return prepared(edit.prepare(), "Create section", identity, signature, 0, true);
        } catch (const RecordError& error) {
            return failed<RecordPreparedOperation>(error);
        }
    };
    ledger::add(ledger::Stage::application,
                ledger::Metric::metadata_copy_bytes,
                moved_string_bytes(*signature.value));
    return success(OperationPlan{std::move(*signature.value), std::move(prepare)});
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
    registered = registry.register_typed<MaterialSetDescriptionInput>(
        InputTraits<MaterialSetDescriptionInput>::definition(),
        [&app](const OperationContext& context, const MaterialSetDescriptionInput& input) {
            return execute(app, context, input, prepare_set_description);
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
