#pragma once
#include "qcae/model_codec.hpp"
#include <array>
#include <span>
#include <string_view>

namespace qcae {
struct NastranStaticReadContext {
    std::uint64_t subcase{};
    std::string unit_system;
    std::string coordinate_basis;
};
struct NastranStaticGridValues {
    EntityId entity;
    std::uint64_t solver_number{};
    std::array<double, 6> components{};
};
// Parsed text only. This DTO does not certify execution or numerical validity.
struct NastranStaticResult {
    std::uint64_t subcase{};
    std::string coordinate_basis;
    std::array<std::string, 6> displacement_units{"mm", "mm", "mm", "rad", "rad", "rad"};
    std::array<std::string, 6> reaction_units{"N", "N", "N", "N*mm", "N*mm", "N*mm"};
    std::vector<NastranStaticGridValues> displacements, spc_reactions;
    std::string reader_version{"qcae.nastran.static-f06.v1"};
};
[[nodiscard]] Result<NastranStaticResult>
read_nastran_static_f06(std::string_view text,
                        const NastranStaticReadContext&,
                        std::span<const ExportIdentifier> frozen_identities);
[[nodiscard]] Result<NastranStaticResult>
read_mystran_static_f06(std::string_view text,
                        const NastranStaticReadContext&,
                        std::span<const ExportIdentifier> frozen_identities);
} // namespace qcae
