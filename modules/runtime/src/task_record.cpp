#include "qcae/task_service.hpp"
#include <bit>
#include <cmath>
#include <stdexcept>

namespace qcae {
namespace {
[[noreturn]] void invalid() {
    throw std::runtime_error("Invalid or unsupported task record");
}
class Writer {
  public:
    std::string bytes;
    void number(std::uint64_t value) {
        for (unsigned index = 0; index < 8; ++index)
            bytes.push_back(static_cast<char>((value >> (index * 8)) & 255));
    }
    void text(std::string_view value) {
        number(value.size());
        bytes.append(value);
    }
};
class Reader {
  public:
    explicit Reader(std::string_view value) : bytes(value) {
        if (bytes.size() > 65536)
            invalid();
    }
    std::uint64_t number() {
        if (bytes.size() - offset < 8)
            invalid();
        std::uint64_t result{};
        for (unsigned index = 0; index < 8; ++index)
            result |= std::uint64_t(static_cast<unsigned char>(bytes[offset++])) << (index * 8);
        return result;
    }
    std::string text() {
        const auto size = number();
        if (size > bytes.size() - offset)
            invalid();
        auto result = std::string(bytes.substr(offset, size));
        offset += size;
        return result;
    }
    bool boolean() {
        auto value = number();
        if (value > 1)
            invalid();
        return value != 0;
    }
    void finish() {
        if (offset != bytes.size())
            invalid();
    }

