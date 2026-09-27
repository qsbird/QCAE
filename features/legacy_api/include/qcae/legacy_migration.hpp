#pragma once
#include "qcae/record_application.hpp"

namespace qcae {
// Returns a detached image. Callers must commit it to a fresh destination store;
// loading never rewrites the legacy workspace or project supplied by the user.
LoadedRows migrate_legacy_workspace(const StoredWorkspace&,
                                    std::shared_ptr<const RecordRegistry>,
                                    Limits = {});
RecordProjectImage
    migrate_legacy_project(std::string_view, std::shared_ptr<const RecordRegistry>, Limits = {});
} // namespace qcae
