#include "qcae/query.hpp"
#include "qcae/edit_session.hpp"
#include "qcae/records.hpp"
#include "qcae/records_model_bridge.hpp"
#include <algorithm>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace qcae;
struct QueryRelation {
    EntityId id;
    std::string name;
    EntityId from;
    EntityId to;
};
namespace qcae {
template <> struct RecordTraits<QueryRelation> {
    inline static constexpr RecordTypeId type_id{900001};
    static const void* token() {
        static const char token{};
        return &token;
    }
};
} // namespace qcae
namespace {
void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
template <class T> T good(Result<T> result) {
    if (!result.ok())
        throw std::runtime_error(result.error ? result.error->message : "Missing result");
    return std::move(*result.value);
}
template <class T> void bad(const Result<T>& result, ErrorCode code, const char* message) {
    check(!result.ok() && result.error && result.error->code == code, message);
}
RecordDescriptor relation_descriptor() {
    RecordDescriptor descriptor;
    descriptor.type = RecordTraits<QueryRelation>::type_id;
    descriptor.name = "QueryRelation";
    descriptor.query_kind = "query_relation";
    descriptor.cpp_type_token = RecordTraits<QueryRelation>::token();
    descriptor.display_name = [](const void* value) -> std::string_view {
        return static_cast<const QueryRelation*>(value)->name;
    };
    const std::vector<RecordTypeId> targets{RecordTraits<records::Node>::type_id};
    descriptor.fields = {{RecordFieldId{2}, "name", RecordFieldKind::text, false, "", {}},
                         {RecordFieldId{3}, "from", RecordFieldKind::reference, false, "", targets},
                         {RecordFieldId{4}, "to", RecordFieldKind::reference, false, "", targets}};
    descriptor.encode = [](const void* object) {
        const auto& value = *static_cast<const QueryRelation*>(object);
        return RecordInput{
            {RecordTraits<QueryRelation>::type_id, value.id.value},
            1,
            {{RecordFieldId{2}, RecordFieldKind::text, "", record_wire::text(value.name)},
             {RecordFieldId{3},
              RecordFieldKind::reference,
              "",
              record_wire::text(value.from.value)},
             {RecordFieldId{4},
              RecordFieldKind::reference,
              "",
              record_wire::text(value.to.value)}}};
    };
    descriptor.decode = [](const RecordInput& input) -> std::shared_ptr<const void> {
        auto read = [&](std::uint32_t id) {
            return record_wire::read_text(record_wire::require(input, RecordFieldId{id}).payload);
        };
        return std::make_shared<const QueryRelation>(QueryRelation{
            EntityId(input.key.identity), read(2), EntityId(read(3)), EntityId(read(4))});
    };
    descriptor.owned_bytes = [](const void* object) {
        const auto& value = *static_cast<const QueryRelation*>(object);
        return sizeof(value) + value.id.value.size() + value.name.size() + value.from.value.size() +
               value.to.value.size();
    };
    descriptor.references = [](const void* object, const RecordReferenceVisitor& visitor) {
        const auto& value = *static_cast<const QueryRelation*>(object);
        const std::array<RecordTypeId, 1> targets{RecordTraits<records::Node>::type_id};
        visitor(RecordFieldId{3}, value.from.value, targets);
        visitor(RecordFieldId{4}, value.to.value, targets);
    };
    descriptor.validate = [](const void*, const DocumentView&) {};
    return descriptor;
}
DocumentView fixture() {
    EditSession edit{
        DocumentView(make_record_registry(), {{DocumentId("doc"), DocumentEpoch("epoch")}, 1})};
    edit.put(records::Material{EntityId("material"), "Steel", 210000, .3});
    edit.put(
        records::BeamSection{EntityId("section"), "Section", EntityId("material"), 1, 1, 1, 1});
    edit.put(records::Node{EntityId("left"), {-1, 0, 0}, {}});
    edit.put(records::Node{EntityId("right"), {1, 0, 0}, {}});
    edit.put(records::Node{EntityId("solo"), {0, 2, 0}, {}});
    edit.put(records::Beam{EntityId("beam"),
                           EntityId("section"),
                           {EntityId("left"), EntityId("right")},
                           {0, 1, 0},
                           {}});
    edit.put(records::Part{EntityId("part"), "Part", {EntityId("beam")}});
    edit.put(records::Assembly{EntityId("assembly"), "Assembly", {EntityId("part")}});
    edit.put(records::EntitySet{EntityId("set"), "Set", {EntityId("beam")}});
    edit.put(records::AnalysisDefinition{
        EntityId("analysis"), "Case", {{"test", "1", "digest"}, "linear_static"}, {}, {}});
    return edit.prepare().candidate;
}
void queries() {
    const auto records = fixture();
    const auto before = record_activity_counters();
    SelectionService service;
    Caller alice{"alice"}, bob{"bob"};
    auto view = good(service.create_view(records, alice, {EntityId("left")}));
    QuerySpec spec;
    spec.predicate.op = QueryOp::member_of;
    spec.predicate.related_entity = EntityId("assembly");
    spec.predicate.membership_kind = MembershipKind::assembly;
    auto ids = good(execute_query(records, view, spec)).ids;
    check(ids == std::vector<EntityId>({EntityId("beam"), EntityId("right")}),
          "Assembly membership lost hidden/endpoint semantics");
    spec.scope.include_hidden = true;
    check(good(execute_query(records, view, spec)).ids.size() == 3,
          "Explicit hidden members missing");
    spec.predicate.op = QueryOp::material;
    spec.predicate.related_entity = EntityId("material");
    check(good(execute_query(records, view, spec)).ids.size() == 5,
          "Material relationship closure changed");
    spec.predicate.op = QueryOp::world_box;
    spec.predicate.box = {{-.2, -.1, -.1}, {.2, .1, .1}, BoxRelation::intersects};
    check(good(execute_query(records, view, spec)).ids == std::vector<EntityId>{EntityId("beam")},
          "Crossing beam spatial query failed");
    spec.predicate.op = QueryOp::kind;
    spec.predicate.text = "node";
    spec.scope.include_hidden = false;
    auto handle = good(service.select(records, alice, view.id, spec));
    check(good(service.page(records, alice, handle.id, 0, 1)).ids.size() == 1,
          "Selection page incorrect");
    check(!service.page(records, bob, handle.id, 0, 1).ok(), "Caller boundary bypassed");
    auto newer = records.with_version({records.version().document, 2});
    check(!service.get_view(newer, alice, view.id).ok(), "Stale model revision accepted");
    auto current = good(service.update_view(newer, alice, view.id, {}, "camera"));
    check(!service.page(newer, alice, handle.id, 0, 1).ok(), "Old handle survived rebasing");
    auto expired = newer.with_version({{DocumentId("doc"), DocumentEpoch("other")}, 2});
    check(!service.get_view(expired, alice, current.id).ok(), "Expired epoch accepted");
    RecordStats stats;
    const auto page = good(query_entities(records, 1, 1, "node", &stats));
    check(page.total == 3 && page.entities.size() == 1, "Entity page materialized wrong slice");
    check(stats.model_bytes_copied < 512, "Entity page copied unrequested model data");
    check(query_entity_count(records, "node") == 3, "Metadata count incorrect");
    check(good(query_fields(records, EntityId("material"), &stats)).key.identity == "material",
          "Entity field record wrong");
    const auto refs = good(query_references(records, EntityId("material"), true, 0, 10, &stats));
    check(refs.total == 1 && refs.references[0].role == "section.material",
          "Record references lost semantic role");
    check(query_affected_analyses(records, EntityId("left")) ==
              std::vector<EntityId>{EntityId("analysis")},
          "Physical edit did not affect analysis");
    const auto after = record_activity_counters();
    check(before.whole_model_serializations == after.whole_model_serializations &&
              before.whole_model_materializations == after.whole_model_materializations,
          "Native query invoked a whole Model bridge");
}
void geometry_display_and_selection() {
    EditSession seed{DocumentView(make_record_registry(),
                                  {{DocumentId("geometry"), DocumentEpoch("epoch")}, 1})};
    seed.put(records::GeometryLine{records::GeometryId("crossing-line"), {-2, 0, 0}, {2, 0, 0}, 1});
    seed.put(records::GeometryLine{records::GeometryId("far-line"), {4, 2, 0}, {6, 2, 0}, 1});
    const auto records = seed.prepare().candidate;
    const auto before = record_activity_counters();
    SelectionService service;
    const Caller caller{"geometry-reader"};
    const auto view = good(service.create_view(records, caller));
    const auto packet = good(service.render_packet(records, caller, view.id));
    check(packet.document.id == records.version().document.id &&
              packet.document.epoch == records.version().document.epoch && packet.revision == 1 &&
              packet.view_session_id == view.id && packet.view_revision == view.view_revision,
          "Geometry packet lost model or view context");
    check(packet.points.empty() && packet.beams.empty() && packet.geometry_lines.size() == 2,
          "Geometry endpoints must not become physical nodes or beams");
    const auto crossing =
        std::find_if(packet.geometry_lines.begin(),
                     packet.geometry_lines.end(),
                     [](const auto& line) { return line.entity == EntityId("crossing-line"); });
    check(crossing != packet.geometry_lines.end() &&
              crossing->start_mm == std::array<double, 3>{-2, 0, 0} &&
              crossing->end_mm == std::array<double, 3>{2, 0, 0},
          "Geometry display must carry the real stable ID and millimetre coordinates");
    bad(produce_render_packet(records, view, 1),
        ErrorCode::resource_limit,
        "Visible geometry must consume the packet entity quota");

    QuerySpec query;
    query.predicate.op = QueryOp::world_box;
    query.predicate.box = {{-.5, -.5, -.5}, {.5, .5, .5}, BoxRelation::intersects};
    check(good(execute_query(records, view, query)).ids ==
              std::vector<EntityId>{EntityId("crossing-line")},
          "A world box must find a geometry segment with both endpoints outside");
    query.predicate.box.relation = BoxRelation::contained;
    check(good(execute_query(records, view, query)).ids.empty(),
          "Contained geometry selection must require both endpoints inside");
    query.predicate.box = {{-2, 0, 0}, {2, 0, 0}, BoxRelation::contained};
    check(good(execute_query(records, view, query)).ids ==
              std::vector<EntityId>{EntityId("crossing-line")},
          "Geometry containment must include boundary endpoints");

    query.predicate.op = QueryOp::kind;
    query.predicate.text = "geometry";
    query.scope.visibility = VisibilityMode::picker_candidates;
    query.scope.candidate_ids =
        std::vector<EntityId>{EntityId("crossing-line"), EntityId("far-line")};
    const auto both = good(service.select(records, caller, view.id, query));
    check(both.count == 2, "Picker candidates must retain real geometry identities");
    const auto hidden =
        good(service.update_view(records, caller, view.id, {EntityId("far-line")}, "camera"));
    const auto visible_packet = good(produce_render_packet(records, hidden, 1));
    check(visible_packet.geometry_lines.size() == 1 &&
              visible_packet.geometry_lines.front().entity == EntityId("crossing-line"),
          "Hidden geometry must be absent and must not consume the packet quota");
    const auto selected = good(service.select(records, caller, view.id, query));
    check(good(service.page(records, caller, selected.id, 0, 10)).ids ==
              std::vector<EntityId>{EntityId("crossing-line")},
          "Picker selection must respect hidden geometry");
    query.scope.include_hidden = true;
    check(good(service.select(records, caller, view.id, query)).count == 2,
          "Explicit hidden selection must remain available for geometry");
    query.scope.candidate_ids = std::vector<EntityId>{EntityId("absent")};
    bad(service.select(records, caller, view.id, query),
        ErrorCode::entity_not_found,
        "Unknown picker IDs must not become geometry entities");
    bad(service.page(records, caller, both.id, 0, 10),
        ErrorCode::revision_conflict,
        "View changes must invalidate geometry selection handles");
    const auto revised = records.with_version({records.version().document, 2});
    bad(produce_render_packet(revised, hidden),
        ErrorCode::revision_conflict,
        "Geometry rendering must reject stale model revisions");
    bad(service.select(revised, caller, view.id, QuerySpec{}),
        ErrorCode::revision_conflict,
        "Geometry selection must reject stale model revisions");
    const auto rebound = good(service.update_view(revised, caller, view.id, {}, "camera"));
    check(rebound.model_revision == 2 && rebound.view_revision > hidden.view_revision &&
              good(service.render_packet(revised, caller, view.id)).geometry_lines.size() == 2,
          "Rebased geometry view must refresh its model and view versions");
    const auto expired =
        revised.with_version({{DocumentId("geometry"), DocumentEpoch("next-epoch")}, 2});
    bad(service.render_packet(expired, caller, view.id),
        ErrorCode::document_epoch_expired,
        "Geometry rendering must reject an expired document epoch");
    const auto after = record_activity_counters();
    check(before.whole_model_serializations == after.whole_model_serializations &&
              before.whole_model_materializations == after.whole_model_materializations,
          "Geometry rendering or selection projected a whole legacy Model");
}
void mixed_geometry_rendering() {
    EditSession seed(fixture());
    seed.put(records::GeometryLine{records::GeometryId("line"), {-1, 0, 0}, {1, 0, 0}, 1});
    const auto records = seed.prepare().candidate;
    const auto before = record_activity_counters();
    SelectionService service;
    const Caller caller{"mixed-reader"};
    auto view = good(service.create_view(records, caller, {EntityId("left")}));
    const auto packet = good(produce_render_packet(records, view, 5));
    check(packet.points.size() == 3 && packet.beams.size() == 1 &&
              packet.geometry_lines.size() == 1,
          "Adding geometry changed the existing point and beam projection");
    const auto& beam = packet.beams.front();
    check(beam.entity == EntityId("beam") &&
              packet.points[beam.points[0]].entity == EntityId("left") &&
              !packet.points[beam.points[0]].visible &&
              packet.points[beam.points[1]].entity == EntityId("right"),
          "Hidden support points must retain stable beam connectivity");
    bad(produce_render_packet(records, view, 4),
        ErrorCode::resource_limit,
        "Packet quota must count nodes, beams and geometry together");
    view = good(service.update_view(
        records,
        caller,
        view.id,
        {EntityId("left"), EntityId("right"), EntityId("solo"), EntityId("beam")},
        {}));
    const auto isolated = good(produce_render_packet(records, view, 1));
    check(isolated.points.empty() && isolated.beams.empty() &&
              isolated.geometry_lines.size() == 1 &&
              isolated.geometry_lines.front().entity == EntityId("line"),
          "Isolating geometry must not expose hidden mesh entities");
    view = good(service.update_view(
        records,
        caller,
        view.id,
        {EntityId("left"), EntityId("right"), EntityId("solo"), EntityId("line")},
        {}));
    const auto isolated_beam = good(produce_render_packet(records, view, 3));
    check(isolated_beam.points.size() == 2 && isolated_beam.beams.size() == 1 &&
              isolated_beam.geometry_lines.empty() &&
              std::none_of(isolated_beam.points.begin(),
                           isolated_beam.points.end(),
                           [](const auto& point) { return point.visible; }),
          "Isolating a beam must preserve hidden endpoint support without drawing geometry");
    const auto after = record_activity_counters();
    check(before.whole_model_serializations == after.whole_model_serializations &&
              before.whole_model_materializations == after.whole_model_materializations,
          "Mixed display projection invoked a whole legacy Model bridge");
}
void unassigned_beams_and_stable_pages() {
    EditSession seed{DocumentView(make_record_registry(),
                                  {{DocumentId("mesh-doc"), DocumentEpoch("epoch")}, 1})};
    seed.put(records::Node{EntityId("z-node"), {0, 0, 0}, {}});
    seed.put(records::Node{EntityId("a-node"), {10, 0, 0}, {}});
    seed.put(records::Node{EntityId("middle-node"), {20, 0, 0}, {}});
    seed.put(records::Beam{
        EntityId("unassigned-beam"), {}, {EntityId("z-node"), EntityId("a-node")}, {0, 1, 0}, {}});
    const auto records = seed.prepare().candidate;
    const auto before = record_activity_counters();
    SelectionService service;
    const Caller caller{"mesh-reader"};
    const auto view = good(service.create_view(records, caller));
    QuerySpec spec;
    spec.predicate.op = QueryOp::kind;
    spec.predicate.text = "beam";
    check(good(execute_query(records, view, spec)).ids ==
              std::vector<EntityId>{EntityId("unassigned-beam")},
          "Beam without a section must remain queryable");
    const auto beam_fields = good(query_fields(records, EntityId("unassigned-beam")));
    check(beam_fields.key.identity == "unassigned-beam", "Fields lost stable beam identity");
    const auto beam_refs =
        good(query_references(records, EntityId("unassigned-beam"), false, 0, 10));
    check(beam_refs.total == 2 && beam_refs.references.size() == 2,
          "Unassigned beam should have node references and no section reference");
    for (const auto& reference : beam_refs.references)
        check(reference.from == EntityId("unassigned-beam") && reference.role == "beam.node",
              "Beam references contain storage positions instead of stable identities");

    spec.predicate.text = "node";
    const auto handle = good(service.select(records, caller, view.id, spec));
    const std::vector<EntityId> expected{
        EntityId("a-node"), EntityId("middle-node"), EntityId("z-node")};
    std::vector<EntityId> collected;
    for (std::size_t offset = 0; offset < handle.count; ++offset) {
        const auto page = good(service.page(records, caller, handle.id, offset, 1));
        check(page.handle.model_revision == 1 && page.offset == offset && page.ids.size() == 1,
              "Selection pagination lost snapshot metadata");
        collected.push_back(page.ids.front());
    }
    check(collected == expected, "Selection pages are not ordered by stable identity");
    check(good(service.page(records, caller, handle.id, std::numeric_limits<std::size_t>::max(), 1))
              .ids.empty(),
          "Large selection offset overflowed");
    check(good(query_entities(records, std::numeric_limits<std::size_t>::max(), 1, "node"))
              .entities.empty(),
          "Large entity offset overflowed");
    check(good(query_references(records,
                                EntityId("unassigned-beam"),
                                false,
                                std::numeric_limits<std::size_t>::max(),
                                1))
              .references.empty(),
          "Large reference offset overflowed");
    const auto all_nodes = good(query_entities(records, 0, 10, "node"));
    std::vector<EntityId> paged_nodes;
    for (std::size_t offset = 0; offset < all_nodes.total; ++offset) {
        const auto page = good(query_entities(records, offset, 1, "node"));
        check(same_record_version(page.version, records.version()) &&
                  page.total == all_nodes.total && page.entities.size() == 1,
              "Entity pages lost immutable snapshot metadata");
        check(page.entities.front().id == all_nodes.entities[offset].id,
              "Entity paging changed a stable identity");
        paged_nodes.push_back(page.entities.front().id);
    }
    std::sort(paged_nodes.begin(), paged_nodes.end());
    check(paged_nodes == expected, "Entity pages lost or duplicated stable node identities");
    EditSession changed(records);
    changed.erase({RecordTraits<records::Node>::type_id, "middle-node"});
    changed.put(records::Node{EntityId("new-node"), {30, 0, 0}, {}});
    const auto revised = changed.prepare().candidate.with_version({records.version().document, 2});
    const auto revised_nodes = good(query_entities(revised, 0, 10, "node"));
    check(revised_nodes.total == 3 && revised_nodes.entities.size() == 3 &&
              query_entity_count(revised, "node") == 3,
          "Entity pagination counted a deleted storage slot as a live record");
    std::vector<EntityId> revised_ids;
    for (std::size_t offset = 0; offset < revised_nodes.total; ++offset)
        revised_ids.push_back(good(query_entities(revised, offset, 1, "node")).entities.front().id);
    std::sort(revised_ids.begin(), revised_ids.end());
    check(revised_ids ==
              std::vector<EntityId>{EntityId("a-node"), EntityId("new-node"), EntityId("z-node")},
          "Insertion after deletion reused an obsolete identity in a query page");
    check(!service.page(revised, caller, handle.id, 0, 1).ok(),
          "A selection page reused a handle from an older model revision");
    const auto after = record_activity_counters();
    check(before.whole_model_serializations == after.whole_model_serializations &&
              before.whole_model_materializations == after.whole_model_materializations,
          "Unassigned beam query required a whole legacy Model");
}
void legacy_wrapper_scope() {
    const auto records = fixture();
    ModelSnapshot snapshot;
    static_cast<Model&>(snapshot) = model_from_records(records);
    snapshot.info.document = records.version().document;
    snapshot.info.revision = records.version().revision;
    const ViewSession view{
        "compatibility", snapshot.info.document, snapshot.info.revision, 1, {}, {}};
    const auto original = good(execute_query(records, view, {}));
    const auto legacy = good(execute_query(snapshot, view, {}));
    check(original.ids == legacy.ids,
          "Legacy query leaked the synthetic imported mesh into its original entity universe");
}
std::set<EntityId> page_ids(const EntityPage& page) {
    std::set<EntityId> result;
    for (const auto& entity : page.entities)
        result.insert(entity.id);
    return result;
}
void descriptor_entity_filters() {
    auto registry = std::make_shared<RecordRegistry>();
    for (auto descriptor : generated_record_descriptors())
        registry->add(std::move(descriptor));
    registry->add(relation_descriptor());
    registry->add_rule(records::validate_relations);
    registry->freeze();
    EditSession seed{DocumentView(registry, {{DocumentId("extended"), DocumentEpoch("epoch")}, 1})};
    seed.put(records::GeometryLine{records::GeometryId("line"), {0, 0, 0}, {10, 0, 0}, 1});
    seed.put(records::Mesh{
        records::MeshId("mesh"), "Named mesh", "geometry", records::GeometryId("line"), 1, false});
    seed.put(records::Node{EntityId("left"), {0, 0, 0}, records::MeshId("mesh")});
    seed.put(records::Node{EntityId("right"), {10, 0, 0}, records::MeshId("mesh")});
    seed.put(
        QueryRelation{EntityId("relation"), "Named relation", EntityId("left"), EntityId("right")});
    const auto records = seed.prepare().candidate;
    const auto before = record_activity_counters();
    const auto all = good(query_entities(records, 0, 100));
    check(all.total == 5, "No-kind enumeration omitted a contributed entity");
    EntityFilter filter;
    for (const auto& [kind, id] :
         std::vector<std::pair<std::string, EntityId>>{{"node", EntityId("left")},
                                                       {"geometry", EntityId("line")},
                                                       {"mesh", EntityId("mesh")},
                                                       {"query_relation", EntityId("relation")}}) {
        filter.kind = kind;
        filter.ids = std::vector<EntityId>{id, EntityId("unknown")};
        const auto page = good(query_entities(records, filter, 0, 1));
        check(page.total == 1 && page.entities.front().id == id,
              "Kind and ID filters do not compose for a registered type");
        check(!good(query_fields(records, id)).fields.empty(),
              "Registered fields were not readable");
        check(good(query_entities(records, filter, 1, 1)).entities.empty(),
              "Filtered page did not apply offset after filtering");
    }
    filter.ids.reset();
    filter.kind = "query_relation";
    filter.name_contains = "relation";
    check(good(query_entities(records, filter, 0, 10)).total == 1,
          "Custom display-name filter failed");
    filter.name_contains = "mesh";
    check(good(query_entities(records, filter, 0, 10)).total == 0,
          "Custom name filter silently ignored its condition");
    filter.kind.clear();
    filter.name_contains = "Named";
    check(good(query_entities(records, filter, 0, 10)).total == 2,
          "Name-only query used a fixed entity universe");
    filter.ids = std::vector<EntityId>{};
    check(good(query_entities(records, filter, 0, 10)).total == 0,
          "Explicit empty ID filter means empty scope");
    const auto unknown = query_entities(records, 0, 10, "unknown");
    check(!unknown.ok() && unknown.error->code == ErrorCode::invalid_input,
          "Unknown kind must not become a successful empty page");
    SelectionService service;
    const auto view = good(service.create_view(records, Caller{"reader"}));
    QuerySpec spec;
    spec.predicate.op = QueryOp::kind;
    spec.predicate.text = "unknown";
    check(!execute_query(records, view, spec).ok(), "Selection query accepted an unknown kind");
    spec.predicate.text = "query_relation";
    check(good(execute_query(records, view, spec)).ids ==
              std::vector<EntityId>{EntityId("relation")},
          "Selection query omitted a registered kind");
    const auto outgoing = good(query_references(records, EntityId("relation"), false, 0, 10));
    check(outgoing.total == 2 && outgoing.references[0].role == "query_relation.from" &&
              outgoing.references[1].role == "query_relation.to",
          "Custom outgoing references lost descriptor roles");
    const auto incoming = good(query_references(records, EntityId("right"), true, 0, 10));
    check(incoming.total == 1 && incoming.references[0].from == EntityId("relation"),
          "Custom incoming references omitted relationship records");
    check(good(query_references(records, EntityId("line"), true, 0, 10)).references[0].role ==
              "mesh.geometry",
          "Geometry incoming references omitted derived mesh");
    check(good(query_references(records, EntityId("mesh"), true, 0, 10)).total == 2,
          "Mesh incoming references omitted node ownership");
    const auto after = record_activity_counters();
    check(before.whole_model_serializations == after.whole_model_serializations &&
              before.whole_model_materializations == after.whole_model_materializations,
          "Generic filtered queries projected a whole legacy Model");
}
void organization_entity_filters() {
    EditSession seed(fixture());
    seed.put(records::Assembly{EntityId("root"), "Root", {EntityId("assembly")}});
    seed.put(records::IncludeDocument{EntityId("root-include"), "root.bdf", {}, {}});
    seed.put(records::IncludeDocument{
        EntityId("child-include"), "child.bdf", EntityId("root-include"), {EntityId("beam")}});
    seed.put(records::SourceIdentifier{EntityId("source"),
                                       EntityId("beam"),
                                       "source-model",
                                       EntityId("child-include"),
                                       {"test", "1", "digest"},
                                       "CBAR",
                                       7});
    const auto records = seed.prepare().candidate;
    const auto before = record_activity_counters();
    const std::set<EntityId> connected{EntityId("beam"), EntityId("left"), EntityId("right")};
    EntityFilter filter;
    filter.view = "part";
    filter.owner = EntityId("part");
    check(page_ids(good(query_entities(records, filter, 0, 100))) == connected,
          "Part entity view lost endpoint closure or included owner");
    filter.view = "assembly";
    filter.owner = EntityId("root");
    auto assembled = connected;
    assembled.insert(EntityId("part"));
    assembled.insert(EntityId("assembly"));
    check(page_ids(good(query_entities(records, filter, 0, 100))) == assembled,
          "Assembly entity view lost intermediate organization rows");
    filter.view = "set";
    filter.owner = EntityId("set");
    check(page_ids(good(query_entities(records, filter, 0, 100))) ==
              std::set<EntityId>{EntityId("beam")},
          "Set entity view must not expand beam endpoints");
    filter.view = "property";
    filter.owner = EntityId("section");
    check(page_ids(good(query_entities(records, filter, 0, 100))) == connected,
          "Property entity view changed");
    filter.view = "material";
    filter.owner = EntityId("material");
    auto physical = connected;
    physical.insert(EntityId("section"));
    check(page_ids(good(query_entities(records, filter, 0, 100))) == physical,
          "Material entity view lost properties or included owner");
    filter.view = "include";
    filter.owner = EntityId("root-include");
    check(page_ids(good(query_entities(records, filter, 0, 100))) ==
              std::set<EntityId>{EntityId("child-include"), EntityId("beam")},
          "INCLUDE entity view lost descendants or expanded endpoint ownership");
    filter.ids = std::vector<EntityId>{EntityId("beam"), EntityId("left")};
    filter.kind = "beam";
    check(page_ids(good(query_entities(records, filter, 0, 100))) ==
              std::set<EntityId>{EntityId("beam")},
          "Organization and ID/kind intersection changed");
    filter.owner = EntityId("absent");
    check(query_entities(records, filter, 0, 100).error->code == ErrorCode::entity_not_found,
          "Missing organization owner must be explicit");
    filter.owner = EntityId("beam");
    check(query_entities(records, filter, 0, 100).error->code == ErrorCode::invalid_input,
          "Wrong organization owner kind was accepted");
    filter.view = "unsupported";
    check(!query_entities(records, filter, 0, 100).ok(), "Unsupported view was ignored");
    const auto refs = good(query_references(records, EntityId("beam"), false, 0, 100));
    check(std::any_of(refs.references.begin(),
                      refs.references.end(),
                      [](const auto& reference) {
                          return reference.role == "source.include" &&
                                 reference.to == EntityId("child-include");
                      }),
          "Source-number ownership reference lost its public role");
    SelectionService service;
    const auto view = good(service.create_view(records, Caller{"reader"}));
    QuerySpec spec;
    spec.predicate.op = QueryOp::source_number_range;
    spec.predicate.text = "CBAR";
    spec.predicate.first_number = spec.predicate.last_number = 7;
    check(good(execute_query(records, view, spec)).ids == std::vector<EntityId>{EntityId("beam")},
          "Source-number selection changed stable identity");
    const auto after = record_activity_counters();
    check(before.whole_model_serializations == after.whole_model_serializations &&
              before.whole_model_materializations == after.whole_model_materializations,
          "Organization entity query projected a whole legacy Model");
}
void organization_kinds_are_not_cpp_types() {
    for (const auto* kind : {"part", "assembly", "set", "include", "material", "section", "beam"}) {
        auto registry = std::make_shared<RecordRegistry>();
        for (auto descriptor : generated_record_descriptors()) {
            // A contributed reference target is valid schema data, even when its query label
            // happens to match an existing organization or element label.
            if (descriptor.type == RecordTraits<records::Part>::type_id) {
                for (auto& field : descriptor.fields)
                    if (field.name == "members")
                        field.reference_types.push_back(RecordTraits<QueryRelation>::type_id);
                descriptor.references =
                    [references = descriptor.references](const void* object,
                                                         const RecordReferenceVisitor& visitor) {
                        references(object,
                                   [&](RecordFieldId field,
                                       std::string_view identity,
                                       std::span<const RecordTypeId> original) {
                                       std::vector<RecordTypeId> targets(original.begin(),
                                                                         original.end());
                                       targets.push_back(RecordTraits<QueryRelation>::type_id);
                                       visitor(field, identity, targets);
                                   });
                    };
            }
            registry->add(std::move(descriptor));
        }
        auto relation = relation_descriptor();
        relation.query_kind = kind;
        registry->add(std::move(relation));
        registry->add_rule(records::validate_relations);
        registry->freeze();
        EditSession seed{DocumentView(registry)};
        seed.put(records::Node{EntityId("from"), {0, 0, 0}, {}});
        seed.put(records::Node{EntityId("to"), {1, 0, 0}, {}});
        seed.put(QueryRelation{EntityId("relation"), "Relation", EntityId("from"), EntityId("to")});
        seed.put(records::Part{EntityId("owner"), "Owner", {EntityId("relation")}});
        const auto records = seed.prepare().candidate;
        EntityFilter filter;
        filter.kind = kind;
        filter.ids = std::vector<EntityId>{EntityId("relation")};
        check(good(query_entities(records, filter, 0, 100)).total == 1,
              "Shared query-kind label must still allow generic filtering");
        filter.kind.clear();
        filter.ids.reset();
        filter.owner = EntityId("relation");
        filter.view = std::string(kind) == "section" ? "property" : kind;
        if (filter.view == "beam")
            filter.view = "part";
        const auto rejected = query_entities(records, filter, 0, 100);
        check(!rejected.ok() && rejected.error->code == ErrorCode::invalid_input,
              "Organization owner must be checked by record type, without an invalid cast");
        filter.owner = EntityId("owner");
        filter.view = "part";
        check(page_ids(good(query_entities(records, filter, 0, 100))) ==
                  std::set<EntityId>{EntityId("relation")},
              "Organization traversal must not cast or expand descendants by their query label");
    }
}
} // namespace
int main() {
    try {
        queries();
        geometry_display_and_selection();
        mixed_geometry_rendering();
        unassigned_beams_and_stable_pages();
        legacy_wrapper_scope();
        descriptor_entity_filters();
        organization_entity_filters();
        organization_kinds_are_not_cpp_types();
        std::cout << "PASS: record queries, geometry display/selection, stable-ID pages, "
                     "unassigned beams and legacy wrappers\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
