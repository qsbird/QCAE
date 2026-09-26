#include "qcae/core.hpp"
#include "qcae/query.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace qcae;
namespace {

EntityId id(const char* value) {
    return EntityId(value);
}

void check(bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error(message);
}

template <class T> const T& good(const Result<T>& result, const std::string& message) {
    check(result.ok(), message + (result.error ? ": " + result.error->message : ""));
    return *result.value;
}

template <class T> void bad(const Result<T>& result, ErrorCode code, const std::string& message) {
    const Status expected_status =
        code == ErrorCode::revision_conflict || code == ErrorCode::document_epoch_expired
            ? Status::conflict
            : Status::failed;
    check(!result.ok() && result.status == expected_status && result.error &&
              result.error->code == code,
          message);
}

ModelSnapshot sample() {
    ModelSnapshot snapshot;
    snapshot.info.document = {DocumentId("doc"), DocumentEpoch("epoch")};
    snapshot.info.revision = 7;
    snapshot.materials.push_back({id("mat"), "Steel", 210000, 0.3});
    snapshot.sections.push_back({id("sec"), "Beam section", id("mat"), 1, 1, 1, 1});
    snapshot.nodes.push_back({id("left"), {-2, 0, 0}});
    snapshot.nodes.push_back({id("mid"), {0, 0, 0}});
    snapshot.nodes.push_back({id("right"), {2, 0, 0}});
    snapshot.nodes.push_back({id("solo"), {0, 2, 0}});
    snapshot.beams.push_back({id("crossing"), id("sec"), {id("left"), id("right")}, {0, 1, 0}});
    snapshot.beams.push_back({id("inside"), id("sec"), {id("mid"), id("right")}, {0, 1, 0}});
    snapshot.parts.push_back({id("part"), "Part", {id("crossing")}});
    snapshot.assemblies.push_back({id("assembly"), "Assembly", {id("part")}});
    snapshot.sets.push_back({id("set"), "Only crossing", {id("crossing")}});
    snapshot.includes.push_back({id("root"),
                                 "root.bdf",
                                 std::nullopt,
                                 {id("mat"),
                                  id("sec"),
                                  id("left"),
                                  id("right"),
                                  id("crossing"),
                                  id("part"),
                                  id("assembly"),
                                  id("set")}});
    snapshot.includes.push_back(
        {id("child"), "child.bdf", id("root"), {id("mid"), id("inside"), id("solo")}});
    snapshot.sources.push_back(
        {id("crossing"), "source", id("root"), {"nastran", "1", "digest"}, "CBEAM", 10});
    return snapshot;
}

QueryPredicate predicate(QueryOp op) {
    QueryPredicate result;
    result.op = op;
    return result;
}

std::vector<EntityId>
query_ids(const ModelSnapshot& snapshot, const ViewSession& view, QuerySpec spec) {
    return good(execute_query(snapshot, view, spec), "query").ids;
}

void check_ids(const std::vector<EntityId>& actual,
               std::initializer_list<EntityId> expected,
               const std::string& message) {
    std::vector<EntityId> sorted(expected);
    std::sort(sorted.begin(), sorted.end());
    check(actual == sorted, message);
}

void predicate_semantics() {
    const auto snapshot = sample();
    const ViewSession view{"view", snapshot.info.document, snapshot.info.revision, 1, {}, {}};
    QuerySpec spec;
    auto kind = predicate(QueryOp::kind);
    kind.text = "beam";
    auto section = predicate(QueryOp::section);
    section.related_entity = id("sec");
    spec.predicate = predicate(QueryOp::and_);
    spec.predicate.children = {kind, section};
    check_ids(query_ids(snapshot, view, spec),
              {id("crossing"), id("inside")},
              "logical AND and section relation");
    spec.predicate = predicate(QueryOp::or_);
    auto named = predicate(QueryOp::name_contains);
    named.text = "Steel";
    spec.predicate.children = {kind, named};
    check_ids(query_ids(snapshot, view, spec),
              {id("crossing"), id("inside"), id("mat")},
              "logical OR and name");
    spec.predicate = predicate(QueryOp::not_);
    spec.predicate.children = {kind};
    spec.scope.candidate_ids = std::vector<EntityId>{id("crossing"), id("left")};
    check_ids(query_ids(snapshot, view, spec), {id("left")}, "logical NOT within candidates");

    spec.scope.candidate_ids.reset();
    spec.predicate = predicate(QueryOp::source_number_range);
    spec.predicate.text = "CBEAM";
    spec.predicate.first_number = 10;
    spec.predicate.last_number = 10;
    check_ids(query_ids(snapshot, view, spec), {id("crossing")}, "source number range");
    spec.predicate = predicate(QueryOp::ids);
    spec.predicate.ids = {id("solo"), id("left")};
    check_ids(query_ids(snapshot, view, spec), {id("solo"), id("left")}, "explicit IDs");
    spec.predicate.ids = {id("unknown")};
    check_ids(query_ids(snapshot, view, spec), {}, "unknown predicate ID never materializes");
}

void membership_and_space() {
    const auto snapshot = sample();
    const ViewSession view{"view", snapshot.info.document, snapshot.info.revision, 1, {}, {}};
    QuerySpec spec;
    spec.predicate = predicate(QueryOp::member_of);
    spec.predicate.related_entity = id("part");
    spec.predicate.membership_kind = MembershipKind::part;
    check_ids(query_ids(snapshot, view, spec),
              {id("crossing"), id("left"), id("right")},
              "part beam endpoints expanded");
    spec.predicate.related_entity = id("assembly");
    spec.predicate.membership_kind = MembershipKind::assembly;
    check_ids(query_ids(snapshot, view, spec),
              {id("crossing"), id("left"), id("right")},
              "assembly descendants expanded");
    spec.predicate.related_entity = id("set");
    spec.predicate.membership_kind = MembershipKind::set;
    check_ids(
        query_ids(snapshot, view, spec), {id("crossing")}, "set excludes inferred beam endpoints");
    spec.predicate.related_entity = id("root");
    spec.predicate.membership_kind = MembershipKind::include;
    check(query_ids(snapshot, view, spec).size() == 11, "include contains nested include members");
    spec.predicate.related_entity = id("child");
    check_ids(query_ids(snapshot, view, spec),
              {id("mid"), id("inside"), id("solo")},
              "child include excludes physical cross-reference endpoints");

    spec.predicate = predicate(QueryOp::world_box);
    spec.predicate.box = {{-1, -1, -1}, {1, 1, 1}, BoxRelation::intersects};
    check_ids(query_ids(snapshot, view, spec),
              {id("crossing"), id("inside"), id("mid")},
              "segment crossing intersects even with both endpoints outside");
    spec.predicate.box.relation = BoxRelation::contained;
    check_ids(query_ids(snapshot, view, spec),
              {id("mid")},
              "containment requires both beam endpoints inside");
    spec.predicate.box.maximum.x = -2;
    bad(execute_query(snapshot, view, spec), ErrorCode::invalid_input, "invalid box rejected");
}

void selection_and_render() {
    auto snapshot = sample();
    const Caller alice{"alice"};
    const Caller bob{"bob"};
    SelectionService service;
    const auto view =
        good(service.create_view(snapshot, alice, {id("left"), id("inside")}, "camera-a"),
             "create view");
    bad(service.get_view(snapshot, bob, view.id), ErrorCode::invalid_input, "owner bound");
    const auto packet = good(service.render_packet(snapshot, alice, view.id), "render packet");
    check(packet.beams.size() == 1 && packet.beams[0].entity == id("crossing"),
          "hidden beam omitted");
    check(packet.points.size() == 4, "standalone and support points included");
    const auto hidden_support =
        std::find_if(packet.points.begin(), packet.points.end(), [&](const RenderPoint& point) {
            return point.entity == id("left");
        });
    check(hidden_support != packet.points.end() && !hidden_support->visible,
          "hidden endpoint retained for beam connectivity but not drawn");
    check(packet.points[packet.beams[0].points[0]].entity == id("left") &&
              packet.points[packet.beams[0].points[1]].entity == id("right"),
          "beam indices map to stable node IDs");
    bad(produce_render_packet(snapshot, view, 1),
        ErrorCode::resource_limit,
        "packet quota enforced before allocation");

    QuerySpec spec;
    spec.predicate = predicate(QueryOp::kind);
    spec.predicate.text = "node";
    spec.scope.candidate_ids = std::vector<EntityId>{id("left"), id("mid")};
    const auto first = good(service.select(snapshot, alice, view.id, spec), "select candidates");
    bad(service.page(snapshot, bob, first.id, 0, 10),
        ErrorCode::invalid_input,
        "other caller cannot read selection");
    check_ids(good(service.page(snapshot, alice, first.id, 0, 10), "page").ids,
              {id("mid")},
              "hidden excluded independently from candidate domain");
    spec.scope.include_hidden = true;
    spec.scope.invert = true;
    auto inverted = good(service.select(snapshot, alice, view.id, spec), "invert");
    check(inverted.count == 0, "inversion uses declared universe");
    spec.scope.invert = false;
    auto both = good(service.select(snapshot, alice, view.id, spec), "include hidden");
    check_ids(good(service.page(snapshot, alice, both.id, 0, 10), "page").ids,
              {id("left"), id("mid")},
              "hidden included when explicit");
    const auto difference =
        good(service.combine(snapshot, alice, view.id, both.id, first.id, SetOperation::difference),
             "difference");
    check_ids(good(service.page(snapshot, alice, difference.id, 0, 10), "page").ids,
              {id("left")},
              "set difference");
    const auto intersection = good(
        service.combine(snapshot, alice, view.id, both.id, first.id, SetOperation::intersection),
        "intersection");
    check(intersection.count == 1, "set intersection");
    const auto united =
        good(service.combine(snapshot, alice, view.id, both.id, first.id, SetOperation::union_),
             "union");
    check(united.count == 2, "set union");
    spec.scope.visibility = VisibilityMode::visible_only;
    bad(service.select(snapshot, alice, view.id, spec),
        ErrorCode::unsupported_capability,
        "exact visible-only requires graphics provider");
    spec.scope.visibility = VisibilityMode::picker_candidates;
    spec.scope.candidate_ids.reset();
    bad(service.select(snapshot, alice, view.id, spec),
        ErrorCode::missing_input,
        "picker must supply candidates");
    spec.scope.candidate_ids = std::vector<EntityId>{id("unknown")};
    bad(service.select(snapshot, alice, view.id, spec),
        ErrorCode::entity_not_found,
        "picker unknown ID rejected");

    const auto updated = good(
        service.update_view(snapshot, alice, view.id, {id("left")}, "camera-b"), "camera update");
    check(updated.view_revision == view.view_revision + 1, "camera changes view revision");
    bad(service.page(snapshot, alice, first.id, 0, 10),
        ErrorCode::revision_conflict,
        "camera invalidates handle");
    bad(service.combine(snapshot, alice, view.id, both.id, first.id, SetOperation::union_),
        ErrorCode::revision_conflict,
        "stale handles cannot combine");
    auto current = good(service.select(snapshot, alice, view.id, QuerySpec{}), "current query");
    snapshot.info.revision++;
    bad(service.page(snapshot, alice, current.id, 0, 10),
        ErrorCode::revision_conflict,
        "model revision invalidates handle");
    const auto rebound = good(service.update_view(snapshot, alice, view.id, {}, "camera-b"),
                              "rebind view to model revision");
    check(rebound.view_revision > updated.view_revision, "model rebind advances view revision");
    bad(service.page(snapshot, alice, current.id, 0, 10),
        ErrorCode::revision_conflict,
        "old model handle remains stale");
    snapshot.info.document.epoch = DocumentEpoch("new-epoch");
    bad(service.get_view(snapshot, alice, view.id),
        ErrorCode::document_epoch_expired,
        "epoch invalidates view");
}

void bounded_benchmark() {
    ModelSnapshot snapshot;
    snapshot.info.document = {DocumentId("benchmark"), DocumentEpoch("epoch")};
    snapshot.info.revision = 1;
    for (int index = 0; index < 1000; ++index)
        snapshot.nodes.push_back(
            {EntityId("node-" + std::to_string(index)), {static_cast<double>(index), 0, 0}});
    SelectionService service;
    const auto view = good(service.create_view(snapshot, Caller{"benchmark"}), "benchmark view");
    QuerySpec spec;
    spec.predicate = predicate(QueryOp::world_box);
    spec.predicate.box = {{100, -1, -1}, {199, 1, 1}, BoxRelation::contained};
    const auto start = std::chrono::steady_clock::now();
    const auto result =
        good(service.select(snapshot, Caller{"benchmark"}, view.id, spec), "benchmark query");
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start);
    check(result.count == 100, "benchmark query count");
    std::cout << "1000-node world-box query: " << elapsed.count() << " us\n";
}

