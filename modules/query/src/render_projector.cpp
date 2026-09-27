#include "qcae/render_projector.hpp"
#include "qcae/records.hpp"
#include <algorithm>
#include <set>

namespace qcae {
namespace {
template <class T> Result<T> success(T value) {
    return {Status::success, std::move(value), std::nullopt};
}
template <class T> Result<T> failure(ErrorCode code, std::string message) {
    return {Status::failed, std::nullopt, Diagnostic{code, std::move(message), "render"}};
}
bool same_document(const DocumentRef& a, const DocumentRef& b) {
    return a.id == b.id && a.epoch == b.epoch;
}
void accumulate(RenderProjectionStats& total, const RenderProjectionStats& work) {
    total.full_rebuilds += work.full_rebuilds;
    total.projected_records += work.projected_records;
    total.model_bytes_copied += work.model_bytes_copied;
    total.dirty_blocks += work.dirty_blocks;
}
std::size_t copied_bytes(const RenderItem& item) {
    return std::visit(
        [](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, RenderPoint>)
                return value.entity.value.size() + sizeof(value.position_mm);
            else if constexpr (std::is_same_v<T, RenderLine2>)
                return value.entity.value.size() + value.nodes[0].value.size() +
                       value.nodes[1].value.size();
            else
                return value.entity.value.size() + sizeof(value.start_mm) + sizeof(value.end_mm);
        },
        item);
}
RenderItem project(const RenderContribution& contribution,
                   const Record& record,
                   RenderProjectionStats& stats) {
    auto item = contribution.project(record);
    if (std::visit([](const auto& value) -> const std::string& { return value.entity.value; },
                   item) != record->key().identity)
        throw RecordError(ErrorCode::invalid_input, "Display contribution changed entity identity");
    ++stats.projected_records;
    stats.model_bytes_copied += copied_bytes(item);
    return item;
}
} // namespace
void RenderContributions::add(RenderContribution contribution) {
    if (frozen_ || !contribution.project || contribution.topology.empty() ||
        !entries_.emplace(contribution.record_type, std::move(contribution)).second)
        throw RecordError(ErrorCode::invalid_input,
                          "Invalid, duplicate or frozen render contribution");
}
void RenderContributions::freeze() {
    frozen_ = true;
}
bool RenderContributions::frozen() const noexcept {
    return frozen_;
}
const RenderContribution* RenderContributions::find(RecordTypeId type) const {
    const auto it = entries_.find(type);
    return it == entries_.end() ? nullptr : &it->second;
}
const std::map<RecordTypeId, RenderContribution>& RenderContributions::entries() const {
    return entries_;
}
RenderContributions default_render_contributions() {
    RenderContributions result;
    result.add(
        {RecordTraits<records::Node>::type_id, "point", [](const Record& record) -> RenderItem {
             const auto& node = record->get<records::Node>();
             return RenderPoint{node.id, node.position, true};
         }});
    result.add(
        {RecordTraits<records::Beam>::type_id, "line2", [](const Record& record) -> RenderItem {
             const auto& beam = record->get<records::Beam>();
             return RenderLine2{beam.id, beam.nodes};
         }});
    result.add({RecordTraits<records::GeometryLine>::type_id,
                "geometry_line",
                [](const Record& record) -> RenderItem {
                    const auto& line = record->get<records::GeometryLine>();
                    return RenderGeometryLine{EntityId(line.id.value), line.start, line.end};
                }});
    result.freeze();
    return result;
}
RenderProjector::RenderProjector(RenderContributions contributions, std::size_t max_entities)
    : contributions_(std::move(contributions)), max_entities_(max_entities) {
    if (!contributions_.frozen())
        throw RecordError(ErrorCode::invalid_input, "Render contributions must be frozen");
}
void RenderProjector::clear() {
    view_.reset();
    point_indices_.clear();
    geometry_indices_.clear();
}
std::optional<RecordVersion> RenderProjector::version() const {
    if (!view_)
        return std::nullopt;
    return RecordVersion{view_->document, view_->model_revision};
}
RenderProjectionStats RenderProjector::stats() const noexcept {
    return stats_;
}
Result<RenderUpdate> RenderProjector::rebuild(const DocumentView& snapshot,
                                              const ViewSession& view) {
    RenderUpdate result;
    result.full.emplace();
    result.work.full_rebuilds = 1;
    auto& packet = *result.full;
    packet.document = view.document;
    packet.revision = view.model_revision;
    packet.view_session_id = view.id;
    packet.view_revision = view.view_revision;
    const std::set<EntityId> hidden(view.hidden_ids.begin(), view.hidden_ids.end());
    for (const auto& id : hidden)
        if (!snapshot.find_identity(id.value))
            return failure<RenderUpdate>(ErrorCode::entity_not_found,
                                         "Hidden display entity does not exist");
    std::map<EntityId, RenderPoint> points;
    std::vector<RenderLine2> lines;
    std::set<EntityId> needed;
    for (const auto& [type, contribution] : contributions_.entries()) {
        snapshot.visit(type, [&](const Record& record) {
            auto item = project(contribution, record, result.work);
            std::visit(
                [&](auto&& value) {
                    using T = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<T, RenderPoint>) {
                        value.visible = !hidden.contains(value.entity);
                        if (value.visible)
                            needed.insert(value.entity);
                        points.emplace(value.entity, std::move(value));
                    } else if constexpr (std::is_same_v<T, RenderLine2>) {
                        if (!hidden.contains(value.entity)) {
                            needed.insert(value.nodes.begin(), value.nodes.end());
                            lines.push_back(std::move(value));
                        }
                    } else if (!hidden.contains(value.entity)) {
                        packet.geometry_lines.push_back(std::move(value));
                    }
                },
                std::move(item));
        });
    }
    if (needed.size() + lines.size() + packet.geometry_lines.size() > max_entities_)
        return failure<RenderUpdate>(ErrorCode::resource_limit, "Render entity limit exceeded");
    std::map<std::string, std::size_t> point_indices, geometry_indices;
    for (const auto& id : needed) {
        auto it = points.find(id);
        if (it == points.end())
            return failure<RenderUpdate>(ErrorCode::entity_not_found,
                                         "Line endpoint is not a display point");
        point_indices.emplace(id.value, packet.points.size());
        packet.points.push_back(std::move(it->second));
    }
    for (auto& line : lines)
        packet.beams.push_back(
            {std::move(line.entity),
             {point_indices.at(line.nodes[0].value), point_indices.at(line.nodes[1].value)}});
    for (std::size_t i = 0; i < packet.geometry_lines.size(); ++i)
        geometry_indices.emplace(packet.geometry_lines[i].entity.value, i);
    result.work.dirty_blocks =
        (packet.points.size() + record_page_capacity - 1) / record_page_capacity;
    auto next_view = view;
    point_indices_.swap(point_indices);
    geometry_indices_.swap(geometry_indices);
    view_ = std::move(next_view);
    accumulate(stats_, result.work);
    return success(std::move(result));
}
Result<RenderUpdate> RenderProjector::update(const DocumentView& snapshot,
                                             const ViewSession& view,
                                             std::span<const DirectedRenderChange> changes,
                                             bool force_full) {
    try {
        if (!same_document(snapshot.version().document, view.document))
            return failure<RenderUpdate>(ErrorCode::document_epoch_expired,
                                         "Display view belongs to another document");
        if (snapshot.version().revision != view.model_revision)
            return failure<RenderUpdate>(ErrorCode::revision_conflict,
                                         "Display view model revision is stale");
        if (force_full || !view_ || !same_document(view_->document, view.document) ||
            view_->id != view.id || view_->hidden_ids != view.hidden_ids)
            return rebuild(snapshot, view);
        if (view.model_revision < view_->model_revision ||
            view.view_revision < view_->view_revision)
            return failure<RenderUpdate>(ErrorCode::revision_conflict,
                                         "Cannot rewind the installed display");
        auto next_revision = view_->model_revision;
        for (const auto& change : changes) {
            if (!change.changes || change.base_revision != next_revision ||
                change.revision != next_revision + 1)
                return failure<RenderUpdate>(ErrorCode::revision_conflict,
                                             "Display change history is not contiguous");
            next_revision = change.revision;
        }
        if (next_revision != view.model_revision)
            return failure<RenderUpdate>(ErrorCode::revision_conflict,
                                         "Display change history is incomplete");
        RenderUpdate result;
        result.delta = {view.document,
                        view_->model_revision,
                        view.model_revision,
                        view.id,
                        view_->view_revision,
                        view.view_revision,
                        {},
                        {}};
        const auto rebuild_after_projection = [&]() {
            auto rebuilt = rebuild(snapshot, view);
            if (rebuilt.ok()) {
                accumulate(stats_, result.work);
                accumulate(rebuilt.value->work, result.work);
            }
            return rebuilt;
        };
        std::map<std::size_t, RenderPoint> point_updates;
        std::map<std::size_t, RenderGeometryLine> geometry_updates;
        std::set<std::size_t> dirty_blocks;
        for (const auto& batch : changes) {
            for (const auto& change : batch.changes->records) {
                const auto* contribution = contributions_.find(change.key.type);
                if (!contribution)
                    continue;
                const auto& before =
                    batch.direction == RecordDirection::forward ? change.before : change.after;
                const auto& after =
                    batch.direction == RecordDirection::forward ? change.after : change.before;
                if (!before || !after)
                    return rebuild_after_projection();
                const auto old_item = project(*contribution, *before, result.work);
                auto item = project(*contribution, *after, result.work);
                if (old_item.index() != item.index())
                    return rebuild_after_projection();
                if (auto* point = std::get_if<RenderPoint>(&item)) {
                    if (point->position_mm == std::get<RenderPoint>(old_item).position_mm)
                        continue;
                    const auto it = point_indices_.find(point->entity.value);
                    if (it == point_indices_.end())
                        continue;
                    point->visible =
                        std::find(view.hidden_ids.begin(), view.hidden_ids.end(), point->entity) ==
                        view.hidden_ids.end();
                    point_updates.insert_or_assign(it->second, std::move(*point));
                    dirty_blocks.insert(it->second / record_page_capacity);
                } else if (const auto* line = std::get_if<RenderLine2>(&item)) {
                    if (line->nodes != std::get<RenderLine2>(old_item).nodes)
                        return rebuild_after_projection();
                } else {
                    auto& geometry = std::get<RenderGeometryLine>(item);
                    const auto& old_geometry = std::get<RenderGeometryLine>(old_item);
                    if (geometry.start_mm == old_geometry.start_mm &&
                        geometry.end_mm == old_geometry.end_mm)
                        continue;
                    const auto it = geometry_indices_.find(geometry.entity.value);
                    if (it != geometry_indices_.end())
                        geometry_updates.insert_or_assign(it->second, std::move(geometry));
                }
            }
        }
        for (auto& [index, point] : point_updates)
            result.delta.points.push_back({index, std::move(point)});
        for (auto& [index, line] : geometry_updates)
            result.delta.geometry_lines.push_back({index, std::move(line)});
        result.work.dirty_blocks = dirty_blocks.size();
        // The visibility set and view identity were checked above; preserve their
        // existing storage while advancing the disposable projection version.
        view_->model_revision = view.model_revision;
        view_->view_revision = view.view_revision;
        accumulate(stats_, result.work);
        return success(std::move(result));
    } catch (const RecordError& error) {
        return failure<RenderUpdate>(error.code(), error.what());
    }
}
} // namespace qcae
