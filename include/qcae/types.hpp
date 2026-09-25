#pragma once
#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

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
    std::optional<double> poisson_ratio{};
    bool operator==(const Material&) const = default;
};
} // namespace qcae
