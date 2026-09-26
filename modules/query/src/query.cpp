#include "qcae/query.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iterator>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <utility>

namespace qcae {
namespace {

template <class T> Result<T> success(T value) {
    return {Status::success, std::move(value), std::nullopt};
}

template <class T> Result<T> failure(ErrorCode code, std::string message, std::string field) {
    const Status status =
        code == ErrorCode::revision_conflict || code == ErrorCode::document_epoch_expired
            ? Status::conflict
            : Status::failed;
    return {status, std::nullopt, Diagnostic{code, std::move(message), std::move(field)}};
}

std::string random_nonce() {
    std::random_device random;
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (int index = 0; index < 4; ++index)
        output << std::setw(8) << random();
    return output.str();
}

bool same_document(const DocumentRef& left, const DocumentRef& right) {
    return left.id == right.id && left.epoch == right.epoch;
}

using IdSet = std::set<EntityId>;

IdSet entity_ids(const ModelSnapshot& snapshot) {
    IdSet ids;
    for (const auto& entity : model_entities(snapshot))
        ids.insert(entity.id);
    return ids;
}

bool valid_ids(const IdSet& available, const std::vector<EntityId>& ids) {
    return std::all_of(
        ids.begin(), ids.end(), [&](const EntityId& id) { return available.contains(id); });
}

std::vector<EntityId> sorted_ids(IdSet ids) {
    return {std::make_move_iterator(ids.begin()), std::make_move_iterator(ids.end())};
}

bool within(double value, double lower, double upper) {
    return lower <= value && value <= upper;
}

bool inside(const Vec3& point, const WorldBox& box) {
    return within(point.x, box.minimum.x, box.maximum.x) &&
           within(point.y, box.minimum.y, box.maximum.y) &&
           within(point.z, box.minimum.z, box.maximum.z);
}

bool segment_intersects_box(const Vec3& start, const Vec3& end, const WorldBox& box) {
    double enter = 0.0;
    double leave = 1.0;
    const double starts[] = {start.x, start.y, start.z};
    const double ends[] = {end.x, end.y, end.z};
    const double minimum[] = {box.minimum.x, box.minimum.y, box.minimum.z};
    const double maximum[] = {box.maximum.x, box.maximum.y, box.maximum.z};
    for (int axis = 0; axis < 3; ++axis) {
        const double delta = ends[axis] - starts[axis];
        if (delta == 0.0) {
            if (!within(starts[axis], minimum[axis], maximum[axis]))
                return false;
            continue;
        }
        const double first = (minimum[axis] - starts[axis]) / delta;
        const double second = (maximum[axis] - starts[axis]) / delta;
        enter = std::max(enter, std::min(first, second));
        leave = std::min(leave, std::max(first, second));
        if (enter > leave)
            return false;
    }
    return true;
}

bool box_valid(const WorldBox& box) {
    return std::isfinite(box.minimum.x) && std::isfinite(box.minimum.y) &&
           std::isfinite(box.minimum.z) && std::isfinite(box.maximum.x) &&
           std::isfinite(box.maximum.y) && std::isfinite(box.maximum.z) &&
           box.minimum.x <= box.maximum.x && box.minimum.y <= box.maximum.y &&
           box.minimum.z <= box.maximum.z;
}

class QueryContext {
  public:
    explicit QueryContext(const ModelSnapshot& snapshot) : snapshot_(snapshot) {
        for (const auto& entity : model_entities(snapshot))
            entities_.emplace(entity.id, entity);
        for (const auto& node : snapshot.nodes)
            nodes_.emplace(node.id, node.position);
        for (const auto& section : snapshot.sections)
            sections_.emplace(section.id, section.material);
    }

    IdSet all() const {
        IdSet result;
        for (const auto& [id, unused] : entities_) {
            (void)unused;
            result.insert(id);
        }
        return result;
    }

