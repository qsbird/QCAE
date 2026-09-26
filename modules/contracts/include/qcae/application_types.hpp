#pragma once

#include "qcae/document_info.hpp"
#include <variant>
#include <vector>

namespace qcae {
enum class ClosePolicy { keep_recovery, discard };
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

} // namespace qcae
