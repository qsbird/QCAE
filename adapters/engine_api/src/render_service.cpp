#include "qcae/render_service.hpp"
#include "qcae/ipc_api.hpp"
#include "qcae/ipc_model.hpp"
#include "qcae/operations.hpp"
#include "qcae/render_wire.hpp"
#include "qcae/json_ledger.hpp"
#include <QJsonArray>
#include <algorithm>
#include <charconv>
#include <limits>
#include <set>

namespace qcae::ipc {
namespace {
namespace json_ledger = transport::json_ledger;
std::string string(const QJsonObject& value, const char* key) {
    const auto field = value.value(QLatin1String(key));
    if (!field.isString())
        throw RecordError(ErrorCode::invalid_input, std::string("Invalid string: ") + key);
    auto text = json_ledger::utf8(json_ledger::string(field));
    if (text.empty() || text.size() > 1024)
        throw RecordError(ErrorCode::invalid_input, std::string("Invalid string: ") + key);
    return text;
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
    for (auto it = value.begin(); it != value.end(); ++it) {
        const auto key = json_ledger::owned_string(it.key());
        if (std::none_of(allowed.begin(), allowed.end(), [&](const char* candidate) {
                return key == QLatin1String(candidate);
            }))
            throw RecordError(ErrorCode::invalid_input, "Unexpected field: " + key.toStdString());
    }
}
template <class T> const T& checked(const Result<T>& value) {
    if (!value.ok())
        throw RecordError(value.error ? value.error->code : ErrorCode::invalid_input,
                          value.error ? value.error->message : "Missing result");
    return *value.value;
}
bool references_changed(const Record& before, const Record& after) {
    const auto references = [](const Record& record) {
        std::vector<std::pair<std::uint32_t, std::string>> result;
        record->descriptor().references(
            record->object(),
            [&](RecordFieldId field, std::string_view id, std::span<const RecordTypeId>) {
                result.emplace_back(field.value, id);
            });
        std::sort(result.begin(), result.end());
        return result;
    };
    return references(before) != references(after);
}
bool same_document(const DocumentRef& a, const DocumentRef& b) {
    return a.id == b.id && a.epoch == b.epoch;
}
void observe_hidden_copy(const std::vector<EntityId>& ids) {
    for (const auto& id : ids)
        ledger::add(ledger::Stage::projection, ledger::Metric::model_copy_bytes, id.value.size());
}
void observe_view_copy(const ViewSession& view) {
    // SelectionService success(T) copies this owned value and then moves it
    // into Result. Count logical string/scalar payload, not allocator capacity
    // or the transferred vector/string descriptors.
    ledger::add(ledger::Stage::projection,
                ledger::Metric::metadata_copy_bytes,
                view.id.size() + view.document.id.value.size() + view.document.epoch.value.size() +
                    view.camera_fingerprint.size() + sizeof(view.model_revision) +
                    sizeof(view.view_revision));
    observe_hidden_copy(view.hidden_ids);
}
void observe_view_result(const Result<ViewSession>& view) {
    if (view.ok())
        observe_view_copy(*view.value);
    else if (view.error && view.error->code == ErrorCode::revision_conflict) {
        // get_view can copy inspect_view's owned value before discovering its
        // old model. The failed Result no longer exposes the copied payload.
        ledger::unknown(ledger::Stage::projection, ledger::Metric::metadata_copy_bytes);
        ledger::unknown(ledger::Stage::projection, ledger::Metric::model_copy_bytes);
    }
}
void observe_resource_version_copy(const ResourceVersion& version) {
    ledger::add(ledger::Stage::projection,
                ledger::Metric::metadata_copy_bytes,
                version.document.id.value.size() + version.document.epoch.value.size() +
                    version.view_session_id.size() + sizeof(version.revision) +
                    sizeof(version.view_revision));
}
void no_binary_payload(ledger::Stage stage) {
    for (const auto metric : {ledger::Metric::model_copy_bytes,
                              ledger::Metric::metadata_copy_bytes,
                              ledger::Metric::encoded_bytes,
                              ledger::Metric::decoded_bytes,
                              ledger::Metric::library_internal_copy_bytes})
        ledger::add(stage, metric, 0);
    ledger::cover(stage);
}
} // namespace
RenderService::RenderService(RecordApplication& app,
                             SelectionService& selections,
                             ResourceStore& resources,
                             RenderContributions contributions)
    : app_(app), selections_(selections), resources_(resources),
      contributions_(std::move(contributions)) {
    if (!contributions_.frozen())
        throw std::invalid_argument("Host render contributions must be frozen");
}
bool RenderService::supports(const QString& op) const {
    return op == "view.render_resource" || op == "resources.describe" || op == "resources.read" ||
           op == "resources.release";
}
QJsonObject RenderService::dispatch(const QJsonObject& request, const Caller& caller) {
    const auto id = json_ledger::string(request.value("request_id"));
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
        if (id.isEmpty() || json_ledger::utf8(id).size() > 128 ||
            string(request, "api_version") != api_version)
            throw RecordError(ErrorCode::invalid_input, "Invalid resource request envelope");
        if (request.value("requested_version") != QJsonValue(1))
            return failure(
                id, "API_VERSION_UNSUPPORTED", "Resource operations require requested_version 1");
        const auto op = json_ledger::from_utf8(string(request, "operation"));
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
            const auto& record_snapshot = checked(snapshot_result);
            const auto& snapshot = record_snapshot.records;
            if (snapshot.version().revision != revision)
                throw RecordError(ErrorCode::revision_conflict, "Resource model revision is stale");
            const auto view_id = string(params, "view_session_id");
            if (op == "view.render_resource") {
                fields(params,
                       {"view_session_id",
                        "expected_view_revision",
                        "base_revision",
                        "base_view_revision",
                        "allow_inline_empty",
                        "allow_model_rebase",
                        "include_changed_rows",
                        "render_wire_version"});
                for (const auto* field :
                     {"allow_inline_empty", "allow_model_rebase", "include_changed_rows"})
                    if (params.contains(field) && !params.value(field).isBool())
                        throw RecordError(ErrorCode::invalid_input,
                                          std::string(field) + " must be a boolean");
                unsigned wire_version = 2;
                if (params.contains("render_wire_version")) {
                    const auto requested = params.value("render_wire_version");
                    if (!requested.isDouble())
                        throw RecordError(ErrorCode::invalid_input,
                                          "render_wire_version must be an integer");
                    const auto number = requested.toDouble();
                    if (number != 1 && number != 2 && number != 3)
                        throw RecordError(ErrorCode::unsupported_capability,
                                          "Unsupported display wire version");
                    wire_version = static_cast<unsigned>(number);
                }
                if (params.contains("base_revision") != params.contains("base_view_revision"))
                    throw RecordError(ErrorCode::invalid_input,
                                      "Both display base versions are required together");
                const bool rebase = params.value("allow_model_rebase").toBool();
                auto view_result = rebase ? selections_.inspect_view(snapshot, caller, view_id)
                                          : selections_.get_view(snapshot, caller, view_id);
                observe_view_result(view_result);
                const auto& previous_view = checked(view_result);
                const auto expected_view = number(params, "expected_view_revision");
                if (previous_view.view_revision != expected_view)
                    throw RecordError(ErrorCode::revision_conflict,
                                      "Resource view revision is stale");
                if (rebase) {
                    if (!params.contains("base_revision"))
                        throw RecordError(ErrorCode::invalid_input,
                                          "Model rebase requires both installed base versions");
                    const auto base = number(params, "base_revision");
                    if (previous_view.model_revision != base || revision <= base ||
                        number(params, "base_view_revision") != expected_view)
                        throw RecordError(ErrorCode::revision_conflict,
                                          "Model rebase has an inconsistent installed baseline");
                    if (expected_view == std::numeric_limits<std::uint64_t>::max())
                        throw RecordError(ErrorCode::resource_limit,
                                          "Display view revision cannot advance");
                    // update_view takes these arguments by value and performs
                    // one internal inspect_view before any validation/mutation.
                    observe_hidden_copy(previous_view.hidden_ids);
                    ledger::add(ledger::Stage::projection,
                                ledger::Metric::metadata_copy_bytes,
                                previous_view.camera_fingerprint.size());
                    observe_view_copy(previous_view);
                    auto rebound = selections_.update_view(snapshot,
                                                           caller,
                                                           view_id,
                                                           previous_view.hidden_ids,
                                                           previous_view.camera_fingerprint);
                    observe_view_result(rebound);
                    const auto& next_view = checked(rebound);
                    if (next_view.model_revision != revision ||
                        next_view.view_revision != expected_view + 1 ||
                        next_view.hidden_ids != previous_view.hidden_ids ||
                        next_view.camera_fingerprint != previous_view.camera_fingerprint)
                        throw std::runtime_error("Display model rebase violated its view contract");
                    view_result = std::move(rebound);
                }
                const auto& view = checked(view_result);
                const ResourceVersion version{document, revision, view_id, view.view_revision};
                observe_resource_version_copy(version);
                if (!document_ || !same_document(*document_, document)) {
                    projectors_.clear();
                    document_ = document;
                }
                if (!projectors_.contains(view_id) && projectors_.size() >= 128)
                    projectors_.erase(projectors_.begin());
                auto& projector = projectors_.try_emplace(view_id, contributions_).first->second;
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
                if (wire_version < 3) {
                    if (!result.value->legacy_line2_compatible)
                        throw RecordError(ErrorCode::unsupported_capability,
                                          "Display cells require render wire version 3");
                    if (reset) {
                        auto& packet = *result.value->full;
                        packet.beams.reserve(packet.cells.size());
                        for (auto& cell : packet.cells) {
                            const auto identity_bytes = cell.entity.value.size();
                            packet.beams.push_back({std::move(cell.entity),
                                                    {cell.points[0], cell.points[1]},
                                                    cell.visible});
                            ledger::add(ledger::Stage::render_encode,
                                        ledger::Metric::model_copy_bytes,
                                        2 * identity_bytes + 2 * sizeof(std::size_t) +
                                            sizeof(bool));
                        }
                        packet.cells.clear();
                    } else {
                        for (auto& update : result.value->delta.visibility)
                            if (update.primitive == RenderPrimitive::cell)
                                update.primitive = RenderPrimitive::beam;
                    }
                    if (wire_version == 1 &&
                        (reset ? transport::render_packet_requires_v2(*result.value->full)
                               : transport::render_delta_requires_v2(result.value->delta)))
                        throw RecordError(ErrorCode::unsupported_capability,
                                          "Display visibility requires render wire version 2");
                }
                const auto& delta = result.value->delta;
                const bool inline_empty = params.value("allow_inline_empty").toBool() && !reset &&
                                          delta.points.empty() && delta.geometry_lines.empty() &&
                                          delta.visibility.empty();
                QJsonArray changed_ids;
                std::set<std::string> identities;
                bool refresh_tree = reset;
                if (journal)
                    for (const auto& batch : journal->changes)
                        for (const auto& change : batch.changes->records) {
                            refresh_tree = refresh_tree || !change.before || !change.after ||
                                           references_changed(*change.before, *change.after);
                            if (identities.size() < 1000) {
                                const auto [entry, inserted] =
                                    identities.insert(change.key.identity);
                                if (inserted)
                                    ledger::add(ledger::Stage::render_encode,
                                                ledger::Metric::model_copy_bytes,
                                                entry->size());
                            } else
                                refresh_tree = true;
                        }
                json_ledger::ObjectCopies changed_ids_copies;
                for (const auto& identity : identities) {
                    const auto value = json_ledger::from_utf8(identity);
                    changed_ids.append(value);
                    changed_ids_copies.append(value);
                }
                EntityRowsJson rows;
                const bool rows_requested = params.value("include_changed_rows").toBool();
                if (rows_requested && !refresh_tree) {
                    const std::vector<std::string> row_ids(identities.begin(), identities.end());
                    for (const auto& identity : row_ids)
                        ledger::add(ledger::Stage::render_encode,
                                    ledger::Metric::model_copy_bytes,
                                    identity.size());
                    auto row_result = entity_rows_json(record_snapshot, row_ids);
                    rows = checked(row_result);
                    if (!rows.complete)
                        refresh_tree = true;
                }
                // Keep one fresh builder through the optional row fields. Borrowed fixed
                // keys avoid initializer-list QString copies; every insert stays observed.
                json_ledger::ObjectCopies data_copies;
                const auto insert_data = [&](const char* key, const QJsonValue& value) {
                    const QLatin1StringView name(key);
                    data.insert(name, value);
                    data_copies.insert(name, value, true, false);
                };
                if (inline_empty) {
                    const QJsonObject acknowledgement{
                        {"document_id", json_ledger::from_utf8(delta.document.id.value)},
                        {"document_epoch", json_ledger::from_utf8(delta.document.epoch.value)},
                        {"revision", json_ledger::number(delta.revision)},
                        {"view_session_id", json_ledger::from_utf8(delta.view_session_id)},
                        {"view_revision", json_ledger::number(delta.view_revision)},
                        {"base_revision", json_ledger::number(delta.base_revision)},
                        {"base_view_revision", json_ledger::number(delta.base_view_revision)}};
                    json_ledger::object(acknowledgement,
                                        {"document_id",
                                         "document_epoch",
                                         "revision",
                                         "view_session_id",
                                         "view_revision",
                                         "base_revision",
                                         "base_view_revision"});
                    insert_data("mode", "version_only");
                    insert_data("acknowledgement", acknowledgement);
                    insert_data("changed_ids", changed_ids);
                    insert_data("refresh_tree", refresh_tree);
                    // This operation carries no binary model payload and creates no resource.
                    // Its actual JSON construction/framing remains in the socket ledger.
                    no_binary_payload(ledger::Stage::render_encode);
                    no_binary_payload(ledger::Stage::resource_publish);
                } else {
                    const auto bytes = reset ? transport::encode_render_packet(*result.value->full)
                                             : transport::encode_render_delta(delta);
                    const auto manifest = resources_.publish(
                        caller,
                        version,
                        bytes,
                        reset
                            ? (transport::render_packet_version(*result.value->full) == 3
                                   ? "qcae.render.packet.v3"
                               : transport::render_packet_requires_v2(*result.value->full)
                                   ? "qcae.render.packet.v2"
                                   : "qcae.render.packet.v1")
                            : (transport::render_delta_version(delta) == 3 ? "qcae.render.delta.v3"
                               : transport::render_delta_requires_v2(delta)
                                   ? "qcae.render.delta.v2"
                                   : "qcae.render.delta.v1"));
                    insert_data("mode", reset ? "full" : "delta");
                    insert_data("manifest", resource_manifest_json(checked(manifest)));
                    insert_data("changed_ids", changed_ids);
                    insert_data("refresh_tree", refresh_tree);
                }
                if (rows_requested) {
                    insert_data("rows_complete", rows.complete && !refresh_tree);
                    if (rows.complete && !refresh_tree) {
                        const QJsonObject row_version{
                            {"document_id", json_ledger::from_utf8(document.id.value)},
                            {"document_epoch", json_ledger::from_utf8(document.epoch.value)},
                            {"revision", json_ledger::number(revision)},
                            {"view_session_id", json_ledger::from_utf8(view_id)},
                            {"view_revision", json_ledger::number(view.view_revision)}};
                        json_ledger::object(row_version,
                                            {"document_id",
                                             "document_epoch",
                                             "revision",
                                             "view_session_id",
                                             "view_revision"});
                        insert_data("rows_version", row_version);
                        insert_data("changed_rows", rows.rows);
                    }
                }
            } else {
                const auto view_result = selections_.get_view(snapshot, caller, view_id);
                observe_view_result(view_result);
                const auto& view = checked(view_result);
                if (view.view_revision != number(params, "expected_view_revision"))
                    throw RecordError(ErrorCode::revision_conflict,
                                      "Resource view revision is stale");
                const ResourceVersion version{document, revision, view_id, view.view_revision};
                observe_resource_version_copy(version);
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
        const QJsonObject response{{"request_id", id}, {"status", "success"}, {"data", data}};
        json_ledger::object(response, {"request_id", "status", "data"});
        return response;
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