    bool matches(const EntityId& id, const QueryPredicate& predicate, unsigned depth = 0) const {
        if (depth > 32)
            return false;
        const auto entity = entities_.find(id);
        if (entity == entities_.end())
            return false;
        switch (predicate.op) {
        case QueryOp::all:
            return true;
        case QueryOp::kind:
            return entity->second.kind == predicate.text;
        case QueryOp::ids:
            return std::find(predicate.ids.begin(), predicate.ids.end(), id) != predicate.ids.end();
        case QueryOp::name_contains:
            return entity->second.name.find(predicate.text) != std::string::npos;
        case QueryOp::source_number_range:
            return source_matches(id, predicate);
        case QueryOp::member_of:
            return members(predicate).contains(id);
        case QueryOp::material:
            return related_to_material(id, predicate.related_entity);
        case QueryOp::section:
            return related_to_section(id, predicate.related_entity);
        case QueryOp::world_box:
            return spatial_match(id, predicate.box);
        case QueryOp::and_:
            return std::all_of(
                predicate.children.begin(),
                predicate.children.end(),
                [&](const QueryPredicate& child) { return matches(id, child, depth + 1); });
        case QueryOp::or_:
            return std::any_of(
                predicate.children.begin(),
                predicate.children.end(),
                [&](const QueryPredicate& child) { return matches(id, child, depth + 1); });
        case QueryOp::not_:
            return predicate.children.size() == 1 &&
                   !matches(id, predicate.children.front(), depth + 1);
        }
        return false;
    }

    bool valid(const QueryPredicate& predicate, unsigned depth = 0) const {
        if (depth > 32 || predicate.children.size() > 64)
            return false;
        switch (predicate.op) {
        case QueryOp::kind:
            if (predicate.text.empty())
                return false;
            break;
        case QueryOp::source_number_range:
            if (predicate.first_number > predicate.last_number)
                return false;
            break;
        case QueryOp::member_of:
            if (!organization_exists(predicate))
                return false;
            break;
        case QueryOp::material:
            if (std::none_of(
                    snapshot_.materials.begin(),
                    snapshot_.materials.end(),
                    [&](const Material& item) { return item.id == predicate.related_entity; }))
                return false;
            break;
        case QueryOp::section:
            if (!sections_.contains(predicate.related_entity))
                return false;
            break;
        case QueryOp::world_box:
            if (!box_valid(predicate.box))
                return false;
            break;
        case QueryOp::not_:
            if (predicate.children.size() != 1)
                return false;
            break;
        case QueryOp::and_:
        case QueryOp::or_:
            if (predicate.children.empty())
                return false;
            break;
        default:
            break;
        }
        return std::all_of(predicate.children.begin(),
                           predicate.children.end(),
                           [&](const QueryPredicate& child) { return valid(child, depth + 1); });
    }

  private:
    bool source_matches(const EntityId& id, const QueryPredicate& predicate) const {
        return std::any_of(snapshot_.sources.begin(),
                           snapshot_.sources.end(),
                           [&](const SourceIdentifier& source) {
                               return source.entity == id &&
                                      (predicate.text.empty() ||
                                       source.name_space == predicate.text) &&
                                      source.number >= predicate.first_number &&
                                      source.number <= predicate.last_number;
                           });
    }

    bool organization_exists(const QueryPredicate& predicate) const {
        switch (predicate.membership_kind) {
        case MembershipKind::part:
            return std::any_of(
                snapshot_.parts.begin(), snapshot_.parts.end(), [&](const Part& item) {
                    return item.id == predicate.related_entity;
                });
        case MembershipKind::assembly:
            return std::any_of(
                snapshot_.assemblies.begin(),
                snapshot_.assemblies.end(),
                [&](const Assembly& item) { return item.id == predicate.related_entity; });
        case MembershipKind::set:
            return std::any_of(
                snapshot_.sets.begin(), snapshot_.sets.end(), [&](const EntitySet& item) {
                    return item.id == predicate.related_entity;
                });
        case MembershipKind::include:
            return std::any_of(
                snapshot_.includes.begin(),
                snapshot_.includes.end(),
                [&](const IncludeDocument& item) { return item.id == predicate.related_entity; });
        }
        return false;
    }

