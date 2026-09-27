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
// Matches values by their persistent identities; source rows get deterministic migration IDs.
RecordChangeSet record_changes_from_models(const Model& before,
                                           const Model& after,
                                           std::shared_ptr<const RecordRegistry>,
                                           RecordStats* = nullptr);
} // namespace qcae
