#pragma once
#include "qcae/typed_host.hpp"
#include <span>

namespace qcae::ipc {
// Static, trusted startup contributions. They prepare the same registry and
// application used by every client; this is not a runtime plugin interface.
struct EngineContribution {
    std::string id;
    std::function<void(RecordRegistry&)> records;
    TypedHost::OperationContributor operations;
};
struct EngineAssembly {
    std::shared_ptr<const RecordRegistry> records;
    TypedHost::OperationContributor operations;
};
std::vector<EngineContribution> default_engine_contributions();
EngineAssembly assemble_engine(std::span<const EngineContribution>);
} // namespace qcae::ipc
