#include "qcae/resource_client.hpp"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>

namespace qcae {
namespace {
template <class T>
Result<T> failure(std::string message, ErrorCode code = ErrorCode::invalid_input) {
    const auto status =
        code == ErrorCode::revision_conflict || code == ErrorCode::document_epoch_expired
            ? Status::conflict
            : Status::failed;
    return {status, std::nullopt, Diagnostic{code, std::move(message), "resource"}};
}
bool unsigned_string(const QJsonValue& value, std::uint64_t& output) {
    if (!value.isString())
        return false;
    const auto bytes = value.toString().toLatin1();
    const auto [end, error] = std::from_chars(bytes.data(), bytes.data() + bytes.size(), output);
    return error == std::errc{} && end == bytes.data() + bytes.size();
}
bool bounded_integer(const QJsonValue& value, std::uint32_t& output) {
    const auto number = value.toDouble(-1);
    if (!value.isDouble() || !std::isfinite(number) || number < 0 ||
        number > std::numeric_limits<std::uint32_t>::max() || std::floor(number) != number)
        return false;
    output = static_cast<std::uint32_t>(number);
    return true;
}
bool bounded_text(const QJsonObject& object,
                  const char* field,
                  std::string& output,
                  bool empty = false) {
    const auto value = object.value(field);
    if (!value.isString())
        return false;
    output = value.toString().toStdString();
    return (empty || !output.empty()) && output.size() <= 256;
}
bool same_manifest(const ResourceManifest& a, const ResourceManifest& b) {
    return a.resource_id == b.resource_id && same_resource_version(a.version, b.version) &&
           a.byte_length == b.byte_length && a.chunk_bytes == b.chunk_bytes &&
           a.chunk_count == b.chunk_count && a.sha256 == b.sha256 && a.media_type == b.media_type;
}
QJsonObject parameters(const ResourceManifest& manifest) {
    QJsonObject result{{"resource_id", QString::fromStdString(manifest.resource_id)}};
    if (!manifest.version.view_session_id.empty()) {
        result.insert("view_session_id", QString::fromStdString(manifest.version.view_session_id));
        result.insert("expected_view_revision", QString::number(manifest.version.view_revision));
    }
    return result;
}
Result<QByteArray> remote_failure(const QJsonObject& reply) {
    const auto error = reply.value("error").toObject();
    const auto code = error.value("code").toString();
    return failure<QByteArray>(
        error.value("message").toString("Resource request failed").toStdString(),
        code == "REVISION_CONFLICT"        ? ErrorCode::revision_conflict
        : code == "DOCUMENT_EPOCH_EXPIRED" ? ErrorCode::document_epoch_expired
        : code == "RESOURCE_LIMIT"         ? ErrorCode::resource_limit
                                           : ErrorCode::invalid_input);
}
} // namespace
struct ResourceClient::Fetch {
    ResourceManifest manifest;
    Reply reply;
    QByteArray bytes;
    std::uint32_t chunks{};
};
ResourceClient::ResourceClient(DesktopClient& client, QObject* parent)
    : QObject(parent), client_(&client) {}
ResourceClient::~ResourceClient() {
    abandon();
}
void ResourceClient::release(const ResourceManifest& manifest) {
    if (client_ && client_->ready())
        (void)client_->request("resources.release",
                               {{"resource_id", QString::fromStdString(manifest.resource_id)}},
                               {{"requested_version", 1}},
                               [](const auto&) {});
}
void ResourceClient::abandon() {
    ++generation_;
    auto old = std::move(active_);
    if (old) {
        old->bytes.clear();
        old->bytes.squeeze();
        old->reply = {};
        release(old->manifest);
    }
}
void ResourceClient::clear() {
    abandon();
    context_.reset();
}
void ResourceClient::setContext(const ResourceVersion& context) {
    if (context_ && same_resource_version(*context_, context))
        return;
    abandon();
    context_ = context;
}
Result<ResourceManifest> ResourceClient::parseManifest(const QJsonObject& object) {
    ResourceManifest manifest;
    auto& version = manifest.version;
    if (!bounded_text(object, "resource_id", manifest.resource_id) ||
        !bounded_text(object, "document_id", version.document.id.value) ||
        !bounded_text(object, "document_epoch", version.document.epoch.value) ||
        !unsigned_string(object.value("revision"), version.revision) ||
        !bounded_text(object, "view_session_id", version.view_session_id, true) ||
        !unsigned_string(object.value("view_revision"), version.view_revision) ||
        (version.view_session_id.empty() ? version.view_revision != 0
                                         : version.view_revision == 0) ||
        !unsigned_string(object.value("byte_length"), manifest.byte_length) ||
        manifest.byte_length > resource_max_bytes ||
        !bounded_integer(object.value("chunk_bytes"), manifest.chunk_bytes) ||
        manifest.chunk_bytes != resource_chunk_bytes ||
        !bounded_integer(object.value("chunk_count"), manifest.chunk_count) ||
        manifest.chunk_count != manifest.byte_length / resource_chunk_bytes +
                                    (manifest.byte_length % resource_chunk_bytes != 0) ||
        !bounded_text(object, "sha256", manifest.sha256) || manifest.sha256.size() != 64 ||
        !std::all_of(manifest.sha256.begin(),
                     manifest.sha256.end(),
                     [](char value) {
                         return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
                     }) ||
        !bounded_text(object, "media_type", manifest.media_type))
        return failure<ResourceManifest>("Invalid or excessive resource manifest");
    return {Status::success, std::move(manifest), std::nullopt};
}
void ResourceClient::request(const std::shared_ptr<Fetch>& fetch,
                             const QString& operation,
                             const QJsonObject& params,
                             DesktopClient::Reply reply) {
    if (!client_) {
        finish(fetch, failure<QByteArray>("Resource transport was destroyed"));
        return;
    }
    const auto& version = fetch->manifest.version;
    const QJsonObject context{
        {"document_id", QString::fromStdString(version.document.id.value)},
        {"document_epoch", QString::fromStdString(version.document.epoch.value)},
        {"expected_revision", QString::number(version.revision)},
        {"requested_version", 1}};
    QPointer<ResourceClient> guard(this);
    const auto generation = generation_;
    (void)client_->request(
        operation,
        params,
        context,
        [guard, generation, fetch, reply = std::move(reply)](const auto& response) {
            if (guard && guard->generation_ == generation && guard->active_ == fetch)
                reply(response);
        });
}
void ResourceClient::fetch(const ResourceManifest& manifest, Reply reply) {
    abandon();
    // Validate the caller's DTO too: it may not have come through parseManifest.
    const auto expected_chunks = manifest.byte_length / resource_chunk_bytes +
                                 (manifest.byte_length % resource_chunk_bytes != 0);
    if (!context_ || !same_resource_version(*context_, manifest.version) ||
        manifest.resource_id.empty() || manifest.byte_length > resource_max_bytes ||
        manifest.chunk_bytes != resource_chunk_bytes || manifest.chunk_count != expected_chunks ||
        manifest.sha256.size() != 64) {
        if (reply)
            reply(failure<QByteArray>("Resource manifest does not match the current context"));
        return;
    }
    auto transfer = std::make_shared<Fetch>();
    transfer->manifest = manifest;
    transfer->reply = std::move(reply);
    active_ = transfer;
    request(transfer,
            "resources.describe",
            parameters(manifest),
            [this, transfer](const auto& response) {
                if (response.value("status") != "success") {
                    finish(transfer, remote_failure(response));
                    return;
                }
                const auto described = parseManifest(response.value("data").toObject());
                if (!described.ok() || !same_manifest(*described.value, transfer->manifest)) {
                    finish(transfer,
                           failure<QByteArray>("Resource description changed its pinned identity"));
                    return;
                }
                transfer->bytes.reserve(static_cast<qsizetype>(transfer->manifest.byte_length));
                readNext(transfer);
            });
}
void ResourceClient::readNext(const std::shared_ptr<Fetch>& transfer) {
    const auto& manifest = transfer->manifest;
    const auto offset = static_cast<std::uint64_t>(transfer->bytes.size());
    if (offset == manifest.byte_length) {
        if (transfer->chunks != manifest.chunk_count ||
            QCryptographicHash::hash(transfer->bytes, QCryptographicHash::Sha256)
                    .toHex()
                    .toStdString() != manifest.sha256) {
            finish(transfer,
                   failure<QByteArray>("Resource length, chunk count or SHA-256 did not match"));
            return;
        }
        finish(transfer, {Status::success, std::move(transfer->bytes), std::nullopt});
        return;
    }
    auto params = parameters(manifest);
    params.insert("offset", QString::number(offset));
    request(transfer, "resources.read", params, [this, transfer, offset](const auto& response) {
        if (response.value("status") != "success") {
            finish(transfer, remote_failure(response));
            return;
        }
        const auto data = response.value("data").toObject();
        const auto described = parseManifest(data.value("manifest").toObject());
        const auto expected_size =
            std::min<std::uint64_t>(resource_chunk_bytes, transfer->manifest.byte_length - offset);
        std::uint64_t chunk_offset{};
        std::uint32_t raw_length{};
        const auto encoded = data.value("data_base64").toString().toLatin1();
        if (!described.ok() || !same_manifest(*described.value, transfer->manifest) ||
            !unsigned_string(data.value("offset"), chunk_offset) || chunk_offset != offset ||
            !bounded_integer(data.value("raw_length"), raw_length) || raw_length != expected_size ||
            data.value("encoding") != "base64" || !data.value("data_base64").isString() ||
            static_cast<std::uint64_t>(encoded.size()) != ((expected_size + 2) / 3) * 4 ||
            QJsonDocument(response).toJson(QJsonDocument::Compact).size() + 1 >
                resource_max_wire_bytes) {
            finish(
                transfer,
                failure<QByteArray>("Resource chunk identity, order, offset or size is invalid"));
            return;
        }
        const auto decoded =
            QByteArray::fromBase64Encoding(encoded, QByteArray::AbortOnBase64DecodingErrors);
        if (!decoded || decoded.decoded.size() != raw_length ||
            decoded.decoded.toBase64() != encoded) {
            finish(transfer, failure<QByteArray>("Resource chunk has invalid base64 encoding"));
            return;
        }
        transfer->bytes.append(decoded.decoded);
        ++transfer->chunks;
        readNext(transfer);
    });
}
void ResourceClient::finish(const std::shared_ptr<Fetch>& transfer, Result<QByteArray> result) {
    if (active_ != transfer)
        return;
    auto reply = std::move(transfer->reply);
    active_.reset();
    transfer->bytes.clear();
    transfer->bytes.squeeze();
    release(transfer->manifest);
    if (reply)
        reply(std::move(result));
}
} // namespace qcae
