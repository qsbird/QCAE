#pragma once
#include "qcae/workspace_store.hpp"
#include <compare>
#include <memory>
#include <vector>

namespace qcae {
enum class StoreSpace : std::uint8_t {
    document_record = 1,
    history_entry,
    operation_fact,
    host_operation_fact,
    document_metadata,
    task_record,
    artifact_record
};
struct StoreKey {
    StoreSpace space;
    std::string identity;
    auto operator<=>(const StoreKey&) const = default;
};
using SharedStoreBytes = std::shared_ptr<const std::string>;
struct StoredRow {
    StoreKey key;
    SharedStoreBytes value;
};
struct RowMutation {
    StoreKey key;
    SharedStoreBytes after; // Null deletes the row; an empty string is a present value.
};
struct StoreBatch {
    std::uint64_t expected_generation{};
    std::string transaction_id;
    std::vector<RowMutation> mutations;
};
struct LoadedRows {
    std::uint64_t generation{};
    std::vector<StoredRow> rows;
    // A legacy workspace must be migrated to a separate destination, never rewritten by load.
    std::optional<StoredWorkspace> legacy;
};
struct BatchReceipt {
    std::uint64_t generation{};
    std::uint64_t rows_written{};
    std::uint64_t payload_bytes{};
};
class IRecordStore {
  public:
    virtual ~IRecordStore() = default;
    virtual LoadedRows load_rows() = 0;
    virtual BatchReceipt commit_rows(const StoreBatch&) = 0;
};
} // namespace qcae
