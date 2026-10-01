#pragma once
#include "qcae/analysis_input.hpp"
#include <array>

namespace qcae::features::results {
struct StaticGridValue {
    EntityId entity;
    std::uint64_t solver_number{};
    std::array<double, 6> components{};
};
struct StaticFields {
    std::uint64_t subcase{};
    std::string coordinate_basis;
    std::array<std::string, 6> displacement_units, reaction_units;
    std::vector<StaticGridValue> displacements, spc_reactions;
};
struct ComponentComparison {
    EntityId entity;
    std::uint64_t solver_number{};
    std::string quantity, component, unit;
    double actual{}, expected{}, absolute_error{}, tolerance{};
    bool matched{};
};
struct StaticComparisonReport {
    std::string reference_id{"qcae.cantilever.numeric-reference.v1"};
    std::string reference_input{"nastran-real-benchmark-v3"};
    bool matched{};
    std::vector<ComponentComparison> components;
};
// This bounded comparison uses the original frozen physics and the preregistered
// v3 reference. The adapter supplies its registered profile, including its digest.
// This does not authenticate a process or certify solver acceptance.
[[nodiscard]] Result<StaticComparisonReport>
compare_preregistered_cantilever(const analysis::FrozenAnalysisInput&,
                                 const StaticFields&,
                                 const ProfileRef& registered_reference_profile);
} // namespace qcae::features::results
