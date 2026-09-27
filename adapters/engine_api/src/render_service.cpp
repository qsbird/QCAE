#include "qcae/render_service.hpp"
#include "qcae/ipc_api.hpp"
#include "qcae/operations.hpp"
#include "qcae/render_wire.hpp"
#include <QJsonArray>
#include <charconv>
#include <set>

namespace qcae::ipc {
namespace {
std::string string(const QJsonObject& value, const char* key) {
    const auto field = value.value(QLatin1String(key));
    if (!field.isString() || field.toString().isEmpty() || field.toString().toUtf8().size() > 1024)
        throw RecordError(ErrorCode::invalid_input, std::string("Invalid string: ") + key);
    return field.toString().toStdString();
}
std::uint64_t number(const QJsonObject& value, const char* key) {
    const auto text = string(value, key);
    std::uint64_t result{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        throw RecordError(ErrorCode::invalid_input, std::string("Invalid unsigned string: ") + key);
    return result;
}
void fields(const QJsonObject& value, std::initializer_list<const char*> allowed) {
    std::set<QString> keys;
    for (const auto* key : allowed)
        keys.insert(QLatin1String(key));
    for (auto it = value.begin(); it != value.end(); ++it)
        if (!keys.contains(it.key()))
            throw RecordError(ErrorCode::invalid_input,
                              "Unexpected field: " + it.key().toStdString());
}
template <class T> const T& checked(const Result<T>& value) {
    if (!value.ok())
        throw RecordError(value.error ? value.error->code : ErrorCode::invalid_input,
                          value.error ? value.error->message : "Missing result");
    return *value.value;
}
bool same_document(const DocumentRef& a, const DocumentRef& b) {
    return a.id == b.id && a.epoch == b.epoch;
}
} // namespace
RenderService::RenderService(RecordApplication& app,
                             SelectionService& selections,
                             ResourceStore& resources)
    : app_(app), selections_(selections), resources_(resources) {}
bool RenderService::supports(const QString& op) const {
    return op == "view.render_resource" || op == "resources.describe" || op == "resources.read" ||
           op == "resources.release";
}
QJsonObject RenderService::dispatch(const QJsonObject& request, const Caller& caller) {
    const auto id = request.value("request_id").toString();
    try {
        fields(request,
               {"api_version",
                "request_id",
                "operation",
                "parameters",
                "document_id",
                "document_epoch",
                "expected_revision",
                "requested_version"});
        if (id.isEmpty() || id.toUtf8().size() > 128 ||
            string(request, "api_version") != api_version)
            throw RecordError(ErrorCode::invalid_input, "Invalid resource request envelope");
        if (request.value("requested_version") != QJsonValue(1))
            return failure(
                id, "API_VERSION_UNSUPPORTED", "Resource operations require requested_version 1");
        const auto op = QString::fromStdString(string(request, "operation"));
        if (!supports(op))
            return failure(id, "UNSUPPORTED_CAPABILITY", "Unknown resource operation");
        if (!request.value("parameters").isObject())
            throw RecordError(ErrorCode::invalid_input, "parameters must be an object");
        const auto params = request.value("parameters").toObject();
        QJsonObject data;
        if (op == "resources.release") {
            fields(params, {"resource_id"});
            const auto released = resources_.release(caller, string(params, "resource_id"));
            data.insert("released", checked(released));
        } else {
            const DocumentRef document{DocumentId(string(request, "document_id")),
                                       DocumentEpoch(string(request, "document_epoch"))};
            const auto revision = number(request, "expected_revision");
            const auto snapshot_result = app_.snapshot(document);
            const auto& snapshot = checked(snapshot_result).records;
            if (snapshot.version().revision != revision)
                throw RecordError(ErrorCode::revision_conflict, "Resource model revision is stale");
            const auto view_id = string(params, "view_session_id");
            const auto view_result = selections_.get_view(snapshot, caller, view_id);
            const auto& view = checked(view_result);
            if (view.view_revision != number(params, "expected_view_revision"))
                throw RecordError(ErrorCode::revision_conflict, "Resource view revision is stale");
            const ResourceVersion version{document, revision, view_id, view.view_revision};
            if (op == "view.render_resource") {
                fields(params,
                       {"view_session_id",
                        "expected_view_revision",
                        "base_revision",
                        "base_view_revision"});
                if (params.contains("base_revision") != params.contains("base_view_revision"))
                    throw RecordError(ErrorCode::invalid_input,
                                      "Both display base versions are required together");
                if (!document_ || !same_document(*document_, document)) {
                    projectors_.clear();
                    document_ = document;
                }
                if (!projectors_.contains(view_id) && projectors_.size() >= 128)
                    projectors_.erase(projectors_.begin());
                auto& projector = projectors_.try_emplace(view_id).first->second;
                const auto previous = projector.version();
                bool full = !previous || !params.contains("base_revision") ||
                            previous->revision != number(params, "base_revision");
                // View revision is checked against the actual projection below; never trust a
                // client's claimed baseline after a lost response or another consumer update.
                std::vector<DirectedRenderChange> changes;
                std::optional<RecordChangeBatch> journal;
                if (!full) {
                    const auto result = app_.changes_since(document, previous->revision);
                    journal = checked(result);
                    full = journal->resync_required;
                    for (const auto& change : journal->changes)
                        changes.push_back({change.base_revision,
                                           change.revision,
                                           change.changes.get(),
                                           change.direction});
                }
                auto result = projector.update(snapshot, view, changes, full);
                checked(result);
                if (!result.value->full && result.value->delta.base_view_revision !=
                                               number(params, "base_view_revision")) {
                    result = projector.update(snapshot, view, {}, true);
                    checked(result);
                }
                const bool reset = result.value->full.has_value();
                const auto bytes = reset ? transport::encode_render_packet(*result.value->full)
                                         : transport::encode_render_delta(result.value->delta);
                const auto manifest =
                    resources_.publish(caller,
                                       version,
                                       bytes,
                                       reset ? "qcae.render.packet.v1" : "qcae.render.delta.v1");
                QJsonArray changed_ids;
                std::set<std::string> identities;
                bool refresh_tree = reset;
                if (journal)
                    for (const auto& batch : journal->changes)
                        for (const auto& change : batch.changes->records) {
                            refresh_tree = refresh_tree || !change.before || !change.after;
                            if (identities.size() < 1000)
                                identities.insert(change.key.identity);
                            else
                                refresh_tree = true;
                        }
                for (const auto& identity : identities)
                    changed_ids.append(QString::fromStdString(identity));
                data = {{"mode", reset ? "full" : "delta"},
                        {"manifest", resource_manifest_json(checked(manifest))},
                        {"changed_ids", changed_ids},
                        {"refresh_tree", refresh_tree}};
            } else {
                fields(params,
                       {"resource_id", "view_session_id", "expected_view_revision", "offset"});
                const auto resource_id = string(params, "resource_id");
                if (op == "resources.describe") {
                    if (params.contains("offset"))
                        throw RecordError(ErrorCode::invalid_input,
                                          "describe does not accept offset");
                    const auto result = resources_.describe(caller, resource_id, version);
                    data = resource_manifest_json(checked(result));
                } else {
                    const auto result =
                        resources_.read(caller, resource_id, version, number(params, "offset"));
                    data = resource_chunk_json(checked(result));
                }
            }
        }
        return {{"request_id", id}, {"status", "success"}, {"data", data}};
    } catch (const RecordError& error) {
        return failure(id,
                       QString::fromLatin1(error_name(error.code())),
                       QString::fromUtf8(error.what()),
                       error.code() == ErrorCode::revision_conflict ||
                               error.code() == ErrorCode::document_epoch_expired
                           ? "conflict"
                           : "failed");
    } catch (const std::exception& error) {
        return failure(id, "INTERNAL_ERROR", QString::fromUtf8(error.what()));
    }
}
} // namespace qcae::ipc
