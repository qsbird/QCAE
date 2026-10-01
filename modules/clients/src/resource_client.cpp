#include "qcae/resource_client.hpp"
#include "qcae/operation_ledger.hpp"
#include "qcae/json_ledger.hpp"
#include <QCryptographicHash>
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
    ledger::add(
        ledger::Stage::resource_decode, ledger::Metric::metadata_copy_bytes, bytes.size() * 3);
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
    ledger::add(
        ledger::Stage::resource_decode, ledger::Metric::metadata_copy_bytes, output.size() * 4);
    return (empty || !output.empty()) && output.size() <= 256;
}
bool same_manifest(const ResourceManifest& a, const ResourceManifest& b) {
    return a.resource_id == b.resource_id && same_resource_version(a.version, b.version) &&
           a.byte_length == b.byte_length && a.chunk_bytes == b.chunk_bytes &&
           a.chunk_count == b.chunk_count && a.sha256 == b.sha256 && a.media_type == b.media_type;
}
std::size_t manifest_copy_bytes(const ResourceManifest& manifest) {
    return sizeof(ResourceManifest) + manifest.resource_id.size() + manifest.sha256.size() +
           manifest.media_type.size() + manifest.version.document.id.value.size() +
           manifest.version.document.epoch.value.size() + manifest.version.view_session_id.size();
}
QJsonObject parameters(const ResourceManifest& manifest,
                       std::optional<std::uint64_t> offset = std::nullopt) {
    QJsonObject result{{"resource_id", transport::json_ledger::from_utf8(manifest.resource_id)}};
    transport::json_ledger::ObjectCopies copies;
    copies.insert(QLatin1StringView("resource_id"), result.value("resource_id"));
    if (!manifest.version.view_session_id.empty()) {
        result.insert("view_session_id",
                      transport::json_ledger::from_utf8(manifest.version.view_session_id));
        copies.insert(QLatin1StringView("view_session_id"), result.value("view_session_id"));
        result.insert("expected_view_revision",
                      transport::json_ledger::number(manifest.version.view_revision));
        copies.insert(QLatin1StringView("expected_view_revision"),
                      result.value("expected_view_revision"));
    }
    if (offset) {
        result.insert("offset", transport::json_ledger::number(*offset));
        copies.insert(QLatin1StringView("offset"), result.value("offset"));
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
    if (client_ && client_->ready()) {
        const QJsonObject params{
            {"resource_id", transport::json_ledger::from_utf8(manifest.resource_id)}};
        const QJsonObject context{{"requested_version", 1}};
        transport::json_ledger::object(params, {"resource_id"});
        transport::json_ledger::object(context, {"requested_version"});
        (void)client_->request(transport::json_ledger::from_utf8("resources.release"),
                               params,
                               context,
                               [](const auto&) {});
    }
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
    ledger::add(ledger::Stage::resource_decode,
                ledger::Metric::metadata_copy_bytes,
                sizeof(ResourceVersion) + context.document.id.value.size() +
                    context.document.epoch.value.size() + context.view_session_id.size());
}
Result<ResourceManifest> ResourceClient::parseManifest(const QJsonObject& object) {
    ResourceManifest manifest;
    ledger::add(ledger::Stage::resource_decode,
                ledger::Metric::metadata_copy_bytes,
                sizeof(ResourceManifest));
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
        {"document_id", transport::json_ledger::from_utf8(version.document.id.value)},
        {"document_epoch", transport::json_ledger::from_utf8(version.document.epoch.value)},
        {"expected_revision", transport::json_ledger::number(version.revision)},
        {"requested_version", 1}};
    transport::json_ledger::object(
        context, {"document_id", "document_epoch", "expected_revision", "requested_version"});
    QPointer<ResourceClient> guard(this);
    const auto generation = generation_;
    (void)client_->request(
        operation,
        params,
        context,
        [guard, generation, fetch, reply = std::move(reply)](const auto& response) {
            if (guard && guard->generation_ == generation && guard->active_ == fetch)
                reply(response);
        },
        resource_max_wire_bytes);
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
    ledger::add(ledger::Stage::resource_decode,
                ledger::Metric::metadata_copy_bytes,
                manifest_copy_bytes(manifest));
    transfer->reply = std::move(reply);
    active_ = transfer;
    if (manifest.byte_length) {
        // Every read response carries the complete pinned manifest and renews
        // a released cached lease. Validate it before assembling any bytes.
        transfer->bytes.reserve(static_cast<qsizetype>(manifest.byte_length));
        readNext(transfer);
        return;
    }
    // An empty resource has no legal read offset. Its describe still checks
    // remote ownership/version/cache state before the empty digest is accepted.
    request(transfer,
            transport::json_ledger::from_utf8("resources.describe"),
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
            transport::json_ledger::sha256_hex(transfer->bytes, ledger::Stage::resource_decode) !=
                manifest.sha256) {
            finish(transfer,
                   failure<QByteArray>("Resource length, chunk count or SHA-256 did not match"));
            return;
        }
        finish(transfer, {Status::success, std::move(transfer->bytes), std::nullopt});
        return;
    }
    const auto params = parameters(manifest, offset);
    request(
        transfer,
        transport::json_ledger::from_utf8("resources.read"),
        params,
        [this, transfer, offset](const auto& response) {
            if (response.value("status") != "success") {
                finish(transfer, remote_failure(response));
                return;
            }
            const auto data = response.value("data").toObject();
            const auto described = parseManifest(data.value("manifest").toObject());
            const auto expected_size = std::min<std::uint64_t>(
                resource_chunk_bytes, transfer->manifest.byte_length - offset);
            std::uint64_t chunk_offset{};
            std::uint32_t raw_length{};
            const auto text = data.value("data_base64").toString();
            const auto encoded = text.toLatin1();
            ledger::add(
                ledger::Stage::resource_decode, ledger::Metric::model_copy_bytes, encoded.size());
            // QCbor stringAt makes this owned UTF-16 QString. toLatin1 and the
            // once-reserved Base64 encode/decode outputs are counted separately.
            ledger::add(ledger::Stage::resource_decode,
                        ledger::Metric::library_internal_copy_bytes,
                        text.size() * 2);
            if (!described.ok() || !same_manifest(*described.value, transfer->manifest) ||
                !unsigned_string(data.value("offset"), chunk_offset) || chunk_offset != offset ||
                !bounded_integer(data.value("raw_length"), raw_length) ||
                raw_length != expected_size || data.value("encoding") != "base64" ||
                !data.value("data_base64").isString() ||
                static_cast<std::uint64_t>(encoded.size()) != ((expected_size + 2) / 3) * 4) {
                finish(transfer,
                       failure<QByteArray>(
                           "Resource chunk identity, order, offset or size is invalid"));
                return;
            }
            const auto decoded =
                QByteArray::fromBase64Encoding(encoded, QByteArray::AbortOnBase64DecodingErrors);
            ledger::add(ledger::Stage::resource_decode,
                        ledger::Metric::decoded_bytes,
                        decoded.decoded.size());
            ledger::add(ledger::Stage::resource_decode,
                        ledger::Metric::model_copy_bytes,
                        decoded.decoded.size() + encoded.size());
            if (!decoded || decoded.decoded.size() != raw_length ||
                decoded.decoded.toBase64() != encoded) {
                finish(transfer, failure<QByteArray>("Resource chunk has invalid base64 encoding"));
                return;
            }
            const auto previous_size = transfer->bytes.size();
            const auto previous_capacity = transfer->bytes.capacity();
            transfer->bytes.append(decoded.decoded);
            ledger::add(ledger::Stage::resource_decode,
                        ledger::Metric::model_copy_bytes,
                        decoded.decoded.size());
            if (transfer->bytes.capacity() != previous_capacity)
                ledger::add(ledger::Stage::resource_decode,
                            ledger::Metric::model_copy_bytes,
                            previous_size);
            ledger::cover(ledger::Stage::resource_decode);
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