    IdSet members(const QueryPredicate& predicate) const {
        IdSet result;
        if (predicate.membership_kind == MembershipKind::set) {
            for (const auto& item : snapshot_.sets)
                if (item.id == predicate.related_entity)
                    result.insert(item.members.begin(), item.members.end());
            return result;
        }
        if (predicate.membership_kind == MembershipKind::include) {
            IdSet includes{predicate.related_entity};
            bool changed = true;
            while (changed) {
                changed = false;
                for (const auto& item : snapshot_.includes)
                    if (item.parent && includes.contains(*item.parent))
                        changed |= includes.insert(item.id).second;
            }
            for (const auto& item : snapshot_.includes)
                if (includes.contains(item.id))
                    result.insert(item.members.begin(), item.members.end());
            return result;
        }
        IdSet parts;
        if (predicate.membership_kind == MembershipKind::part) {
            parts.insert(predicate.related_entity);
        } else {
            IdSet assemblies{predicate.related_entity};
            bool changed = true;
            while (changed) {
                changed = false;
                for (const auto& assembly : snapshot_.assemblies) {
                    if (!assemblies.contains(assembly.id))
                        continue;
                    for (const auto& child : assembly.children) {
                        const auto entity = entities_.find(child);
                        if (entity == entities_.end())
                            continue;
                        if (entity->second.kind == "assembly")
                            changed |= assemblies.insert(child).second;
                        else
                            parts.insert(child);
                    }
                }
            }
        }
        for (const auto& part : snapshot_.parts)
            if (parts.contains(part.id))
                result.insert(part.members.begin(), part.members.end());
        // Part and assembly views include endpoints required by their beams.
        for (const auto& beam : snapshot_.beams)
            if (result.contains(beam.id))
                result.insert(beam.nodes.begin(), beam.nodes.end());
        return result;
    }

    bool related_to_section(const EntityId& id, const EntityId& section) const {
        if (id == section)
            return true;
        for (const auto& beam : snapshot_.beams) {
            if (beam.section != section)
                continue;
            if (beam.id == id || beam.nodes[0] == id || beam.nodes[1] == id)
                return true;
        }
        return false;
    }

    bool related_to_material(const EntityId& id, const EntityId& material) const {
        if (id == material)
            return true;
        for (const auto& [section, owner] : sections_)
            if (owner == material && related_to_section(id, section))
                return true;
        return false;
    }

    bool spatial_match(const EntityId& id, const WorldBox& box) const {
        const auto node = nodes_.find(id);
        if (node != nodes_.end())
            return inside(node->second, box);
        for (const auto& beam : snapshot_.beams) {
            if (beam.id != id)
                continue;
            const auto first = nodes_.find(beam.nodes[0]);
            const auto second = nodes_.find(beam.nodes[1]);
            if (first == nodes_.end() || second == nodes_.end())
                return false;
            if (box.relation == BoxRelation::contained)
                return inside(first->second, box) && inside(second->second, box);
            return segment_intersects_box(first->second, second->second, box);
        }
        return false;
    }