  private:
    std::string_view bytes;
    std::size_t offset{};
};
bool allowed_transition(TaskState before, TaskState after, bool artifact_completed) {
    if (artifact_completed &&
        (before == TaskState::interrupted || before == TaskState::outcome_unknown) &&
        after == TaskState::succeeded)
        return true;
    if (after == TaskState::outcome_unknown)
        return !task_terminal(before);
    if (after == TaskState::interrupted)
        return !task_terminal(before) || before == TaskState::outcome_unknown;
    if (before == TaskState::queued)
        return after == TaskState::running || after == TaskState::cancel_requested ||
               after == TaskState::cancelled;
    if (before == TaskState::running)
        return after == TaskState::running || after == TaskState::cancel_requested ||
               after == TaskState::committing || after == TaskState::failed;
    if (before == TaskState::cancel_requested)
        return after == TaskState::cancelled || after == TaskState::failed;
    if (before == TaskState::committing)
        return after == TaskState::succeeded || after == TaskState::conflicted ||
               after == TaskState::failed || after == TaskState::outcome_unknown;
    return false;
}
void validate(const TaskRecord& record) {
    const auto& profile = record.input.profile;
    const bool any_profile = !profile.profile_id.empty() || !profile.profile_version.empty() ||
                             !profile.definition_digest.empty();
    const bool complete_profile = !profile.profile_id.empty() && !profile.profile_version.empty() &&
                                  !profile.definition_digest.empty();
    if (record.id.empty() || record.id.size() > 128 || record.caller.principal.empty() ||
        record.idempotency_key.empty() || record.operation.empty() || record.signature.empty() ||
        record.input.document.id.value.empty() || record.input.document.epoch.value.empty() ||
        (any_profile && !complete_profile) || !std::isfinite(record.progress) ||
        record.progress < 0 || record.progress > 1 || record.events.empty() ||
        record.events.size() > 128 || record.events.front().state != TaskState::queued ||
        record.events.back().state != record.state ||
        record.events.back().progress != record.progress ||
        (record.receipt && record.artifact_receipt) ||
        (record.state == TaskState::succeeded) !=
            (bool(record.receipt) || bool(record.artifact_receipt)))
        invalid();
    std::uint64_t previous_sequence = 0;
    double previous_progress = 0;
    for (std::size_t index = 0; index < record.events.size(); ++index) {
        const auto& event = record.events[index];
        if (event.sequence <= previous_sequence || !std::isfinite(event.progress) ||
            event.progress < previous_progress || event.progress > 1 ||
            (index && !allowed_transition(record.events[index - 1].state,
                                          event.state,
                                          record.artifact_receipt.has_value())))
            invalid();
        previous_sequence = event.sequence;
        previous_progress = event.progress;
    }
    if (record.receipt && (record.receipt->transaction.value.empty() || record.progress != 1 ||
                           record.receipt->committed_revision <= record.input.revision))
        invalid();
    if (record.artifact_receipt &&
        (record.artifact_receipt->artifact_id.empty() ||
         record.artifact_receipt->artifact_id.size() > 128 ||
         record.artifact_receipt->input_revision != record.input.revision ||
         record.artifact_receipt->manifest_sha256.size() != 64 ||
         record.artifact_receipt->manifest_sha256.find_first_not_of("0123456789abcdef") !=
             std::string::npos ||
         record.progress != 1))
        invalid();
}
TaskState state(Reader& reader) {
    const auto value = reader.number();
    if (value > static_cast<unsigned>(TaskState::outcome_unknown))
        invalid();
    return static_cast<TaskState>(value);
}
} // namespace
bool task_terminal(TaskState value) noexcept {
    return value == TaskState::succeeded || value == TaskState::cancelled ||
           value == TaskState::conflicted || value == TaskState::failed ||
           value == TaskState::interrupted || value == TaskState::outcome_unknown;
}
const char* task_state_name(TaskState value) noexcept {
    switch (value) {
    case TaskState::queued:
        return "queued";
    case TaskState::running:
        return "running";
    case TaskState::cancel_requested:
        return "cancel_requested";
    case TaskState::committing:
        return "committing";
    case TaskState::succeeded:
        return "succeeded";
    case TaskState::cancelled:
        return "cancelled";
    case TaskState::conflicted:
        return "conflicted";
    case TaskState::failed:
        return "failed";
    case TaskState::interrupted:
        return "interrupted";
    case TaskState::outcome_unknown:
        return "outcome_unknown";
    }
    return "invalid";
}
std::string encode_task_record(const TaskRecord& record) {
    validate(record);
    Writer w;
    w.text("QCAE-TASK");
    w.number(record.artifact_receipt ? 3 : 2);
    w.text(record.id);
    w.text(record.caller.principal);
    w.text(record.idempotency_key);
    w.text(record.operation);
    w.text(record.signature);
    w.text(record.input.document.id.value);
    w.text(record.input.document.epoch.value);
    w.number(record.input.revision);
    w.text(record.input.profile.profile_id);
    w.text(record.input.profile.profile_version);
    w.text(record.input.profile.definition_digest);
    w.number(static_cast<unsigned>(record.state));
    w.number(std::bit_cast<std::uint64_t>(record.progress));
    w.number(record.events.size());
    for (const auto& event : record.events) {
        w.number(event.sequence);
        w.number(static_cast<unsigned>(event.state));
        w.number(std::bit_cast<std::uint64_t>(event.progress));
    }
    w.number(bool(record.receipt));
    if (record.receipt) {
        const auto& receipt = *record.receipt;
        w.text(receipt.transaction.value);
        w.number(receipt.committed_revision);
        w.number(receipt.current_revision);
        w.text(receipt.current_content_state);
        w.number(receipt.replayed);
        w.text(receipt.primary_entity.value);
    }
    w.number(bool(record.diagnostic));
    if (record.diagnostic) {
        w.number(static_cast<unsigned>(record.diagnostic->code));
        w.text(record.diagnostic->message);
        w.text(record.diagnostic->field);
    }
    if (record.artifact_receipt) {
        w.number(1);
        w.text(record.artifact_receipt->artifact_id);
        w.number(record.artifact_receipt->input_revision);
        w.text(record.artifact_receipt->manifest_sha256);
    }
    if (w.bytes.size() > 65536)
        invalid();
    return std::move(w.bytes);
}
TaskRecord decode_task_record(std::string_view bytes) {
    Reader r(bytes);
    if (r.text() != "QCAE-TASK")
        invalid();
    const auto schema = r.number();
    if (schema != 1 && schema != 2 && schema != 3)
        invalid();
    TaskRecord record;
    record.id = r.text();
    record.caller.principal = r.text();
    record.idempotency_key = r.text();
    record.operation = r.text();
    record.signature = r.text();
    record.input.document.id = DocumentId(r.text());
    record.input.document.epoch = DocumentEpoch(r.text());
    record.input.revision = r.number();
    record.input.profile = {r.text(), r.text(), r.text()};
    record.state = state(r);
    record.progress = std::bit_cast<double>(r.number());
    const auto count = r.number();
    if (count > 128)
        invalid();
    for (std::uint64_t index = 0; index < count; ++index)
        record.events.push_back({r.number(), state(r), std::bit_cast<double>(r.number())});
    if (r.boolean()) {
        ChangeReceipt receipt;
        receipt.transaction = TransactionId(r.text());
        receipt.committed_revision = r.number();
        receipt.current_revision = r.number();
        receipt.current_content_state = r.text();
        receipt.replayed = r.boolean();
        if (schema >= 2)
            receipt.primary_entity = EntityId(r.text());
        record.receipt = std::move(receipt);
    }
    if (r.boolean()) {
        const auto code = r.number();
        if (code > static_cast<unsigned>(ErrorCode::schema_unsupported))
            invalid();
        record.diagnostic = Diagnostic{static_cast<ErrorCode>(code), r.text(), r.text()};
    }
    if (schema == 3) {
        if (!r.boolean())
            invalid();
        record.artifact_receipt = TaskArtifactReceipt{r.text(), r.number(), r.text()};
    }
    r.finish();
    validate(record);
    return record;
}
} // namespace qcae
