#include "qcae/ipc_api.hpp"
#include "qcae/ipc_model.hpp"
#include "qcae/ipc_selection.hpp"
#include "qcae/operations.hpp"

#include <QJsonArray>
#include <QSet>
#include <charconv>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace qcae::ipc {
namespace {
QString qs(std::string_view value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}
QString number(Revision revision) {
    return QString::number(static_cast<qulonglong>(revision));
}
struct InvalidRequest : std::runtime_error {
    using std::runtime_error::runtime_error;
};

QString string_field(const QJsonObject& object, const char* key, bool allow_empty = false) {
    const auto value = object.value(QLatin1String(key));
    if (!value.isString() || (!allow_empty && value.toString().isEmpty()))
        throw InvalidRequest(std::string("Expected a non-empty string: ") + key);
    return value.toString();
}
void fields(const QJsonObject& object, std::initializer_list<const char*> allowed) {
    QSet<QString> names;
    for (const auto* name : allowed)
        names.insert(QLatin1String(name));
    for (auto it = object.begin(); it != object.end(); ++it)
        if (!names.contains(it.key()))
            throw InvalidRequest("Unexpected field: " + it.key().toStdString());
}
DocumentRef document_ref(const QJsonObject& request) {
    return {DocumentId{string_field(request, "document_id").toStdString()},
            DocumentEpoch{string_field(request, "document_epoch").toStdString()}};
}
WriteContext context(const QJsonObject& request) {
    const auto text = string_field(request, "expected_revision").toStdString();
    Revision revision{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), revision);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        throw InvalidRequest("expected_revision must be an unsigned integer string");
    return {document_ref(request), revision};
}
Quantity quantity(const QJsonObject& params) {
    if (!params.value("young_modulus").isObject())
        throw InvalidRequest("young_modulus must be an object");
    const auto object = params.value("young_modulus").toObject();
    fields(object, {"value", "unit"});
    if (!object.value("value").isDouble())
        throw InvalidRequest("young_modulus.value must be numeric");
    const auto unit =
        object.contains("unit") ? string_field(object, "unit", true).toStdString() : std::string{};
    return {object.value("value").toDouble(), unit};
}
QJsonObject info_json(const DocumentInfo& info) {
    return {{"document_id", qs(info.document.id.value)},
            {"document_epoch", qs(info.document.epoch.value)},
            {"revision", number(info.revision)},
            {"content_state", qs(info.content_state)},
            {"name", qs(info.name)},
            {"material_count", static_cast<qint64>(info.material_count)},
            {"dirty", info.dirty},
            {"durable", info.durable},
            {"project_id", qs(info.project_id)},
            {"saved_path", qs(info.saved_path)},
            {"saved_content_state", qs(info.saved_content_state)}};
}
template <class T, class Convert>
QJsonObject result_json(const QString& id, const Result<T>& result, Convert convert) {
    QJsonObject response{{"request_id", id},
                         {"status", QString::fromLatin1(status_name(result.status))}};
    if (result.ok())
        response.insert("data", convert(*result.value));
    else if (result.error)
        response.insert("error",
                        QJsonObject{{"code", QString::fromLatin1(error_name(result.error->code))},
                                    {"message", qs(result.error->message)},
                                    {"field", qs(result.error->field)}});
    else
        response.insert("error",
                        QJsonObject{{"code", "INTERNAL_ERROR"}, {"message", "Missing diagnostic"}});
    return response;
}
QJsonObject receipt_json(const ChangeReceipt& receipt) {
    return {{"transaction_id", qs(receipt.transaction.value)},
            {"committed_revision", number(receipt.committed_revision)},
            {"current_revision", number(receipt.current_revision)},
            {"current_content_state", qs(receipt.current_content_state)},
            {"replayed", receipt.replayed}};
}
bool supported(std::string_view name) {
    return name == "project.current" || name == "project.open" || name == "project.close" ||
           name == "project.save" || name == "project.save_as" || name == "view.create" ||
           name == "view.update" || name == "view.render_data" || name == "selection.evaluate" ||
           name == "selection.combine" || name == "selection.get" || name == "entity.query" ||
           name == "entity.references" || name == "model.export_preview" ||
           name == "project.create" || name == "project.status" || name == "model.summary" ||
           name == "changes.preview" || name == "changes.commit" || name == "history.list" ||
           name == "history.undo" || name == "history.redo" || name == "operations.get" ||
           name == "capabilities.list";
}
} // namespace

