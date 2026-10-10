#pragma once
#include "qcae/feature_operation.hpp"
#include "qcae/operation_inputs.hpp"

namespace qcae::features::materials {
[[nodiscard]] Result<OperationPlan> prepare_create_material(const operations::MaterialCreateInput&);
[[nodiscard]] Result<OperationPlan>
prepare_set_young_modulus(const operations::MaterialSetYoungModulusInput&);
// An absent description clears the optional field; supplied text must be nonempty.
[[nodiscard]] Result<OperationPlan>
prepare_set_description(const operations::MaterialSetDescriptionInput&);
[[nodiscard]] Result<OperationPlan> prepare_create_section(const operations::SectionCreateInput&);
[[nodiscard]] Result<bool> register_handlers(operations::OperationRegistry&, RecordApplication&);
} // namespace qcae::features::materials
