#include "qcae/legacy_migration.hpp"
#include "legacy_state.hpp"
#include "qcae/records_model_bridge.hpp"

namespace qcae {
LoadedRows migrate_legacy_workspace(const StoredWorkspace& workspace,
                                    std::shared_ptr<const RecordRegistry> registry,
                                    Limits limits) {
    const auto old = legacy_detail::decode_data(workspace.payload, limits);
    RecordStateImage state(registry);
    state.application_nonce = old.application_nonce;
    state.next_id = old.next_id;
    state.document = old.document;
    state.initial_content_state = old.initial_content_state;
    state.cursor = old.cursor;
    state.recoverable = old.recoverable;
    state.records = records_from_model(
        old.model,
        registry,
        old.document ? RecordVersion{old.document->document, old.document->revision}
                     : RecordVersion{});
    auto baseline = old.model;
    for (std::size_t i = old.cursor; i > 0; --i)
        baseline = apply_model_delta(baseline, old.history[i - 1].delta, false);
    for (const auto& entry : old.history) {
        auto after = apply_model_delta(baseline, entry.delta, true);
        state.history.push_back(std::make_shared<const RecordHistoryImage>(
            RecordHistoryImage{entry.transaction,
                               entry.label,
                               record_changes_from_models(baseline, after, registry),
                               entry.content_state,
                               {}}));
        baseline = std::move(after);
    }
    for (const auto& [key, o] : old.operations)
        state.operations.emplace(key, RecordOperationImage{o.signature, o.receipt, {}});
    for (const auto& [key, o] : old.host_operations)
        state.host_operations.emplace(key, RecordHostImage{o.signature, o.result, {}});
    if (old.save_intent) {
        const auto& i = *old.save_intent;
        state.save_intent = std::make_shared<const RecordSaveImage>(RecordSaveImage{
            i.host_key, i.signature, i.path, i.token, i.project_id, i.snapshot, i.save_as});
    }
    return {0, encode_record_state_image(state), {}};
}
RecordProjectImage migrate_legacy_project(std::string_view payload,
                                          std::shared_ptr<const RecordRegistry> registry,
                                          Limits limits) {
    auto old = legacy_detail::decode_project(payload, limits);
    return {records_from_model(old.model, std::move(registry)),
            old.document->project_id,
            old.document->name,
            old.document->content_state};
}
} // namespace qcae
