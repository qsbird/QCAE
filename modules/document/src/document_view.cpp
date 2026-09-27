#include "qcae/document_view.hpp"

#include <algorithm>
#include <set>

namespace qcae {
namespace {
struct RecordPage {
    std::array<Record, record_page_capacity> entries;
};
using RecordLocator = std::map<std::string, std::size_t, std::less<>>;
using IdentityLocator = std::map<std::string, RecordTypeId, std::less<>>;
struct RecordTable {
    std::shared_ptr<const RecordLocator> positions = std::make_shared<const RecordLocator>();
    std::vector<std::shared_ptr<const RecordPage>> pages;
    std::size_t slots{};
    std::size_t count{};
};
bool images_equal(const Record& left, const Record& right) {
    return left == right || (left && right && left->encoded() == right->encoded());
}
Record optional_image(const std::optional<Record>& value) {
    if (value && !*value)
        throw RecordError(ErrorCode::invalid_input, "Null image in a present record change");
    return value ? *value : Record{};
}
void check_change_shape(const RecordChange& change) {
    const auto before = optional_image(change.before);
    const auto after = optional_image(change.after);
    if ((!before && !after) || !change.key.type.value || change.key.identity.empty() ||
        (before && before->key() != change.key) || (after && after->key() != change.key))
        throw RecordError(ErrorCode::invalid_input, "Invalid record change image or identity");
    const auto& descriptor = after ? after->descriptor() : before->descriptor();
    std::set<RecordFieldId> fields;
    for (const auto field : change.fields)
        if (!fields.insert(field).second ||
            std::none_of(descriptor.fields.begin(),
                         descriptor.fields.end(),
                         [&](const auto& known) { return known.id == field; }))
            throw RecordError(ErrorCode::invalid_input, "Unknown or duplicate change field ID");
}
void check_image(const DocumentView& view, const RecordKey& key, const Record& image) {
    if (image && (image->key() != key || view.registry()->find(key.type) != &image->descriptor()))
        throw RecordError(ErrorCode::invalid_input, "Change image has the wrong key or registry");
}
} // namespace

struct RecordDocumentState {
    std::shared_ptr<const RecordRegistry> registry;
    std::map<RecordTypeId, std::shared_ptr<const RecordTable>> tables;
    std::shared_ptr<const IdentityLocator> identities = std::make_shared<const IdentityLocator>();
    std::size_t count{};
};

bool RecordChangeSet::empty() const noexcept {
    return records.empty();
}
DocumentView::DocumentView(std::shared_ptr<const RecordRegistry> registry, RecordVersion version)
    : version_(std::move(version)) {
    if (!registry || !registry->frozen())
        throw RecordError(ErrorCode::invalid_input, "A frozen record registry is required");
    auto state = std::make_shared<RecordDocumentState>();
    state->registry = std::move(registry);
    state_ = std::move(state);
}
RecordVersion DocumentView::version() const {
    return version_;
}
std::shared_ptr<const RecordRegistry> DocumentView::registry() const noexcept {
    return state_->registry;
}
std::size_t DocumentView::size() const noexcept {
    return state_->count;
}
std::size_t DocumentView::count(RecordTypeId type) const noexcept {
    const auto found = state_->tables.find(type);
    return found == state_->tables.end() ? 0 : found->second->count;
}
Record DocumentView::find(RecordTypeId type, std::string_view identity) const noexcept {
    const auto table = state_->tables.find(type);
    if (table == state_->tables.end())
        return {};
    const auto found = table->second->positions->find(identity);
    if (found == table->second->positions->end())
        return {};
    const auto slot = found->second;
    return table->second->pages[slot / record_page_capacity]->entries[slot % record_page_capacity];
}
Record DocumentView::find(const RecordKey& key) const noexcept {
    return find(key.type, key.identity);
}
Record DocumentView::find_identity(std::string_view identity) const noexcept {
    const auto found = state_->identities->find(identity);
    return found == state_->identities->end() ? Record{} : find(found->second, identity);
}
void DocumentView::visit(RecordTypeId type,
                         const std::function<void(const Record&)>& visitor) const {
    const auto found = state_->tables.find(type);
    if (found == state_->tables.end())
        return;
    for (const auto& page : found->second->pages)
        for (const auto& record : page->entries)
            if (record)
                visitor(record);
}
void DocumentView::visit(const std::function<void(const Record&)>& visitor) const {
    for (const auto& [type, unused] : state_->tables) {
        (void)unused;
        visit(type, visitor);
    }
}
void DocumentView::validate(RecordLimits limits) const {
    if (size() > limits.max_records)
        throw RecordError(ErrorCode::resource_limit, "Document record quota exceeded");
    std::size_t allocated_slots = 0;
    for (const auto& [unused, table] : state_->tables) {
        (void)unused;
        if (table->slots > limits.max_records - allocated_slots)
            throw RecordError(ErrorCode::resource_limit, "Document slot quota exceeded");
        allocated_slots += table->slots;
    }
    std::size_t references = 0;
    visit([&](const Record& record) {
        if (record->encoded().size() > limits.max_record_bytes)
            throw RecordError(ErrorCode::resource_limit, "Record byte quota exceeded");
        record->descriptor().references(
            record->object(),
            [&](RecordFieldId field,
                std::string_view identity,
                std::span<const RecordTypeId> allowed) {
                if (++references > limits.max_references)
                    throw RecordError(ErrorCode::resource_limit,
                                      "Document reference quota exceeded");
                const auto target = find_identity(identity);
                if (!target ||
                    std::find(allowed.begin(), allowed.end(), target->key().type) == allowed.end())
                    throw RecordError(ErrorCode::invalid_input,
                                      "Reference is missing or has the wrong record type",
                                      record->key().identity + "." + std::to_string(field.value));
            });
        record->descriptor().validate(record->object(), *this);
    });
    for (const auto& rule : state_->registry->rules())
        rule(*this);
}
DocumentView DocumentView::with_version(RecordVersion version) const {
    auto result = *this;
    result.version_ = std::move(version);
    return result;
}

DocumentView apply_record_changes(const DocumentView& base,
                                  const RecordChangeSet& changes,
                                  RecordDirection direction,
                                  RecordStats* stats) {
    if (changes.empty())
        return base;
    std::set<RecordKey> seen;
    for (const auto& change : changes.records) {
        check_change_shape(change);
        if (!seen.insert(change.key).second || (!change.before && !change.after))
            throw RecordError(ErrorCode::invalid_input, "Duplicate or empty change record");
        const auto before = optional_image(change.before);
        const auto after = optional_image(change.after);
        check_image(base, change.key, before);
        check_image(base, change.key, after);
        const auto& expected = direction == RecordDirection::forward ? before : after;
        if (!images_equal(base.find(change.key), expected))
            throw RecordError(ErrorCode::revision_conflict,
                              "Record change base does not match",
                              change.key.identity);
    }
    auto state = std::make_shared<RecordDocumentState>(*base.state_);
    if (stats)
        stats->metadata_bytes_copied +=
            sizeof(RecordDocumentState) +
            state->tables.size() * sizeof(decltype(state->tables)::value_type);
    std::map<RecordTypeId, std::shared_ptr<RecordTable>> writable_tables;
    std::map<std::pair<RecordTypeId, std::size_t>, std::shared_ptr<RecordPage>> writable_pages;
    std::map<RecordTypeId, std::shared_ptr<RecordLocator>> writable_positions;
    std::shared_ptr<IdentityLocator> writable_identities;
    auto identities = [&]() -> IdentityLocator& {
        if (!writable_identities) {
            writable_identities = std::make_shared<IdentityLocator>(*state->identities);
            state->identities = writable_identities;
            if (stats) {
                stats->metadata_bytes_copied +=
                    writable_identities->size() * sizeof(IdentityLocator::value_type);
                for (const auto& [identity, unused] : *writable_identities) {
                    (void)unused;
                    stats->metadata_bytes_copied += identity.size();
                }
            }
        }
        return *writable_identities;
    };
    // Release removed identities first: a composite replacement may reuse an ID with a new type.
    for (const auto& change : changes.records) {
        const auto& after = direction == RecordDirection::forward ? change.after : change.before;
        if (!after)
            identities().erase(change.key.identity);
    }
    for (const auto& change : changes.records) {
        const auto from =
            optional_image(direction == RecordDirection::forward ? change.before : change.after);
        const auto to =
            optional_image(direction == RecordDirection::forward ? change.after : change.before);
        if (images_equal(from, to))
            continue;
        auto [table_it, fresh_table] = writable_tables.try_emplace(change.key.type);
        if (fresh_table) {
            const auto existing = state->tables.find(change.key.type);
            table_it->second = existing == state->tables.end()
                                   ? std::make_shared<RecordTable>()
                                   : std::make_shared<RecordTable>(*existing->second);
            state->tables[change.key.type] = table_it->second;
            if (stats)
                stats->metadata_bytes_copied +=
                    sizeof(RecordTable) +
                    table_it->second->pages.size() * sizeof(std::shared_ptr<const RecordPage>);
        }
        auto& table = *table_it->second;
        const auto position = table.positions->find(change.key.identity);
        std::size_t slot{};
        if (position == table.positions->end()) {
            auto [index_it, fresh_index] = writable_positions.try_emplace(change.key.type);
            if (fresh_index) {
                index_it->second = std::make_shared<RecordLocator>(*table.positions);
                if (stats) {
                    stats->metadata_bytes_copied +=
                        index_it->second->size() * sizeof(RecordLocator::value_type);
                    for (const auto& [identity, unused] : *index_it->second) {
                        (void)unused;
                        stats->metadata_bytes_copied += identity.size();
                    }
                }
                table.positions = index_it->second;
            }
            slot = table.slots++;
            index_it->second->emplace(change.key.identity, slot);
        } else {
            slot = position->second;
        }
        const auto page_index = slot / record_page_capacity;
        auto [page_it, fresh_page] = writable_pages.try_emplace({change.key.type, page_index});
        if (fresh_page) {
            page_it->second = page_index < table.pages.size()
                                  ? std::make_shared<RecordPage>(*table.pages[page_index])
                                  : std::make_shared<RecordPage>();
            if (page_index == table.pages.size())
                table.pages.push_back(page_it->second);
            else
                table.pages[page_index] = page_it->second;
            if (stats) {
                ++stats->dirty_pages;
                stats->metadata_bytes_copied += sizeof(RecordPage);
            }
        }
        if (!from && to) {
            const auto found = state->identities->find(change.key.identity);
            if (found != state->identities->end() && found->second != change.key.type)
                throw RecordError(ErrorCode::invalid_input,
                                  "Record identity is duplicated across types");
            identities()[change.key.identity] = change.key.type;
            ++table.count;
            ++state->count;
        } else if (from && !to) {
            --table.count;
            --state->count;
        }
        page_it->second->entries[slot % record_page_capacity] = to;
        if (stats)
            ++stats->changed_records;
    }
    auto result = base;
    result.state_ = std::move(state);
    return result;
}

RecordChangeSet diff_record_views(const DocumentView& before, const DocumentView& after) {
    if (before.registry() != after.registry())
        throw RecordError(ErrorCode::invalid_input, "Cannot diff different record registries");
    RecordChangeSet result;
    before.visit([&](const Record& old) {
        const auto current = after.find(old->key());
        if (!images_equal(old, current))
            result.records.push_back(
                {old->key(), old, current ? std::optional<Record>(current) : std::nullopt, {}});
    });
    after.visit([&](const Record& current) {
        if (!before.find(current->key()))
            result.records.push_back({current->key(), std::nullopt, current, {}});
    });
    return result;
}
std::string encode_record_changes(const RecordChangeSet& changes, RecordStats* stats) {
    std::vector<std::string> values;
    std::set<RecordKey> seen;
    values.reserve(1 + changes.records.size() * 5);
    values.emplace_back("QCAE-RECORD-CHANGES-1");
    for (const auto& change : changes.records) {
        check_change_shape(change);
        if (!seen.insert(change.key).second)
            throw RecordError(ErrorCode::invalid_input, "Duplicate history record key");
        values.push_back(record_wire::number(change.key.type.value));
        values.push_back(change.key.identity);
        values.push_back(change.before ? (*change.before)->encoded() : std::string());
        values.push_back(change.after ? (*change.after)->encoded() : std::string());
        std::string fields;
        for (const auto field : change.fields)
            fields += record_wire::number(field.value);
        values.push_back(std::move(fields));
        if (stats) {
            const auto bytes = (change.before ? (*change.before)->encoded().size() : 0) +
                               (change.after ? (*change.after)->encoded().size() : 0);
            stats->model_bytes_copied += bytes * 2;
        }
    }
    auto result = record_wire::strings(values);
    if (stats)
        stats->model_bytes_encoded += result.size();
    return result;
}
RecordChangeSet decode_record_changes(const RecordRegistry& registry, std::string_view bytes) {
    const auto values = record_wire::read_strings(bytes);
    if (values.empty() || values.front() != "QCAE-RECORD-CHANGES-1" || (values.size() - 1) % 5)
        throw RecordError(ErrorCode::schema_unsupported, "Invalid record history encoding");
    RecordChangeSet result;
    std::set<RecordKey> seen;
    for (std::size_t index = 1; index < values.size(); index += 5) {
        const auto type = record_wire::read_number(values[index]);
        if (type > UINT32_MAX)
            throw RecordError(ErrorCode::invalid_input, "Invalid history record type");
        RecordChange change;
        change.key = {RecordTypeId{static_cast<std::uint32_t>(type)}, values[index + 1]};
        if (!values[index + 2].empty())
            change.before = registry.decode(values[index + 2]);
        if (!values[index + 3].empty())
            change.after = registry.decode(values[index + 3]);
        if ((!change.before && !change.after) || !seen.insert(change.key).second ||
            (change.before && (*change.before)->key() != change.key) ||
            (change.after && (*change.after)->key() != change.key) || values[index + 4].size() % 8)
            throw RecordError(ErrorCode::invalid_input, "Invalid history record image");
        for (std::size_t offset = 0; offset < values[index + 4].size(); offset += 8) {
            const auto field =
                record_wire::read_number(std::string_view(values[index + 4]).substr(offset, 8));
            if (!field || field > UINT32_MAX)
                throw RecordError(ErrorCode::invalid_input, "Invalid history field ID");
            change.fields.push_back(RecordFieldId{static_cast<std::uint32_t>(field)});
        }
        check_change_shape(change);
        result.records.push_back(std::move(change));
    }
    return result;
}
} // namespace qcae
