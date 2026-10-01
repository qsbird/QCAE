#include "qcae/ipc_model.hpp"
#include "qcae/json_ledger.hpp"
#include "qcae/query.hpp"
#include "qcae/records.hpp"
#include <QJsonArray>
#include <QUuid>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>

namespace qcae::ipc {
namespace {
QString qs(const std::string& s) {
    return transport::json_ledger::from_utf8(s);
}
struct BadInput : std::runtime_error {
    using std::runtime_error::runtime_error;
};
void fields(const QJsonObject& o, std::initializer_list<const char*> allowed) {
    for (auto i = o.begin(); i != o.end(); ++i) {
        if (std::none_of(allowed.begin(), allowed.end(), [&](const char* s) {
                return i.keyView() == QAnyStringView(QLatin1StringView(s));
            }))
            throw BadInput("Unexpected field: " + i.key().toStdString());
    }
}
std::string str(const QJsonObject& o, const char* key, bool empty = false) {
    const auto v = o.value(QLatin1String(key));
    if (!v.isString())
        throw BadInput(std::string("Expected string: ") + key);
    if (!empty && transport::json_ledger::trimmed_empty(v.toStringView()))
        throw BadInput(std::string("Expected string: ") + key);
    return transport::json_ledger::utf8(v.toStringView());
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
        if (!item.isString() || item.toStringView().isEmpty())
            throw BadInput("Invalid entity ID");
        out.emplace_back(transport::json_ledger::utf8(item.toStringView()));
    }
    return out;
}
QJsonArray id_json(const std::vector<EntityId>& list) {
    QJsonArray out;
    transport::json_ledger::ObjectCopies copies;
    for (const auto& i : list) {
        const auto value = qs(i.value);
        out.append(value);
        copies.append(value);
    }
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
    transport::json_ledger::ObjectCopies copies;
    copies.retained(data);
    const auto revision = transport::json_ledger::number(rev);
    data.insert("revision", revision);
    copies.insert(QLatin1StringView("revision"), revision);
    const QJsonObject output{{"request_id", id},
                             {"status", "success"},
                             {"revision", transport::json_ledger::number(rev)},
                             {"data", data}};
    transport::json_ledger::object(output, {"request_id", "status", "revision", "data"});
    return output;
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
QJsonValue field_json(const RecordFieldInput& field) {
    switch (field.kind) {
    case RecordFieldKind::text:
    case RecordFieldKind::reference:
        return qs(record_wire::read_text(field.payload));
    case RecordFieldKind::real:
        return record_wire::read_real(field.payload);
    case RecordFieldKind::unsigned_integer:
        return transport::json_ledger::number(record_wire::read_number(field.payload));
    case RecordFieldKind::boolean:
        return record_wire::read_boolean(field.payload);
    case RecordFieldKind::vector3: {
        const auto value = record_wire::read_vector3(field.payload);
        const QJsonArray result{value[0], value[1], value[2]};
        transport::json_ledger::ObjectCopies copies;
        for (const auto& component : result)
            copies.append(component);
        return result;
    }
    case RecordFieldKind::references: {
        QJsonArray result;
        for (const auto& id : record_wire::read_strings(field.payload))
            result.append(qs(id));
        return result;
    }
    case RecordFieldKind::profile:
        return profile_ref_json(record_wire::read_profile(field.payload));
    case RecordFieldKind::target: {
        const auto value = record_wire::read_target(field.payload);
        return QJsonObject{{"profile", profile_ref_json(value.profile)},
                           {"analysis_kind", qs(value.analysis_kind)}};
    }
    }
    throw BadInput("Unsupported record field type");
}
QString entity_field_name(const RecordFieldDescriptor& field) {
    // Preserve the established entity.query wire names; entity.fields uses schema names.
    if (field.kind == RecordFieldKind::reference)
        return qs(field.name + "_id");
    if (field.kind == RecordFieldKind::vector3 && field.unit == "mm")
        return qs(field.name + "_mm");
    return qs(field.name);
}
} // namespace
QJsonObject record_entity_json(const EntitySummary& entity, const DocumentView& view) {
    QJsonObject result{
        {"entity_id", qs(entity.id.value)}, {"kind", qs(entity.kind)}, {"name", qs(entity.name)}};
    transport::json_ledger::ObjectCopies copies;
    copies.insert(QLatin1StringView("entity_id"), result.value("entity_id"));
    copies.insert(QLatin1StringView("kind"), result.value("kind"));
    copies.insert(QLatin1StringView("name"), result.value("name"));
    const auto record = view.find_identity(entity.id.value);
    const auto input = record_wire::decode(record->encoded());
    for (const auto& descriptor : record->descriptor().fields) {
        const auto* field = record_wire::find(input, descriptor.id);
        if (field) {
            const auto key = entity_field_name(descriptor);
            const auto value = field_json(*field);
            result.insert(key, value);
            copies.insert(key, value, true, false);
            if (descriptor.kind == RecordFieldKind::target)
                result.insert("profile_ref",
                              profile_ref_json(record_wire::read_target(field->payload).profile));
        } else if (descriptor.optional && descriptor.introduced_version > 1) {
            // Newly introduced optional fields expose their null default on old records too.
            const auto key = entity_field_name(descriptor);
            const QJsonValue value;
            result.insert(key, value);
            copies.insert(key, value, true, false);
        } else if (entity.kind == "beam" && descriptor.name == "section")
            result.insert("section_id", QJsonValue());
    }
    QJsonArray sources;
    ledger::add(ledger::Stage::socket_send,
                ledger::Metric::scanned_records,
                view.count(RecordTraits<records::SourceIdentifier>::type_id));
    view.visit(RecordTraits<records::SourceIdentifier>::type_id, [&](const Record& item) {
        const auto& source = item->get<records::SourceIdentifier>();
        if (source.entity == entity.id)
            sources.append(QJsonObject{{"source_model_id", qs(source.source_model_id)},
                                       {"namespace", qs(source.name_space)},
                                       {"number", QString::number(source.number)},
                                       {"include_id", qs(source.include.value)}});
    });
    result.insert("sources", sources);
    copies.insert(QLatin1StringView("sources"), sources);
    return result;
}
namespace {
Result<EntityPage> query_page(const DocumentView& view,
                              const EntityFilter& filter,
                              std::size_t offset,
                              std::size_t limit) {
    RecordStats stats;
    auto page = query_entities(view, filter, offset, limit, &stats);
    ledger::add(
        ledger::Stage::socket_send, ledger::Metric::model_copy_bytes, stats.model_bytes_copied);
    if (!page.ok() || !ledger::current())
        return page;
    if (filter.view == "all")
        ledger::add(ledger::Stage::socket_send, ledger::Metric::scanned_records, view.size());
    else
        // Organization scopes may traverse relation-specific tables before
        // the final page scan. Those visits need a separate observer.
        ledger::unknown(ledger::Stage::socket_send, ledger::Metric::scanned_records);
    // query_entities copies three strings into a temporary EntitySummary,
    // then moves it into its vector. RecordStats includes each object's size,
    // bounding that first move's inline string bytes. EntityId's by-value
    // construction and previous elements' growth moves are additional.
    struct alignas(EntitySummary) Shape {
        std::array<std::byte, sizeof(EntitySummary)> bytes;
    };
    try {
        std::vector<Shape> shape;
        std::uint64_t previous_inline{}, extra{};
        const auto inline_size = [](const std::string& value) {
            return value.size() <= std::string{}.capacity() ? value.size() : 0;
        };
        for (const auto& entity : page.value->entities) {
            const auto old_capacity = shape.capacity();
            shape.emplace_back();
            if (shape.capacity() != old_capacity)
                extra += previous_inline;
            extra += inline_size(entity.id.value);
            previous_inline +=
                inline_size(entity.id.value) + inline_size(entity.kind) + inline_size(entity.name);
        }
        ledger::add(ledger::Stage::socket_send, ledger::Metric::model_copy_bytes, extra);
    } catch (...) {
        ledger::unknown(ledger::Stage::socket_send, ledger::Metric::model_copy_bytes);
    }
    // DocumentView::version observes the EntityPage's owned version at source.
    return page;
}
QJsonArray page_rows_json(const EntityPage& page, const DocumentView& view) {
    QJsonArray rows;
    transport::json_ledger::ObjectCopies copies;
    for (const auto& entity : page.entities) {
        const auto row = record_entity_json(entity, view);
        rows.append(row);
        copies.append(row);
    }
    return rows;
}
// This deliberately bounds encoded JSON without making another owned encoding.
// Each UTF-16 code unit needs at most six escaped bytes; 64 covers every finite
// JSON double formatting. Borrowed nested containers are never detached.
bool take_json_budget(std::uint64_t amount, std::uint64_t& remaining) {
    if (amount > remaining)
        return false;
    remaining -= amount;
    return true;
}
bool bounded_string(QAnyStringView value, std::uint64_t& remaining) {
    const auto length = static_cast<std::uint64_t>(value.size());
    return length <= (std::numeric_limits<std::uint64_t>::max() - 2) / 6 &&
           take_json_budget(2 + length * 6, remaining);
}
bool bounded_json(const QJsonValue& value, std::uint64_t& remaining, unsigned depth = 0) {
    if (depth > 64)
        return false;
    if (value.isString())
        return bounded_string(value.toStringView(), remaining);
    if (value.isDouble())
        return take_json_budget(64, remaining);
    if (value.isBool())
        return take_json_budget(5, remaining);
    if (value.isArray()) {
        const auto array = value.toArray();
        if (!take_json_budget(2 + (array.isEmpty() ? 0 : array.size() - 1), remaining))
            return false;
        for (const auto& member : array)
            if (!bounded_json(member, remaining, depth + 1))
                return false;
        return true;
    }
    if (value.isObject()) {
        const auto object = value.toObject();
        if (!take_json_budget(2 + (object.isEmpty() ? 0 : object.size() - 1), remaining))
            return false;
        for (auto it = object.constBegin(); it != object.constEnd(); ++it)
            if (!bounded_string(it.keyView(), remaining) || !take_json_budget(1, remaining) ||
                !bounded_json(it.value(), remaining, depth + 1))
                return false;
        return true;
    }
    return take_json_budget(4, remaining);
}
int page_index(const QJsonObject& parameters, const char* key, int fallback, int maximum) {
    if (!parameters.contains(key))
        return fallback;
    const auto value = parameters.value(QLatin1String(key));
    if (!value.isDouble() || value.toDouble() < 0 || value.toDouble() > maximum ||
        std::floor(value.toDouble()) != value.toDouble())
        throw BadInput("Invalid pagination");
    return value.toInt();
}
QJsonObject
entity_query(const QString& id, const QJsonObject& parameters, const RecordSnapshot& snapshot) {
    fields(parameters, {"kind", "name_contains", "ids", "view", "owner_id", "offset", "limit"});
    EntityFilter filter;
    if (parameters.contains("kind"))
        filter.kind = str(parameters, "kind");
    if (parameters.contains("name_contains"))
        filter.name_contains = str(parameters, "name_contains", true);
    if (parameters.contains("ids"))
        filter.ids = ids(parameters, "ids");
    if (parameters.contains("view"))
        filter.view = str(parameters, "view");
    if (parameters.contains("owner_id"))
        filter.owner = EntityId(str(parameters, "owner_id"));
    const auto offset = page_index(parameters, "offset", 0, 100000);
    const auto limit = page_index(parameters, "limit", 100, 1000);
    const auto page = query_page(snapshot.records, filter, offset, limit);
    if (!page.ok())
        return error(id, page);
    const auto rows = page_rows_json(*page.value, snapshot.records);
    const QJsonObject data{{"entities", rows},
                           {"total", static_cast<qint64>(page.value->total)},
                           {"offset", offset},
                           {"limit", limit}};
    transport::json_ledger::object(data, {"entities", "total", "offset", "limit"});
    return ok(id, data, snapshot.info.revision);
}
QJsonObject entity_references(const QString& id,
                              const QJsonObject& parameters,
                              const RecordSnapshot& snapshot) {
    fields(parameters, {"entity_id", "direction"});
    const EntityId selected{str(parameters, "entity_id")};
    const auto direction =
        parameters.contains("direction") ? str(parameters, "direction") : "incoming";
    if (direction != "incoming" && direction != "outgoing")
        throw BadInput("Unknown reference direction");
    QJsonArray rows;
    std::size_t offset = 0;
    // The existing response contains every reference. Read bounded record pages without
    // projecting the document or constructing a second all-references collection.
    for (;;) {
        const auto page =
            query_references(snapshot.records, selected, direction == "incoming", offset, 1000);
        if (!page.ok())
            return error(id, page);
        for (const auto& reference : page.value->references)
            rows.append(QJsonObject{{"from", qs(reference.from.value)},
                                    {"to", qs(reference.to.value)},
                                    {"role", qs(reference.role)}});
        offset += page.value->references.size();
        if (offset >= page.value->total)
            break;
    }
    return ok(id,
              {{"references", rows},
               {"affected_analyses", id_json(query_affected_analyses(snapshot.records, selected))}},
              snapshot.info.revision);
}

} // namespace
Result<EntityRowsJson> entity_rows_json(const RecordSnapshot& snapshot,
                                        std::span<const std::string> identities,
                                        std::size_t row_limit,
                                        std::size_t encoded_byte_limit) {
    const auto incomplete = [] {
        return Result<EntityRowsJson>{Status::success, EntityRowsJson{}, {}};
    };
    if (!row_limit || row_limit > 1000 || !encoded_byte_limit)
        return {Status::failed,
                {},
                Diagnostic{ErrorCode::invalid_input, "Invalid changed row budget", "changed_rows"}};
    if (identities.size() > row_limit)
        return incomplete();
    if (identities.empty())
        return encoded_byte_limit >= 2
                   ? Result<EntityRowsJson>{Status::success, EntityRowsJson{{}, true, 2}, {}}
                   : incomplete();
    std::set<std::string_view> seen;
    for (const auto& identity : identities) {
        if (identity.empty() || !seen.insert(identity).second)
            return {Status::failed,
                    {},
                    Diagnostic{ErrorCode::invalid_input,
                               "Changed row IDs must be non-empty and unique",
                               "changed_rows"}};
    }
    for (const auto& identity : identities) {
        const auto record = snapshot.records.find_identity(identity);
        if (!record || record->descriptor().query_kind.empty())
            return incomplete();
    }
    EntityFilter filter;
    filter.ids.emplace();
    filter.ids->reserve(identities.size());
    for (const auto& identity : identities) {
        filter.ids->emplace_back(identity);
        // EntityId owns its string argument and moves it once into its value.
        const auto move = identity.size() <= std::string{}.capacity() ? identity.size() : 0;
        ledger::add(
            ledger::Stage::socket_send, ledger::Metric::model_copy_bytes, identity.size() + move);
    }
    const auto page = query_page(snapshot.records, filter, 0, row_limit);
    if (!page.ok())
        return {page.status, {}, page.error};
    if (page.value->total != identities.size() || page.value->entities.size() != identities.size())
        return incomplete();
    auto rows = page_rows_json(*page.value, snapshot.records);
    auto remaining = static_cast<std::uint64_t>(encoded_byte_limit);
    if (!bounded_json(rows, remaining))
        return incomplete();
    return {
        Status::success, EntityRowsJson{std::move(rows), true, encoded_byte_limit - remaining}, {}};
}
QJsonObject profile_json(const ProfileDefinition& p) {
    return {{"profile_ref", profile_ref_json(p.reference)},
            {"solver_family", qs(p.solver_family)},
            {"analysis_kind", qs(p.analysis_kind)},
            {"configured", p.configured},
            {"validated", p.validated},
            {"implementation_fingerprint", qs(p.implementation_fingerprint)},
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
        if (op == "entity.query" || op == "entity.references") {
            const auto snapshot = app.record_application().snapshot(ref(r));
            if (!snapshot.ok())
                return error(id, snapshot);
            return op == "entity.query" ? entity_query(id, p, *snapshot.value)
                                        : entity_references(id, p, *snapshot.value);
        }
        if (op == "model.export_preview") {
            const auto records = app.record_application().snapshot(ref(r));
            if (!records.ok())
                return error(id, records);
            if (ctx(r).expected_revision != records.value->info.revision)
                return failure(
                    id, "REVISION_CONFLICT", "Document revision has changed", "conflict");
            if (records.value->records.count(RecordTraits<records::Tri3>::type_id))
                return failure(
                    id, "UNSUPPORTED_CAPABILITY", "Tri3 shell export is not implemented");
        }
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
        return std::nullopt;
    } catch (const BadInput& e) {
        return failure(id, "INVALID_INPUT", QString::fromUtf8(e.what()));
    }
}
} // namespace qcae::ipc
