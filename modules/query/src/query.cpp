#include "qcae/query.hpp"
#include "qcae/records.hpp"

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

struct EntityLess {
    using is_transparent = void;
    bool operator()(const EntityId& a, const EntityId& b) const {
        return a.value < b.value;
    }
    bool operator()(const EntityId& a, std::string_view b) const {
        return a.value < b;
    }
    bool operator()(std::string_view a, const EntityId& b) const {
        return a < b.value;
    }
};
using IdSet = std::set<EntityId, EntityLess>;

bool valid_ids(const DocumentView& snapshot, const std::vector<EntityId>& ids) {
    return std::all_of(ids.begin(), ids.end(), [&](const EntityId& id) {
        auto record = snapshot.find_identity(id.value);
        return record && !record->descriptor().query_kind.empty();
    });
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
    explicit QueryContext(const DocumentView& snapshot) : snapshot_(snapshot) {}
    bool matches(std::string_view id, const QueryPredicate& predicate, unsigned depth = 0) const {
        if (depth > 32)
            return false;
        const auto record = snapshot_.find_identity(id);
        if (!record || record->descriptor().query_kind.empty())
            return false;
        switch (predicate.op) {
        case QueryOp::all:
            return true;
        case QueryOp::kind:
            return record->descriptor().query_kind == predicate.text;
        case QueryOp::ids:
            return std::any_of(predicate.ids.begin(), predicate.ids.end(), [&](const auto& value) {
                return value.value == id;
            });
        case QueryOp::name_contains:
            return (record->descriptor().display_name
                        ? record->descriptor().display_name(record->object())
                        : std::string_view{})
                       .find(predicate.text) != std::string_view::npos;
        case QueryOp::source_number_range:
        case QueryOp::member_of:
        case QueryOp::material:
        case QueryOp::section:
            return cached(predicate).contains(id);
        case QueryOp::world_box:
            return spatial_match(record, predicate.box);
        case QueryOp::and_:
            return std::all_of(predicate.children.begin(),
                               predicate.children.end(),
                               [&](const auto& child) { return matches(id, child, depth + 1); });
        case QueryOp::or_:
            return std::any_of(predicate.children.begin(),
                               predicate.children.end(),
                               [&](const auto& child) { return matches(id, child, depth + 1); });
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
            if (!query_kind_registered(snapshot_, predicate.text))
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
            if (!snapshot_.find<records::Material>(predicate.related_entity))
                return false;
            break;
        case QueryOp::section:
            if (!snapshot_.find<records::BeamSection>(predicate.related_entity))
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
                           [&](const auto& child) { return valid(child, depth + 1); });
    }

  private:
    template <class T, class Visitor> void visit(Visitor visitor) const {
        snapshot_.visit(RecordTraits<T>::type_id,
                        [&](const Record& record) { visitor(record->get<T>()); });
    }
    bool organization_exists(const QueryPredicate& p) const {
        switch (p.membership_kind) {
        case MembershipKind::part:
            return bool(snapshot_.find<records::Part>(p.related_entity));
        case MembershipKind::assembly:
            return bool(snapshot_.find<records::Assembly>(p.related_entity));
        case MembershipKind::set:
            return bool(snapshot_.find<records::EntitySet>(p.related_entity));
        case MembershipKind::include:
            return bool(snapshot_.find<records::IncludeDocument>(p.related_entity));
        }
        return false;
    }
    const IdSet& cached(const QueryPredicate& p) const {
        auto [found, fresh] = cache_.try_emplace(&p);
        if (!fresh)
            return found->second;
        auto& result = found->second;
        if (p.op == QueryOp::source_number_range) {
            visit<records::SourceIdentifier>([&](const auto& source) {
                if ((p.text.empty() || source.name_space == p.text) &&
                    source.number >= p.first_number && source.number <= p.last_number)
                    result.insert(source.entity);
            });
            return result;
        }
        if (p.op == QueryOp::material || p.op == QueryOp::section) {
            result.insert(p.related_entity);
            IdSet sections;
            if (p.op == QueryOp::section)
                sections.insert(p.related_entity);
            else
                visit<records::BeamSection>([&](const auto& section) {
                    if (section.material == p.related_entity) {
                        sections.insert(section.id);
                        result.insert(section.id);
                    }
                });
            visit<records::Beam>([&](const auto& beam) {
                if (beam.section && sections.contains(*beam.section)) {
                    result.insert(beam.id);
                    result.insert(beam.nodes.begin(), beam.nodes.end());
                }
            });
            return result;
        }
        if (p.membership_kind == MembershipKind::set) {
            const auto& set =
                snapshot_.find<records::EntitySet>(p.related_entity)->get<records::EntitySet>();
            result.insert(set.members.begin(), set.members.end());
            return result;
        }
        if (p.membership_kind == MembershipKind::include) {
            IdSet includes{p.related_entity};
            bool changed = true;
            while (changed) {
                changed = false;
                visit<records::IncludeDocument>([&](const auto& item) {
                    if (item.parent && includes.contains(*item.parent))
                        changed |= includes.insert(item.id).second;
                });
            }
            visit<records::IncludeDocument>([&](const auto& item) {
                if (includes.contains(item.id))
                    result.insert(item.members.begin(), item.members.end());
            });
            return result;
        }
        IdSet parts;
        if (p.membership_kind == MembershipKind::part)
            parts.insert(p.related_entity);
        else {
            IdSet assemblies{p.related_entity};
            std::vector<EntityId> pending{p.related_entity};
            while (!pending.empty()) {
                auto id = pending.back();
                pending.pop_back();
                const auto& assembly =
                    snapshot_.find<records::Assembly>(id)->get<records::Assembly>();
                for (const auto& child : assembly.children) {
                    if (snapshot_.find<records::Assembly>(child)) {
                        if (assemblies.insert(child).second)
                            pending.push_back(child);
                    } else
                        parts.insert(child);
                }
            }
        }
        for (const auto& id : parts) {
            const auto record = snapshot_.find<records::Part>(id);
            if (record) {
                const auto& part = record->get<records::Part>();
                result.insert(part.members.begin(), part.members.end());
            }
        }
        visit<records::Beam>([&](const auto& beam) {
            if (result.contains(beam.id))
                result.insert(beam.nodes.begin(), beam.nodes.end());
        });
        return result;
    }
    bool spatial_match(const Record& record, const WorldBox& box) const {
        auto point = [](const std::array<double, 3>& p) { return Vec3{p[0], p[1], p[2]}; };
        if (record->key().type == RecordTraits<records::Node>::type_id)
            return inside(point(record->get<records::Node>().position), box);
        if (record->key().type == RecordTraits<records::Beam>::type_id) {
            const auto& beam = record->get<records::Beam>();
            const auto a = snapshot_.find<records::Node>(beam.nodes[0]);
            const auto b = snapshot_.find<records::Node>(beam.nodes[1]);
            if (!a || !b)
                return false;
            const auto first = point(a->get<records::Node>().position),
                       second = point(b->get<records::Node>().position);
            return box.relation == BoxRelation::contained
                       ? (inside(first, box) && inside(second, box))
                       : segment_intersects_box(first, second, box);
        }
        if (record->key().type == RecordTraits<records::GeometryLine>::type_id) {
            const auto& line = record->get<records::GeometryLine>();
            const auto first = point(line.start), second = point(line.end);
            return box.relation == BoxRelation::contained
                       ? (inside(first, box) && inside(second, box))
                       : segment_intersects_box(first, second, box);
        }
        return false;
    }
    const DocumentView& snapshot_;
    mutable std::map<const QueryPredicate*, IdSet> cache_;
};

} // namespace

