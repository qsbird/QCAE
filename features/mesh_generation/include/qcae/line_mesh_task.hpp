#pragma once
#include "qcae/records.hpp"
#include "qcae/task_application.hpp"

namespace qcae {
struct LineMeshInput {
    records::GeometryId geometry{};
    std::uint32_t segments{10};
    std::optional<EntityId> section;
};
records::MeshId line_mesh_identity(std::string_view task_id);
TaskRequest line_mesh_task(
    const RecordSnapshot&, Caller, ProfileRef, LineMeshInput, std::string idempotency_key);
} // namespace qcae
