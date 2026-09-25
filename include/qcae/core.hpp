#pragma once

#include <compare>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace qcae {

template <class Tag> struct Id {
    std::string value;
    explicit Id(std::string text = {}) : value(std::move(text)) {}
    auto operator<=>(const Id&) const = default;
};
struct DocumentTag; struct EpochTag; struct EntityTag; struct PreviewTag; struct TransactionTag;
using DocumentId = Id<DocumentTag>;
using DocumentEpoch = Id<EpochTag>;
using EntityId = Id<EntityTag>;
using PreviewId = Id<PreviewTag>;
using TransactionId = Id<TransactionTag>;
using Revision = std::uint64_t;

struct ProfileRef {
    std::string profile_id;
    std::string profile_version;
    std::string definition_digest;
    bool operator==(const ProfileRef&) const = default;
};
struct TargetBinding {
    ProfileRef profile;
    std::string analysis_kind;
    bool operator==(const TargetBinding&) const = default;
};
struct Quantity {
    double value{};
    std::string unit;
};
enum class Status { success, needs_input, conflict, failed };
enum class ErrorCode {
    missing_input, invalid_input, invalid_unit, entity_not_found,
    document_not_found, document_already_open, document_epoch_expired,
    revision_conflict, preview_expired, idempotency_key_conflict,
    nothing_to_undo, nothing_to_redo, resource_limit, unsupported_capability
};
struct Diagnostic { ErrorCode code; std::string message; std::string field; };

template <class T> struct Result {
    Status status{Status::failed};
    std::optional<T> value;
    std::optional<Diagnostic> error;
    [[nodiscard]] bool ok() const { return status == Status::success && value.has_value(); }
};

// Supplied by a trusted host, never decoded from request parameters.
struct Caller { std::string principal; };
struct DocumentRef { DocumentId id; DocumentEpoch epoch; };
struct WriteContext { DocumentRef document; Revision expected_revision{}; };
struct Material {
    EntityId id;
    std::string name;
    double young_modulus_mpa{};
    bool operator==(const Material&) const = default;
};
struct DocumentInfo {
    DocumentRef document;
    Revision revision{};
    std::string content_state;
    std::string name;
    std::size_t material_count{};
    bool dirty{};
    bool durable{false};
};
struct ModelSnapshot { DocumentInfo info; std::vector<Material> materials; };
struct CreateMaterial { std::string name; Quantity young_modulus; };
struct SetYoungModulus { EntityId id; Quantity young_modulus; };
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
struct HistoryItem { TransactionId transaction; std::string label; bool applied{}; };
struct HistorySnapshot { std::vector<HistoryItem> items; std::size_t cursor{}; Revision revision{}; };
struct Limits {
    std::size_t max_materials{10000};
    std::size_t max_history_entries{128};
    std::size_t max_previews{128};
    std::size_t max_idempotency_records{4096};
    std::size_t max_name_bytes{1024};
};

// One in-memory document; no storage/recovery claims. All writes serialize internally.
class MemoryApplication {
public:
    explicit MemoryApplication(Limits limits = {});
    ~MemoryApplication();
    MemoryApplication(const MemoryApplication&) = delete;
    MemoryApplication& operator=(const MemoryApplication&) = delete;

    Result<DocumentInfo> create_document(const Caller&, const std::string& name,
                                         const std::string& idempotency_key);
    Result<ModelSnapshot> snapshot(const DocumentRef&) const;
    Result<ChangePreview> preview(const Caller&, const WriteContext&, const MaterialCommand&);
    Result<ChangeReceipt> commit(const Caller&, const WriteContext&, const PreviewId&,
                                const std::string& idempotency_key);
    Result<ChangeReceipt> undo(const Caller&, const WriteContext&, const std::string& idempotency_key);
    Result<ChangeReceipt> redo(const Caller&, const WriteContext&, const std::string& idempotency_key);
    Result<HistorySnapshot> history(const DocumentRef&) const;
    Result<ChangeReceipt> operation(const Caller&, const DocumentRef&,
                                   const std::string& operation_name, const std::string& idempotency_key) const;

private:
    struct State;
    std::unique_ptr<State> state_;
};

[[nodiscard]] const char* status_name(Status);
[[nodiscard]] const char* error_name(ErrorCode);

} // namespace qcae
