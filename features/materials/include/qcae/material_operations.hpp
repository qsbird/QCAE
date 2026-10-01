#pragma once
#include "qcae/operation_inputs.hpp"
#include "qcae/record_application.hpp"

namespace qcae::features::materials {
struct OperationPlan {
    std::string signature;
    RecordPrepare prepare;
};
[[nodiscard]] Result<OperationPlan> prepare_create_material(const operations::MaterialCreateInput&);
[[nodiscard]] Result<OperationPlan>
prepare_set_young_modulus(const operations::MaterialSetYoungModulusInput&);
// An absent description clears the optional field; supplied text must be nonempty.
[[nodiscard]] Result<OperationPlan>
prepare_set_description(const operations::MaterialSetDescriptionInput&);
[[nodiscard]] Result<OperationPlan> prepare_create_section(const operations::SectionCreateInput&);
[[nodiscard]] Result<bool> register_handlers(operations::OperationRegistry&, RecordApplication&);
} // namespace qcae::features::materials
