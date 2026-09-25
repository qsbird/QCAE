#pragma once

#include "qcae/core.hpp"

namespace qcae {
// Metadata contract only in M0. No production solver profile is installed yet.
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
