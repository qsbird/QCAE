#pragma once
#include "qcae/planar_mesher.hpp"

namespace qcae {
// Meshes flat regions with the Gmsh library inside this process. Gmsh keeps its models and
// options in process-wide state, so calls from every instance run one at a time, and each call
// removes the Gmsh model it built, also when Gmsh fails.
class GmshPlanarMesher final : public IPlanarMesher {
  public:
    // Starts Gmsh once per process. Gmsh's start-up changes PATH, PYTHONPATH and the process
    // locale, and the constructor puts them back; construct the mesher during start-up, before
    // other threads run, because changing them is not thread-safe.
    GmshPlanarMesher();
    // "gmsh <version>", as reported by the loaded library.
    std::string identity() const override;
    Result<PlanarMesh> mesh(const PlanarRegion&, const PlanarMeshOptions&) const override;
};
} // namespace qcae