    const ModelSnapshot& snapshot_;
    std::map<EntityId, EntitySummary> entities_;
    std::map<EntityId, Vec3> nodes_;
    std::map<EntityId, EntityId> sections_;
};

} // namespace

Result<QueryResult>
execute_query(const ModelSnapshot& snapshot, const ViewSession& view, const QuerySpec& spec) {
    if (!same_document(snapshot.info.document, view.document))
        return failure<QueryResult>(
            ErrorCode::document_epoch_expired, "View belongs to another document or epoch", "view");
    if (snapshot.info.revision != view.model_revision)
        return failure<QueryResult>(
            ErrorCode::revision_conflict, "View model revision is stale", "view");
    if (spec.scope.visibility == VisibilityMode::visible_only)
        return failure<QueryResult>(ErrorCode::unsupported_capability,
                                    "Visible-only selection requires a visibility query provider",
                                    "scope.visibility");
    if (spec.scope.visibility == VisibilityMode::picker_candidates && !spec.scope.candidate_ids)
        return failure<QueryResult>(ErrorCode::missing_input,
                                    "Picker candidates must be supplied by the graphical host",
                                    "scope.candidate_ids");
    QueryContext context(snapshot);
    if (!context.valid(spec.predicate))
        return failure<QueryResult>(
            ErrorCode::invalid_input, "Invalid query predicate", "predicate");
    const IdSet available = context.all();
    if (!valid_ids(available, view.hidden_ids) ||
        (spec.scope.candidate_ids && !valid_ids(available, *spec.scope.candidate_ids)))
        return failure<QueryResult>(
            ErrorCode::entity_not_found, "View or candidate contains an unknown entity", "scope");
    IdSet universe = spec.scope.candidate_ids
                         ? IdSet(spec.scope.candidate_ids->begin(), spec.scope.candidate_ids->end())
                         : available;
    if (!spec.scope.include_hidden) {
        for (const auto& hidden : view.hidden_ids)
            universe.erase(hidden);
    }
    IdSet selected;
    for (const auto& id : universe)
        if (context.matches(id, spec.predicate) != spec.scope.invert)
            selected.insert(id);
    return success(QueryResult{snapshot.info.document,
                               snapshot.info.revision,
                               view.id,
                               view.view_revision,
                               sorted_ids(std::move(selected))});
}

Result<RenderPacket> produce_render_packet(const ModelSnapshot& snapshot,
                                           const ViewSession& view,
                                           std::size_t max_entities) {
    if (!same_document(snapshot.info.document, view.document))
        return failure<RenderPacket>(
            ErrorCode::document_epoch_expired, "View belongs to another document or epoch", "view");
    if (snapshot.info.revision != view.model_revision)
        return failure<RenderPacket>(
            ErrorCode::revision_conflict, "View model revision is stale", "view");
    if (!valid_ids(entity_ids(snapshot), view.hidden_ids))
        return failure<RenderPacket>(ErrorCode::entity_not_found,
                                     "View contains an unknown hidden entity",
                                     "view.hidden_ids");
    const IdSet hidden(view.hidden_ids.begin(), view.hidden_ids.end());
    IdSet needed;
    for (const auto& node : snapshot.nodes)
        if (!hidden.contains(node.id))
            needed.insert(node.id);
    for (const auto& beam : snapshot.beams)
        if (!hidden.contains(beam.id))
            needed.insert(beam.nodes.begin(), beam.nodes.end());
    std::size_t visible_beams = 0;
    for (const auto& beam : snapshot.beams)
        visible_beams += !hidden.contains(beam.id);
    if (needed.size() + visible_beams > max_entities)
        return failure<RenderPacket>(
            ErrorCode::resource_limit, "Render packet entity limit exceeded", "max_entities");
    RenderPacket packet;
    packet.document = snapshot.info.document;
    packet.revision = snapshot.info.revision;
    packet.view_session_id = view.id;
    packet.view_revision = view.view_revision;
    std::map<EntityId, std::size_t> indices;
    for (const auto& node : snapshot.nodes) {
        if (!needed.contains(node.id))
            continue;
        indices.emplace(node.id, packet.points.size());
        packet.points.push_back({node.id,
                                 {node.position.x, node.position.y, node.position.z},
                                 !hidden.contains(node.id)});
    }
    for (const auto& beam : snapshot.beams) {
        if (hidden.contains(beam.id))
            continue;
        const auto first = indices.find(beam.nodes[0]);
        const auto second = indices.find(beam.nodes[1]);
        if (first == indices.end() || second == indices.end())
            return failure<RenderPacket>(
                ErrorCode::entity_not_found, "Visible beam has a missing endpoint", "beam.nodes");
        packet.beams.push_back({beam.id, {first->second, second->second}});
    }
    return success(std::move(packet));
}

SelectionService::SelectionService(SelectionLimits limits)
    : limits_(limits), nonce_(random_nonce()) {}

void SelectionService::remember_expired_view(const std::string& id) {
    expired_views_.insert(id);
    expired_view_order_.push_back(id);
    if (expired_view_order_.size() > limits_.max_views) {
        expired_views_.erase(expired_view_order_.front());
        expired_view_order_.pop_front();
    }
}

void SelectionService::remember_expired_selection(const std::string& id) {
    expired_selections_.insert(id);
    expired_selection_order_.push_back(id);
    if (expired_selection_order_.size() > limits_.max_handles) {
        expired_selections_.erase(expired_selection_order_.front());
        expired_selection_order_.pop_front();
    }
}

Result<ViewSession> SelectionService::create_view(const ModelSnapshot& snapshot,
                                                  const Caller& caller,
                                                  std::vector<EntityId> hidden_ids,
                                                  std::string camera_fingerprint) {
    if (caller.principal.empty())
        return failure<ViewSession>(ErrorCode::invalid_input, "Caller is required", "caller");
    if (!valid_ids(entity_ids(snapshot), hidden_ids))
        return failure<ViewSession>(
            ErrorCode::entity_not_found, "Hidden entity does not exist", "hidden_ids");
    // A single engine has one active document. Its older document/epoch views
    // cannot be used and must not consume the live session quota indefinitely.
    for (auto item = views_.begin(); item != views_.end();) {
        if (!same_document(item->second.view.document, snapshot.info.document)) {
            remember_expired_view(item->first);
            item = views_.erase(item);
        } else {
            ++item;
        }
    }
    for (auto item = selections_.begin(); item != selections_.end();) {
        if (!same_document(item->second.handle.document, snapshot.info.document)) {
            remember_expired_selection(item->first);
            item = selections_.erase(item);
        } else {
            ++item;
        }
    }
    if (views_.size() >= limits_.max_views)
        return failure<ViewSession>(
            ErrorCode::resource_limit, "View session limit exceeded", "views");
    std::sort(hidden_ids.begin(), hidden_ids.end());
    hidden_ids.erase(std::unique(hidden_ids.begin(), hidden_ids.end()), hidden_ids.end());
    ViewSession view{"view-" + nonce_ + "-" + std::to_string(next_id_++),
                     snapshot.info.document,
                     snapshot.info.revision,
                     1,
                     std::move(hidden_ids),
                     std::move(camera_fingerprint)};
    views_.emplace(view.id, OwnedView{view, caller.principal});
    return success(std::move(view));
}

Result<ViewSession> SelectionService::checked_view(const ModelSnapshot& snapshot,
                                                   const Caller& caller,
                                                   const std::string& view_id) const {
    auto inspected = inspect_view(snapshot, caller, view_id);
    if (!inspected.ok())
        return inspected;
    if (inspected.value->model_revision != snapshot.info.revision)
        return failure<ViewSession>(
            ErrorCode::revision_conflict, "View model revision is stale", "view");
    return inspected;
}

Result<ViewSession> SelectionService::get_view(const ModelSnapshot& snapshot,
                                               const Caller& caller,
                                               const std::string& view_id) const {
    return checked_view(snapshot, caller, view_id);
}

Result<ViewSession> SelectionService::inspect_view(const ModelSnapshot& snapshot,
                                                   const Caller& caller,
                                                   const std::string& view_id) const {
    const auto found = views_.find(view_id);
    if (found == views_.end())
        return expired_views_.contains(view_id)
                   ? failure<ViewSession>(ErrorCode::document_epoch_expired,
                                          "View document or epoch expired",
                                          "view_id")
                   : failure<ViewSession>(
                         ErrorCode::entity_not_found, "View does not exist", "view_id");
    if (found->second.owner != caller.principal)
        return failure<ViewSession>(
            ErrorCode::invalid_input, "View belongs to another caller", "caller");
    if (!same_document(found->second.view.document, snapshot.info.document))
        return failure<ViewSession>(
            ErrorCode::document_epoch_expired, "View belongs to another document or epoch", "view");
    return success(found->second.view);
}

Result<ViewSession> SelectionService::update_view(const ModelSnapshot& snapshot,
                                                  const Caller& caller,
                                                  const std::string& view_id,
                                                  std::vector<EntityId> hidden_ids,
                                                  std::string camera_fingerprint) {
    auto inspected = inspect_view(snapshot, caller, view_id);
    if (!inspected.ok())
        return inspected;
    if (!valid_ids(entity_ids(snapshot), hidden_ids))
        return failure<ViewSession>(
            ErrorCode::entity_not_found, "Hidden entity does not exist", "hidden_ids");
    std::sort(hidden_ids.begin(), hidden_ids.end());
    hidden_ids.erase(std::unique(hidden_ids.begin(), hidden_ids.end()), hidden_ids.end());
    auto& view = views_.at(view_id).view;
    if (view.model_revision != snapshot.info.revision || view.hidden_ids != hidden_ids ||
        view.camera_fingerprint != camera_fingerprint) {
        view.model_revision = snapshot.info.revision;
        view.hidden_ids = std::move(hidden_ids);
        view.camera_fingerprint = std::move(camera_fingerprint);
        ++view.view_revision;
    }
    return success(view);
}

Result<SelectionHandle> SelectionService::store_selection(const ViewSession& view,
                                                          const Caller& caller,
                                                          std::vector<EntityId> ids) {
    if (limits_.max_handles == 0)
        return failure<SelectionHandle>(
            ErrorCode::resource_limit, "Selection handle limit exceeded", "selections");
    if (selections_.size() >= limits_.max_handles) {
        for (auto item = selections_.begin(); item != selections_.end();) {
            const auto live_view = views_.find(item->second.handle.view_session_id);
            const bool stale = live_view == views_.end() || live_view->second.view.view_revision !=
                                                                item->second.handle.view_revision;
            if (stale)
                item = selections_.erase(item);
            else
                ++item;
        }
    }
    if (selections_.size() >= limits_.max_handles) {
        const auto oldest = std::min_element(
            selections_.begin(), selections_.end(), [](const auto& left, const auto& right) {
                return left.second.order < right.second.order;
            });
        selections_.erase(oldest);
    }
    SelectionHandle handle{"selection-" + nonce_ + "-" + std::to_string(next_id_++),
                           view.document,
                           view.model_revision,
                           view.id,
                           view.view_revision,
                           ids.size()};
    selections_.emplace(handle.id,
                        StoredSelection{handle, caller.principal, std::move(ids), next_id_});
    return success(std::move(handle));
}

Result<SelectionHandle> SelectionService::select(const ModelSnapshot& snapshot,
                                                 const Caller& caller,
                                                 const std::string& view_id,
                                                 const QuerySpec& spec) {
    auto view = checked_view(snapshot, caller, view_id);
    if (!view.ok())
        return failure<SelectionHandle>(view.error->code, view.error->message, view.error->field);
    auto query = execute_query(snapshot, *view.value, spec);
    if (!query.ok())
        return failure<SelectionHandle>(
            query.error->code, query.error->message, query.error->field);
    return store_selection(*view.value, caller, std::move(query.value->ids));
}

Result<SelectionHandle> SelectionService::combine(const ModelSnapshot& snapshot,
                                                  const Caller& caller,
                                                  const std::string& view_id,
                                                  const std::string& left_handle,
                                                  const std::string& right_handle,
                                                  SetOperation operation) {
    auto view = checked_view(snapshot, caller, view_id);
    if (!view.ok())
        return failure<SelectionHandle>(view.error->code, view.error->message, view.error->field);
    const auto left = selections_.find(left_handle);
    const auto right = selections_.find(right_handle);
    if (left == selections_.end() || right == selections_.end())
        return failure<SelectionHandle>(expired_selections_.contains(left_handle) ||
                                                expired_selections_.contains(right_handle)
                                            ? ErrorCode::document_epoch_expired
                                            : ErrorCode::entity_not_found,
                                        "Selection handle does not exist or expired",
                                        "handle");
    auto compatible = [&](const StoredSelection& selection) {
        return selection.owner == caller.principal &&
               same_document(selection.handle.document, view.value->document) &&
               selection.handle.model_revision == view.value->model_revision &&
               selection.handle.view_session_id == view.value->id &&
               selection.handle.view_revision == view.value->view_revision;
    };
    if (!compatible(left->second) || !compatible(right->second))
        return failure<SelectionHandle>(
            ErrorCode::revision_conflict,
            "Selection handle is stale or belongs to another view/caller",
            "handle");
    const IdSet a(left->second.ids.begin(), left->second.ids.end());
    const IdSet b(right->second.ids.begin(), right->second.ids.end());
    IdSet output;
    switch (operation) {
    case SetOperation::intersection:
        std::set_intersection(
            a.begin(), a.end(), b.begin(), b.end(), std::inserter(output, output.end()));
        break;
    case SetOperation::union_:
        std::set_union(a.begin(), a.end(), b.begin(), b.end(), std::inserter(output, output.end()));
        break;
    case SetOperation::difference:
        std::set_difference(
            a.begin(), a.end(), b.begin(), b.end(), std::inserter(output, output.end()));
        break;
    }
    return store_selection(*view.value, caller, sorted_ids(std::move(output)));
}

Result<SelectionPage> SelectionService::page(const ModelSnapshot& snapshot,
                                             const Caller& caller,
                                             const std::string& handle_id,
                                             std::size_t offset,
                                             std::size_t limit) const {
    const auto found = selections_.find(handle_id);
    if (found == selections_.end())
        return failure<SelectionPage>(expired_selections_.contains(handle_id)
                                          ? ErrorCode::document_epoch_expired
                                          : ErrorCode::entity_not_found,
                                      "Selection handle does not exist or expired",
                                      "handle_id");
    const auto view = checked_view(snapshot, caller, found->second.handle.view_session_id);
    if (!view.ok())
        return failure<SelectionPage>(view.error->code, view.error->message, view.error->field);
    const auto& selection = found->second;
    if (selection.owner != caller.principal ||
        selection.handle.view_revision != view.value->view_revision)
        return failure<SelectionPage>(ErrorCode::revision_conflict,
                                      "Selection handle is stale or belongs to another caller",
                                      "handle_id");
    if (limit > limits_.max_page_size)
        return failure<SelectionPage>(
            ErrorCode::resource_limit, "Selection page limit exceeded", "limit");
    const auto begin = std::min(offset, selection.ids.size());
    const auto end = begin + std::min(limit, selection.ids.size() - begin);
    return success(SelectionPage{selection.handle,
                                 offset,
                                 {selection.ids.begin() + static_cast<std::ptrdiff_t>(begin),
                                  selection.ids.begin() + static_cast<std::ptrdiff_t>(end)}});
}

Result<RenderPacket> SelectionService::render_packet(const ModelSnapshot& snapshot,
                                                     const Caller& caller,
                                                     const std::string& view_id) const {
    auto view = checked_view(snapshot, caller, view_id);
    if (!view.ok())
        return failure<RenderPacket>(view.error->code, view.error->message, view.error->field);
    return produce_render_packet(snapshot, *view.value, limits_.max_packet_entities);
}

} // namespace qcae
