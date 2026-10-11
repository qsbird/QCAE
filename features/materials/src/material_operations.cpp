#include "qcae/material_operations.hpp"

namespace qcae::features::materials {
using namespace operations;

Result<OperationPlan> prepare_create_material(const MaterialCreateInput& input) {
    auto canonical = canonical_copy(input);
    if (auto normalized =
            normalize(canonical.young_modulus, parameters::Dimension::pressure, "young_modulus");
        !normalized.ok())
        return forward_failure<OperationPlan>(normalized);
    // Reuse the generated field rules; this probe is never published.
    if (const auto error = check_record_fields(records::Material{EntityId{"material-field-check"},
                                                                 canonical.name,
                                                                 canonical.young_modulus.value,
                                                                 canonical.poisson_ratio}))
        return failed<OperationPlan>(*error);
    return record_plan(
        std::move(canonical), [](const MaterialCreateInput& canonical, PlanEdit& edit) {
            const auto identity = edit.allocate();
            edit.put(records::Material{
                identity, canonical.name, canonical.young_modulus.value, canonical.poisson_ratio});
            return ChangeSummary{"Create material", identity, canonical.young_modulus.value, true};
        });
}

Result<OperationPlan> prepare_set_young_modulus(const MaterialSetYoungModulusInput& input) {
    auto canonical = canonical_copy(input);
    if (auto normalized =
            normalize(canonical.young_modulus, parameters::Dimension::pressure, "young_modulus");
        !normalized.ok())
        return forward_failure<OperationPlan>(normalized);
    return record_plan(
        std::move(canonical), [](const MaterialSetYoungModulusInput& canonical, PlanEdit& edit) {
            edit.update<records::Material>(canonical.entity_id, [&](auto& material) {
                material.young_modulus_mpa = canonical.young_modulus.value;
            });
            return ChangeSummary{
                "Set Young modulus", canonical.entity_id, canonical.young_modulus.value, false};
        });
}

Result<OperationPlan> prepare_set_description(const MaterialSetDescriptionInput& input) {
    return record_plan(
        canonical_copy(input), [](const MaterialSetDescriptionInput& canonical, PlanEdit& edit) {
            edit.update<records::Material>(canonical.entity_id, [&](auto& material) {
                copy_into(material.description, canonical.description);
            });
            return ChangeSummary{"Set material description", canonical.entity_id, 0, false};
        });
}

Result<OperationPlan> prepare_create_section(const SectionCreateInput& input) {
    using parameters::Dimension;
    auto canonical = canonical_copy(input);
    for (auto [value, unit, dimension, field] :
         {std::tuple{&canonical.area_mm2, "mm2", Dimension::area, "area_mm2"},
          std::tuple{&canonical.i1_mm4, "mm4", Dimension::inertia, "i1_mm4"},
          std::tuple{&canonical.i2_mm4, "mm4", Dimension::inertia, "i2_mm4"},
          std::tuple{&canonical.torsion_mm4, "mm4", Dimension::inertia, "torsion_mm4"}}) {
        if (auto normalized = normalize(*value, unit, dimension, field); !normalized.ok())
            return forward_failure<OperationPlan>(normalized);
    }
    if (const auto error = check_record_fields(records::BeamSection{EntityId{"section-field-check"},
                                                                    canonical.name,
                                                                    canonical.material_id,
                                                                    canonical.area_mm2,
                                                                    canonical.i1_mm4,
                                                                    canonical.i2_mm4,
                                                                    canonical.torsion_mm4}))
        return failed<OperationPlan>(*error);
    return record_plan(std::move(canonical),
                       [](const SectionCreateInput& canonical, PlanEdit& edit) {
                           const auto identity = edit.allocate();
                           edit.put(records::BeamSection{identity,
                                                         canonical.name,
                                                         canonical.material_id,
                                                         canonical.area_mm2,
                                                         canonical.i1_mm4,
                                                         canonical.i2_mm4,
                                                         canonical.torsion_mm4});
                           return ChangeSummary{"Create section", identity, 0, true};
                       });
}

Result<bool> register_handlers(OperationRegistry& registry, RecordApplication& app) {
    auto registered = register_plan(registry, app, prepare_create_material);
    if (registered.ok())
        registered = register_plan(registry, app, prepare_set_young_modulus);
    if (registered.ok())
        registered = register_plan(registry, app, prepare_set_description);
    if (registered.ok())
        registered = register_plan(registry, app, prepare_create_section);
    return registered;
}
} // namespace qcae::features::materials
