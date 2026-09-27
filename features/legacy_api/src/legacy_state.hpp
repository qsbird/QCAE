#pragma once
#include "qcae/core.hpp"
#include "qcae/model_delta.hpp"
#include "qcae/state_codec.hpp"
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace qcae::legacy_detail {
struct HistoryEntry {
    TransactionId transaction;
    std::string label;
    ModelDelta delta;
    std::string content_state;
};
struct RecordedOperation {
    std::string signature;
    ChangeReceipt receipt;
};
struct HostOperation {
    std::string signature;
    DocumentInfo result;
};
struct SaveIntent {
    std::string host_key;
    std::string signature;
    std::string path;
    std::string token;
    std::string project_id;
    std::string snapshot;
    bool save_as{};
};
struct Data {
    Limits limits;
    std::string application_nonce;
    std::uint64_t next_id{1};
    std::optional<DocumentInfo> document;
    Model model;
    std::string initial_content_state;
    std::vector<HistoryEntry> history;
    std::size_t cursor{};
    std::map<std::string, RecordedOperation> operations;
    std::map<std::string, HostOperation> host_operations;
    std::optional<SaveIntent> save_intent;
    bool recoverable{};
};
std::string nonce();
void update_document(Data&);
std::size_t entity_count(const Model&);
std::optional<Diagnostic> validate_candidate(const Model&, const Limits&);
Data decode_data(std::string_view, Limits);
Data decode_project(std::string_view, Limits);
} // namespace qcae::legacy_detail
