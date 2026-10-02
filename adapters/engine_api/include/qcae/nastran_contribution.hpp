#pragma once
#include "qcae/engine_contributions.hpp"
#include "qcae/analysis_input.hpp"
#include "qcae/artifacts_local.hpp"
#include "qcae/nastran_codec.hpp"
#include "qcae/render_projector.hpp"

namespace qcae::ipc {
// Concrete host glue. The filesystem adapter and the package's domain capabilities
// remain independent of Qt and of this transport composition interface.
class NastranArtifactCoordinator {
  public:
    NastranArtifactCoordinator();
    ~NastranArtifactCoordinator();
    [[nodiscard]] TaskPublisher publisher(RecordApplication&,
                                          std::function<bool(const ProfileRef&)>);
    [[nodiscard]] Result<bool> register_operations(operations::OperationRegistry&,
                                                   RecordApplication&,
                                                   std::function<TaskService&()>);
    [[nodiscard]] Result<features::analysis::FrozenAnalysisInput>
    frozen_input(const Caller&, const DocumentRef&, std::string_view artifact_id) const;
    // Uses the same principal, task/manifest cross-row and completed-file checks as results.
    [[nodiscard]] Result<std::pair<features::analysis::FrozenAnalysisInput, LocalArtifactIntent>>
    verified_artifact(const Caller&, const DocumentRef&, std::string_view artifact_id) const;
    [[nodiscard]] static OwnedRowHandler row_handler();
    [[nodiscard]] static OwnedRowHandler reconcile_row_handler();
    [[nodiscard]] const NastranCodec& codec() const noexcept;
    [[nodiscard]] EngineCodecBinding codec_binding() const;

  private:
    struct State;
    std::shared_ptr<State> state_;
};
[[nodiscard]] EngineContribution
nastran_engine_contribution(const std::shared_ptr<NastranArtifactCoordinator>&);
[[nodiscard]] RenderContributions nastran_render_contributions();
} // namespace qcae::ipc
