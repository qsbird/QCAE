#include "application_state.hpp"
#include "qcae/operation_ledger.hpp"
#include <algorithm>
#include <iomanip>
#include <random>
#include <sstream>
#include <set>

namespace qcae {
namespace {
void set_record_version(RecordStateImage& data) {
    const auto& document = *data.document;
    RecordVersion version{document.document, document.revision};
    ledger::add(ledger::Stage::records,
                ledger::Metric::metadata_copy_bytes,
                sizeof(RecordVersion) + version.document.id.value.size() +
                    version.document.epoch.value.size());
    data.records = data.records.with_version(std::move(version));
}
class Writer {
  public:
    explicit Writer(std::size_t capacity = 0) {
        bytes_.reserve(capacity);
    }
    void number(std::uint64_t value) {
        for (unsigned i = 0; i < 8; ++i) {
            const auto previous_size = bytes_.size();
            const auto previous_capacity = bytes_.capacity();
            bytes_.push_back(static_cast<char>((value >> (i * 8)) & 255));
            if (bytes_.capacity() != previous_capacity)
                copied_previous(previous_size);
        }
        ledger::add(ledger::Stage::application, ledger::Metric::metadata_copy_bytes, 8);
    }
    void boolean(bool value) {
        number(value ? 1 : 0);
    }
    void text(std::string_view value, bool model_payload = false) {
        number(value.size());
        const auto previous_size = bytes_.size();
        const auto previous_capacity = bytes_.capacity();
        bytes_.append(value);
        ledger::add(ledger::Stage::application,
                    model_payload ? ledger::Metric::model_copy_bytes
                                  : ledger::Metric::metadata_copy_bytes,
                    value.size());
        if (bytes_.capacity() != previous_capacity)
            copied_previous(previous_size);
        if (model_payload)
            model_bytes_ += value.size();
    }
    std::string take() {
        ledger::add(ledger::Stage::application, ledger::Metric::encoded_bytes, bytes_.size());
        return std::move(bytes_);
    }

  private:
    std::string bytes_;
    std::size_t model_bytes_{};
    void copied_previous(std::size_t previous_size) {
        ledger::add(ledger::Stage::application, ledger::Metric::model_copy_bytes, model_bytes_);
        ledger::add(ledger::Stage::application,
                    ledger::Metric::metadata_copy_bytes,
                    previous_size - model_bytes_);
    }
};
class EncodedSize {
  public:
    void number() {
        add(8);
    }
    void text(std::string_view value) {
        number();
        add(value.size());
    }
    std::size_t size() const noexcept {
        return size_;
    }

  private:
    std::size_t size_{};
    void add(std::size_t value) {
        if (value > std::string{}.max_size() - size_)
            throw std::length_error("Record application image is too large");
        size_ += value;
    }
};
void size_info(EncodedSize& size, const DocumentInfo& info) {
    for (const auto* text : {&info.document.id.value,
                             &info.document.epoch.value,
                             &info.content_state,
                             &info.name,
                             &info.project_id,
                             &info.saved_path,
                             &info.saved_content_state})
        size.text(*text);
    for (int index = 0; index < 4; ++index)
        size.number();
}
void size_receipt(EncodedSize& size, const ChangeReceipt& receipt) {
    size.text(receipt.transaction.value);
    size.number();
    size.number();
    size.text(receipt.current_content_state);
    size.text(receipt.primary_entity.value);
}
class Reader {
  public:
    explicit Reader(std::string_view bytes) : bytes_(bytes) {}
    std::uint64_t number() {
        require(8);
        std::uint64_t value{};
        for (unsigned i = 0; i < 8; ++i)
            value |= std::uint64_t(static_cast<unsigned char>(bytes_[offset_++])) << (i * 8);
        return value;
    }
    bool boolean() {
        auto value = number();
        if (value > 1)
            bad();
        return value != 0;
    }
    std::string text() {
        auto size = number();
        require(size);
        auto value = std::string(bytes_.substr(offset_, size));
        offset_ += size;
        return value;
    }
    std::size_t count() {
        auto value = number();
        if (value > 2000000 || value > (bytes_.size() - offset_) / 8)
            bad();
        return value;
    }
    void finish() const {
        if (offset_ != bytes_.size())
            bad();
    }

