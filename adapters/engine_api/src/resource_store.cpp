#include "qcae/resource_store.hpp"
#include <QCryptographicHash>
#include <QUuid>
#include <algorithm>
#include <stdexcept>

namespace qcae::ipc {
namespace {
template <class T> Result<T> success(T value) {
    return {Status::success, std::move(value), std::nullopt};
}
template <class T> Result<T> failure(ErrorCode code, const char* message, const char* field) {
    const auto status =
        code == ErrorCode::revision_conflict || code == ErrorCode::document_epoch_expired
            ? Status::conflict
            : Status::failed;
    return {status, std::nullopt, Diagnostic{code, message, field}};
}
QString text(const std::string& value) {
    return QString::fromStdString(value);
}
bool valid(const ResourceVersion& version) {
    return !version.document.id.value.empty() && !version.document.epoch.value.empty() &&
           version.document.id.value.size() <= 256 && version.document.epoch.value.size() <= 256 &&
           version.view_session_id.size() <= 256 &&
           (version.view_session_id.empty() ? version.view_revision == 0
                                            : version.view_revision != 0);
}
} // namespace
QJsonObject resource_manifest_json(const ResourceManifest& manifest) {
    return {{"resource_id", text(manifest.resource_id)},
            {"document_id", text(manifest.version.document.id.value)},
            {"document_epoch", text(manifest.version.document.epoch.value)},
            {"revision", QString::number(manifest.version.revision)},
            {"view_session_id", text(manifest.version.view_session_id)},
            {"view_revision", QString::number(manifest.version.view_revision)},
            {"byte_length", QString::number(manifest.byte_length)},
            {"chunk_bytes", static_cast<int>(manifest.chunk_bytes)},
            {"chunk_count", static_cast<int>(manifest.chunk_count)},
            {"sha256", text(manifest.sha256)},
            {"media_type", text(manifest.media_type)}};
}
QJsonObject resource_chunk_json(const ResourceChunk& chunk) {
    return {{"manifest", resource_manifest_json(chunk.manifest)},
            {"offset", QString::number(chunk.offset)},
            {"raw_length", chunk.bytes.size()},
            {"encoding", "base64"},
            {"data_base64", QString::fromLatin1(chunk.bytes.toBase64())}};
}
ResourceStore::ResourceStore() : ResourceStore(Limits{}) {}
ResourceStore::ResourceStore(Limits limits, Clock clock)
    : limits_(limits), clock_(std::move(clock)) {
    if (!limits_.max_resource_bytes || limits_.max_resource_bytes > resource_max_bytes ||
        limits_.max_cached_bytes < limits_.max_resource_bytes || !limits_.max_resources ||
        limits_.lease.count() <= 0)
        throw std::invalid_argument("Invalid resource cache limits");
    if (!clock_)
        clock_ = [] { return std::chrono::steady_clock::now(); };
}
void ResourceStore::prune() {
    const auto now = clock_();
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->second.expires <= now) {
            cached_bytes_ -= it->second.manifest.byte_length;
            it = entries_.erase(it);
        } else
            ++it;
    }
}
Result<ResourceManifest> ResourceStore::publish(const Caller& caller,
                                                const ResourceVersion& version,
                                                QByteArray bytes,
                                                std::string media_type) {
    if (caller.principal.empty() || !valid(version) || media_type.empty() ||
        media_type.size() > 256)
        return failure<ResourceManifest>(ErrorCode::invalid_input,
                                         "Resource publication needs owner, version and media type",
                                         "resource");
    const auto length = static_cast<std::uint64_t>(bytes.size());
    if (length > limits_.max_resource_bytes)
        return failure<ResourceManifest>(
            ErrorCode::resource_limit, "Resource exceeds its byte limit", "byte_length");
    prune();
    while (entries_.size() >= limits_.max_resources ||
           cached_bytes_ > limits_.max_cached_bytes - length) {
        auto oldest = entries_.end();
        for (auto it = entries_.begin(); it != entries_.end(); ++it)
            if (!it->second.leased &&
                (oldest == entries_.end() || it->second.use_order < oldest->second.use_order))
                oldest = it;
        if (oldest == entries_.end())
            return failure<ResourceManifest>(ErrorCode::resource_limit,
                                             "Resource cache is occupied by active leases",
                                             "resources");
        cached_bytes_ -= oldest->second.manifest.byte_length;
        entries_.erase(oldest);
    }
    ResourceManifest manifest;
    manifest.resource_id = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    manifest.version = version;
    manifest.byte_length = length;
    manifest.chunk_count = static_cast<std::uint32_t>(length / resource_chunk_bytes +
                                                      (length % resource_chunk_bytes != 0));
    manifest.sha256 =
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex().toStdString();
    manifest.media_type = std::move(media_type);
    entries_.emplace(manifest.resource_id,
                     Entry{manifest,
                           std::move(bytes),
                           caller.principal,
                           clock_() + limits_.lease,
                           ++next_order_,
                           true});
    cached_bytes_ += length;
    return success(std::move(manifest));
}
Result<ResourceStore::Entry*> ResourceStore::find(const Caller& caller,
                                                  const std::string& resource_id,
                                                  const ResourceVersion* current) {
    prune();
    const auto found = entries_.find(resource_id);
    if (found == entries_.end() || found->second.owner != caller.principal)
        return failure<Entry*>(ErrorCode::entity_not_found,
                               "Resource is unavailable or its lease expired",
                               "resource_id");
    auto& entry = found->second;
    if (current) {
        if (entry.manifest.version.document.id != current->document.id ||
            entry.manifest.version.document.epoch != current->document.epoch)
            return failure<Entry*>(ErrorCode::document_epoch_expired,
                                   "Resource belongs to another document or epoch",
                                   "resource_id");
        if (!same_resource_version(entry.manifest.version, *current))
            return failure<Entry*>(ErrorCode::revision_conflict,
                                   "Resource model or view version is stale",
                                   "resource_id");
    }
    entry.use_order = ++next_order_;
    return success(&entry);
}
Result<ResourceManifest> ResourceStore::describe(const Caller& caller,
                                                 const std::string& resource_id,
                                                 const ResourceVersion& current) {
    const auto found = find(caller, resource_id, &current);
    if (!found.ok())
        return {found.status, std::nullopt, found.error};
    auto& entry = **found.value;
    if (!entry.leased) {
        entry.leased = true;
        entry.expires = clock_() + limits_.lease;
    }
    return success(entry.manifest);
}
Result<ResourceChunk> ResourceStore::read(const Caller& caller,
                                          const std::string& resource_id,
                                          const ResourceVersion& current,
                                          std::uint64_t offset) {
    const auto found = find(caller, resource_id, &current);
    if (!found.ok())
        return {found.status, std::nullopt, found.error};
    const auto& entry = **found.value;
    if (offset >= entry.manifest.byte_length || offset % resource_chunk_bytes != 0)
        return failure<ResourceChunk>(ErrorCode::invalid_input,
                                      "Chunk offset is outside the resource or not aligned",
                                      "offset");
    const auto size =
        std::min<std::uint64_t>(resource_chunk_bytes, entry.manifest.byte_length - offset);
    return success(ResourceChunk{
        entry.manifest,
        offset,
        entry.bytes.mid(static_cast<qsizetype>(offset), static_cast<qsizetype>(size))});
}
Result<bool> ResourceStore::release(const Caller& caller, const std::string& resource_id) {
    const auto found = find(caller, resource_id, nullptr);
    if (!found.ok())
        return {found.status, std::nullopt, found.error};
    (**found.value).leased = false;
    return success(true);
}
} // namespace qcae::ipc
