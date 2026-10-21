#include "qcae/core.hpp"
#include "qcae/geometry_operations.hpp"
#include "qcae/records.hpp"
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>

using namespace qcae;
using namespace qcae::operations;
namespace {
using Point = std::array<double, 3>;

void check(bool value, const std::string& message) {
    if (!value)
        throw std::runtime_error(message);
}
template <class T> T good(Result<T> value, const std::string& message) {
    if (!value.ok())
        throw std::runtime_error(message + ": " + (value.error ? value.error->message : ""));
    return std::move(*value.value);
}

struct MemoryRows final : IRecordStore {
    std::uint64_t generation{};
    std::map<StoreKey, SharedStoreBytes> rows;
    LoadedRows load_rows() override {
        LoadedRows loaded;
        loaded.generation = generation;
        for (const auto& [key, value] : rows)
            loaded.rows.push_back({key, value});
        return loaded;
    }
    BatchReceipt commit_rows(const StoreBatch& batch) override {
        check(batch.expected_generation == generation, "Row generation must agree");
        std::uint64_t bytes{};
        for (const auto& row : batch.mutations) {
            if (row.after) {
                rows[row.key] = row.after;
                bytes += row.after->size();
            } else
                rows.erase(row.key);
        }
        return {++generation, batch.mutations.size(), bytes};
    }
};

struct Fixture {
    std::shared_ptr<MemoryRows> rows = std::make_shared<MemoryRows>();
    RecordApplication app{[&] {
        RecordApplicationOptions options;
        options.registry = make_record_registry();
        options.records = rows;
        return options;
    }()};
    OperationRegistry registry;
    Caller caller{"geometry-region-tests"};
    DocumentInfo document;
    Fixture() {
        document = good(app.create_document(caller, "Regions", "create"), "create document");
        good(features::geometry::register_handlers(registry, app), "register geometry");
    }
    OperationContext context(std::string key) {
        const auto info = good(app.current_document(), "current document");
        return {caller, info.document, info.revision, std::move(key), std::nullopt, "request"};
    }
    Result<Value> create(const Value& input, std::string key) {
        return registry.invoke("geometry.create_region", 1, context(std::move(key)), input);
    }
    DocumentView view() {
        return good(app.snapshot(document.document), "snapshot").records;
    }
    Revision revision() {
        return good(app.current_document(), "current document").revision;
    }
};

Value corners(std::initializer_list<Point> points) {
    Value::Array result;
    for (const auto& point : points)
        result.emplace_back(Value::Object{
            {"position_mm",
             Value(Value::Array{Value(point[0]), Value(point[1]), Value(point[2])})}});
    return Value(Value::Object{{"corners", Value(std::move(result))}});
}
const std::initializer_list<Point> plate{{0, 0, 0}, {200, 0, 0}, {200, 100, 0}, {0, 100, 0}};
const std::initializer_list<Point> square_hole{
    {90, 40, 0}, {110, 40, 0}, {110, 60, 0}, {90, 60, 0}};

Value region_input(std::initializer_list<Point> outer, std::optional<Value> hole = std::nullopt) {
    Value::Object input{{"outer", corners(outer)}};
    if (hole)
        input.emplace("holes", Value(Value::Array{*hole}));
    return Value(std::move(input));
}

const Value::Object& object(const Value& value) {
    return std::get<Value::Object>(value.data);
}
std::string text(const Value& value, const std::string& field) {
    return std::get<std::string>(object(value).at(field).data);
}
template <class T> std::size_t count(const DocumentView& view) {
    std::size_t result{};
    view.visit(RecordTraits<T>::type_id, [&](const Record&) { ++result; });
    return result;
}
std::vector<Point> corner_positions(const DocumentView& view, const EntityId& loop) {
    std::vector<Point> result;
    for (const auto& corner :
         view.find<records::GeometryLoop>(loop)->get<records::GeometryLoop>().corners)
        result.push_back(
            view.find<records::GeometryPoint>(corner)->get<records::GeometryPoint>().position);
    return result;
}

void a_plate_becomes_points_a_loop_and_a_region() {
    Fixture fixture;
    const auto receipt = good(fixture.create(region_input(plate), "plate"), "create plate");
    const auto view = fixture.view();
    check(count<records::GeometryRegion>(view) == 1 && count<records::GeometryLoop>(view) == 1 &&
              count<records::GeometryPoint>(view) == 4,
          "A plate must create one region, one loop and four points");
    const auto region =
        view.find<records::GeometryRegion>(records::GeometryId(text(receipt, "entity_id")));
    check(region != nullptr, "The receipt must name the region");
    const auto& value = region->get<records::GeometryRegion>();
    check(value.holes.empty() && value.geometry_revision == 1,
          "A new plate has no holes, revision 1");
    check(corner_positions(view, value.outer) == std::vector<Point>(plate),
          "The loop must keep the corners in input order");
}

void holes_become_loops_of_the_region() {
    Fixture fixture;
    const auto receipt = good(fixture.create(region_input(plate, corners(square_hole)), "holed"),
                              "create holed plate");
    const auto view = fixture.view();
    const auto& region =
        view.find<records::GeometryRegion>(records::GeometryId(text(receipt, "entity_id")))
            ->get<records::GeometryRegion>();
    check(region.holes.size() == 1 && count<records::GeometryLoop>(view) == 2 &&
              count<records::GeometryPoint>(view) == 8,
          "A plate with one hole must have two loops and eight points");
    check(corner_positions(view, region.holes[0]) == std::vector<Point>(square_hole),
          "The hole loop must keep its corners in input order");
}

void an_invalid_region_names_the_input_field_and_commits_nothing() {
    Fixture fixture;
    const auto before = fixture.revision();
    const auto crossing =
        region_input(plate, corners({{190, 10, 0}, {220, 10, 0}, {220, 30, 0}, {190, 30, 0}}));
    const auto result = fixture.create(crossing, "crossing");
    check(!result.ok() && result.error->code == ErrorCode::invalid_input &&
              result.error->field == "holes[0]" &&
              result.error->message == "Edges of the region cross or touch",
          "A hole crossing the outline must be refused against holes[0]");
    check(fixture.revision() == before && count<records::GeometryPoint>(fixture.view()) == 0,
          "A refused region must not commit anything");
}

void retries_replay_and_history_restores_the_region() {
    Fixture fixture;
    const auto first = good(fixture.create(region_input(plate), "plate"), "create plate");
    // The retry carries the context of the original request.
    const auto retry = good(fixture.registry.invoke("geometry.create_region",
                                                    1,
                                                    {fixture.caller,
                                                     fixture.document.document,
                                                     fixture.document.revision,
                                                     "plate",
                                                     std::nullopt,
                                                     "retry"},
                                                    region_input(plate)),
                            "retry");
    check(text(retry, "transaction_id") == text(first, "transaction_id") &&
              std::get<bool>(object(retry).at("replayed").data),
          "A retry with the same key must replay the original commit");
    const auto conflict = fixture.registry.invoke("geometry.create_region",
                                                  1,
                                                  {fixture.caller,
                                                   fixture.document.document,
                                                   fixture.document.revision,
                                                   "plate",
                                                   std::nullopt,
                                                   "conflict"},
                                                  region_input(square_hole));
    check(!conflict.ok() && conflict.error->code == ErrorCode::idempotency_key_conflict,
          "The same key with a different region must be refused");

    good(fixture.app.undo(fixture.caller, {fixture.document.document, fixture.revision()}, "undo"),
         "undo");
    check(count<records::GeometryRegion>(fixture.view()) == 0 &&
              count<records::GeometryPoint>(fixture.view()) == 0,
          "Undo must remove the region with its loop and points");
    good(fixture.app.redo(fixture.caller, {fixture.document.document, fixture.revision()}, "redo"),
         "redo");
    check(count<records::GeometryRegion>(fixture.view()) == 1 &&
              count<records::GeometryPoint>(fixture.view()) == 4,
          "Redo must restore the region with its loop and points");
}

// Writes records directly, bypassing geometry.create_region, to exercise the record rules.
Result<ChangeReceipt>
write(Fixture& fixture,
      const std::string& key,
      std::function<EntityId(EditSession&, const RecordIdentityAllocator&)> build) {
    return fixture.app.execute(
        fixture.caller,
        {fixture.document.document, fixture.revision()},
        "test.write",
        key,
        [build, key](const DocumentView& view,
                     const RecordIdentityAllocator& allocate) -> Result<RecordPreparedOperation> {
            EditSession edit(view);
            const auto primary = build(edit, allocate);
            return {Status::success,
                    RecordPreparedOperation{edit.prepare(), "Write", primary, key, 0, true},
                    {}};
        },
        key);
}
EntityId put_loop(EditSession& edit,
                  const RecordIdentityAllocator& allocate,
                  std::initializer_list<Point> points) {
    records::GeometryLoop loop{allocate(), {}};
    for (const auto& point : points) {
        const auto id = allocate();
        edit.put(records::GeometryPoint{id, point});
        loop.corners.push_back(id);
    }
    edit.put(loop);
    return loop.id;
}

void the_record_rules_guard_regions_written_directly() {
    Fixture fixture;
    const auto crossing =
        write(fixture, "crossing", [](EditSession& edit, const RecordIdentityAllocator& allocate) {
            const auto outer = put_loop(edit, allocate, plate);
            const auto hole =
                put_loop(edit, allocate, {{190, 10, 0}, {220, 10, 0}, {220, 30, 0}, {190, 30, 0}});
            const records::GeometryId id(allocate().value);
            edit.put(records::GeometryRegion{id, outer, {hole}, 1});
            return EntityId(id.value);
        });
    check(!crossing.ok() && crossing.error->code == ErrorCode::invalid_input &&
              crossing.error->message == "holes[0]: Edges of the region cross or touch",
          "The region rule must refuse a hole crossing the outline");
    const auto short_loop =
        write(fixture, "short", [](EditSession& edit, const RecordIdentityAllocator& allocate) {
            return put_loop(edit, allocate, {{0, 0, 0}, {1, 0, 0}});
        });
    check(!short_loop.ok() &&
              short_loop.error->message == "A geometry loop needs at least three corners",
          "The loop rule must refuse a two-corner loop");
    check(count<records::GeometryPoint>(fixture.view()) == 0,
          "Refused writes must not commit anything");
}

void a_mesh_may_be_derived_from_a_region() {
    Fixture fixture;
    const auto receipt = good(fixture.create(region_input(plate), "plate"), "create plate");
    const records::GeometryId region(text(receipt, "entity_id"));
    auto derived = [&](std::uint64_t revision) {
        return write(
            fixture,
            "mesh-" + std::to_string(revision),
            [=](EditSession& edit, const RecordIdentityAllocator& allocate) {
                const records::MeshId mesh(allocate().value);
                edit.put(records::Mesh{mesh, "Region mesh", "geometry", region, revision, false});
                return EntityId(mesh.value);
            });
    };
    const auto outdated = derived(2);
    check(!outdated.ok() &&
              outdated.error->message == "Outdated geometry binding must be marked stale",
          "A mesh claiming another region revision must be refused");
    good(derived(1), "A mesh bound to the current region revision must be accepted");
}
} // namespace

int main() {
    try {
        a_plate_becomes_points_a_loop_and_a_region();
        holes_become_loops_of_the_region();
        an_invalid_region_names_the_input_field_and_commits_nothing();
        retries_replay_and_history_restores_the_region();
        the_record_rules_guard_regions_written_directly();
        a_mesh_may_be_derived_from_a_region();
        std::cout << "PASS: geometry regions: created as points, loops and a region; invalid input "
                     "refused by field; retry, undo and redo; record rules; region-derived mesh\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
