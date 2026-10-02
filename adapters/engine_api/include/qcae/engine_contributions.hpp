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
struct EngineValidationBinding {
    std::string id;
    std::uint32_t version{1};
    std::function<Result<ArtifactPlan>(const DocumentView&, const EntityId&)> validate_export;
};
struct EngineContribution {
    std::string id;
    std::function<void(RecordRegistry&)> records;
    TypedHost::OperationContributor operations;
    std::function<std::vector<OwnedRowHandler>()> owned_rows{};
    TypedHost::TaskPublisherFactory publisher_factory{};
    std::optional<EngineCodecBinding> profile_codec{};
    std::function<RenderContributions()> render_factory{};
    // When supplied, these names must match the rules actually appended by records.
    std::vector<std::string> core_rule_ids{};
    std::vector<EngineValidationBinding> validators{};
    // References to this contribution's available, read-only operation registrations.
    std::vector<std::string> ui_operations{};
};
struct EngineCoreRuleRegistration {
    std::string id;
    std::size_t rule_index{};
};
struct EngineContributionRegistration {
    std::string id;
    std::vector<RecordTypeId> record_types;
    std::vector<EngineCoreRuleRegistration> core_rules;
    std::vector<operations::OperationDescriptor> operations;
    std::optional<EngineCodecBinding> profile_codec;
    std::vector<EngineValidationBinding> validators;
    std::vector<std::string> ui_operations;
    // Copies of the actual selected factory entries retain the same callable projections.
    std::vector<RenderContribution> render;
};
struct EngineAssembly;
class EngineContributionCatalog {
  public:
    // Available only after every operation contributor successfully registered.
    const std::vector<EngineContributionRegistration>& entries() const;
    QJsonArray describe() const;

  private:
    friend EngineAssembly assemble_engine(std::span<const EngineContribution>);
    std::shared_ptr<const RecordRegistry> records_;
    std::vector<EngineContributionRegistration> entries_;
    bool operations_registered_{};
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
    std::shared_ptr<const EngineContributionCatalog> catalog{};

    const IModelCodec* model_codec() const noexcept;
    const ProfileDefinition* profile_definition() const noexcept;
};
bool nastran_package_enabled() noexcept;
struct LocalSolverConfiguration;
std::vector<EngineContribution> default_engine_contributions();
std::vector<EngineContribution> default_engine_contributions(const LocalSolverConfiguration&);
EngineAssembly assemble_engine(std::span<const EngineContribution>);
} // namespace qcae::ipc
