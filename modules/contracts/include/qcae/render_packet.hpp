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
    bool visible{true};
};
struct RenderGeometryLine {
    EntityId entity;
    std::array<double, 3> start_mm{}, end_mm{};
    bool visible{true};
};
enum class RenderCellKind : std::uint64_t { polyline, polygon };
inline constexpr std::size_t render_cell_point_limit = 4096;
// Topology describes a disposable display cell, independent of persistent types.
struct RenderCell {
    EntityId entity;
    RenderCellKind kind{RenderCellKind::polyline};
    std::vector<std::size_t> points;
    bool visible{true};
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
    std::vector<RenderCell> cells{};
};
struct RenderPointUpdate {
    std::size_t index{};
    RenderPoint point;
};
struct RenderGeometryUpdate {
    std::size_t index{};
    RenderGeometryLine line;
};
enum class RenderPrimitive { point, beam, geometry_line, cell };
struct RenderVisibilityUpdate {
    RenderPrimitive primitive;
    std::size_t index{};
    EntityId entity;
    bool visible{true};
};
// Applies only to the named installed scene. Topology changes use a new packet;
// hidden entities retain coordinates and stable indices for local visibility changes.
struct RenderDelta {
    DocumentRef document;
    Revision base_revision{}, revision{};
    std::string view_session_id;
    std::uint64_t base_view_revision{}, view_revision{};
    std::vector<RenderPointUpdate> points;
    std::vector<RenderGeometryUpdate> geometry_lines;
    std::vector<RenderVisibilityUpdate> visibility{};
};
} // namespace qcae
