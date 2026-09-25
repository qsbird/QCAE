#pragma once
#include "qcae/workspace_store.hpp"
#include <functional>
#include <memory>

namespace qcae {
struct StoreOptions {
    std::size_t max_payload_bytes{64 * 1024 * 1024};
    // Deterministic tests only. A production host never configures request-selected hooks.
    std::function<void(const std::string&)> fault;
};
class SqliteWorkspaceStore final : public IWorkspaceStore {
  public:
    explicit SqliteWorkspaceStore(const std::string& path, StoreOptions options = {});
    ~SqliteWorkspaceStore();
    std::optional<StoredWorkspace> load() override;
    std::uint64_t commit(std::uint64_t expected_generation, const std::string& payload) override;
    std::string acquire_project(const std::string& path) override;
    void release_projects_except(const std::string& path) noexcept override;
    StoredProject read_project(const std::string& path) override;
    void publish_project(const std::string& path, const StoredProject&) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace qcae
