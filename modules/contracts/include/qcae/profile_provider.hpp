#pragma once

#include "qcae/types.hpp"

namespace qcae {
// Codec availability is separate from configured/validated solver execution.
struct ProfileDefinition {
    ProfileRef reference;
    std::string solver_family;
    std::string analysis_kind;
    bool configured{false};
    bool validated{false};
};

class IProfileProvider {
  public:
    virtual ~IProfileProvider() = default;
    [[nodiscard]] virtual const ProfileDefinition& definition() const noexcept = 0;
};
} // namespace qcae
