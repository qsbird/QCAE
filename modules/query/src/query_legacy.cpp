#include "qcae/query.hpp"
#include "qcae/records_model_bridge.hpp"

namespace qcae {
namespace {
DocumentView legacy_view(const ModelSnapshot& snapshot) {
    static const auto registry = [] {
        auto generated = make_record_registry();
        auto legacy = std::make_shared<RecordRegistry>();
        for (auto type : generated->types()) {
            auto descriptor = *generated->find(type);
            if (descriptor.query_kind == "mesh" || descriptor.query_kind == "geometry")
                descriptor.query_kind.clear();
            legacy->add(std::move(descriptor));
        }
        for (const auto& rule : generated->rules())
            legacy->add_rule(rule);
        legacy->freeze();
        return legacy;
    }();
    // This intentionally invokes the fully instrumented legacy import bridge.
    return records_from_model(snapshot, registry, {snapshot.info.document, snapshot.info.revision});
}
} // namespace
Result<QueryResult>
execute_query(const ModelSnapshot& snapshot, const ViewSession& view, const QuerySpec& spec) {
    return execute_query(legacy_view(snapshot), view, spec);
}
Result<RenderPacket>
produce_render_packet(const ModelSnapshot& snapshot, const ViewSession& view, std::size_t limit) {
    return produce_render_packet(legacy_view(snapshot), view, limit);
}
Result<ViewSession> SelectionService::create_view(const ModelSnapshot& snapshot,
                                                  const Caller& caller,
                                                  std::vector<EntityId> hidden,
                                                  std::string camera) {
    return create_view(legacy_view(snapshot), caller, std::move(hidden), std::move(camera));
}
Result<ViewSession> SelectionService::get_view(const ModelSnapshot& snapshot,
                                               const Caller& caller,
                                               const std::string& view_id) const {
    return get_view(legacy_view(snapshot), caller, view_id);
}
Result<ViewSession> SelectionService::inspect_view(const ModelSnapshot& snapshot,
                                                   const Caller& caller,
                                                   const std::string& view_id) const {
    return inspect_view(legacy_view(snapshot), caller, view_id);
}
Result<ViewSession> SelectionService::update_view(const ModelSnapshot& snapshot,
                                                  const Caller& caller,
                                                  const std::string& view_id,
                                                  std::vector<EntityId> hidden,
                                                  std::string camera) {
    return update_view(
        legacy_view(snapshot), caller, view_id, std::move(hidden), std::move(camera));
}
Result<SelectionHandle> SelectionService::select(const ModelSnapshot& snapshot,
                                                 const Caller& caller,
                                                 const std::string& view_id,
                                                 const QuerySpec& spec) {
    return select(legacy_view(snapshot), caller, view_id, spec);
}
Result<SelectionHandle> SelectionService::combine(const ModelSnapshot& snapshot,
                                                  const Caller& caller,
                                                  const std::string& view_id,
                                                  const std::string& left_handle,
                                                  const std::string& right_handle,
                                                  SetOperation operation) {
    return combine(legacy_view(snapshot), caller, view_id, left_handle, right_handle, operation);
}
Result<SelectionPage> SelectionService::page(const ModelSnapshot& snapshot,
                                             const Caller& caller,
                                             const std::string& handle_id,
                                             std::size_t offset,
                                             std::size_t limit) const {
    return page(legacy_view(snapshot), caller, handle_id, offset, limit);
}
Result<RenderPacket> SelectionService::render_packet(const ModelSnapshot& snapshot,
                                                     const Caller& caller,
                                                     const std::string& view_id) const {
    return render_packet(legacy_view(snapshot), caller, view_id);
}
} // namespace qcae
