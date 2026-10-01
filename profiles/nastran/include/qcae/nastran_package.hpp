#pragma once
#include "qcae/nastran_codec.hpp"
#include "qcae/edit_session.hpp"

namespace qcae {
struct NastranUiContribution {
    std::string action_id, label, operation, units;
    ProfileRef profile;
    std::vector<std::string> required_fields;
};
[[nodiscard]] NastranUiContribution nastran_ui_contribution(const NastranCodec&);
void register_nastran_core_rules(RecordRegistry&, const ProfileRef& installed);
[[nodiscard]] Result<ArtifactPlan>
validate_nastran_export(const DocumentView&, const EntityId&, const NastranCodec&);
[[nodiscard]] Result<PreparedRecordChange> migrate_nastran_profile(const DocumentView&,
                                                                   const ProfileRef& expected_old,
                                                                   const ProfileRef& installed);
// Reparse disk resources through the import codec, then compare canonical engineering
// cards and their solver numbering after a separate export of the reconstructed model.
void verify_nastran_readback(const ArtifactPlan& frozen,
                             std::span<const TextResource> disk_resources,
                             const NastranCodec&);
} // namespace qcae
