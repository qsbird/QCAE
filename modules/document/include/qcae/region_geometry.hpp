#pragma once
#include "qcae/planar_mesher.hpp"
#include "qcae/records.hpp"

namespace qcae::records {
// The corner positions of a region's outer loop and holes, read from the view. A missing loop or
// corner throws RecordError. The region rule and the region mesh task both read regions this way.
[[nodiscard]] PlanarRegion planar_region(const DocumentView&, const GeometryRegion&);
} // namespace qcae::records
