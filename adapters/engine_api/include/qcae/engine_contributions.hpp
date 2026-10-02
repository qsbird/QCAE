#pragma once
#include "qcae/typed_host.hpp"
#include "qcae/model_codec.hpp"
#include "qcae/profile_provider.hpp"
#include "qcae/render_projector.hpp"
#include <optional>
#include <span>

namespace qcae::ipc {
// Static, trusted startup contributions. They prepare the same registry and
// application used by every client; this is not a runtime plugin interface.
struct EngineCodecBinding {
    // Both ports share the contribution's lifetime; neither is a borrowed host pointer.
    std::shared_ptr<const IModelCodec> codec;
    std::shared_ptr<const IProfileProvider> profile;
};
struct EngineContribution {
    std::string id;
    std::function<void(RecordRegistry&)> records;
    TypedHost::OperationContributor operations;
    std::function<std::vector<OwnedRowHandler>()> owned_rows{};
    TypedHost::TaskPublisherFactory publisher_factory{};
    std::optional<EngineCodecBinding> profile_codec{};
    std::function<RenderContributions()> render_factory{};
};
struct EngineAssembly {
    std::shared_ptr<const RecordRegistry> records;
    TypedHost::OperationContributor operations;
    std::vector<OwnedRowHandler> owned_rows{};
    TypedHost::TaskPublisherFactory publisher_factory{};
    std::optional<EngineCodecBinding> profile_codec{};
    std::string profile_codec_owner{};
    RenderContributions render;
    // Empty denotes the existing generic rendering fallback, not a package registration.
    std::string render_owner{};

    const IModelCodec* model_codec() const noexcept;
    const ProfileDefinition* profile_definition() const noexcept;
};
bool nastran_package_enabled() noexcept;
struct LocalSolverConfiguration;
std::vector<EngineContribution> default_engine_contributions();
std::vector<EngineContribution> default_engine_contributions(const LocalSolverConfiguration&);
EngineAssembly assemble_engine(std::span<const EngineContribution>);
} // namespace qcae::ipc
