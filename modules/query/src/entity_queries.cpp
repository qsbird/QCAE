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
template <class T, class Visitor> void visit(const DocumentView& view, Visitor visitor) {
    view.visit(RecordTraits<T>::type_id, [&](const Record& record) { visitor(record->get<T>()); });
}
using IdSet = std::set<std::string_view>;
Result<IdSet> organization_scope(const DocumentView& view, const EntityFilter& filter) {
    if (filter.view == "all") {
        if (filter.owner)
            return fail<IdSet>(ErrorCode::invalid_input, "owner_id requires an organization view");
        return {Status::success, IdSet{}, {}};
    }
    static constexpr std::array<std::pair<std::string_view, RecordTypeId>, 6> views{
        {{"part", RecordTraits<records::Part>::type_id},
         {"assembly", RecordTraits<records::Assembly>::type_id},
         {"set", RecordTraits<records::EntitySet>::type_id},
         {"include", RecordTraits<records::IncludeDocument>::type_id},
         {"material", RecordTraits<records::Material>::type_id},
         {"property", RecordTraits<records::BeamSection>::type_id}}};
    const auto organization = std::find_if(
        views.begin(), views.end(), [&](const auto& entry) { return entry.first == filter.view; });
    if (organization == views.end() || !filter.owner)
        return fail<IdSet>(ErrorCode::invalid_input,
                           "Organization view requires a supported view and owner");
    const auto owner = view.find_identity(filter.owner->value);
    if (!owner || owner->descriptor().query_kind.empty())
        return fail<IdSet>(ErrorCode::entity_not_found, "Unknown view owner");
    if (owner->key().type != organization->second)
        return fail<IdSet>(ErrorCode::invalid_input, "View owner kind does not match");

    // Scope identities borrow immutable records; only returned page summaries copy strings.
    IdSet scope;
    std::vector<std::string_view> pending{owner->key().identity};
    auto append = [&](const auto& ids) {
        for (const auto& id : ids)
            pending.push_back(id.value);
    };
    while (!pending.empty()) {
        const auto current = pending.back();
        pending.pop_back();
        if (!scope.insert(current).second)
            continue;
        const auto record = view.find_identity(current);
        const auto type = record->key().type;
        if (type == RecordTraits<records::Part>::type_id)
            append(record->get<records::Part>().members);
        else if (type == RecordTraits<records::Assembly>::type_id)
            append(record->get<records::Assembly>().children);
        else if (type == RecordTraits<records::EntitySet>::type_id)
            append(record->get<records::EntitySet>().members);
        else if (type == RecordTraits<records::IncludeDocument>::type_id) {
            append(record->get<records::IncludeDocument>().members);
            visit<records::IncludeDocument>(view, [&](const auto& item) {
                if (item.parent && item.parent->value == current)
                    pending.push_back(item.id.value);
            });
        } else if (type == RecordTraits<records::Material>::type_id && filter.view == "material") {
            visit<records::BeamSection>(view, [&](const auto& section) {
                if (section.material.value == current)
                    pending.push_back(section.id.value);
            });
        } else if (type == RecordTraits<records::BeamSection>::type_id &&
                   (filter.view == "material" || filter.view == "property")) {
            visit<records::Beam>(view, [&](const auto& beam) {
                if (beam.section && beam.section->value == current)
                    pending.push_back(beam.id.value);
            });
        } else if (type == RecordTraits<records::Beam>::type_id &&
                   (filter.view == "material" || filter.view == "property" ||
                    filter.view == "part" || filter.view == "assembly"))
            append(record->get<records::Beam>().nodes);
    }
    scope.erase(filter.owner->value);
    return {Status::success, std::move(scope), {}};
}
} // namespace
bool query_kind_registered(const DocumentView& view, std::string_view kind) {
    if (kind.empty())
        return false;
    const auto types = view.registry()->types();
    return std::any_of(types.begin(), types.end(), [&](const auto type) {
        return view.registry()->find(type)->query_kind == kind;
    });
}
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
    EntityFilter filter;
    filter.kind = kind;
    return query_entities(view, filter, offset, limit, stats);
}
Result<EntityPage> query_entities(const DocumentView& view,
                                  const EntityFilter& filter,
                                  std::size_t offset,
                                  std::size_t limit,
                                  RecordStats* stats) {
    if (limit > 10000)
        return fail<EntityPage>(ErrorCode::resource_limit, "Entity page limit exceeded");
    if (!filter.kind.empty() && !query_kind_registered(view, filter.kind))
        return fail<EntityPage>(ErrorCode::invalid_input, "Unknown entity kind");
    const auto scope = organization_scope(view, filter);
    if (!scope.ok())
        return {scope.status, {}, scope.error};
    std::optional<IdSet> selected;
    if (filter.ids) {
        selected.emplace();
        for (const auto& id : *filter.ids)
            selected->insert(id.value);
    }
    EntityPage page{view.version(), offset, 0, {}};
    view.visit([&](const Record& record) {
        const auto& descriptor = record->descriptor();
        const std::string_view id = record->key().identity;
        if (descriptor.query_kind.empty() ||
            (!filter.kind.empty() && filter.kind != descriptor.query_kind) ||
            (selected && !selected->contains(id)) ||
            (filter.view != "all" && !scope.value->contains(id)))
            return;
        const auto name = descriptor.display_name ? descriptor.display_name(record->object())
                                                  : std::string_view{};
        if (name.find(filter.name_contains) == std::string_view::npos)
            return;
        const auto index = page.total++;
        if (index < offset || page.entities.size() >= limit)
            return;
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
