#pragma once
#include "qcae/record_store.hpp"
#include "qcae/workspace_store.hpp"
#include <functional>
#include <memory>

namespace qcae {
// Reads a protected legacy source without configuring a writer or migrating it.
// The source must be quiescent; an active engine's inode lease rejects this probe.
// Main/WAL bytes are verified while staging a disposable snapshot, where SQLite
// alone reconstructs sidecars. A changing foreign-writer source is rejected.
StoredWorkspace read_legacy_workspace_readonly(const std::string& path,
                                               std::size_t max_payload_bytes = 64 * 1024 * 1024);
struct StoreOptions {
    std::size_t max_payload_bytes{64 * 1024 * 1024};
    // Deterministic tests only. A production host never configures request-selected hooks.
    std::function<void(const std::string&)> fault;
    std::size_t max_rows{500000};
    std::size_t max_batch_rows{500000};
    std::size_t max_key_bytes{4096};
};
class SqliteWorkspaceStore final : public IWorkspaceStore, public IRecordStore {
  public:
    explicit SqliteWorkspaceStore(const std::string& path, StoreOptions options = {});
    ~SqliteWorkspaceStore();
    std::optional<StoredWorkspace> load() override;
    std::uint64_t commit(std::uint64_t expected_generation, const std::string& payload) override;
    LoadedRows load_rows() override;
    BatchReceipt commit_rows(const StoreBatch&) override;
    std::string acquire_project(const std::string& path) override;
    void release_projects_except(const std::string& path) noexcept override;
    StoredProject read_project(const std::string& path) override;
    void publish_project(const std::string& path, const StoredProject&) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace qcae
