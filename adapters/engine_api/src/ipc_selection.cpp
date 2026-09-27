#include "qcae/ipc_selection.hpp"
#include <QJsonArray>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <set>
#include <stdexcept>

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
void fields(const QJsonObject& object, std::initializer_list<const char*> allowed) {
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (std::none_of(allowed.begin(), allowed.end(), [&](const auto name) {
                return it.key() == QLatin1String(name);
            }))
            throw BadSelectionRequest("Unexpected field: " + it.key().toStdString());
    }
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
bool boolean(const QJsonObject& object, const char* key, bool fallback = false) {
    if (!object.contains(key))
        return fallback;
    if (!object.value(key).isBool())
        throw BadSelectionRequest(std::string("Expected boolean: ") + key);
    return object.value(key).toBool();
}
std::vector<EntityId> entity_ids(const QJsonObject& object, const char* key) {
    if (!object.value(key).isArray())
        throw BadSelectionRequest(std::string("Expected ID array: ") + key);
    std::vector<EntityId> result;
    for (const auto& value : object.value(key).toArray()) {
        if (!value.isString() || value.toString().isEmpty())
            throw BadSelectionRequest("Invalid entity ID");
        result.emplace_back(value.toString().toStdString());
    }
    return result;
}
QJsonArray ids_json(const std::vector<EntityId>& ids) {
    QJsonArray result;
    for (const auto& id : ids)
        result.append(text(id.value));
    return result;
}
Vec3 vector(const QJsonObject& object, const char* key) {
    if (!object.value(key).isArray() || object.value(key).toArray().size() != 3)
        throw BadSelectionRequest("Expected coordinate triple");
    const auto array = object.value(key).toArray();
    for (const auto& value : array)
        if (!value.isDouble() || !std::isfinite(value.toDouble()))
            throw BadSelectionRequest("Invalid coordinate");
    return {array[0].toDouble(), array[1].toDouble(), array[2].toDouble()};
}
QueryPredicate predicate(const QJsonObject& object, std::size_t depth, std::size_t& count) {
    if (depth > 16 || ++count > 128)
        throw BadSelectionRequest("Query expression exceeds limits");
    const auto op = string(object, "op");
    QueryPredicate result;
    if (op == "all") {
        fields(object, {"op"});
        result.op = QueryOp::all;
    } else if (op == "and" || op == "or" || op == "not") {
        fields(object, {"op", "children"});
        if (!object.value("children").isArray())
            throw BadSelectionRequest("Logical query requires children");
        result.op = op == "and" ? QueryOp::and_ : op == "or" ? QueryOp::or_ : QueryOp::not_;
        for (const auto& child : object.value("children").toArray()) {
            if (!child.isObject())
                throw BadSelectionRequest("Query child must be an object");
            result.children.push_back(predicate(child.toObject(), depth + 1, count));
        }
    } else if (op == "kind" || op == "name_contains") {
        fields(object, {"op", "value"});
        result.op = op == "kind" ? QueryOp::kind : QueryOp::name_contains;
        result.text = string(object, "value");
    } else if (op == "ids") {
        fields(object, {"op", "ids"});
        result.op = QueryOp::ids;
        result.ids = entity_ids(object, "ids");
    } else if (op == "member_of") {
        fields(object, {"op", "kind", "entity_id"});
        result.op = QueryOp::member_of;
        result.related_entity = EntityId{string(object, "entity_id")};
        const auto kind = string(object, "kind");
        if (kind == "part")
            result.membership_kind = MembershipKind::part;
        else if (kind == "assembly")
            result.membership_kind = MembershipKind::assembly;
        else if (kind == "set")
            result.membership_kind = MembershipKind::set;
        else if (kind == "include")
            result.membership_kind = MembershipKind::include;
        else
            throw BadSelectionRequest("Invalid membership kind");
    } else if (op == "material" || op == "section") {
        fields(object, {"op", "entity_id"});
        result.op = op == "material" ? QueryOp::material : QueryOp::section;
        result.related_entity = EntityId{string(object, "entity_id")};
    } else if (op == "source_number_range") {
        fields(object, {"op", "namespace", "first", "last"});
        result.op = QueryOp::source_number_range;
        result.text = string(object, "namespace");
        result.first_number = integer(object, "first");
        result.last_number = integer(object, "last");
    } else if (op == "world_box") {
        fields(object, {"op", "minimum", "maximum", "relation"});
        result.op = QueryOp::world_box;
        result.box.minimum = vector(object, "minimum");
        result.box.maximum = vector(object, "maximum");
        const auto relation = string(object, "relation");
        if (relation == "contained")
            result.box.relation = BoxRelation::contained;
        else if (relation == "intersects")
            result.box.relation = BoxRelation::intersects;
        else
            throw BadSelectionRequest("Invalid box relation");
    } else
        throw BadSelectionRequest("Unknown query operation");
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
std::size_t page_index(const QJsonObject& parameters, const char* name, std::size_t fallback) {
    if (!parameters.contains(name))
        return fallback;
    const auto value = parameters.value(name);
    const auto n = value.toDouble(-1);
    if (!value.isDouble() || n < 0 || n > 1000000 || std::floor(n) != n)
        throw BadSelectionRequest("Invalid page bounds");
    return static_cast<std::size_t>(n);
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
            fields(p, {"hidden_ids", "camera_fingerprint"});
            return encode(id,
                          selections.create_view(snapshot,
                                                 caller,
                                                 p.contains("hidden_ids")
                                                     ? entity_ids(p, "hidden_ids")
                                                     : std::vector<EntityId>{},
                                                 p.contains("camera_fingerprint")
                                                     ? string(p, "camera_fingerprint", true)
                                                     : std::string{}),
                          view_json);
        }
        if (op == "selection.get") {
            fields(p, {"selection_handle", "offset", "limit"});
            return encode(id,
                          selections.page(snapshot,
                                          caller,
                                          string(p, "selection_handle"),
                                          page_index(p, "offset", 0),
                                          page_index(p, "limit", 1000)),
                          [](const SelectionPage& page) {
                              auto result = handle_json(page.handle);
                              result.insert("entity_ids", ids_json(page.ids));
                              result.insert("offset", static_cast<qint64>(page.offset));
                              return result;
                          });
        }
        const auto view_id = string(p, "view_session_id");
        const auto view = op == "view.update" ? selections.inspect_view(snapshot, caller, view_id)
                                              : selections.get_view(snapshot, caller, view_id);
        if (!view.ok())
            return encode(id, view, view_json);
        if ((op != "view.render_data" || p.contains("expected_view_revision")) &&
            integer(p, "expected_view_revision") != view.value->view_revision)
            return failure(id, "REVISION_CONFLICT", "View changed", "conflict");
        if (op == "view.update") {
            fields(
                p,
                {"view_session_id", "expected_view_revision", "hidden_ids", "camera_fingerprint"});
            return encode(id,
                          selections.update_view(snapshot,
                                                 caller,
                                                 view_id,
                                                 entity_ids(p, "hidden_ids"),
                                                 string(p, "camera_fingerprint", true)),
                          view_json);
        }
        if (op == "view.render_data") {
            fields(p, {"view_session_id", "expected_view_revision"});
            return encode(
                id,
                selections.render_packet(snapshot, caller, view_id),
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
            fields(p, {"view_session_id", "expected_view_revision", "left", "right", "operator"});
            const auto operation = string(p, "operator");
            if (operation != "union" && operation != "intersection" && operation != "difference")
                throw BadSelectionRequest("Invalid set operator");
            return encode(id,
                          selections.combine(snapshot,
                                             caller,
                                             view_id,
                                             string(p, "left"),
                                             string(p, "right"),
                                             operation == "union" ? SetOperation::union_
                                             : operation == "intersection"
                                                 ? SetOperation::intersection
                                                 : SetOperation::difference),
                          handle_json);
        }
        fields(p, {"view_session_id", "expected_view_revision", "predicate", "scope"});
        if (!p.value("predicate").isObject() || !p.value("scope").isObject())
            throw BadSelectionRequest("Predicate and scope must be objects");
        std::size_t count = 0;
        QuerySpec query;
        query.predicate = predicate(p.value("predicate").toObject(), 0, count);
        const auto scope = p.value("scope").toObject();
        fields(scope, {"candidate_ids", "include_hidden", "invert", "visibility"});
        if (scope.contains("candidate_ids"))
            query.scope.candidate_ids = entity_ids(scope, "candidate_ids");
        query.scope.include_hidden = boolean(scope, "include_hidden");
        query.scope.invert = boolean(scope, "invert");
        const auto visibility =
            scope.contains("visibility") ? string(scope, "visibility") : "through";
        if (visibility == "through")
            query.scope.visibility = VisibilityMode::through;
        else if (visibility == "visible_only")
            query.scope.visibility = VisibilityMode::visible_only;
        else if (visibility == "picker_candidates")
            query.scope.visibility = VisibilityMode::picker_candidates;
        else
            throw BadSelectionRequest("Unknown visibility mode");
        return encode(id, selections.select(snapshot, caller, view_id, query), handle_json);
    } catch (const BadSelectionRequest& error) {
        return failure(id, "INVALID_INPUT", QString::fromUtf8(error.what()));
    }
}
} // namespace qcae::ipc
