#pragma once
#include "qcae/planar_mesher.hpp"
#include "qcae/records.hpp"
#include "qcae/task_application.hpp"
#include <memory>

namespace qcae {
struct RegionMeshInput {
    records::GeometryId region;
    double element_size_mm{};
};
// Larger requests are refused. A running Gmsh mesh cannot be interrupted, so a cancel waits for
// it; 20,000 triangles mesh in about half a second and publish about 30,000 records, well inside
// the default limit of 100,000 entities per document.
inline constexpr std::uint32_t region_mesh_max_triangles = 20000;

records::MeshId region_mesh_identity(std::string_view task_id);
// Meshes a region with triangles in the background. The task publishes one Mesh bound to the
// region revision, a Node per mesh node and a Tri3 per triangle, with identities derived from the
// task. The signature includes the mesher identity, so a retry with another mesher version is a
// different request.
TaskRequest region_mesh_task(const RecordSnapshot&,
                             Caller,
                             RegionMeshInput,
                             std::string idempotency_key,
                             std::shared_ptr<const IPlanarMesher>);
} // namespace qcae
