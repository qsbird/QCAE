#include "qcae/query.hpp"
#include "qcae/edit_session.hpp"
#include "qcae/records.hpp"
#include "qcae/records_model_bridge.hpp"
#include <algorithm>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace qcae;
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
} // namespace
int main() {
    try {
        queries();
        unassigned_beams_and_stable_pages();
        legacy_wrapper_scope();
        std::cout
            << "PASS: record queries, stable-ID pages, unassigned beams and legacy wrappers\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