Result<QueryResult>
execute_query(const DocumentView& snapshot, const ViewSession& view, const QuerySpec& spec) {
    if (!same_document(snapshot.version().document, view.document))
        return failure<QueryResult>(
            ErrorCode::document_epoch_expired, "View belongs to another document or epoch", "view");
    if (snapshot.version().revision != view.model_revision)
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
    if (!valid_ids(snapshot, view.hidden_ids) ||
        (spec.scope.candidate_ids && !valid_ids(snapshot, *spec.scope.candidate_ids)))
        return failure<QueryResult>(
            ErrorCode::entity_not_found, "View or candidate contains an unknown entity", "scope");
    const IdSet hidden(view.hidden_ids.begin(), view.hidden_ids.end());
    IdSet selected;
    auto consider = [&](std::string_view id) {
        if ((spec.scope.include_hidden || !hidden.contains(id)) &&
            context.matches(id, spec.predicate) != spec.scope.invert)
            selected.emplace(std::string(id));
    };
    if (spec.scope.candidate_ids) {
        for (const auto& id : *spec.scope.candidate_ids)
            consider(id.value);
    } else
        snapshot.visit([&](const Record& record) {
            if (!record->descriptor().query_kind.empty())
                consider(record->key().identity);
        });
    return success(QueryResult{snapshot.version().document,
                               snapshot.version().revision,
                               view.id,
                               view.view_revision,
                               sorted_ids(std::move(selected))});
}