void session_identity_and_expiry() {
    auto snapshot = sample();
    const Caller owner{"owner"};
    SelectionLimits limits;
    limits.max_views = 1;
    limits.max_handles = 1;
    SelectionService first_service(limits);
    SelectionService second_service(limits);
    const auto first = good(first_service.create_view(snapshot, owner), "first service view");
    const auto independent =
        good(second_service.create_view(snapshot, owner), "second service view");
    check(first.id != independent.id, "view IDs differ across service instances");
    const auto selection = good(first_service.select(snapshot, owner, first.id, QuerySpec{}),
                                "first service selection");
    bad(first_service.create_view(snapshot, owner),
        ErrorCode::resource_limit,
        "live same-document view consumes quota");
    snapshot.info.document.epoch = DocumentEpoch("next-epoch");
    const auto next =
        good(first_service.create_view(snapshot, owner), "new epoch prunes expired view");
    check(next.id != first.id, "new epoch gets distinct session ID");
    bad(first_service.get_view(snapshot, owner, first.id),
        ErrorCode::document_epoch_expired,
        "retired view reports explicit epoch conflict");
    bad(first_service.page(snapshot, owner, selection.id, 0, 10),
        ErrorCode::document_epoch_expired,
        "retired selection reports explicit epoch conflict");
    const auto fresh =
        good(first_service.select(snapshot, owner, next.id, QuerySpec{}), "new epoch selection");
    check(fresh.id != selection.id, "selection handles differ across epochs");
}

} // namespace

int main() {
    try {
        predicate_semantics();
        membership_and_space();
        selection_and_render();
        session_identity_and_expiry();
        bounded_benchmark();
        std::cout << "query tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
