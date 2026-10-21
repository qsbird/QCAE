#include "runtime_test_support.hpp"
#include "qcae/geometry_operations.hpp"
#include "qcae/mesh_generation_operations.hpp"
#include "qcae/region_mesh_task.hpp"
#include <iostream>
#ifdef QCAE_TEST_SQLITE
#include "qcae/sqlite_store.hpp"
#include <filesystem>
#include <random>
#endif

using namespace runtime_test;
namespace {
using Point = std::array<double, 3>;

// Stands in for Gmsh: fans triangles out from the first corner of the outer loop, a valid mesh
// for a convex outline without holes. An optional gate holds each call the way a running Gmsh
// mesh holds its task; an optional refusal answers like a mesher that cannot mesh the region.
struct FanMesher final : IPlanarMesher {
    std::shared_ptr<Gate> gate;
    std::optional<Diagnostic> refusal;
    std::string identity() const override {
        return "fan 1";
    }
    Result<PlanarMesh> mesh(const PlanarRegion& region, const PlanarMeshOptions&) const override {
        if (gate)
            gate->arrive_and_wait();
        if (refusal)
            return {Status::failed, std::nullopt, *refusal};
        PlanarMesh mesh{region.outer.corners_mm, {}, {}};
        for (std::uint32_t i = 1; i + 1 < mesh.nodes_mm.size(); ++i)
            mesh.triangles.push_back({0, i, i + 1});
        return success(std::move(mesh));
    }
};

struct FixtureStore {
#ifdef QCAE_TEST_SQLITE
    std::filesystem::path directory;
    std::shared_ptr<IRecordStore> records;
    FixtureStore() {
        do {
            directory = std::filesystem::temp_directory_path() /
                        ("qcae-region-mesh-" + std::to_string(std::random_device{}()));
        } while (!std::filesystem::create_directory(directory));
        records = std::make_shared<SqliteWorkspaceStore>((directory / "workspace.sqlite").string());
    }
    ~FixtureStore() {
        records.reset();
        std::filesystem::remove_all(directory);
    }
    void reopen() {
        records.reset();
        records = std::make_shared<SqliteWorkspaceStore>((directory / "workspace.sqlite").string());
    }
#else
    std::shared_ptr<IRecordStore> records = std::make_shared<Store>();
    void reopen() {}
#endif
};

const std::vector<Point> plate{{0, 0, 0}, {200, 0, 0}, {200, 100, 0}, {0, 100, 0}};

// Creates a region through the real geometry.create_region plan.
records::GeometryId
create_region(RecordApplication& app, const std::vector<Point>& corners, const std::string& key) {
    operations::GeometryCreateRegionInput input;
    for (const auto& corner : corners)
        input.outer.corners.push_back({corner});
    const auto plan = good(features::geometry::prepare_create_region(input));
    const auto receipt = good(app.execute(caller,
                                          at(good(app.current_document())),
                                          "geometry.create_region",
                                          plan.signature,
                                          plan.prepare,
                                          key));
    return records::GeometryId(receipt.primary_entity.value);
}

TaskRequest mesh_task(RecordApplication& app,
                      const records::GeometryId& region,
                      std::shared_ptr<const IPlanarMesher> mesher,
                      std::string key,
                      double element_size_mm = 50) {
    const auto snapshot = good(app.snapshot(good(app.current_document()).document));
    return region_mesh_task(
        snapshot, caller, {region, element_size_mm}, std::move(key), std::move(mesher));
}

DocumentView current_view(RecordApplication& app) {
    return good(app.snapshot(good(app.current_document()).document)).records;
}
std::size_t mesh_records(const DocumentView& view) {
    return view.count(RecordTraits<records::Mesh>::type_id) +
           view.count(RecordTraits<records::Node>::type_id) +
           view.count(RecordTraits<records::Tri3>::type_id);
}

void a_region_mesh_publishes_nodes_and_triangles() {
    FixtureStore store;
    TaskRecord result;
    {
        RecordApplication app(options(store.records));
        good(app.create_document(caller, "Plate", "create"));
        const auto region = create_region(app, plate, "plate");
        const auto before = good(app.current_document()).revision;
        TaskService service(publisher(app));
        const auto task =
            good(service.start(mesh_task(app, region, std::make_shared<FanMesher>(), "mesh")));
        result = good(service.wait(caller, task.id));
        check(result.state == TaskState::succeeded && result.receipt,
              "The region mesh task must succeed");
        const auto view = current_view(app);
        const auto mesh_id = region_mesh_identity(task.id);
        check(result.receipt->primary_entity.value == mesh_id.value,
              "The receipt must name the mesh");
        const auto mesh = view.find<records::Mesh>(mesh_id);
        check(mesh && mesh->get<records::Mesh>().origin == "geometry" &&
                  mesh->get<records::Mesh>().geometry == region &&
                  mesh->get<records::Mesh>().geometry_revision == 1 &&
                  !mesh->get<records::Mesh>().stale,
              "The mesh must be bound to the region and its revision");
        check(view.count(RecordTraits<records::Node>::type_id) == 4 &&
                  view.count(RecordTraits<records::Tri3>::type_id) == 2,
              "A fan over four corners gives four nodes and two triangles");
        for (std::size_t index = 0; index < plate.size(); ++index) {
            const auto node =
                view.find<records::Node>(EntityId("node-" + task.id + "-" + std::to_string(index)));
            check(node && node->get<records::Node>().position == plate[index] &&
                      node->get<records::Node>().mesh == mesh_id,
                  "Nodes keep the mesher's positions and belong to the mesh");
        }
        const auto node = [&](int index) {
            return EntityId("node-" + task.id + "-" + std::to_string(index));
        };
        const auto triangle = view.find<records::Tri3>(EntityId("tri-" + task.id + "-1"));
        check(triangle && triangle->get<records::Tri3>().nodes ==
                              std::array<EntityId, 3>{node(0), node(2), node(3)},
              "Triangles keep the mesher's connectivity");
        check(good(app.current_document()).revision == before + 1,
              "Meshing is exactly one model transaction");
    }
    store.reopen();
    {
        RecordApplication app(options(store.records));
        const auto recovered = good(app.recover_document(caller, "recover"));
        const auto view = good(app.snapshot(recovered.document)).records;
        check(view.count(RecordTraits<records::Tri3>::type_id) == 2 &&
                  view.count(RecordTraits<records::Node>::type_id) == 4,
              "The mesh survives a reopen");
        TaskService restored(publisher(app));
        check(good(restored.query(caller, result.id)).state == TaskState::succeeded,
              "The task's outcome survives a reopen");
    }
}

// mode 1 cancels while the mesher runs; mode 2 commits another change meanwhile.
void an_interrupted_mesh_publishes_nothing(int mode) {
    FixtureStore store;
    RecordApplication app(options(store.records));
    good(app.create_document(caller, "Plate", "create"));
    const auto region = create_region(app, plate, "plate");
    const auto before = good(app.current_document()).revision;
    auto mesher = std::make_shared<FanMesher>();
    mesher->gate = std::make_shared<Gate>();
    TaskService service(publisher(app));
    const auto task = good(service.start(mesh_task(app, region, mesher, "mesh")));
    mesher->gate->wait_for();
    if (mode == 1)
        check(good(service.cancel(caller, task.id)).accepted,
              "A cancel while meshing must be accepted");
    else
        create_region(app, {{300, 0, 0}, {400, 0, 0}, {400, 100, 0}}, "other");
    mesher->gate->release();
    const auto result = good(service.wait(caller, task.id));
    check(result.state == (mode == 1 ? TaskState::cancelled : TaskState::conflicted),
          mode == 1 ? "A cancelled mesh must end cancelled once the mesher returns"
                    : "A mesh of an outdated document must end conflicted");
    check(mesh_records(current_view(app)) == 0, "An interrupted mesh must publish nothing");
    check(good(app.current_document()).revision == before + (mode == 2 ? 1 : 0),
          "Only the other change may advance the revision");
}

void refused_and_bad_requests_publish_nothing() {
    FixtureStore store;
    RecordApplication app(options(store.records));
    good(app.create_document(caller, "Plate", "create"));
    const auto region = create_region(app, plate, "plate");
    TaskService service(publisher(app));

    auto refusing = std::make_shared<FanMesher>();
    refusing->refusal =
        Diagnostic{ErrorCode::resource_limit, "Too many triangles", "element_size_mm"};
    const auto refused =
        good(service.wait(caller, good(service.start(mesh_task(app, region, refusing, "big"))).id));
    // The task service reports any failure inside a task as invalid input of that task; the
    // mesher's message is what reaches the caller.
    check(refused.state == TaskState::failed && refused.diagnostic &&
              refused.diagnostic->message == "Too many triangles",
          "A mesher refusal must fail the task with the mesher's message");

    const auto missing = good(service.wait(
        caller,
        good(service.start(mesh_task(
                 app, records::GeometryId("no-region"), std::make_shared<FanMesher>(), "missing")))
            .id));
    check(missing.state == TaskState::failed && missing.diagnostic &&
              missing.diagnostic->message == "Region geometry does not exist",
          "A missing region must fail the task");

    bool rejected = false;
    try {
        (void)mesh_task(app, region, std::make_shared<FanMesher>(), "zero", 0);
    } catch (const RecordError& error) {
        rejected = error.code() == ErrorCode::invalid_input && error.field() == "element_size_mm";
    }
    check(rejected, "A non-positive element size must be refused before a task starts");
    check(mesh_records(current_view(app)) == 0, "Refused requests must publish nothing");
}

void without_a_mesher_the_operation_is_unavailable() {
    FixtureStore store;
    RecordApplication app(options(store.records));
    const auto info = good(app.create_document(caller, "Plate", "create"));
    operations::OperationRegistry registry;
    good(features::mesh_generation::register_region_mesh(
        registry,
        app,
        [](TaskRequest) -> Result<operations::Value> {
            throw std::logic_error("An unavailable operation must not start a task");
        },
        nullptr));
    const auto result = registry.invoke(
        "mesh.generate_region",
        1,
        {caller, info.document, info.revision, "mesh", std::nullopt, "request"},
        operations::Value(operations::Value::Object{{"geometry_id", operations::Value("region")},
                                                    {"element_size_mm", operations::Value(10.0)}}));
    check(!result.ok() && result.error->code == ErrorCode::unsupported_capability &&
              result.error->message.find("QCAE_WITH_GMSH=OFF") != std::string::npos,
          "Without a mesher the operation must be unavailable, with the reason");
}
} // namespace

int main() {
    try {
        a_region_mesh_publishes_nodes_and_triangles();
        an_interrupted_mesh_publishes_nothing(1);
        an_interrupted_mesh_publishes_nothing(2);
        refused_and_bad_requests_publish_nothing();
        without_a_mesher_the_operation_is_unavailable();
#ifdef QCAE_TEST_SQLITE
        std::cout << "PASS: region mesh tasks on SQLite: ";
#else
        std::cout << "PASS: region mesh tasks in memory: ";
#endif
        std::cout << "publication and reopen, cancel and stale while meshing, refusals, "
                     "unavailable without a mesher\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
