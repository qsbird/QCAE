#include "qcae/nastran_package.hpp"
#include "qcae/records.hpp"
#include "qcae/records_model_bridge.hpp"
#include <algorithm>
#include <set>
#include <sstream>

namespace qcae {
namespace {
template <class T> Result<T> good(T value) {
    return {Status::success, std::move(value), {}};
}
template <class T> Result<T> bad(ErrorCode code, std::string message) {
    return {Status::failed, {}, Diagnostic{code, std::move(message), "profile"}};
}
std::multiset<std::string> physical_cards(std::span<const TextResource> resources) {
    std::multiset<std::string> cards;
    for (const auto& resource : resources) {
        std::istringstream input(resource.text);
        std::string line;
        while (std::getline(input, line)) {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.empty() || line.starts_with("$") || line.starts_with("INCLUDE"))
                continue;
            cards.insert(std::move(line));
        }
    }
    return cards;
}
} // namespace
NastranUiContribution nastran_ui_contribution(const NastranCodec& codec) {
    return {"nastran.export",
            "Export Nastran files…",
            "model.export",
            "mm-N-MPa",
            codec.definition().reference,
            {"analysis_id", "output_directory"}};
}
void register_nastran_core_rules(RecordRegistry& registry, const ProfileRef& installed) {
    registry.add_rule([installed](const DocumentView& view) {
        view.visit(RecordTraits<records::AnalysisDefinition>::type_id, [&](const Record& image) {
            const auto& analysis = image->get<records::AnalysisDefinition>();
            if (analysis.target.profile.profile_id == installed.profile_id &&
                analysis.target.analysis_kind != "linear_static")
                throw RecordError(ErrorCode::invalid_input,
                                  "Nastran controlled profile requires linear_static analysis",
                                  analysis.id.value);
        });
        view.visit(RecordTraits<records::SourceIdentifier>::type_id, [&](const Record& image) {
            const auto& source = image->get<records::SourceIdentifier>();
            if (source.profile.profile_id == installed.profile_id &&
                (source.number == 0 || source.number > 99999999))
                throw RecordError(
                    ErrorCode::invalid_input,
                    "Nastran source number is outside the supported eight-digit range",
                    source.id.value);
        });
    });
}
Result<ArtifactPlan> validate_nastran_export(const DocumentView& view,
                                             const EntityId& analysis,
                                             const NastranCodec& codec) {
    if (view.count(RecordTraits<records::Tri3>::type_id))
        return bad<ArtifactPlan>(
            ErrorCode::unsupported_capability,
            "Tri3 shell export is not implemented in the controlled Nastran profile");
    const auto exported =
        codec.encode(model_from_records(view), analysis, codec.definition().reference);
    if (!exported.report.complete || !exported.artifact) {
        std::string message = "Controlled Nastran export validation failed";
        for (const auto& issue : exported.report.issues)
            if (issue.blocking)
                message += "; " + issue.code + ": " + issue.message;
        return bad<ArtifactPlan>(ErrorCode::invalid_input, std::move(message));
    }
    return good(*exported.artifact);
}
Result<PreparedRecordChange> migrate_nastran_profile(const DocumentView& view,
                                                     const ProfileRef& old,
                                                     const ProfileRef& installed) {
    // Frozen engine-generated 0.1.0 identity: tests/fixtures/legacy/manifest.json.
    const ProfileRef legacy{
        "qcae.nastran.linear-static",
        "0.1.0",
        "sha256:bb856be32336514d1aebf67b524ed0403664291cb4ba9e473e922ae7bb71b662"};
    if (old != legacy || installed.profile_version != "0.2.0" ||
        installed != NastranCodec{}.definition().reference)
        return bad<PreparedRecordChange>(
            ErrorCode::schema_unsupported,
            "Only the frozen controlled 0.1.0 definition can migrate to the installed profile");
    try {
        EditSession edit(view);
        std::size_t count = 0;
        view.visit(RecordTraits<records::AnalysisDefinition>::type_id, [&](const Record& image) {
            const auto& value = image->get<records::AnalysisDefinition>();
            if (value.target.profile != old || value.target.analysis_kind != "linear_static")
                throw RecordError(ErrorCode::schema_unsupported,
                                  "Migration source analysis has an incompatible target",
                                  value.id.value);
            edit.update<records::AnalysisDefinition>(
                value.id, [&](auto& item) { item.target.profile = installed; });
            ++count;
        });
        view.visit(RecordTraits<records::SourceIdentifier>::type_id, [&](const Record& image) {
            const auto& value = image->get<records::SourceIdentifier>();
            if (value.profile != old)
                throw RecordError(ErrorCode::schema_unsupported,
                                  "Migration source numbering has an incompatible target",
                                  value.id.value);
            edit.update<records::SourceIdentifier>(value.id,
                                                   [&](auto& item) { item.profile = installed; });
            ++count;
        });
        if (!count)
            return bad<PreparedRecordChange>(ErrorCode::invalid_input,
                                             "No old profile bindings require migration");
        return good(edit.prepare());
    } catch (const RecordError& error) {
        return bad<PreparedRecordChange>(error.code(), error.what());
    }
}
void verify_nastran_readback(const ArtifactPlan& frozen,
                             std::span<const TextResource> disk_resources,
                             const NastranCodec& codec) {
    const auto imported = codec.decode({frozen.root_resource,
                                        {disk_resources.begin(), disk_resources.end()},
                                        frozen.profile,
                                        "artifact-independent-readback",
                                        "mm-N-MPa"});
    if (!imported.report.complete || !imported.candidate ||
        imported.candidate->analyses.size() != 1)
        throw RecordError(ErrorCode::invalid_input,
                          "Published BDF/INCLUDE failed independent semantic import");
    const auto rebuilt =
        codec.encode(*imported.candidate, imported.candidate->analyses.front().id, frozen.profile);
    if (!rebuilt.report.complete || !rebuilt.artifact ||
        physical_cards(rebuilt.artifact->resources) != physical_cards(frozen.resources))
        throw RecordError(
            ErrorCode::invalid_input,
            "Published engineering cards or export numbering differ after independent readback");
}
} // namespace qcae
