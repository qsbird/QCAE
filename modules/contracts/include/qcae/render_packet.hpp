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
struct RenderGeometryLine {
    EntityId entity;
    std::array<double, 3> start_mm{}, end_mm{};
};
// Disposable tool preview. Its display points and lines are not model entities.
struct RenderPreview {
    std::vector<std::array<double, 3>> points;
    std::vector<std::array<std::size_t, 2>> lines;
};
// Read-only display contract. Consumers never receive a domain object or storage pointer.
struct RenderPacket {
    DocumentRef document;
    Revision revision{};
    std::string view_session_id;
    std::uint64_t view_revision{};
    std::vector<RenderPoint> points;
    std::vector<RenderBeam> beams;
    std::vector<RenderGeometryLine> geometry_lines;
};
} // namespace qcae
