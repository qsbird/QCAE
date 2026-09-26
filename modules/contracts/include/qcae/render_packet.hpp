#pragma once
#include "qcae/types.hpp"
#include <array>
#include <vector>

namespace qcae {
struct RenderPoint {
    EntityId entity;
    std::array<double, 3> position_mm{};
    bool visible{true};
};
struct RenderBeam {
    EntityId entity;
    std::array<std::size_t, 2> points{};
};
// Read-only display contract. Consumers never receive a domain object or storage pointer.
struct RenderPacket {
    DocumentRef document;
    Revision revision{};
    std::string view_session_id;
    std::uint64_t view_revision{};
    std::vector<RenderPoint> points;
    std::vector<RenderBeam> beams;
};
} // namespace qcae
