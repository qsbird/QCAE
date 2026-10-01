#pragma once
#include "qcae/operation_registry.hpp"
#include "qcae/result_service.hpp"

namespace qcae::ipc {
[[nodiscard]] Result<bool> register_fixture_result_handlers(operations::OperationRegistry&,
                                                            RecordApplication&,
                                                            features::results::FrozenInputResolver);
} // namespace qcae::ipc
