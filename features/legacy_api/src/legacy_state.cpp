#include "legacy_state.hpp"

#include <algorithm>
#include <iomanip>
#include <random>
#include <sstream>

namespace qcae::legacy_detail {
std::string nonce() {
    std::random_device source;
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (int i = 0; i != 4; ++i)
        out << std::setw(8) << source();
    return out.str();
}

void update_document(Data& data) {
    data.document->material_count = data.model.materials.size();
    const auto& clean = data.document->saved_content_state.empty()
                            ? data.initial_content_state
                            : data.document->saved_content_state;
    data.document->dirty = data.document->content_state != clean;
}

std::size_t entity_count(const Model& model) {
    return model.materials.size() + model.nodes.size() + model.sections.size() +
           model.beams.size() + model.parts.size() + model.assemblies.size() + model.sets.size() +
           model.includes.size() + model.forces.size() + model.constraints.size() +
           model.analyses.size();
}

std::size_t relation_count(const Model& model) {
    std::size_t count = model.sources.size() * 2;
    for (const auto& beam : model.beams) {
        (void)beam;
        count += 3;
    }
    for (const auto& section : model.sections) {
        (void)section;
        ++count;
    }
    for (const auto& force : model.forces) {
        (void)force;
        ++count;
    }
    for (const auto& part : model.parts)
        count += part.members.size();
    for (const auto& assembly : model.assemblies)
        count += assembly.children.size();
    for (const auto& set : model.sets)
        count += set.members.size();
    for (const auto& include : model.includes)
        count += include.members.size() + (include.parent ? 1 : 0);
    for (const auto& constraint : model.constraints)
        count += constraint.nodes.size();
    for (const auto& analysis : model.analyses)
        count += analysis.forces.size() + analysis.constraints.size();
    return count;
}

bool has_control(const std::string& value) {
    return std::any_of(
        value.begin(), value.end(), [](unsigned char c) { return c < 32 || c == 127; });
}

std::optional<Diagnostic> validate_candidate(const Model& model, const Limits& limits) {
    if (model.materials.size() > limits.max_materials)
        return Diagnostic{ErrorCode::resource_limit, "Material limit reached", "materials"};
    if (entity_count(model) > limits.max_entities)
        return Diagnostic{ErrorCode::resource_limit, "Entity limit reached", "model"};
    if (relation_count(model) > limits.max_relations)
        return Diagnostic{ErrorCode::resource_limit, "Relation limit reached", "model"};
    for (const auto& entity : model_entities(model)) {
        if (entity.name.size() > limits.max_name_bytes)
            return Diagnostic{
                ErrorCode::resource_limit, "Entity name exceeds byte limit", entity.id.value};
        if (has_control(entity.name))
            return Diagnostic{ErrorCode::invalid_input,
                              "Entity name contains control characters",
                              entity.id.value};
    }
    for (const auto& include : model.includes)
        if (include.path.size() > limits.max_name_bytes)
            return Diagnostic{
                ErrorCode::resource_limit, "Include path exceeds byte limit", include.id.value};
        else if (has_control(include.path))
            return Diagnostic{ErrorCode::invalid_input,
                              "Include path contains control characters",
                              include.id.value};
    for (const auto& diagnostic : validate_model(model))
        return diagnostic;
    return std::nullopt;
}

using state_codec::Reader;
DocumentInfo read_info(Reader& r) {
    DocumentInfo info;
    info.document.id = DocumentId(r.text());
    info.document.epoch = DocumentEpoch(r.text());
    info.revision = r.number();
    info.content_state = r.text();
    info.name = r.text();
    info.project_id = r.text();
    info.saved_path = r.text();
    info.saved_content_state = r.text();
    return info;
}
ChangeReceipt read_receipt(Reader& r) {
    ChangeReceipt value;
    value.transaction = TransactionId(r.text());
    value.committed_revision = r.number();
    value.current_revision = r.number();
    value.current_content_state = r.text();
    return value;
}
std::uint64_t bounded_count(Reader& r, std::uint64_t unit = 8) {
    const auto count = r.number();
    if (count > 500000 || count > r.remaining() / unit)
        throw state_codec::CodecError("Malformed state count");
    return count;
}
Data decode_data(std::string_view bytes, Limits limits) {
    Reader r(bytes);
    if (r.text() != "QCAE-WORKSPACE" || r.number() != 2)
        throw state_codec::CodecError("Unsupported workspace schema");
    Data data;
    data.limits = limits;
    data.application_nonce = r.text();
    data.next_id = r.number();
    if (data.application_nonce.empty() || data.next_id == 0)
        throw state_codec::CodecError("Malformed workspace identity");
    if (r.boolean())
        data.document = read_info(r);
    data.model = state_codec::read_model(r, limits.max_entities, limits.max_relations);
    data.initial_content_state = r.text();
    const auto history_count = bounded_count(r, 32);
    if (history_count > limits.max_history_entries)
        throw state_codec::CodecError("History exceeds configured limit");
    for (std::uint64_t i = 0; i < history_count; ++i) {
        HistoryEntry entry;
        entry.transaction = TransactionId(r.text());
        entry.label = r.text();
        entry.content_state = r.text();
        entry.delta = read_model_delta(r);
        data.history.push_back(std::move(entry));
    }
    data.cursor = r.number();
    if (data.cursor > data.history.size())
        throw state_codec::CodecError("Malformed history cursor");
    const auto operation_count = bounded_count(r, 32);
    if (operation_count > limits.max_idempotency_records)
        throw state_codec::CodecError("Operation count exceeds limit");
    for (std::uint64_t i = 0; i < operation_count; ++i) {
        auto key = r.text();
        RecordedOperation op{r.text(), read_receipt(r)};
        if (!data.operations.emplace(std::move(key), std::move(op)).second)
            throw state_codec::CodecError("Duplicate operation");
    }
    const auto host_count = bounded_count(r, 32);
    if (host_count > limits.max_idempotency_records - data.operations.size())
        throw state_codec::CodecError("Operation count exceeds limit");
    for (std::uint64_t i = 0; i < host_count; ++i) {
        auto key = r.text();
        HostOperation op{r.text(), read_info(r)};
        if (!data.host_operations.emplace(std::move(key), std::move(op)).second)
            throw state_codec::CodecError("Duplicate host operation");
    }
    if (r.boolean()) {
        SaveIntent intent;
        intent.host_key = r.text();
        intent.signature = r.text();
        intent.path = r.text();
        intent.token = r.text();
        intent.project_id = r.text();
        intent.snapshot = r.text();
        intent.save_as = r.boolean();
        data.save_intent = std::move(intent);
    }
    data.recoverable = r.boolean();
    r.finish();
    if (data.operations.size() + data.host_operations.size() > limits.max_idempotency_records)
        throw state_codec::CodecError("Operation count exceeds limit");
    if (data.document) {
        if (auto diagnostic = validate_candidate(data.model, limits))
            throw state_codec::CodecError("Invalid stored model: " + diagnostic->message);
        if (data.initial_content_state.empty() || data.document->content_state.empty() ||
            data.document->document.id.value.empty() ||
            data.document->document.epoch.value.empty() || data.document->name.empty())
            throw state_codec::CodecError("Malformed document identity");
        const auto expected_state = data.cursor == 0 ? data.initial_content_state
                                                     : data.history[data.cursor - 1].content_state;
        if (data.document->content_state != expected_state)
            throw state_codec::CodecError("History content state mismatch");
        Model baseline = data.model;
        for (std::size_t i = data.cursor; i > 0; --i)
            baseline = apply_model_delta(baseline, data.history[i - 1].delta, false);
        if (auto diagnostic = validate_candidate(baseline, limits))
            throw state_codec::CodecError("Invalid history baseline: " + diagnostic->message);
        Model replay = baseline;
        for (std::size_t i = 0; i < data.history.size(); ++i) {
            if (data.history[i].transaction.value.empty() || data.history[i].content_state.empty())
                throw state_codec::CodecError("Malformed history identity");
            replay = apply_model_delta(replay, data.history[i].delta, true);
            if (auto diagnostic = validate_candidate(replay, limits))
                throw state_codec::CodecError("Invalid history model: " + diagnostic->message);
            if (i + 1 == data.cursor && replay != data.model)
                throw state_codec::CodecError("History does not match current model");
        }
        if (data.cursor == 0 && baseline != data.model)
            throw state_codec::CodecError("History baseline mismatch");
        if (data.save_intent) {
            const auto& intent = *data.save_intent;
            if (intent.host_key.empty() || intent.signature.empty() || intent.path.empty() ||
                intent.token.empty() || intent.project_id.empty())
                throw state_codec::CodecError("Malformed save intent");
            Reader project_reader(intent.snapshot);
            if (project_reader.text() != "QCAE-PROJECT" || project_reader.number() != 1 ||
                project_reader.text() != intent.project_id ||
                project_reader.text() != data.document->name ||
                project_reader.text() != data.document->content_state ||
                state_codec::read_model(
                    project_reader, limits.max_entities, limits.max_relations) != data.model)
                throw state_codec::CodecError("Save intent snapshot mismatch");
            project_reader.finish();
        }
        update_document(data);
        data.document->durable = true;
    } else if (entity_count(data.model) != 0 || !data.model.sources.empty() ||
               !data.history.empty() || data.cursor != 0 || !data.operations.empty() ||
               data.save_intent || !data.initial_content_state.empty() || data.recoverable)
        throw state_codec::CodecError("Orphan model state");
    return data;
}
Data decode_project(std::string_view bytes, Limits limits) {
    Reader r(bytes);
    if (r.text() != "QCAE-PROJECT" || r.number() != 1)
        throw state_codec::CodecError("Unsupported project schema");
    Data data;
    data.limits = limits;
    data.application_nonce = nonce();
    DocumentInfo info;
    info.project_id = r.text();
    info.name = r.text();
    info.content_state = r.text();
    data.model = state_codec::read_model(r, limits.max_entities, limits.max_relations);
    r.finish();
    if (info.project_id.empty() || info.name.empty() || info.content_state.empty())
        throw state_codec::CodecError("Malformed project identity");
    if (auto diagnostic = validate_candidate(data.model, limits))
        throw state_codec::CodecError("Invalid project model: " + diagnostic->message);
    data.initial_content_state = info.content_state;
    data.document = std::move(info);
    return data;
}
} // namespace qcae::legacy_detail
