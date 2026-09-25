#pragma once

#include "qcae/model.hpp"
#include "qcae/workspace_store.hpp"

#include <compare>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace qcae {

struct DocumentInfo {
    DocumentRef document;
    Revision revision{};
    std::string content_state;
    std::string name;
    std::string project_id;
    std::string saved_path;
    std::string saved_content_state;
    std::size_t material_count{};
    bool dirty{};
    bool durable{false};
};
enum class ClosePolicy { keep_recovery, discard };
struct ModelSnapshot : Model {
    DocumentInfo info;
};
struct CreateMaterial {
    std::string name;
    Quantity young_modulus;
};
struct SetYoungModulus {
    EntityId id;
    Quantity young_modulus;
};
using MaterialCommand = std::variant<CreateMaterial, SetYoungModulus>;
struct ChangePreview {
    PreviewId id;
    WriteContext context;
    EntityId affected_entity;
    double normalized_modulus_mpa{};
    bool creates_entity{};
};
struct ChangeReceipt {
    TransactionId transaction;
    Revision committed_revision{};
    Revision current_revision{};
    std::string current_content_state;
    bool replayed{};
};
struct HistoryItem {
    TransactionId transaction;
    std::string label;
    bool applied{};
};
struct HistorySnapshot {
    std::vector<HistoryItem> items;
    std::size_t cursor{};
    Revision revision{};
};
struct Limits {
    std::size_t max_materials{10000};
    std::size_t max_history_entries{128};
    std::size_t max_previews{128};
    std::size_t max_idempotency_records{4096};
    std::size_t max_name_bytes{1024};
    std::size_t max_entities{100000};
    std::size_t max_relations{500000};
};

// One in-memory document; no storage/recovery claims. All writes serialize internally.
class MemoryApplication {
  public:
    explicit MemoryApplication(Limits limits = {},
                               std::shared_ptr<IWorkspaceStore> store = {},
                               std::function<bool(const ProfileRef&)> profile_supported = {});
    ~MemoryApplication();
    MemoryApplication(const MemoryApplication&) = delete;
    MemoryApplication& operator=(const MemoryApplication&) = delete;
    bool durable() const noexcept;
    bool recovery_available() const;

    Result<DocumentInfo>
    create_document(const Caller&, const std::string& name, const std::string& idempotency_key);
    Result<DocumentInfo> current_document() const;
    Result<DocumentInfo>
    open_document(const Caller&, const std::string& path, const std::string& idempotency_key);
    Result<DocumentInfo> recover_document(const Caller&, const std::string& idempotency_key);
    Result<DocumentInfo> save_document(const Caller&,
                                       const WriteContext&,
                                       const std::string& path,
                                       bool save_as,
                                       const std::string& idempotency_key);
    Result<DocumentInfo> close_document(const Caller&,
                                        const WriteContext&,
                                        ClosePolicy,
                                        const std::string& idempotency_key);
    Result<DocumentInfo> host_operation(const Caller&,
                                        const std::string& operation_name,
                                        const std::string& idempotency_key) const;
    Result<ModelSnapshot> snapshot(const DocumentRef&) const;
    Result<ChangePreview> preview(const Caller&, const WriteContext&, const MaterialCommand&);
    // Import into an empty document only. No direct replacement/write bypass.
    Result<ChangePreview> preview_import(const Caller&, const WriteContext&, const Model&);
    Result<ChangePreview> preview_edit(const Caller&, const WriteContext&, const ModelEdit&);
    Result<ChangeReceipt> commit(const Caller&,
                                 const WriteContext&,
                                 const PreviewId&,
                                 const std::string& idempotency_key);
    Result<ChangeReceipt>
    undo(const Caller&, const WriteContext&, const std::string& idempotency_key);
    Result<ChangeReceipt>
    redo(const Caller&, const WriteContext&, const std::string& idempotency_key);
    Result<HistorySnapshot> history(const DocumentRef&) const;
    Result<ChangeReceipt> operation(const Caller&,
                                    const DocumentRef&,
                                    const std::string& operation_name,
                                    const std::string& idempotency_key) const;

  private:
    struct State;
    std::unique_ptr<State> state_;
};

[[nodiscard]] const char* status_name(Status);
[[nodiscard]] const char* error_name(ErrorCode);

} // namespace qcae
