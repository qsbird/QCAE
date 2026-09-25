#include "qcae/ipc_model.hpp"
#include <QJsonArray>
#include <QUuid>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>

namespace qcae::ipc {
namespace {
QString qs(const std::string& s) {
    return QString::fromStdString(s);
}
struct BadInput : std::runtime_error {
    using std::runtime_error::runtime_error;
};
void fields(const QJsonObject& o, std::initializer_list<const char*> allowed) {
    for (auto i = o.begin(); i != o.end(); ++i) {
        if (std::none_of(allowed.begin(), allowed.end(), [&](const char* s) {
                return i.key() == QLatin1String(s);
            }))
            throw BadInput("Unexpected field: " + i.key().toStdString());
    }
}
std::string str(const QJsonObject& o, const char* key, bool empty = false) {
    const auto v = o.value(QLatin1String(key));
    if (!v.isString() || (!empty && v.toString().trimmed().isEmpty()))
        throw BadInput(std::string("Expected string: ") + key);
    return v.toString().toStdString();
}
DocumentRef ref(const QJsonObject& r) {
    return {DocumentId{str(r, "document_id")}, DocumentEpoch{str(r, "document_epoch")}};
}
WriteContext ctx(const QJsonObject& r) {
    const auto s = str(r, "expected_revision");
    Revision rev{};
    const auto [p, e] = std::from_chars(s.data(), s.data() + s.size(), rev);
    if (e != std::errc{} || p != s.data() + s.size())
        throw BadInput("Invalid expected_revision");
    return {ref(r), rev};
}
std::vector<EntityId> ids(const QJsonObject& o, const char* key) {
    const auto v = o.value(QLatin1String(key));
    if (!v.isArray())
        throw BadInput(std::string("Expected entity array: ") + key);
    std::vector<EntityId> out;
    for (const auto& item : v.toArray()) {
        if (!item.isString() || item.toString().isEmpty())
            throw BadInput("Invalid entity ID");
        out.emplace_back(item.toString().toStdString());
    }
    return out;
}
QJsonArray id_json(const std::vector<EntityId>& list) {
    QJsonArray out;
    for (const auto& i : list)
        out.append(qs(i.value));
    return out;
}
ProfileRef profile_ref(const QJsonObject& o, const char* key) {
    if (!o.value(QLatin1String(key)).isObject())
        throw BadInput(std::string("Expected profile reference: ") + key);
    const auto p = o.value(QLatin1String(key)).toObject();
    fields(p, {"profile_id", "profile_version", "definition_digest"});
    return {str(p, "profile_id"), str(p, "profile_version"), str(p, "definition_digest")};
}
QJsonObject profile_ref_json(const ProfileRef& p) {
    return {{"profile_id", qs(p.profile_id)},
            {"profile_version", qs(p.profile_version)},
            {"definition_digest", qs(p.definition_digest)}};
}
QJsonArray issues_json(const std::vector<FormatIssue>& issues) {
    QJsonArray out;
    for (const auto& i : issues)
        out.append(QJsonObject{{"code", qs(i.code)},
                               {"message", qs(i.message)},
                               {"resource", qs(i.resource)},
                               {"line", static_cast<qint64>(i.line)},
                               {"blocking", i.blocking}});
    return out;
}
QJsonObject ok(const QString& id, QJsonObject data, Revision rev) {
    data.insert("revision", QString::number(rev));
    return {{"request_id", id},
            {"status", "success"},
            {"revision", QString::number(rev)},
            {"data", data}};
}
template <typename T> QJsonObject error(const QString& id, const Result<T>& r) {
    const auto& e = *r.error;
    auto out = failure(id,
                       QString::fromLatin1(error_name(e.code)),
                       qs(e.message),
                       QString::fromLatin1(status_name(r.status)));
    auto detail = out.value("error").toObject();
    detail.insert("field", qs(e.field));
    out.insert("error", detail);
    return out;
}
QJsonObject preview_json(const ChangePreview& p) {
    return {{"preview_id", qs(p.id.value)},
            {"affected_entity_id", qs(p.affected_entity.value)},
            {"creates_entity", p.creates_entity},
            {"revision", QString::number(p.context.expected_revision)}};
}
Vec3 vec(const QJsonObject& o, const char* key) {
    const auto v = o.value(QLatin1String(key));
    if (!v.isArray() || v.toArray().size() != 3)
        throw BadInput("Position must contain three finite numbers in mm");
    const auto a = v.toArray();
    for (const auto& item : a)
        if (!item.isDouble() || !std::isfinite(item.toDouble()))
            throw BadInput("Invalid position number");
    return {a[0].toDouble(), a[1].toDouble(), a[2].toDouble()};
}
QJsonArray vec_json(const Vec3& v) {
    return {v.x, v.y, v.z};
}
QJsonObject entity_json(const EntitySummary& e, const Model& m) {
    QJsonObject o{{"entity_id", qs(e.id.value)}, {"kind", qs(e.kind)}, {"name", qs(e.name)}};
    for (const auto& n : m.nodes)
        if (n.id == e.id)
            o.insert("position_mm", vec_json(n.position));
    for (const auto& b : m.beams)
        if (b.id == e.id) {
            o.insert("nodes", QJsonArray{qs(b.nodes[0].value), qs(b.nodes[1].value)});
            o.insert("section_id", qs(b.section.value));
            o.insert("orientation", vec_json(b.orientation));
        }
    for (const auto& s : m.sections)
        if (s.id == e.id) {
            o.insert("material_id", qs(s.material.value));
            o.insert("area_mm2", s.area_mm2);
            o.insert("i1_mm4", s.i1_mm4);
            o.insert("i2_mm4", s.i2_mm4);
            o.insert("torsion_mm4", s.torsion_mm4);
        }
    for (const auto& v : m.materials)
        if (v.id == e.id) {
            o.insert("young_modulus_mpa", v.young_modulus_mpa);
            if (v.poisson_ratio)
                o.insert("poisson_ratio", *v.poisson_ratio);
        }
    for (const auto& v : m.parts)
        if (v.id == e.id)
            o.insert("members", id_json(v.members));
    for (const auto& v : m.sets)
        if (v.id == e.id)
            o.insert("members", id_json(v.members));
    for (const auto& v : m.assemblies)
        if (v.id == e.id)
            o.insert("children", id_json(v.children));
    for (const auto& v : m.includes)
        if (v.id == e.id) {
            o.insert("path", qs(v.path));
            o.insert("members", id_json(v.members));
            if (v.parent)
                o.insert("parent_id", qs(v.parent->value));
        }
    for (const auto& v : m.forces)
        if (v.id == e.id) {
            o.insert("node_id", qs(v.node.value));
            o.insert("force_n", vec_json(v.force_n));
        }
    for (const auto& v : m.constraints)
        if (v.id == e.id) {
            o.insert("nodes", id_json(v.nodes));
            o.insert("dofs", qs(v.dofs));
        }
    for (const auto& v : m.analyses)
        if (v.id == e.id) {
            o.insert("profile_ref", profile_ref_json(v.target.profile));
            o.insert("forces", id_json(v.forces));
            o.insert("constraints", id_json(v.constraints));
        }
    QJsonArray sources;
    for (const auto& s : m.sources)
        if (s.entity == e.id)
            sources.append(QJsonObject{{"source_model_id", qs(s.source_model_id)},
                                       {"namespace", qs(s.name_space)},
                                       {"number", QString::number(s.number)},
                                       {"include_id", qs(s.include.value)}});
    o.insert("sources", sources);
    return o;
}
} // namespace
QJsonObject profile_json(const ProfileDefinition& p) {
    return {{"profile_ref", profile_ref_json(p.reference)},
            {"solver_family", qs(p.solver_family)},
            {"analysis_kind", qs(p.analysis_kind)},
            {"configured", p.configured},
            {"validated", p.validated},
            {"codec_available", true},
            {"unit_system", "mm-N-MPa"},
            {"codec_scope", "strict free-field SOL 101 beam subset; no solver binary validation"},
            {"cards", QJsonArray{"GRID", "CBAR", "PBAR", "MAT1", "FORCE", "SPC1", "INCLUDE"}}};
}

std::optional<QJsonObject> dispatch_model(MemoryApplication& app,
                                          const QJsonObject& r,
                                          const Caller& caller,
                                          const IModelCodec* codec,
                                          const ProfileDefinition* profile) {
    const auto id = r.value("request_id").toString();
    const auto op = r.value("operation").toString();
    const auto p = r.value("parameters").toObject();
    const auto command = p.value("command").toString();
    const bool edit =
        op == "changes.preview" &&
        (command == "model.import" || command == "part.upsert" || command == "assembly.upsert" ||
         command == "set.upsert" || command == "node.move" || command == "entity.delete");
    if (!edit && op != "entity.query" && op != "entity.references" && op != "model.export_preview")
        return std::nullopt;
    try {
        const auto snap = app.snapshot(ref(r));
        if (!snap.ok())
            return error(id, snap);
        const auto& m = *snap.value;
        if (edit || op == "model.export_preview") {
            if (ctx(r).expected_revision != m.info.revision)
                return failure(
                    id, "REVISION_CONFLICT", "Document revision has changed", "conflict");
        }
        if (edit && command == "model.import") {
            fields(p,
                   {"command", "root_resource", "resources", "source_profile_ref", "unit_system"});
            if (!codec || !profile)
                return failure(id, "UNSUPPORTED_CAPABILITY", "No model codec registered");
            ImportRequest input;
            input.root_resource = str(p, "root_resource");
            input.source_profile = profile_ref(p, "source_profile_ref");
            input.unit_system = str(p, "unit_system");
            input.source_model_id =
                QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
            if (!p.value("resources").isArray())
                throw BadInput("resources must be an array");
            for (const auto& resource : p.value("resources").toArray()) {
                if (!resource.isObject())
                    throw BadInput("Invalid resource");
                const auto item = resource.toObject();
                fields(item, {"path", "text"});
                input.resources.push_back({str(item, "path"), str(item, "text", true)});
            }
            const auto imported = codec->decode(input);
            const QJsonObject report{
                {"complete", imported.report.complete},
                {"profile_ref", profile_ref_json(imported.report.profile)},
                {"supported_records", static_cast<qint64>(imported.report.supported_records)},
                {"issues", issues_json(imported.report.issues)}};
            if (!imported.candidate) {
                auto out = failure(
                    id, "IMPORT_REJECTED", "Input does not match the controlled format subset");
                out.insert("report", report);
                return out;
            }
            const auto preview = app.preview_import(caller, ctx(r), *imported.candidate);
            if (!preview.ok())
                return error(id, preview);
            auto data = preview_json(*preview.value);
            data.insert("import_report", report);
            data.insert("entity_count",
                        static_cast<qint64>(model_entities(*imported.candidate).size()));
            return ok(id, data, m.info.revision);
        }
        if (edit) {
            ModelEdit action;
            const auto entity_id =
                p.contains("entity_id") ? EntityId{str(p, "entity_id")} : EntityId{};
            if (command == "part.upsert") {
                fields(p, {"command", "entity_id", "name", "members"});
                action = UpsertPart{{entity_id, str(p, "name"), ids(p, "members")}};
            }
            if (command == "assembly.upsert") {
                fields(p, {"command", "entity_id", "name", "children"});
                action = UpsertAssembly{{entity_id, str(p, "name"), ids(p, "children")}};
            }
            if (command == "set.upsert") {
                fields(p, {"command", "entity_id", "name", "members"});
                action = UpsertSet{{entity_id, str(p, "name"), ids(p, "members")}};
            }
            if (command == "node.move") {
                fields(p, {"command", "entity_id", "position_mm"});
                action = MoveNode{EntityId{str(p, "entity_id")}, vec(p, "position_mm")};
            }
            if (command == "entity.delete") {
                fields(p, {"command", "entity_id"});
                action = DeleteEntity{EntityId{str(p, "entity_id")}};
            }
            const auto preview = app.preview_edit(caller, ctx(r), action);
            if (!preview.ok())
                return error(id, preview);
            auto data = preview_json(*preview.value);
            data.insert("affected_analyses",
                        id_json(affected_analyses(m, preview.value->affected_entity)));
            return ok(id, data, m.info.revision);
        }
        if (op == "model.export_preview") {
            fields(p, {"analysis_id", "expected_profile_ref"});
            if (!codec)
                return failure(id, "UNSUPPORTED_CAPABILITY", "No model codec registered");
            const auto encoded = codec->encode(
                m, EntityId{str(p, "analysis_id")}, profile_ref(p, "expected_profile_ref"));
            const QJsonObject report{{"complete", encoded.report.complete},
                                     {"profile_ref", profile_ref_json(encoded.report.profile)},
                                     {"issues", issues_json(encoded.report.issues)}};
            if (!encoded.artifact) {
                auto out = failure(
                    id, "EXPORT_REJECTED", "Model is not exportable to the selected target");
                out.insert("report", report);
                return out;
            }
            QJsonArray resources, mapping;
            for (const auto& resource : encoded.artifact->resources)
                resources.append(
                    QJsonObject{{"path", qs(resource.path)}, {"text", qs(resource.text)}});
            for (const auto& item : encoded.artifact->identities)
                mapping.append(QJsonObject{{"entity_id", qs(item.entity.value)},
                                           {"namespace", qs(item.name_space)},
                                           {"number", QString::number(item.number)}});
            return ok(id,
                      {{"root_resource", qs(encoded.artifact->root_resource)},
                       {"resources", resources},
                       {"identities", mapping},
                       {"export_report", report},
                       {"analysis_id", qs(encoded.artifact->analysis.value)},
                       {"profile_ref", profile_ref_json(encoded.artifact->profile)},
                       {"document_id", qs(m.info.document.id.value)},
                       {"document_epoch", qs(m.info.document.epoch.value)},
                       {"content_state", qs(m.info.content_state)},
                       {"published", false}},
                      m.info.revision);
        }
        const auto entities = model_entities(m);
        const auto references = model_references(m);
        std::map<EntityId, EntitySummary> by_id;
        for (const auto& e : entities)
            by_id.emplace(e.id, e);
        if (op == "entity.references") {
            fields(p, {"entity_id", "direction"});
            const EntityId selected{str(p, "entity_id")};
            if (!by_id.contains(selected))
                return failure(id, "ENTITY_NOT_FOUND", "Unknown entity");
            const auto direction = p.contains("direction") ? str(p, "direction") : "incoming";
            if (direction != "incoming" && direction != "outgoing")
                throw BadInput("direction must be incoming or outgoing");
            QJsonArray rows;
            for (const auto& v : references)
                if ((direction == "incoming" ? v.to : v.from) == selected)
                    rows.append(QJsonObject{
                        {"from", qs(v.from.value)}, {"to", qs(v.to.value)}, {"role", qs(v.role)}});
            return ok(id,
                      {{"references", rows},
                       {"affected_analyses", id_json(affected_analyses(m, selected))}},
                      m.info.revision);
        }
        fields(p, {"kind", "name_contains", "ids", "view", "owner_id", "offset", "limit"});
        const auto kind = p.contains("kind") ? str(p, "kind") : "";
        const auto name = p.contains("name_contains") ? str(p, "name_contains", true) : "";
        if (!kind.empty() && !std::set<std::string>{"node",
                                                    "beam",
                                                    "material",
                                                    "section",
                                                    "part",
                                                    "assembly",
                                                    "set",
                                                    "include",
                                                    "force",
                                                    "constraint",
                                                    "analysis"}
                                  .contains(kind))
            throw BadInput("Unknown entity kind");
        std::optional<std::set<EntityId>> selected;
        if (p.contains("ids")) {
            const auto list = ids(p, "ids");
            selected = std::set<EntityId>(list.begin(), list.end());
        }
        const auto view = p.contains("view") ? str(p, "view") : "all";
        if (view == "all" && p.contains("owner_id"))
            throw BadInput("owner_id requires an organization view");
        if (view != "all") {
            const EntityId owner{str(p, "owner_id")};
            if (!by_id.contains(owner))
                return failure(id, "ENTITY_NOT_FOUND", "Unknown view owner");
            const auto owner_kind = view == "property" ? "section" : view;
            if (!std::set<std::string>{"part", "assembly", "set", "include", "material", "property"}
                     .contains(view) ||
                by_id.at(owner).kind != owner_kind)
                throw BadInput("View owner kind does not match");
            std::set<EntityId> scope;
            std::vector<EntityId> pending{owner};
            while (!pending.empty()) {
                const auto current = pending.back();
                pending.pop_back();
                if (!scope.insert(current).second)
                    continue;
                const auto& k = by_id.at(current).kind;
                if (k == "part")
                    for (const auto& v : m.parts)
                        if (v.id == current)
                            pending.insert(pending.end(), v.members.begin(), v.members.end());
                if (k == "assembly")
                    for (const auto& v : m.assemblies)
                        if (v.id == current)
                            pending.insert(pending.end(), v.children.begin(), v.children.end());
                if (k == "set")
                    for (const auto& v : m.sets)
                        if (v.id == current)
                            pending.insert(pending.end(), v.members.begin(), v.members.end());
                if (k == "include")
                    for (const auto& v : m.includes) {
                        if (v.id == current)
                            pending.insert(pending.end(), v.members.begin(), v.members.end());
                        if (v.parent == current)
                            pending.push_back(v.id);
                    }
                if (k == "material" && view == "material")
                    for (const auto& v : m.sections)
                        if (v.material == current)
                            pending.push_back(v.id);
                if (k == "section" && (view == "material" || view == "property"))
                    for (const auto& v : m.beams)
                        if (v.section == current)
                            pending.push_back(v.id);
                if (k == "beam" && (view == "material" || view == "property" || view == "part" ||
                                    view == "assembly"))
                    for (const auto& v : m.beams)
                        if (v.id == current)
                            pending.insert(pending.end(), v.nodes.begin(), v.nodes.end());
            }
            scope.erase(owner);
            if (selected) {
                std::set<EntityId> intersection;
                std::set_intersection(scope.begin(),
                                      scope.end(),
                                      selected->begin(),
                                      selected->end(),
                                      std::inserter(intersection, intersection.begin()));
                selected = std::move(intersection);
            } else
                selected = std::move(scope);
        }
        auto index = [&](const char* key, int fallback, int maximum) {
            if (!p.contains(key))
                return fallback;
            const auto v = p.value(key);
            if (!v.isDouble() || v.toDouble() < 0 || v.toDouble() > maximum ||
                std::floor(v.toDouble()) != v.toDouble())
                throw BadInput("Invalid pagination");
            return v.toInt();
        };
        const auto offset = index("offset", 0, 100000);
        const auto limit = index("limit", 100, 1000);
        QJsonArray rows;
        std::size_t count = 0;
        for (const auto& e : entities) {
            if ((!kind.empty() && e.kind != kind) ||
                (!name.empty() && e.name.find(name) == std::string::npos) ||
                (selected && !selected->contains(e.id)))
                continue;
            if (count >= static_cast<std::size_t>(offset) && rows.size() < limit)
                rows.append(entity_json(e, m));
            ++count;
        }
        return ok(id,
                  {{"entities", rows},
                   {"total", static_cast<qint64>(count)},
                   {"offset", offset},
                   {"limit", limit}},
                  m.info.revision);
    } catch (const BadInput& e) {
        return failure(id, "INVALID_INPUT", QString::fromUtf8(e.what()));
    }
}
} // namespace qcae::ipc
