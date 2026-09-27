#include "qcae/query.hpp"
#include "qcae/records.hpp"
#include <algorithm>
#include <set>

namespace qcae {
namespace {
template <class T> Result<T> fail(ErrorCode code, const char* message) {
    return {Status::failed, {}, Diagnostic{code, message, "query"}};
}
std::string reference_role(const Record& record, RecordFieldId id) {
    const auto& descriptor = record->descriptor();
    auto field = std::find_if(descriptor.fields.begin(),
                              descriptor.fields.end(),
                              [&](const auto& f) { return f.id == id; });
    if (field == descriptor.fields.end())
        return descriptor.query_kind;
    std::string role = field->name;
    if (role == "members")
        role = "member";
    else if (role == "children")
        role = "child";
    else if (role == "nodes")
        role = "node";
    else if (role == "forces")
        role = "force";
    else if (role == "constraints")
        role = "constraint";
    return descriptor.query_kind + "." + role;
}
} // namespace
std::size_t query_entity_count(const DocumentView& view, std::string_view kind) {
    std::size_t count = 0;
    for (const auto type : view.registry()->types()) {
        const auto* descriptor = view.registry()->find(type);
        if (!descriptor->query_kind.empty() && (kind.empty() || kind == descriptor->query_kind))
            count += view.count(type);
    }
    return count;
}
Result<EntityPage> query_entities(const DocumentView& view,
                                  std::size_t offset,
                                  std::size_t limit,
                                  std::string_view kind,
                                  RecordStats* stats) {
    if (limit > 10000)
        return fail<EntityPage>(ErrorCode::resource_limit, "Entity page limit exceeded");
    EntityPage page{view.version(), offset, 0, {}};
    view.visit([&](const Record& record) {
        const auto& descriptor = record->descriptor();
        if (descriptor.query_kind.empty() || (!kind.empty() && kind != descriptor.query_kind))
            return;
        const auto index = page.total++;
        if (index < offset || page.entities.size() >= limit)
            return;
        const auto name = descriptor.display_name ? descriptor.display_name(record->object())
                                                  : std::string_view{};
        page.entities.push_back(
            {EntityId(record->key().identity), descriptor.query_kind, std::string(name)});
        if (stats)
            stats->model_bytes_copied += sizeof(EntitySummary) + record->key().identity.size() +
                                         descriptor.query_kind.size() + name.size();
    });
    return {Status::success, std::move(page), {}};
}
Result<RecordInput> query_fields(const DocumentView& view, const EntityId& id, RecordStats* stats) {
    const auto record = view.find_identity(id.value);
    if (!record || record->descriptor().query_kind.empty())
        return fail<RecordInput>(ErrorCode::entity_not_found, "Entity does not exist");
    if (stats)
        stats->model_bytes_copied += record->encoded().size();
    return {Status::success, record_wire::decode(record->encoded()), {}};
}
Result<ReferencePage> query_references(const DocumentView& view,
                                       const EntityId& selected,
                                       bool incoming,
                                       std::size_t offset,
                                       std::size_t limit,
                                       RecordStats* stats) {
    const auto chosen = view.find_identity(selected.value);
    if (!chosen || chosen->descriptor().query_kind.empty())
        return fail<ReferencePage>(ErrorCode::entity_not_found, "Entity does not exist");
    if (limit > 10000)
        return fail<ReferencePage>(ErrorCode::resource_limit, "Reference page limit exceeded");
    ReferencePage page{view.version(), offset, 0, {}};
    auto append = [&](std::string_view from, std::string_view to, auto make_role) {
        if ((incoming ? to : from) != selected.value)
            return;
        const auto index = page.total++;
        if (index < offset || page.references.size() >= limit)
            return;
        auto role = make_role();
        if (stats)
            stats->model_bytes_copied += sizeof(Reference) + from.size() + to.size() + role.size();
        page.references.push_back(
            {EntityId(std::string(from)), EntityId(std::string(to)), std::move(role)});
    };
    view.visit([&](const Record& record) {
        if (record->key().type == RecordTraits<records::SourceIdentifier>::type_id) {
            const auto& source = record->get<records::SourceIdentifier>();
            append(source.entity.value, source.include.value, [] {
                return std::string("source.include");
            });
            return;
        }
        if (record->descriptor().query_kind.empty())
            return;
        if (!incoming && record->key().identity != selected.value)
            return;
        record->descriptor().references(
            record->object(),
            [&](RecordFieldId field, std::string_view target, std::span<const RecordTypeId>) {
                append(
                    record->key().identity, target, [&] { return reference_role(record, field); });
            });
    });
    return {Status::success, std::move(page), {}};
}
std::vector<EntityId> query_affected_analyses(const DocumentView& view, const EntityId& id) {
    std::set<std::string_view> affected{id.value};
    bool grew = true;
    while (grew) {
        grew = false;
        view.visit([&](const Record& record) {
            bool related = false;
            record->descriptor().references(
                record->object(),
                [&](RecordFieldId, std::string_view target, std::span<const RecordTypeId>) {
                    if (affected.contains(target))
                        related = true;
                });
            if (related)
                grew |= affected.insert(record->key().identity).second;
        });
    }
    bool geometry = false;
    view.visit(RecordTraits<records::Beam>::type_id, [&](const Record& record) {
        geometry |= affected.contains(record->key().identity);
    });
    std::vector<EntityId> result;
    view.visit(RecordTraits<records::AnalysisDefinition>::type_id, [&](const Record& record) {
        if (geometry || affected.contains(record->key().identity))
            result.emplace_back(record->key().identity);
    });
    return result;
}
} // namespace qcae
