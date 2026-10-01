#pragma once
#include "qcae/record_application.hpp"
#include "qcae/records.hpp"

namespace qcae {
struct LineGeometryInput {
    std::array<double, 3> start_mm{};
    std::array<double, 3> end_mm{};
};
std::string line_geometry_signature(const LineGeometryInput&);
std::string line_endpoint_signature(const records::GeometryId&, const std::array<double, 3>&);
RecordPrepare create_line_handler(LineGeometryInput);
RecordPrepare move_line_endpoint_handler(records::GeometryId, std::array<double, 3> end_mm);
// The line parameter is dimensionless, in the closed interval [0, 1].
std::array<double, 3> evaluate_line(const DocumentView&, const records::GeometryId&, double u);
} // namespace qcae