  private:
    static void bad() {
        throw RecordError(ErrorCode::schema_unsupported, "Malformed record application image");
    }
    void require(std::uint64_t size) const {
        if (size > bytes_.size() - offset_)
            bad();
    }
    std::string_view bytes_;
    std::size_t offset_{};
};
SharedStoreBytes bytes(std::string value) {
    return std::make_shared<const std::string>(std::move(value));
}
void write_info(Writer& w, const DocumentInfo& i) {
    w.text(i.document.id.value);
    w.text(i.document.epoch.value);
    w.number(i.revision);
    w.text(i.content_state);
    w.text(i.name);
    w.text(i.project_id);
    w.text(i.saved_path);
    w.text(i.saved_content_state);
    w.number(i.material_count);
    w.boolean(i.dirty);
    w.boolean(i.durable);
}
DocumentInfo read_info(Reader& r) {
    DocumentInfo i;
    i.document.id = DocumentId(r.text());
    i.document.epoch = DocumentEpoch(r.text());
    i.revision = r.number();
    i.content_state = r.text();
    i.name = r.text();
    i.project_id = r.text();
    i.saved_path = r.text();
    i.saved_content_state = r.text();
    i.material_count = r.number();
    i.dirty = r.boolean();
    i.durable = r.boolean();
    return i;
}
void write_receipt(Writer& w, const ChangeReceipt& v) {
    w.text(v.transaction.value);
    w.number(v.committed_revision);
    w.number(v.current_revision);
    w.text(v.current_content_state);
    w.text(v.primary_entity.value);
}
ChangeReceipt read_receipt(Reader& r, bool primary_entity) {
    ChangeReceipt v;
    v.transaction = TransactionId(r.text());
    v.committed_revision = r.number();
    v.current_revision = r.number();
    v.current_content_state = r.text();
    if (primary_entity)
        v.primary_entity = EntityId(r.text());
    return v;
}
StoreKey record_key(const RecordKey& key) {
    return {StoreSpace::document_record, std::to_string(key.type.value) + ":" + key.identity};
}
SharedStoreBytes record_bytes(const Record& record) {
    return SharedStoreBytes(record, &record->encoded());
}
SharedStoreBytes encode_history(const RecordHistoryImage& h, RecordStats* stats = nullptr) {
    const auto changes = encode_record_changes(h.changes, stats);
    EncodedSize size;
    size.text(h.transaction.value);
    size.text(h.label);
    size.text(h.content_state);
    size.text(changes);
    Writer w(size.size());
    w.text(h.transaction.value);
    w.text(h.label);
    w.text(h.content_state);
    w.text(changes, true);
    if (stats)
        for (const auto& change : h.changes.records)
            stats->model_bytes_copied += (change.before ? (*change.before)->encoded().size() : 0) +
                                         (change.after ? (*change.after)->encoded().size() : 0);
    return bytes(w.take());
}
SharedStoreBytes encode_operation(const RecordOperationImage& o) {
    EncodedSize size;
    size.text("QCAE-OPERATION-FACT");
    size.number();
    size.text(o.signature);
    size_receipt(size, o.receipt);
    Writer w(size.size());
    w.text("QCAE-OPERATION-FACT");
    w.number(1);
    w.text(o.signature);
    write_receipt(w, o.receipt);
    return bytes(w.take());
}
SharedStoreBytes encode_host(const RecordHostImage& h) {
    EncodedSize size;
    size.text(h.signature);
    size_info(size, h.result);
    Writer w(size.size());
    w.text(h.signature);
    write_info(w, h.result);
    return bytes(w.take());
}
const OwnedRowHandler& row_handler(std::span<const OwnedRowHandler> handlers,
                                   const OwnedRowImage& row) {
    if ((row.key.space != StoreSpace::task_record &&
         row.key.space != StoreSpace::artifact_record) ||
        row.key.identity.empty() || row.owner.empty() || !row.schema_version || !row.payload)
        throw RecordError(ErrorCode::schema_unsupported, "Malformed owned workspace row");
    const OwnedRowHandler* result = nullptr;
    for (const auto& handler : handlers)
        if (handler.space == row.key.space && handler.owner == row.owner) {
            if (result || !handler.validate)
                throw RecordError(ErrorCode::schema_unsupported,
                                  "Ambiguous owned-row registration");
            result = &handler;
        }
    if (!result)
        throw RecordError(
            ErrorCode::schema_unsupported, "Unregistered workspace row owner", row.owner);
    result->validate(row);
    return *result;
}
std::shared_ptr<const OwnedRowImage> prepare_owned(const OwnedRowImage& row,
                                                   std::span<const OwnedRowHandler> handlers) {
    row_handler(handlers, row);
    auto result = std::make_shared<OwnedRowImage>(row);
    Writer w;
    w.text("QCAE-OWNED-ROW");
    w.number(1);
    w.text(row.owner);
    w.number(row.schema_version);
    w.text(*row.payload, true);
    result->encoded = bytes(w.take());
    return result;
}
std::shared_ptr<const OwnedRowImage> decode_owned(const StoreKey& key,
                                                  SharedStoreBytes encoded,
                                                  std::span<const OwnedRowHandler> handlers) {
    if (!encoded || encoded->size() > 128 * 1024)
        throw RecordError(ErrorCode::resource_limit, "Owned-row encoding exceeds quota");
    Reader r(*encoded);
    if (r.text() != "QCAE-OWNED-ROW" || r.number() != 1)
        throw RecordError(ErrorCode::schema_unsupported, "Unsupported owned-row envelope");
    auto result = std::make_shared<OwnedRowImage>();
    result->key = key;
    result->owner = r.text();
    const auto version = r.number();
    if (version > UINT32_MAX)
        throw RecordError(ErrorCode::schema_unsupported, "Unsupported owned-row schema");
    result->schema_version = static_cast<std::uint32_t>(version);
    result->payload = bytes(r.text());
    r.finish();
    result->encoded = std::move(encoded);
    row_handler(handlers, *result);
    return result;
}
SharedStoreBytes encode_metadata(const RecordStateImage& d) {
    EncodedSize size;
    size.text("QCAE-RECORD-WORKSPACE");
    size.text(d.application_nonce);
    size.text(d.initial_content_state);
    for (int index = 0; index < 7; ++index)
        size.number();
    if (d.document)
        size_info(size, *d.document);
    for (const auto& history : d.history)
        size.text(history->transaction.value);
    if (d.save_intent) {
        const auto& intent = *d.save_intent;
        for (const auto* text : {&intent.host_key,
                                 &intent.signature,
                                 &intent.path,
                                 &intent.token,
                                 &intent.project_id,
                                 &intent.snapshot})
            size.text(*text);
        size.number();
    }
    Writer w(size.size());
    w.text("QCAE-RECORD-WORKSPACE");
    w.number(1);
    w.text(d.application_nonce);
    w.number(d.next_id);
    w.boolean(d.document.has_value());
    if (d.document)
        write_info(w, *d.document);
    w.text(d.initial_content_state);
    w.number(d.history.size());
    for (const auto& h : d.history)
        w.text(h->transaction.value);
    w.number(d.cursor);
    w.boolean(d.recoverable);
    w.boolean(bool(d.save_intent));
    if (d.save_intent) {
        const auto& i = *d.save_intent;
        w.text(i.host_key);
        w.text(i.signature);
        w.text(i.path);
        w.text(i.token);
        w.text(i.project_id);
        w.text(i.snapshot);
        w.boolean(i.save_as);
    }
    return bytes(w.take());
}
void verify_image(RecordStateImage& d, Limits limits) {
    if (d.application_nonce.empty() || d.next_id == 0 || d.cursor > d.history.size() ||
        d.history.size() > limits.max_history_entries ||
        d.operations.size() + d.host_operations.size() > limits.max_idempotency_records)
        throw RecordError(ErrorCode::schema_unsupported, "Invalid record workspace metadata");
    if (!d.document) {
        if (d.records.size() || !d.history.empty() || d.cursor || !d.operations.empty() ||
            d.save_intent || !d.initial_content_state.empty() || d.recoverable ||
            !d.owned_rows->empty())
            throw RecordError(ErrorCode::schema_unsupported, "Orphan record workspace");
        return;
    }
    if (d.document->document.id.value.empty() || d.document->document.epoch.value.empty() ||
        d.document->name.empty() || d.initial_content_state.empty() ||
        d.document->content_state.empty())
        throw RecordError(ErrorCode::schema_unsupported, "Invalid document identity");
    if (d.document->content_state !=
        (d.cursor ? d.history[d.cursor - 1]->content_state : d.initial_content_state))
        throw RecordError(ErrorCode::schema_unsupported, "History content state mismatch");
    set_record_version(d);
    d.records.validate();
    auto replay = d.records;
    for (std::size_t i = d.cursor; i > 0; --i)
        replay = apply_record_changes(replay, d.history[i - 1]->changes, RecordDirection::reverse);
    replay.validate();
    for (std::size_t i = 0; i < d.history.size(); ++i) {
        if (d.history[i]->transaction.value.empty() || d.history[i]->content_state.empty())
            throw RecordError(ErrorCode::schema_unsupported, "Invalid history identity");
        replay = apply_record_changes(replay, d.history[i]->changes);
        replay.validate();
        if (i + 1 == d.cursor && !diff_record_views(replay, d.records).empty())
            throw RecordError(ErrorCode::schema_unsupported,
                              "History does not match current records");
    }
}
} // namespace

std::vector<StoredRow> encode_record_state_image(const RecordStateImage& d) {
    std::vector<StoredRow> rows;
    rows.push_back({{StoreSpace::document_metadata, "state"}, encode_metadata(d)});
    d.records.visit([&](const Record& record) {
        rows.push_back({record_key(record->key()), record_bytes(record)});
    });
    for (const auto& h : d.history)
        rows.push_back({{StoreSpace::history_entry, h->transaction.value},
                        h->encoded ? h->encoded : encode_history(*h)});
    for (const auto& [key, o] : d.operations)
        rows.push_back(
            {{StoreSpace::operation_fact, key}, o.encoded ? o.encoded : encode_operation(o)});
    for (const auto& [key, h] : d.host_operations)
        rows.push_back(
            {{StoreSpace::host_operation_fact, key}, h.encoded ? h.encoded : encode_host(h)});
    for (const auto& [key, row] : *d.owned_rows) {
        if (!row || !row->encoded)
            throw RecordError(ErrorCode::schema_unsupported, "Unencoded owned workspace row");
        rows.push_back({key, row->encoded});
    }
    return rows;
}
RecordStateImage decode_record_state_image(std::span<const StoredRow> rows,
                                           std::shared_ptr<const RecordRegistry> registry,
                                           Limits limits,
                                           std::span<const OwnedRowHandler> handlers) {
    RecordStateImage d(registry);
    std::map<StoreKey, SharedStoreBytes> table;
    for (const auto& row : rows)
        if (!row.value || !table.emplace(row.key, row.value).second)
            throw RecordError(ErrorCode::schema_unsupported, "Duplicate or empty store row");
    auto meta = table.find({StoreSpace::document_metadata, "state"});
    if (meta == table.end())
        throw RecordError(ErrorCode::schema_unsupported, "Workspace metadata missing");
    Reader r(*meta->second);
    if (r.text() != "QCAE-RECORD-WORKSPACE" || r.number() != 1)
        throw RecordError(ErrorCode::schema_unsupported, "Unsupported record workspace schema");
    d.application_nonce = r.text();
    d.next_id = r.number();
    if (r.boolean())
        d.document = read_info(r);
    d.initial_content_state = r.text();
    const auto count = r.count();
    if (count > limits.max_history_entries)
        throw RecordError(ErrorCode::resource_limit, "History limit");
    std::set<std::string> history_ids;
    for (std::size_t i = 0; i < count; ++i) {
        auto id = r.text();
        if (!history_ids.insert(id).second)
            throw RecordError(ErrorCode::schema_unsupported,
                              "Duplicate history transaction identity");
        auto h = table.find({StoreSpace::history_entry, id});
        if (h == table.end())
            throw RecordError(ErrorCode::schema_unsupported, "Missing history row");
        Reader hr(*h->second);
        auto entry = std::make_shared<RecordHistoryImage>();
        entry->transaction = TransactionId(hr.text());
        entry->label = hr.text();
        entry->content_state = hr.text();
        entry->changes = decode_record_changes(*registry, hr.text());
        hr.finish();
        entry->encoded = h->second;
        if (entry->transaction.value != id)
            throw RecordError(ErrorCode::schema_unsupported, "History row identity mismatch");
        d.history.push_back(std::move(entry));
    }
    d.cursor = r.number();
    d.recoverable = r.boolean();
    if (r.boolean()) {
        auto i = std::make_shared<RecordSaveImage>();
        i->host_key = r.text();
        i->signature = r.text();
        i->path = r.text();
        i->token = r.text();
        i->project_id = r.text();
        i->snapshot = r.text();
        i->save_as = r.boolean();
        d.save_intent = std::move(i);
    }
    r.finish();
    RecordChangeSet imported;
    auto owned = std::make_shared<OwnedRowTable>();
    for (const auto& [key, value] : table) {
        switch (key.space) {
        case StoreSpace::document_record: {
            auto record = registry->decode(*value);
            if (record_key(record->key()) != key)
                throw RecordError(ErrorCode::schema_unsupported, "Record key mismatch");
            imported.records.push_back({record->key(), {}, record, {}});
            break;
        }
        case StoreSpace::operation_fact: {
            Reader rr(*value);
            RecordOperationImage o;
            o.signature = rr.text();
            const bool versioned = o.signature == "QCAE-OPERATION-FACT";
            if (versioned) {
                if (rr.number() != 1)
                    throw RecordError(ErrorCode::schema_unsupported,
                                      "Unsupported operation fact schema");
                o.signature = rr.text();
            }
            o.receipt = read_receipt(rr, versioned);
            rr.finish();
            o.encoded = value;
            d.operations.emplace(key.identity, std::move(o));
            break;
        }
        case StoreSpace::host_operation_fact: {
            Reader rr(*value);
            RecordHostImage o;
            o.signature = rr.text();
            o.result = read_info(rr);
            rr.finish();
            o.encoded = value;
            d.host_operations.emplace(key.identity, std::move(o));
            break;
        }
        case StoreSpace::history_entry:
            if (std::none_of(d.history.begin(), d.history.end(), [&](const auto& h) {
                    return h->transaction.value == key.identity;
                }))
                throw RecordError(ErrorCode::schema_unsupported, "Orphan history row");
            break;
        case StoreSpace::document_metadata:
            if (key.identity != "state")
                throw RecordError(ErrorCode::schema_unsupported, "Unknown metadata row");
            break;
        case StoreSpace::task_record:
        case StoreSpace::artifact_record:
            owned->emplace(key, decode_owned(key, value, handlers));
            break;
        default:
            throw RecordError(ErrorCode::schema_unsupported, "Unsupported workspace row space");
        }
    }
    d.owned_rows = std::move(owned);
    d.records = apply_record_changes(d.records, imported);
    verify_image(d, limits);
    return d;
}
std::string encode_record_rows(std::span<const StoredRow> rows) {
    Writer w;
    w.text("QCAE-RECORD-ROWS");
    w.number(1);
    w.number(rows.size());
    for (const auto& row : rows) {
        w.number(static_cast<unsigned>(row.key.space));
        w.text(row.key.identity);
        w.text(*row.value, true);
    }
    return w.take();
}
std::vector<StoredRow> decode_record_rows(std::string_view payload) {
    Reader r(payload);
    if (r.text() != "QCAE-RECORD-ROWS" || r.number() != 1)
        throw RecordError(ErrorCode::schema_unsupported, "Unsupported row image");
    auto count = r.count();
    std::vector<StoredRow> rows;
    for (std::size_t i = 0; i < count; ++i) {
        auto space = r.number();
        if (space < 1 || space > 7)
            throw RecordError(ErrorCode::schema_unsupported, "Unknown row space");
        StoreKey key{static_cast<StoreSpace>(space), r.text()};
        rows.push_back({std::move(key), bytes(r.text())});
    }
    r.finish();
    return rows;
}

namespace record_detail {
SharedStoreBytes encode_operation_record(const RecordedOperation& operation) {
    return encode_operation(operation);
}
Data copy_data(const Data& source) {
    // DocumentView's central copy hook counts this same inline RecordVersion.
    std::uint64_t bytes = sizeof(Data) - sizeof(RecordVersion) + source.application_nonce.size() +
                          source.initial_content_state.size() +
                          source.history.size() * sizeof(decltype(source.history)::value_type);
    const auto info_bytes = [](const DocumentInfo& info) {
        return info.document.id.value.size() + info.document.epoch.value.size() +
               info.content_state.size() + info.name.size() + info.project_id.size() +
               info.saved_path.size() + info.saved_content_state.size();
    };
    if (source.document)
        bytes += info_bytes(*source.document);
    for (const auto& [key, preview] : source.previews) {
        (void)preview;
        bytes += sizeof(decltype(source.previews)::value_type) + key.size();
    }
    for (const auto& mutation : source.pending)
        bytes += sizeof(RowMutation) + mutation.key.identity.size();
    ledger::add(ledger::Stage::application, ledger::Metric::metadata_copy_bytes, bytes);
    return source;
}
std::string nonce() {
    std::random_device source;
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (int i = 0; i < 4; ++i)
        out << std::setw(8) << source();
    return out.str();
}
void update_document(Data& d) {
    if (!d.document)
        return;
    d.document->material_count =
        d.options->material_count ? d.options->material_count(d.records) : 0;
    const auto& clean = d.document->saved_content_state.empty() ? d.initial_content_state
                                                                : d.document->saved_content_state;
    d.document->dirty = d.document->content_state != clean;
    set_record_version(d);
}
std::optional<Diagnostic> validate_candidate(const DocumentView& view, const Data& d) {
    try {
        view.validate(
            {d.limits.max_entities, d.limits.max_relations, record_wire::maximum_record_bytes});
        if (d.options->validate)
            return d.options->validate(view, d.limits);
    } catch (const RecordError& e) {
        return Diagnostic{e.code(), e.what(), e.field()};
    }
    return {};
}
std::string encode_project(Data& d, const std::string& project_id) {
    Writer w;
    w.text("QCAE-RECORD-PROJECT");
    w.number(2);
    w.text(project_id);
    w.text(d.document->name);
    w.text(d.document->content_state);
    w.number(d.records.size());
    d.records.visit([&](const Record& record) {
        w.text(record->encoded(), true);
        d.stats.model_bytes_copied += record->encoded().size();
    });
    w.number(d.owned_rows->size());
    for (const auto& [key, row] : *d.owned_rows) {
        w.number(static_cast<unsigned>(key.space));
        w.text(key.identity);
        w.text(*row->encoded, true);
    }
    note_whole_model_serialization(&d.stats);
    return w.take();
}
Data decode_project(std::string_view payload,
                    std::shared_ptr<const RecordApplicationOptions> options) {
    Data d(options);
    Reader r(payload);
    auto magic = r.text();
    RecordProjectImage image{DocumentView(options->registry), {}, {}, {}};
    if (magic != "QCAE-RECORD-PROJECT") {
        if (!options->decode_legacy_project)
            throw RecordError(ErrorCode::schema_unsupported, "Unsupported project schema");
        image = options->decode_legacy_project(payload);
    } else {
        const auto project_version = r.number();
        if (project_version != 1 && project_version != 2)
            throw RecordError(ErrorCode::schema_unsupported, "Unsupported record project schema");
        image.project_id = r.text();
        image.name = r.text();
        image.content_state = r.text();
        auto count = r.count();
        RecordChangeSet changes;
        for (std::size_t i = 0; i < count; ++i) {
            auto record = options->registry->decode(r.text());
            changes.records.push_back({record->key(), {}, record, {}});
        }
        if (project_version == 2) {
            auto owned = std::make_shared<OwnedRowTable>();
            const auto owned_count = r.count();
            if (owned_count > options->max_owned_rows)
                throw RecordError(ErrorCode::resource_limit, "Owned-row count exceeds quota");
            for (std::size_t index = 0; index < owned_count; ++index) {
                const auto space = r.number();
                if (space != static_cast<unsigned>(StoreSpace::task_record) &&
                    space != static_cast<unsigned>(StoreSpace::artifact_record))
                    throw RecordError(ErrorCode::schema_unsupported,
                                      "Invalid project owned-row space");
                StoreKey key{static_cast<StoreSpace>(space), r.text()};
                auto row = decode_owned(key, bytes(r.text()), options->owned_row_handlers);
                if (!owned->emplace(key, std::move(row)).second)
                    throw RecordError(ErrorCode::schema_unsupported, "Duplicate project owned row");
            }
            image.owned_rows = std::move(owned);
        }
        r.finish();
        image.records = apply_record_changes(image.records, changes);
    }
    if (image.project_id.empty() || image.name.empty() || image.content_state.empty())
        throw RecordError(ErrorCode::schema_unsupported, "Malformed project identity");
    d.records = std::move(image.records);
    d.owned_rows = std::move(image.owned_rows);
    DocumentInfo info;
    d.document = std::move(info);
    d.document->project_id = image.project_id;
    d.document->name = image.name;
    d.document->content_state = image.content_state;
    d.initial_content_state = image.content_state;
    d.application_nonce = nonce();
    if (auto error = validate_candidate(d.records, d))
        throw RecordError(error->code, error->message, error->field);
    return d;
}
Data decode_data(const LoadedRows& loaded,
                 std::shared_ptr<const RecordApplicationOptions> options) {
    if (loaded.legacy)
        throw RecordError(ErrorCode::schema_unsupported,
                          "Legacy workspace requires migration to a separate destination");
    Data d(options);
    if (loaded.rows.empty()) {
        d.application_nonce = nonce();
        return d;
    }
    static_cast<RecordStateImage&>(d) = decode_record_state_image(
        loaded.rows, options->registry, options->limits, options->owned_row_handlers);
    if (d.owned_rows->size() > options->max_owned_rows)
        throw RecordError(ErrorCode::resource_limit, "Owned-row count exceeds quota");
    for (const auto& [unused, row] : *d.owned_rows) {
        (void)unused;
        if (row->payload->size() > options->max_owned_row_bytes)
            throw RecordError(ErrorCode::resource_limit, "Owned-row payload exceeds quota");
    }
    if (d.save_intent) {
        const auto& intent = *d.save_intent;
        if (intent.host_key.empty() || intent.signature.empty() || intent.path.empty() ||
            intent.token.empty() || intent.project_id.empty())
            throw RecordError(ErrorCode::schema_unsupported, "Malformed save intent");
        const auto snapshot = decode_project(intent.snapshot, options);
        if (!d.document || snapshot.document->project_id != intent.project_id ||
            snapshot.document->name != d.document->name ||
            snapshot.document->content_state != d.document->content_state ||
            !diff_record_views(snapshot.records, d.records).empty())
            throw RecordError(ErrorCode::schema_unsupported, "Save intent snapshot mismatch");
    }
    if (auto error = validate_candidate(d.records, d))
        throw RecordError(error->code, error->message, error->field);
    return d;
}
void queue_changes(Data& d, const RecordChangeSet& changes, RecordDirection direction) {
    for (const auto& change : changes.records) {
        const auto& record = direction == RecordDirection::forward ? change.after : change.before;
        d.pending.push_back({record_key(change.key), record ? record_bytes(*record) : nullptr});
    }
}
void queue_owned_rows(Data& d, std::span<const OwnedRowUpdate> updates) {
    if (updates.empty())
        return;
    auto rows = std::make_shared<OwnedRowTable>(*d.owned_rows);
    std::set<StoreKey> seen;
    std::vector<RowMutation> pending;
    for (const auto& update : updates) {
        if (!seen.insert(update.key).second)
            throw RecordError(ErrorCode::invalid_input, "Duplicate owned-row update");
        const auto found = rows->find(update.key);
        const auto previous = found == rows->end() ? nullptr : found->second;
        if (bool(previous) != bool(update.expected) ||
            (previous && *previous->payload != *update.expected))
            throw RecordError(ErrorCode::revision_conflict, "Owned-row compare-and-swap failed");
        if (update.after) {
            if (update.after->key != update.key ||
                (previous && previous->owner != update.after->owner) || !update.after->payload)
                throw RecordError(ErrorCode::invalid_input, "Owned-row identity or owner mismatch");
            if (update.after->payload->size() > d.options->max_owned_row_bytes)
                throw RecordError(ErrorCode::resource_limit, "Owned-row payload exceeds quota");
            auto row = prepare_owned(*update.after, d.options->owned_row_handlers);
            (*rows)[update.key] = row;
            pending.push_back({update.key, row->encoded});
        } else {
            if (!previous)
                throw RecordError(ErrorCode::entity_not_found, "Owned row does not exist");
            row_handler(d.options->owned_row_handlers, *previous);
            rows->erase(update.key);
            pending.push_back({update.key, {}});
        }
    }
    if (rows->size() > d.options->max_owned_rows)
        throw RecordError(ErrorCode::resource_limit, "Owned-row count exceeds quota");
    d.pending.insert(d.pending.end(), pending.begin(), pending.end());
    d.owned_rows = std::move(rows);
}
void recover_owned_rows(Data& d) {
    std::vector<OwnedRowUpdate> updates;
    for (const auto& [key, row] : *d.owned_rows) {
        const auto& handler = row_handler(d.options->owned_row_handlers, *row);
        if (handler.recover)
            if (auto recovered = handler.recover(*row))
                updates.push_back({key, row->payload, std::move(recovered)});
    }
    queue_owned_rows(d, updates);
}
bool owned_rows_block_close(const Data& d) {
    for (const auto& [unused, row] : *d.owned_rows) {
        (void)unused;
        const auto& handler = row_handler(d.options->owned_row_handlers, *row);
        if (handler.blocks_close && handler.blocks_close(*row))
            return true;
    }
    return false;
}
void replace_records(Data& d, DocumentView view) {
    d.records.visit(
        [&](const Record& record) { d.pending.push_back({record_key(record->key()), {}}); });
    view.visit([&](const Record& record) {
        d.pending.push_back({record_key(record->key()), record_bytes(record)});
    });
    d.records = std::move(view);
}
void clear_document_rows(Data& d) {
    replace_records(d, DocumentView(d.records.registry()));
    for (const auto& h : d.history)
        d.pending.push_back({{StoreSpace::history_entry, h->transaction.value}, {}});
    for (const auto& [key, o] : d.operations) {
        (void)o;
        d.pending.push_back({{StoreSpace::operation_fact, key}, {}});
    }
    for (const auto& [key, unused] : *d.owned_rows) {
        (void)unused;
        d.pending.push_back({key, {}});
    }
    d.owned_rows = std::make_shared<const OwnedRowTable>();
}
std::shared_ptr<const HistoryEntry> make_history(TransactionId transaction,
                                                 std::string label,
                                                 RecordChangeSet changes,
                                                 std::string content,
                                                 RecordStats* stats) {
    std::uint64_t metadata = sizeof(HistoryEntry) + sizeof(RecordChangeSet) +
                             transaction.value.size() + label.size() + content.size();
    for (const auto& change : changes.records)
        metadata += sizeof(RecordChange) + change.key.identity.size() +
                    change.fields.size() * sizeof(decltype(change.fields)::value_type);
    // lvalue arguments own one copied string/vector payload at this boundary;
    // SSO moves can additionally copy the short in-object characters.
    for (const auto* value : {&transaction.value, &label, &content})
        if (value->size() <= std::string{}.capacity())
            metadata += value->size();
    ledger::add(ledger::Stage::application, ledger::Metric::metadata_copy_bytes, metadata);
    auto h = std::make_shared<HistoryEntry>();
    h->transaction = std::move(transaction);
    h->label = std::move(label);
    h->changes = std::move(changes);
    h->content_state = std::move(content);
    h->encoded = encode_history(*h, stats);
    return h;
}
std::optional<Diagnostic>
persist(Data& candidate, IRecordStore* store, std::uint64_t& generation, bool& poisoned) {
    if (poisoned)
        return Diagnostic{
            ErrorCode::storage_uncertain, "Storage outcome needs recovery", "storage"};
    try {
        candidate.pending.push_back(
            {{StoreSpace::document_metadata, "state"}, encode_metadata(candidate)});
        for (const auto& h : candidate.history) {
            if (!h->encoded)
                throw RecordError(ErrorCode::schema_unsupported, "Unencoded history row");
        }
        for (const auto& [key, o] : candidate.operations)
            if (!o.encoded) {
                auto encoded = o;
                encoded.encoded = encode_operation(o);
                candidate.pending.push_back({{StoreSpace::operation_fact, key}, encoded.encoded});
                ledger::add(ledger::Stage::application,
                            ledger::Metric::metadata_copy_bytes,
                            o.signature.size() + o.receipt.transaction.value.size() +
                                o.receipt.current_content_state.size() +
                                o.receipt.primary_entity.value.size());
                candidate.operations.replace(key, std::move(encoded));
            }
        for (const auto& [key, h] : candidate.host_operations)
            if (!h.encoded) {
                auto encoded = h;
                encoded.encoded = encode_host(h);
                candidate.pending.push_back(
                    {{StoreSpace::host_operation_fact, key}, encoded.encoded});
                ledger::add(ledger::Stage::application,
                            ledger::Metric::metadata_copy_bytes,
                            h.signature.size() + h.result.document.id.value.size() +
                                h.result.document.epoch.value.size() +
                                h.result.content_state.size() + h.result.name.size() +
                                h.result.project_id.size() + h.result.saved_path.size() +
                                h.result.saved_content_state.size());
                candidate.host_operations.replace(key, std::move(encoded));
            }
        // Last mutation wins when replacing a record set during explicit open.
        std::map<StoreKey, SharedStoreBytes> unique;
        for (const auto& row : candidate.pending)
            unique[row.key] = row.after;
        StoreBatch batch{generation, "application-" + std::to_string(generation + 1), {}};
        for (auto& [key, value] : unique)
            batch.mutations.push_back({key, std::move(value)});
        candidate.pending.clear();
        if (store)
            generation = store->commit_rows(batch).generation;
        return {};
    } catch (const RecordError& error) {
        return Diagnostic{error.code(), error.what(), error.field()};
    } catch (const StorageError& error) {
        if (error.uncertain())
            poisoned = true;
        return Diagnostic{error.uncertain() ? ErrorCode::storage_uncertain
                                            : ErrorCode::storage_failure,
                          error.what(),
                          "storage"};
    }
}
} // namespace record_detail
} // namespace qcae
