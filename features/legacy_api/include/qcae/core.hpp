#pragma once

#include "qcae/application_types.hpp"
#include "qcae/record_application.hpp"
#include "qcae/read_view.hpp"
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

// Compatibility facade over one authoritative RecordApplication.
// Persistence requires an explicitly supplied store.
class MemoryApplication {
  public:
    explicit MemoryApplication(Limits limits = {},
                               std::shared_ptr<IWorkspaceStore> store = {},
                               std::function<bool(const ProfileRef&)> profile_supported = {},
                               std::vector<OwnedRowHandler> owned_row_handlers = {},
                               std::shared_ptr<const RecordRegistry> registry = {});
    ~MemoryApplication();
    MemoryApplication(const MemoryApplication&) = delete;
    MemoryApplication& operator=(const MemoryApplication&) = delete;
    RecordApplication& record_application() noexcept;
    const RecordApplication& record_application() const noexcept;
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
