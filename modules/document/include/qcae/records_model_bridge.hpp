#pragma once

#include "qcae/document_view.hpp"
#include "qcae/model.hpp"

namespace qcae {
// Full import/export only. These functions are never part of a local-edit path.
std::shared_ptr<const RecordRegistry> make_record_registry();
DocumentView records_from_model(const Model&,
                                std::shared_ptr<const RecordRegistry>,
                                RecordVersion = {},
                                RecordStats* = nullptr);
Model model_from_records(const DocumentView&, RecordStats* = nullptr);
// Record types model_from_records represents, plus geometry and mesh containers it omits because
// their discretized nodes and beams are projected. Any other type is lost by the projection, so
// consumers that need complete semantics pass this list to unsupported_record_type.
std::span<const RecordTypeId> model_projection_record_types() noexcept;
// Matches values by their persistent identities; source rows get deterministic migration IDs.
RecordChangeSet record_changes_from_models(const Model& before,
                                           const Model& after,
                                           std::shared_ptr<const RecordRegistry>,
                                           RecordStats* = nullptr);
} // namespace qcae
