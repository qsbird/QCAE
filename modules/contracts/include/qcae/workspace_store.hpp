#pragma once
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

namespace qcae {
struct StoredWorkspace {
    std::uint64_t generation{};
    std::string payload;
};
struct StoredProject {
    std::string save_token;
    std::string payload;
};
// Uncertain means a durable write may already have committed: stop further writes and recover.
class StorageError : public std::runtime_error {
  public:
    StorageError(const std::string& message, bool uncertain = false)
        : std::runtime_error(message), uncertain_(uncertain) {}
    bool uncertain() const noexcept {
        return uncertain_;
    }

  private:
    bool uncertain_;
};
class IWorkspaceStore {
  public:
    virtual ~IWorkspaceStore() = default;
    virtual std::optional<StoredWorkspace> load() = 0;
    virtual std::uint64_t commit(std::uint64_t expected_generation, const std::string& payload) = 0;
    // Retained exclusive project lease; returns canonical path. Includes inode identity locking.
    virtual std::string acquire_project(const std::string& path) = 0;
    virtual void release_projects_except(const std::string& path) noexcept = 0;
    virtual StoredProject read_project(const std::string& canonical_path) = 0;
    // Publishes a complete independent SQLite snapshot; never copies an active database file.
    virtual void publish_project(const std::string& canonical_path, const StoredProject&) = 0;
};
} // namespace qcae
