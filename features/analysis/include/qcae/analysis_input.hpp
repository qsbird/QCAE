#pragma once
#include "qcae/document_view.hpp"
#include "qcae/model_codec.hpp"
#include <span>

namespace qcae::features::analysis {
struct FrozenAnalysisInput {
    RecordVersion version;
    EntityId analysis;
    TargetBinding target;
    // Empty for the historical direct-reference representation only.
    EntityId load_case;
    std::string case_label{"LC1"};
    // Exact deterministic physical signature; this is not a cryptographic hash.
    std::string input_signature;
    std::vector<ExportIdentifier> identities;
};
// Resolves the controlled static physics and validates the supplied export map.
// Organization, source numbering, names and view state do not enter the signature.
[[nodiscard]] Result<FrozenAnalysisInput>
freeze_analysis_input(const DocumentView&,
                      const EntityId& analysis,
                      const ProfileRef& expected_profile,
                      std::span<const ExportIdentifier> identities);
[[nodiscard]] std::string physical_signature_hex(const FrozenAnalysisInput&);
[[nodiscard]] std::string encode_frozen_analysis_input(const FrozenAnalysisInput&);
[[nodiscard]] FrozenAnalysisInput decode_frozen_analysis_input(std::string_view);
} // namespace qcae::features::analysis