QJsonObject
failure(const QString& id, const QString& code, const QString& message, const QString& status) {
    return {{"request_id", id},
            {"status", status},
            {"error", QJsonObject{{"code", code}, {"message", message}}}};
}

QJsonObject dispatch(MemoryApplication& app,
                     const QJsonObject& request,
                     const Caller& caller,
                     const IModelCodec* codec,
                     const ProfileDefinition* profile,
                     SelectionService* selections) {
    const QString id =
        request.value("request_id").isString() ? request.value("request_id").toString() : QString{};
    if (id.toUtf8().size() > 128)
        return failure({}, "INVALID_INPUT", "request_id exceeds 128 UTF-8 bytes");
    try {
        fields(request,
               {"api_version",
                "request_id",
                "operation",
                "parameters",
                "document_id",
                "document_epoch",
                "expected_revision",
                "idempotency_key"});
        (void)string_field(request, "request_id");
        if (string_field(request, "api_version") != qs(api_version))
            return failure(
                id, "API_VERSION_UNSUPPORTED", "Expected API version " + qs(api_version));
        const auto op = string_field(request, "operation").toStdString();
        if (!find_operation(op) || !supported(op))
            return failure(id,
                           "UNSUPPORTED_CAPABILITY",
                           "Operation is not implemented in the current memory slice");
        if (!request.value("parameters").isObject())
            throw InvalidRequest("parameters must be an object");
        const auto params = request.value("parameters").toObject();
        if (selections) {
            if (const auto selection_response =
                    dispatch_selection(app, *selections, request, caller))
                return *selection_response;
        }
        if (const auto model_response = dispatch_model(app, request, caller, codec, profile))
            return *model_response;
        QJsonObject response;
        if (op == "capabilities.list") {
            fields(params, {});
            QJsonArray catalog;
            for (const auto& descriptor : operation_catalog) {
                QJsonObject entry{
                    {"name", qs(descriptor.name)},
                    {"description", qs(descriptor.description)},
                    {"effect", qs(descriptor.effect)},
                    {"input_type", qs(descriptor.input_type)},
                    {"output_type", qs(descriptor.output_type)},
                    {"target_context", qs(descriptor.target_context)},
                    {"requires_document", descriptor.requires_document},
                    {"requires_epoch", descriptor.requires_epoch},
                    {"requires_revision", descriptor.requires_revision},
                    {"requires_idempotency_key", descriptor.requires_idempotency_key},
                    {"requires_profile_match", descriptor.requires_profile_match},
                    {"implementation_status", supported(descriptor.name) ? "partial" : "planned"}};
                if (descriptor.name == "changes.preview")
                    entry.insert("supported_commands",
                                 QJsonArray{"material.create",
                                            "material.set_young_modulus",
                                            "model.import",
                                            "part.upsert",
                                            "assembly.upsert",
                                            "set.upsert",
                                            "node.move",
                                            "entity.delete"});
                if (descriptor.name == "operations.get")
                    entry.insert("supported_scope", "host_lifecycle_and_document_change_outcomes");
                if (descriptor.name == "capabilities.list")
                    entry.insert("supported_scope", "global_catalog_only");
                catalog.append(entry);
            }
            const QJsonObject data{
                {"operations", catalog},
                {"storage_mode", app.durable() ? "sqlite" : "memory"},
                {"durable", app.durable()},
                {"recovery_available", app.recovery_available()},
                {"implementation_scope",
                 "M2 workspace/save/recovery and M3 query/render contracts; no solver execution or "
                 "AI bridge"},
                {"max_name_bytes", 1024},
                {"configured_solver_profiles", QJsonArray{}},
                {"declared_solver_profiles",
                 profile ? QJsonArray{profile_json(*profile)} : QJsonArray{}},
                {"supported_pressure_units", QJsonArray{"Pa", "kPa", "MPa", "GPa"}}};
            response = {{"request_id", id}, {"status", "success"}, {"data", data}};
        } else if (op == "project.current") {
            fields(params, {});
            response = result_json(id, app.current_document(), info_json);
        } else if (op == "project.open") {
            fields(params, {"mode", "path"});
            const auto mode = string_field(params, "mode");
            const auto key = string_field(request, "idempotency_key").toStdString();
            if (mode == "normal") {
                response = result_json(
                    id,
                    app.open_document(caller, string_field(params, "path").toStdString(), key),
                    info_json);
            } else if (mode == "recover" && !params.contains("path")) {
                response = result_json(id, app.recover_document(caller, key), info_json);
            } else
                throw InvalidRequest("Use mode normal with path, or recover without path");
        } else if (op == "project.save" || op == "project.save_as") {
            fields(params, {"path"});
            const auto path = params.contains("path")
                                  ? string_field(params, "path", true).toStdString()
                                  : std::string{};
            response = result_json(
                id,
                app.save_document(caller,
                                  context(request),
                                  path,
                                  op == "project.save_as",
                                  string_field(request, "idempotency_key").toStdString()),
                info_json);
        } else if (op == "project.close") {
            fields(params, {"policy"});
            const auto policy = string_field(params, "policy");
            if (policy != "discard" && policy != "keep_recovery")
                throw InvalidRequest("Unknown close policy");
            response = result_json(
                id,
                app.close_document(caller,
                                   context(request),
                                   policy == "discard" ? ClosePolicy::discard
                                                       : ClosePolicy::keep_recovery,
                                   string_field(request, "idempotency_key").toStdString()),
                info_json);
        } else if (op == "project.create") {
            fields(params, {"name"});
            response = result_json(
                id,
                app.create_document(caller,
                                    string_field(params, "name").toStdString(),
                                    string_field(request, "idempotency_key").toStdString()),
                info_json);
        } else if (op == "model.summary" || op == "project.status") {
            fields(params, {});
            response = result_json(
                id, app.snapshot(document_ref(request)), [&](const ModelSnapshot& snapshot) {
                    QJsonObject data = info_json(snapshot.info);
                    if (op == "model.summary") {
                        QJsonArray materials;
                        for (const auto& material : snapshot.materials)
                            materials.append(
                                QJsonObject{{"entity_id", qs(material.id.value)},
                                            {"name", qs(material.name)},
                                            {"young_modulus_mpa", material.young_modulus_mpa}});
                        data.insert("materials", materials);
                        data.insert("node_count", static_cast<qint64>(snapshot.nodes.size()));
                        data.insert("beam_count", static_cast<qint64>(snapshot.beams.size()));
                        data.insert("section_count", static_cast<qint64>(snapshot.sections.size()));
                        data.insert("part_count", static_cast<qint64>(snapshot.parts.size()));
                        data.insert("assembly_count",
                                    static_cast<qint64>(snapshot.assemblies.size()));
                        data.insert("set_count", static_cast<qint64>(snapshot.sets.size()));
                        data.insert("include_count", static_cast<qint64>(snapshot.includes.size()));
                        data.insert("analysis_count",
                                    static_cast<qint64>(snapshot.analyses.size()));
                    }
                    return data;
                });
        } else if (op == "changes.preview") {
            const auto command = string_field(params, "command");
            MaterialCommand material_command;
            if (command == "material.create") {
                fields(params, {"command", "name", "young_modulus"});
                material_command =
                    CreateMaterial{string_field(params, "name").toStdString(), quantity(params)};
            } else if (command == "material.set_young_modulus") {
                fields(params, {"command", "entity_id", "young_modulus"});
                material_command = SetYoungModulus{
                    EntityId{string_field(params, "entity_id").toStdString()}, quantity(params)};
            } else
                return failure(id, "UNSUPPORTED_CAPABILITY", "Unsupported material command");
            response = result_json(
                id,
                app.preview(caller, context(request), material_command),
                [](const ChangePreview& preview) {
                    return QJsonObject{
                        {"preview_id", qs(preview.id.value)},
                        {"affected_entity_id", qs(preview.affected_entity.value)},
                        {"normalized_young_modulus_mpa", preview.normalized_modulus_mpa},
                        {"creates_entity", preview.creates_entity},
                        {"revision", number(preview.context.expected_revision)}};
                });
        } else if (op == "changes.commit") {
            fields(params, {"preview_id"});
            response =
                result_json(id,
                            app.commit(caller,
                                       context(request),
                                       PreviewId{string_field(params, "preview_id").toStdString()},
                                       string_field(request, "idempotency_key").toStdString()),
                            receipt_json);
        } else if (op == "history.undo" || op == "history.redo") {
            fields(params, {});
            const auto ctx = context(request);
            const auto key = string_field(request, "idempotency_key").toStdString();
            response = result_json(id,
                                   op == "history.undo" ? app.undo(caller, ctx, key)
                                                        : app.redo(caller, ctx, key),
                                   receipt_json);
        } else if (op == "history.list") {
            fields(params, {});
            response = result_json(
                id, app.history(document_ref(request)), [](const HistorySnapshot& history) {
                    QJsonArray items;
                    for (const auto& item : history.items)
                        items.append(QJsonObject{{"transaction_id", qs(item.transaction.value)},
                                                 {"label", qs(item.label)},
                                                 {"applied", item.applied}});
                    return QJsonObject{{"items", items},
                                       {"cursor", static_cast<qint64>(history.cursor)},
                                       {"revision", number(history.revision)}};
                });
        } else if (op == "operations.get") {
            fields(params,
                   {"lookup_scope", "original_operation", "original_mode", "idempotency_key"});
            const auto lookup_scope = string_field(params, "lookup_scope");
            if (lookup_scope == "host") {
                auto original = string_field(params, "original_operation");
                if (original == "project.create")
                    original = "create_document";
                else if (original == "project.open") {
                    const auto mode = params.contains("original_mode")
                                          ? string_field(params, "original_mode")
                                          : QStringLiteral("normal");
                    if (mode != "normal" && mode != "recover")
                        throw InvalidRequest("Invalid original_mode");
                    original = mode == "recover" ? "recover_document" : "open_document";
                } else if (original == "project.save" || original == "project.save_as")
                    original = "save_document";
                else if (original == "project.close")
                    original = "close_document";
                else
                    throw InvalidRequest("Unknown lifecycle operation");
                return result_json(
                    id,
                    app.host_operation(caller,
                                       original.toStdString(),
                                       string_field(params, "idempotency_key").toStdString()),
                    info_json);
            }
            if (lookup_scope != "document")
                throw InvalidRequest("Unknown lookup_scope");
            auto original = string_field(params, "original_operation").toStdString();
            if (original == "changes.commit")
                original = "commit";
            else if (original == "history.undo")
                original = "undo";
            else if (original == "history.redo")
                original = "redo";
            else
                return failure(
                    id,
                    "UNSUPPORTED_CAPABILITY",
                    "Only changes.commit/history.undo/history.redo outcomes are available");
            response =
                result_json(id,
                            app.operation(caller,
                                          document_ref(request),
                                          original,
                                          string_field(params, "idempotency_key").toStdString()),
                            receipt_json);
        }
        if (response.value("data").isObject()) {
            const auto data = response.value("data").toObject();
            if (data.contains("current_revision"))
                response.insert("revision", data.value("current_revision"));
            else if (data.contains("revision"))
                response.insert("revision", data.value("revision"));
        }
        return response;
    } catch (const InvalidRequest& error) {
        return failure(id, "INVALID_INPUT", QString::fromUtf8(error.what()));
    } catch (const std::exception&) {
        return failure(id,
                       "INTERNAL_ERROR",
                       "Request failed without a completed response; query before retrying");
    }
}
} // namespace qcae::ipc
