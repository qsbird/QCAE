#pragma once
#include "qcae/model_codec.hpp"
#include "qcae/profile_provider.hpp"

namespace qcae {
// A documented, strict text codec; availability does not certify a solver executable.
class NastranCodec final : public IModelCodec, public IProfileProvider {
  public:
    NastranCodec();
    const ProfileDefinition& definition() const noexcept override {
        return definition_;
    }
    ImportOutcome decode(const ImportRequest&) const override;
    ExportOutcome encode(const Model&, const EntityId&, const ProfileRef&) const override;

  private:
    ProfileDefinition definition_;
};
} // namespace qcae
