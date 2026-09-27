#pragma once
#include "qcae/resource.hpp"
#include <QByteArray>
#include <QJsonObject>
#include <chrono>
#include <functional>
#include <map>

namespace qcae::ipc {
struct ResourceChunk {
    ResourceManifest manifest;
    std::uint64_t offset{};
    QByteArray bytes;
};
QJsonObject resource_manifest_json(const ResourceManifest&);
QJsonObject resource_chunk_json(const ResourceChunk&);

// Engine-internal publication only. The host supplies the current authoritative version
// on describe/read; client-declared versions alone must never authorize stale resources.
class ResourceStore {
  public:
    struct Limits {
        std::uint64_t max_resource_bytes{resource_max_bytes};
        std::uint64_t max_cached_bytes{64 * 1024 * 1024};
        std::size_t max_resources{32};
        std::chrono::milliseconds lease{60000};
    };
    using Clock = std::function<std::chrono::steady_clock::time_point()>;
    ResourceStore();
    explicit ResourceStore(Limits, Clock = {});
    Result<ResourceManifest>
    publish(const Caller&, const ResourceVersion&, QByteArray, std::string media_type);
    Result<ResourceManifest>
    describe(const Caller&, const std::string& resource_id, const ResourceVersion& current);
    Result<ResourceChunk> read(const Caller&,
                               const std::string& resource_id,
                               const ResourceVersion& current,
                               std::uint64_t offset);
    Result<bool> release(const Caller&, const std::string& resource_id);
    [[nodiscard]] std::uint64_t cached_bytes() const {
        return cached_bytes_;
    }
    [[nodiscard]] std::size_t size() const {
        return entries_.size();
    }

  private:
    struct Entry {
        ResourceManifest manifest;
        QByteArray bytes;
        std::string owner;
        std::chrono::steady_clock::time_point expires;
        std::uint64_t use_order{};
        bool leased{true};
    };
    Result<Entry*> find(const Caller&, const std::string&, const ResourceVersion*);
    void prune();
    Limits limits_;
    Clock clock_;
    std::map<std::string, Entry> entries_;
    std::uint64_t cached_bytes_{}, next_order_{};
};
} // namespace qcae::ipc
