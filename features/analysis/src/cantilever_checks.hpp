#pragma once
#include "qcae/analysis_features.hpp"

namespace qcae::features::analysis::detail {
[[nodiscard]] std::span<const RuleDefinition> cantilever_rules() noexcept;
void check_cantilever(const DocumentView&, const Record& analysis, CheckReport&);
} // namespace qcae::features::analysis::detail
