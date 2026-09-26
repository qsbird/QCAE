#pragma once
#include "qcae/model.hpp"
#include <map>

namespace qcae {
struct TextResource {
    std::string path;
    std::string text;
};
struct FormatIssue {
    std::string code;
    std::string message;
    std::string resource;
    std::size_t line{};
    bool blocking{true};
};
struct ImportRequest {
    std::string root_resource;
    std::vector<TextResource> resources;
    ProfileRef source_profile;
    std::string source_model_id;
    std::string unit_system; // Required: mm-N-MPa; no automatic guesses from BDF.
};
struct ImportReport {
    ProfileRef profile;
    std::vector<FormatIssue> issues;
    std::size_t supported_records{};
    bool complete{false};
};
struct ImportOutcome {
    std::optional<Model> candidate;
    ImportReport report;
};
struct ExportIdentifier {
    EntityId entity;
    std::string name_space;
    std::uint64_t number{};
};
struct ExportReport {
    ProfileRef profile;
    std::vector<FormatIssue> issues;
    bool complete{false};
};
struct ArtifactPlan {
    std::string root_resource;
    std::vector<TextResource> resources;
    std::vector<ExportIdentifier> identities;
    EntityId analysis;
    ProfileRef profile;
};
struct ExportOutcome {
    std::optional<ArtifactPlan> artifact;
    ExportReport report;
};
class IModelCodec {
  public:
    virtual ~IModelCodec() = default;
    virtual ImportOutcome decode(const ImportRequest&) const = 0;
    virtual ExportOutcome
    encode(const Model&, const EntityId& analysis, const ProfileRef& expected) const = 0;
};
} // namespace qcae