Result<RenderPacket> produce_render_packet(const DocumentView& snapshot,
                                           const ViewSession& view,
                                           std::size_t max_entities) {
    if (!same_document(snapshot.version().document, view.document))
        return failure<RenderPacket>(
            ErrorCode::document_epoch_expired, "View belongs to another document or epoch", "view");
    if (snapshot.version().revision != view.model_revision)
        return failure<RenderPacket>(
            ErrorCode::revision_conflict, "View model revision is stale", "view");
    if (!valid_ids(snapshot, view.hidden_ids))
        return failure<RenderPacket>(ErrorCode::entity_not_found,
                                     "View contains an unknown hidden entity",
                                     "view.hidden_ids");
    const IdSet hidden(view.hidden_ids.begin(), view.hidden_ids.end());
    IdSet needed;
    snapshot.visit(RecordTraits<records::Node>::type_id, [&](const Record& record) {
        const auto& node = record->get<records::Node>();
        if (!hidden.contains(node.id))
            needed.insert(node.id);
    });
    std::size_t visible_beams = 0;
    snapshot.visit(RecordTraits<records::Beam>::type_id, [&](const Record& record) {
        const auto& beam = record->get<records::Beam>();
        if (!hidden.contains(beam.id)) {
            ++visible_beams;
            needed.insert(beam.nodes.begin(), beam.nodes.end());
        }
    });
    std::size_t visible_geometry_lines = 0;
    snapshot.visit(RecordTraits<records::GeometryLine>::type_id, [&](const Record& record) {
        if (!hidden.contains(record->key().identity))
            ++visible_geometry_lines;
    });
    if (needed.size() + visible_beams + visible_geometry_lines > max_entities)
        return failure<RenderPacket>(
            ErrorCode::resource_limit, "Render packet entity limit exceeded", "max_entities");
    RenderPacket packet;
    packet.document = snapshot.version().document;
    packet.revision = snapshot.version().revision;
    packet.view_session_id = view.id;
    packet.view_revision = view.view_revision;
    std::map<EntityId, std::size_t> indices;
    snapshot.visit(RecordTraits<records::Node>::type_id, [&](const Record& record) {
        const auto& node = record->get<records::Node>();
        if (needed.contains(node.id)) {
            indices.emplace(node.id, packet.points.size());
            packet.points.push_back({node.id, node.position, !hidden.contains(node.id)});
        }
    });
    bool missing = false;
    snapshot.visit(RecordTraits<records::Beam>::type_id, [&](const Record& record) {
        const auto& beam = record->get<records::Beam>();
        if (!hidden.contains(beam.id)) {
            auto a = indices.find(beam.nodes[0]), b = indices.find(beam.nodes[1]);
            if (a == indices.end() || b == indices.end())
                missing = true;
            else
                packet.beams.push_back({beam.id, {a->second, b->second}});
        }
    });
    if (missing)
        return failure<RenderPacket>(
            ErrorCode::entity_not_found, "Visible beam has a missing endpoint", "beam.nodes");
    snapshot.visit(RecordTraits<records::GeometryLine>::type_id, [&](const Record& record) {
        const auto& line = record->get<records::GeometryLine>();
        if (!hidden.contains(line.id.value))
            packet.geometry_lines.push_back({EntityId(line.id.value), line.start, line.end});
    });
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

Result<ViewSession> SelectionService::create_view(const DocumentView& snapshot,
                                                  const Caller& caller,
                                                  std::vector<EntityId> hidden_ids,
                                                  std::string camera_fingerprint) {
    if (caller.principal.empty())
        return failure<ViewSession>(ErrorCode::invalid_input, "Caller is required", "caller");
    if (!valid_ids(snapshot, hidden_ids))
        return failure<ViewSession>(
            ErrorCode::entity_not_found, "Hidden entity does not exist", "hidden_ids");
    // A single engine has one active document. Its older document/epoch views
    // cannot be used and must not consume the live session quota indefinitely.
    for (auto item = views_.begin(); item != views_.end();) {
        if (!same_document(item->second.view.document, snapshot.version().document)) {
            remember_expired_view(item->first);
            item = views_.erase(item);
        } else {
            ++item;
        }
    }
    for (auto item = selections_.begin(); item != selections_.end();) {
        if (!same_document(item->second.handle.document, snapshot.version().document)) {
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
                     snapshot.version().document,
                     snapshot.version().revision,
                     1,
                     std::move(hidden_ids),
                     std::move(camera_fingerprint)};
    views_.emplace(view.id, OwnedView{view, caller.principal});
    return success(std::move(view));
}

Result<ViewSession> SelectionService::checked_view(const DocumentView& snapshot,
                                                   const Caller& caller,
                                                   const std::string& view_id) const {
    auto inspected = inspect_view(snapshot, caller, view_id);
    if (!inspected.ok())
        return inspected;
    if (inspected.value->model_revision != snapshot.version().revision)
        return failure<ViewSession>(
            ErrorCode::revision_conflict, "View model revision is stale", "view");
    return inspected;
}

Result<ViewSession> SelectionService::get_view(const DocumentView& snapshot,
                                               const Caller& caller,
                                               const std::string& view_id) const {
    return checked_view(snapshot, caller, view_id);
}

Result<ViewSession> SelectionService::inspect_view(const DocumentView& snapshot,
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
    if (!same_document(found->second.view.document, snapshot.version().document))
        return failure<ViewSession>(
            ErrorCode::document_epoch_expired, "View belongs to another document or epoch", "view");
    return success(found->second.view);
}

Result<ViewSession> SelectionService::update_view(const DocumentView& snapshot,
                                                  const Caller& caller,
                                                  const std::string& view_id,
                                                  std::vector<EntityId> hidden_ids,
                                                  std::string camera_fingerprint) {
    auto inspected = inspect_view(snapshot, caller, view_id);
    if (!inspected.ok())
        return inspected;
    if (!valid_ids(snapshot, hidden_ids))
        return failure<ViewSession>(
            ErrorCode::entity_not_found, "Hidden entity does not exist", "hidden_ids");
    std::sort(hidden_ids.begin(), hidden_ids.end());
    hidden_ids.erase(std::unique(hidden_ids.begin(), hidden_ids.end()), hidden_ids.end());
    auto& view = views_.at(view_id).view;
    if (view.model_revision != snapshot.version().revision || view.hidden_ids != hidden_ids ||
        view.camera_fingerprint != camera_fingerprint) {
        view.model_revision = snapshot.version().revision;
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

Result<SelectionHandle> SelectionService::select(const DocumentView& snapshot,
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

Result<SelectionHandle> SelectionService::combine(const DocumentView& snapshot,
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

Result<SelectionPage> SelectionService::page(const DocumentView& snapshot,
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

Result<RenderPacket> SelectionService::render_packet(const DocumentView& snapshot,
                                                     const Caller& caller,
                                                     const std::string& view_id) const {
    auto view = checked_view(snapshot, caller, view_id);
    if (!view.ok())
        return failure<RenderPacket>(view.error->code, view.error->message, view.error->field);
    return produce_render_packet(snapshot, *view.value, limits_.max_packet_entities);
}

} // namespace qcae
