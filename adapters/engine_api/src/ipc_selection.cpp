#include "qcae/ipc_selection.hpp"
#include "qcae/operation_inputs.hpp"
#include "typed_json.hpp"
#include <QJsonArray>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <variant>

namespace qcae::ipc {
namespace {
struct BadSelectionRequest : std::runtime_error {
    using std::runtime_error::runtime_error;
};
QString text(const std::string& value) {
    return QString::fromStdString(value);
}
QString number(std::uint64_t value) {
    return QString::number(value);
}
std::string string(const QJsonObject& object, const char* key, bool allow_empty = false) {
    const auto value = object.value(QLatin1String(key));
    if (!value.isString() || (!allow_empty && value.toString().isEmpty()))
        throw BadSelectionRequest(std::string("Expected string: ") + key);
    return value.toString().toStdString();
}
std::uint64_t integer(const QJsonObject& object, const char* key) {
    const auto value = string(object, key);
    std::uint64_t result{};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size())
        throw BadSelectionRequest(std::string("Expected unsigned integer string: ") + key);
    return result;
}
QJsonArray ids_json(const std::vector<EntityId>& ids) {
    QJsonArray result;
    for (const auto& id : ids)
        result.append(text(id.value));
    return result;
}
// The generated contract has already checked the expression's shape; this bounds its size.
QueryPredicate
predicate(const operations::SelectionPredicate& input, std::size_t depth, std::size_t& count) {
    if (depth > 16 || ++count > 128)
        throw BadSelectionRequest("Query expression exceeds limits");
    QueryPredicate result;
    const auto logical = [&](QueryOp op, const std::vector<operations::SelectionPredicate>& nodes) {
        result.op = op;
        for (const auto& child : nodes)
            result.children.push_back(predicate(child, depth + 1, count));
    };
    std::visit(
        [&](const auto& node) {
            using Node = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<Node, operations::SelectionPredicateAll>) {
                result.op = QueryOp::all;
            } else if constexpr (std::is_same_v<Node, operations::SelectionPredicateAnd>) {
                logical(QueryOp::and_, node.children);
            } else if constexpr (std::is_same_v<Node, operations::SelectionPredicateOr>) {
                logical(QueryOp::or_, node.children);
            } else if constexpr (std::is_same_v<Node, operations::SelectionPredicateNot>) {
                logical(QueryOp::not_, node.children);
            } else if constexpr (std::is_same_v<Node, operations::SelectionPredicateKind>) {
                result.op = QueryOp::kind;
                result.text = node.value;
            } else if constexpr (std::is_same_v<Node, operations::SelectionPredicateNameContains>) {
                result.op = QueryOp::name_contains;
                result.text = node.value;
            } else if constexpr (std::is_same_v<Node, operations::SelectionPredicateIds>) {
                result.op = QueryOp::ids;
                result.ids = node.ids;
            } else if constexpr (std::is_same_v<Node, operations::SelectionPredicateMemberOf>) {
                result.op = QueryOp::member_of;
                result.related_entity = node.entity_id;
                result.membership_kind = node.kind == "part"       ? MembershipKind::part
                                         : node.kind == "assembly" ? MembershipKind::assembly
                                         : node.kind == "set"      ? MembershipKind::set
                                                                   : MembershipKind::include;
            } else if constexpr (std::is_same_v<Node, operations::SelectionPredicateMaterial>) {
                result.op = QueryOp::material;
                result.related_entity = node.entity_id;
            } else if constexpr (std::is_same_v<Node, operations::SelectionPredicateSection>) {
                result.op = QueryOp::section;
                result.related_entity = node.entity_id;
            } else if constexpr (std::is_same_v<Node,
                                                operations::SelectionPredicateSourceNumberRange>) {
                result.op = QueryOp::source_number_range;
                result.text = node.namespace_;
                result.first_number = node.first;
                result.last_number = node.last;
            } else {
                static_assert(std::is_same_v<Node, operations::SelectionPredicateWorldBox>);
                result.op = QueryOp::world_box;
                result.box.minimum = {node.minimum[0], node.minimum[1], node.minimum[2]};
                result.box.maximum = {node.maximum[0], node.maximum[1], node.maximum[2]};
                result.box.relation =
                    node.relation == "contained" ? BoxRelation::contained : BoxRelation::intersects;
            }
        },
        input.value);
    return result;
}
QJsonObject view_json(const ViewSession& view) {
    return {{"view_session_id", text(view.id)},
            {"document_id", text(view.document.id.value)},
            {"document_epoch", text(view.document.epoch.value)},
            {"revision", number(view.model_revision)},
            {"view_revision", number(view.view_revision)},
            {"hidden_ids", ids_json(view.hidden_ids)},
            {"camera_fingerprint", text(view.camera_fingerprint)}};
}
QJsonObject handle_json(const SelectionHandle& handle) {
    return {{"selection_handle", text(handle.id)},
            {"document_id", text(handle.document.id.value)},
            {"document_epoch", text(handle.document.epoch.value)},
            {"revision", number(handle.model_revision)},
            {"view_session_id", text(handle.view_session_id)},
            {"view_revision", number(handle.view_revision)},
            {"count", static_cast<qint64>(handle.count)}};
}
template <class T, class Convert>
QJsonObject encode(const QString& id, const Result<T>& result, Convert convert) {
    if (!result.ok()) {
        if (!result.error)
            return failure(id, "INTERNAL_ERROR", "Missing diagnostic");
        return failure(id,
                       QString::fromLatin1(error_name(result.error->code)),
                       text(result.error->message),
                       QString::fromLatin1(status_name(result.status)));
    }
    auto data = convert(*result.value);
    return {{"request_id", id},
            {"status", "success"},
            {"revision", data.value("revision")},
            {"data", data}};
}
} // namespace
std::optional<QJsonObject> dispatch_selection(MemoryApplication& app,
                                              SelectionService& selections,
                                              const QJsonObject& request,
                                              const Caller& caller) {
    const auto op = request.value("operation").toString();
    const auto id = request.value("request_id").toString();
    if (!std::set<QString>{"view.create",
                           "view.update",
                           "view.render_data",
                           "selection.evaluate",
                           "selection.combine",
                           "selection.get"}
             .contains(op))
        return std::nullopt;
    try {
        const DocumentRef ref{DocumentId{string(request, "document_id")},
                              DocumentEpoch{string(request, "document_epoch")}};
        const auto snap = app.record_application().snapshot(ref);
        if (!snap.ok())
            return encode(id, snap, [](const auto&) { return QJsonObject{}; });
        const auto& snapshot = snap.value->records;
        if (op != "selection.get" &&
            integer(request, "expected_revision") != snapshot.version().revision)
            return failure(id, "REVISION_CONFLICT", "Model changed", "conflict");
        const auto p = request.value("parameters").toObject();
        if (op == "view.create") {
            const auto input = detail::typed_input<operations::ViewCreateInput>(p);
            if (!input.ok())
                return detail::typed_input_failure(id, input.status, *input.error);
            return encode(
                id,
                selections.create_view(snapshot,
                                       caller,
                                       input.value->hidden_ids.value_or(std::vector<EntityId>{}),
                                       input.value->camera_fingerprint.value_or(std::string{})),
                view_json);
        }
        if (op == "selection.get") {
            const auto input = detail::typed_input<operations::SelectionGetInput>(p);
            if (!input.ok())
                return detail::typed_input_failure(id, input.status, *input.error);
            const auto offset = input.value->offset.value_or(0);
            const auto limit = input.value->limit.value_or(1000);
            if (offset > 1000000 || limit > 1000000)
                throw BadSelectionRequest("Invalid page bounds");
            return encode(id,
                          selections.page(snapshot,
                                          caller,
                                          input.value->selection_handle,
                                          static_cast<std::size_t>(offset),
                                          static_cast<std::size_t>(limit)),
                          [](const SelectionPage& page) {
                              auto result = handle_json(page.handle);
                              result.insert("entity_ids", ids_json(page.ids));
                              result.insert("offset", static_cast<qint64>(page.offset));
                              return result;
                          });
        }
        // The remaining operations address an existing view; a stated view revision must match it.
        const auto checked_view = [&](const std::string& view_id,
                                      std::optional<std::uint64_t> expected,
                                      bool inspect) -> std::optional<QJsonObject> {
            const auto view = inspect ? selections.inspect_view(snapshot, caller, view_id)
                                      : selections.get_view(snapshot, caller, view_id);
            if (!view.ok())
                return encode(id, view, view_json);
            if (expected && *expected != view.value->view_revision)
                return failure(id, "REVISION_CONFLICT", "View changed", "conflict");
            return std::nullopt;
        };
        if (op == "view.update") {
            const auto input = detail::typed_input<operations::ViewUpdateInput>(p);
            if (!input.ok())
                return detail::typed_input_failure(id, input.status, *input.error);
            if (auto rejected = checked_view(
                    input.value->view_session_id, input.value->expected_view_revision, true))
                return *rejected;
            return encode(id,
                          selections.update_view(snapshot,
                                                 caller,
                                                 input.value->view_session_id,
                                                 input.value->hidden_ids,
                                                 input.value->camera_fingerprint),
                          view_json);
        }
        if (op == "view.render_data") {
            const auto input = detail::typed_input<operations::ViewRenderDataInput>(p);
            if (!input.ok())
                return detail::typed_input_failure(id, input.status, *input.error);
            if (auto rejected = checked_view(
                    input.value->view_session_id, input.value->expected_view_revision, false))
                return *rejected;
            return encode(
                id,
                selections.render_packet(snapshot, caller, input.value->view_session_id),
                [](const RenderPacket& packet) {
                    QJsonArray points, beams, geometry_lines;
                    for (const auto& point : packet.points)
                        points.append(QJsonObject{{"entity_id", text(point.entity.value)},
                                                  {"position_mm",
                                                   QJsonArray{point.position_mm[0],
                                                              point.position_mm[1],
                                                              point.position_mm[2]}},
                                                  {"visible", point.visible}});
                    for (const auto& beam : packet.beams)
                        beams.append(
                            QJsonObject{{"entity_id", text(beam.entity.value)},
                                        {"points",
                                         QJsonArray{static_cast<qint64>(beam.points[0]),
                                                    static_cast<qint64>(beam.points[1])}}});
                    for (const auto& line : packet.geometry_lines)
                        geometry_lines.append(QJsonObject{
                            {"entity_id", text(line.entity.value)},
                            {"start_mm",
                             QJsonArray{line.start_mm[0], line.start_mm[1], line.start_mm[2]}},
                            {"end_mm",
                             QJsonArray{line.end_mm[0], line.end_mm[1], line.end_mm[2]}}});
                    return QJsonObject{{"document_id", text(packet.document.id.value)},
                                       {"document_epoch", text(packet.document.epoch.value)},
                                       {"revision", number(packet.revision)},
                                       {"view_session_id", text(packet.view_session_id)},
                                       {"view_revision", number(packet.view_revision)},
                                       {"points", points},
                                       {"beams", beams},
                                       {"geometry_lines", geometry_lines}};
                });
        }
        if (op == "selection.combine") {
            const auto input = detail::typed_input<operations::SelectionCombineInput>(p);
            if (!input.ok())
                return detail::typed_input_failure(id, input.status, *input.error);
            if (auto rejected = checked_view(
                    input.value->view_session_id, input.value->expected_view_revision, false))
                return *rejected;
            const auto& operation = input.value->operator_;
            return encode(id,
                          selections.combine(snapshot,
                                             caller,
                                             input.value->view_session_id,
                                             input.value->left,
                                             input.value->right,
                                             operation == "union" ? SetOperation::union_
                                             : operation == "intersection"
                                                 ? SetOperation::intersection
                                                 : SetOperation::difference),
                          handle_json);
        }
        const auto input = detail::typed_input<operations::SelectionEvaluateInput>(p);
        if (!input.ok())
            return detail::typed_input_failure(id, input.status, *input.error);
        const auto& view_id = input.value->view_session_id;
        if (auto rejected = checked_view(view_id, input.value->expected_view_revision, false))
            return *rejected;
        std::size_t count = 0;
        QuerySpec query;
        query.predicate = predicate(input.value->predicate, 0, count);
        const auto& scope = input.value->scope;
        if (scope.candidate_ids)
            query.scope.candidate_ids = *scope.candidate_ids;
        query.scope.include_hidden = scope.include_hidden.value_or(false);
        query.scope.invert = scope.invert.value_or(false);
        const auto visibility = scope.visibility.value_or("through");
        query.scope.visibility = visibility == "visible_only" ? VisibilityMode::visible_only
                                 : visibility == "picker_candidates"
                                     ? VisibilityMode::picker_candidates
                                     : VisibilityMode::through;
        return encode(id, selections.select(snapshot, caller, view_id, query), handle_json);
    } catch (const BadSelectionRequest& error) {
        return failure(id, "INVALID_INPUT", QString::fromUtf8(error.what()));
    }
}
} // namespace qcae::ipc
