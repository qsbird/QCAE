#include "qcae/nastran_contribution.hpp"
#include "qcae/nastran_package.hpp"
#include "qcae/operation_inputs.hpp"
#include "qcae/records.hpp"
#include "qcae/records_model_bridge.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
using namespace qcae;
using namespace qcae::ipc;
using namespace qcae::operations;
void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
template <class T> T good(Result<T> result) {
    check(result.ok(), result.error ? result.error->message.c_str() : "Missing result");
    return std::move(*result.value);
}
Model fixture(const NastranCodec& codec) {
    const auto root = std::filesystem::path(__FILE__).parent_path() / "fixtures/nastran";
    ImportRequest request{
        "cantilever.bdf", {}, codec.definition().reference, "validation-binding", "mm-N-MPa"};
    for (const auto* path :
         {"cantilever.bdf", "mesh/nodes.bdf", "mesh/beams.bdf", "properties.bdf"}) {
        std::ifstream input(root / path, std::ios::binary);
        check(static_cast<bool>(input), "Validation fixture is missing");
        request.resources.push_back(
            {path, {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()}});
    }
    auto decoded = codec.decode(request);
    check(decoded.report.complete && decoded.candidate && !decoded.candidate->analyses.empty(),
          "Validation fixture import failed");
    return std::move(*decoded.candidate);
}
void retains_validation_state() {
    EngineValidationBinding binding;
    std::weak_ptr<NastranArtifactCoordinator> wrapper;
    std::weak_ptr<const IModelCodec> state;
    Model imported;
    {
        auto coordinator = std::make_shared<NastranArtifactCoordinator>();
        wrapper = coordinator;
        state = coordinator->codec_binding().codec;
        imported = fixture(coordinator->codec());
        binding = coordinator->export_validation();
    }
    check(wrapper.expired() && !state.expired(),
          "Validation binding must own state without retaining its coordinator wrapper");
    check(binding.id == "qcae.nastran.export" && binding.version == 1 && binding.validate_export,
          "Export validation identity is incomplete");
    const auto view = records_from_model(imported, make_record_registry());
    const auto analysis = imported.analyses.front().id;
    const auto plan = good(binding.validate_export(view, analysis));
    check(plan.profile == imported.analyses.front().target.profile && !plan.root_resource.empty() &&
              !plan.resources.empty(),
          "Retained binding must produce a real controlled export");
    verify_nastran_readback(plan, plan.resources, NastranCodec{});
    const auto missing = binding.validate_export(view, EntityId("missing-analysis"));
    check(!missing.ok() && missing.error && missing.error->code == ErrorCode::invalid_input,
          "Validation binding accepted an absent analysis");
    auto incompatible = imported;
    incompatible.analyses.front().target.analysis_kind = "nonlinear_static";
    const auto wrong_target =
        binding.validate_export(records_from_model(incompatible, make_record_registry()), analysis);
    check(!wrong_target.ok() && wrong_target.error &&
              wrong_target.error->code == ErrorCode::invalid_input,
          "Validation binding accepted an incompatible analysis target");
    EditSession edit(view);
    const records::MeshId mesh("validation-tri-mesh");
    edit.put(records::Mesh{mesh, "Validation mesh", "manual", {}, 0, false});
    const std::array<EntityId, 3> nodes{
        EntityId("validation-tri-1"), EntityId("validation-tri-2"), EntityId("validation-tri-3")};
    edit.put(records::Node{nodes[0], {0, 0, 0}, mesh});
    edit.put(records::Node{nodes[1], {1, 0, 0}, mesh});
    edit.put(records::Node{nodes[2], {0, 1, 0}, mesh});
    edit.put(records::Tri3{EntityId("validation-tri"), nodes, mesh});
    const auto shell = binding.validate_export(edit.prepare().candidate, analysis);
    check(!shell.ok() && shell.error && shell.error->code == ErrorCode::unsupported_capability,
          "Validation binding accepted unsupported Tri3 export");
    binding = {};
    check(state.expired(), "Validation binding introduced a shared-state ownership cycle");
}
struct Store final : IRecordStore {
    std::uint64_t generation{};
    std::map<StoreKey, SharedStoreBytes> rows;
    LoadedRows load_rows() override {
        LoadedRows result;
        result.generation = generation;
        for (const auto& [key, value] : rows)
            result.rows.push_back({key, value});
        return result;
    }
    BatchReceipt commit_rows(const StoreBatch& batch) override {
        check(batch.expected_generation == generation, "Store generation mismatch");
        auto candidate = rows;
        for (const auto& mutation : batch.mutations)
            if (mutation.after)
                candidate[mutation.key] = mutation.after;
            else
                candidate.erase(mutation.key);
        rows.swap(candidate);
        return {++generation, batch.mutations.size(), 0};
    }
    std::map<StoreKey, std::string> bytes() const {
        std::map<StoreKey, std::string> result;
        for (const auto& [key, value] : rows)
            result.emplace(key, *value);
        return result;
    }
};
bool same_history(const HistorySnapshot& before, const HistorySnapshot& after) {
    if (before.cursor != after.cursor || before.revision != after.revision ||
        before.items.size() != after.items.size())
        return false;
    for (std::size_t index = 0; index < before.items.size(); ++index)
        if (before.items[index].transaction != after.items[index].transaction ||
            before.items[index].label != after.items[index].label ||
            before.items[index].applied != after.items[index].applied)
            return false;
    return true;
}
void invokes_registered_validation(const std::filesystem::path& directory) {
    auto coordinator = std::make_shared<NastranArtifactCoordinator>();
    const auto imported = fixture(coordinator->codec());
    auto store = std::make_shared<Store>();
    RecordApplicationOptions options;
    options.registry = make_record_registry();
    options.records = store;
    options.owned_row_handlers = {task_row_handler(),
                                  NastranArtifactCoordinator::row_handler(),
                                  NastranArtifactCoordinator::reconcile_row_handler()};
    RecordApplication app(std::move(options));
    const Caller caller{"validation-binding-test"};
    auto info = good(app.create_document(caller, "Validation fixture", "create"));
    good(app.execute(
        caller,
        {info.document, info.revision},
        "test.seed",
        "validation-fixture",
        [imported](const DocumentView& before, const RecordIdentityAllocator&) {
            auto candidate = records_from_model(imported, before.registry(), before.version());
            PreparedRecordChange change{before.version(),
                                        std::move(candidate),
                                        record_changes_from_models({}, imported, before.registry()),
                                        {}};
            return Result<RecordPreparedOperation>{
                Status::success,
                RecordPreparedOperation{std::move(change),
                                        "Seed validation fixture",
                                        imported.analyses.front().id,
                                        "validation-fixture",
                                        0,
                                        false},
                {}};
        },
        "seed"));
    info = good(app.current_document());
    const auto snapshot = good(app.snapshot(info.document));
    const auto history = good(app.history(info.document));
    const auto rows = store->bytes();
    const auto generation = store->generation;
    int calls = 0, task_accesses = 0;
    EngineValidationBinding validation{
        "test.reject-export",
        1,
        [&](const DocumentView& view, const EntityId& analysis) -> Result<ArtifactPlan> {
            ++calls;
            check(view.matches_version(snapshot.records.version()) &&
                      diff_record_views(snapshot.records, view).empty() &&
                      analysis == imported.analyses.front().id,
                  "Registered validator received a different model or analysis");
            return {Status::failed,
                    {},
                    Diagnostic{ErrorCode::unsupported_capability,
                               "Injected export validator rejected this fixture",
                               "test.reject-export"}};
        }};
    OperationRegistry registry;
    good(coordinator->register_operations(
        registry,
        app,
        [&]() -> TaskService& {
            ++task_accesses;
            throw std::runtime_error("Rejected export must not access the task service");
        },
        validation));
    // The registry must own its callback copy after the caller releases the binding.
    validation = {};
    const OperationContext context{caller,
                                   info.document,
                                   info.revision,
                                   "rejected-export",
                                   coordinator->codec().definition().reference,
                                   "validation-request",
                                   1};
    const ModelExportInput input{imported.analyses.front().id, (directory / "export").string()};
    for (int attempt = 0; attempt < 2; ++attempt) {
        const auto rejected = registry.invoke(
            "model.export", 1, context, InputTraits<ModelExportInput>::to_value(input));
        check(!rejected.ok() && rejected.error &&
                  rejected.error->code == ErrorCode::unsupported_capability &&
                  rejected.error->message == "Injected export validator rejected this fixture" &&
                  rejected.error->field == "test.reject-export" && calls == attempt + 1,
              "model.export did not invoke and preserve the injected validation failure");
        const auto after = good(app.snapshot(info.document));
        check(after.info.revision == snapshot.info.revision &&
                  after.info.content_state == snapshot.info.content_state &&
                  diff_record_views(snapshot.records, after.records).empty() &&
                  same_history(history, good(app.history(info.document))) &&
                  store->generation == generation && store->bytes() == rows,
              "Rejected validation changed model, history or persistent rows");
        check(task_accesses == 0 &&
                  good(app.owned_rows(info.document, StoreSpace::task_record, "qcae.runtime.task"))
                      .empty() &&
                  good(app.owned_rows(
                           info.document, StoreSpace::artifact_record, "qcae.nastran.artifact"))
                      .empty() &&
                  !std::filesystem::exists(directory / "export") &&
                  std::filesystem::is_empty(directory),
              "Rejected validation started a task or produced an artifact");
    }
}
} // namespace
int main() {
    const auto directory =
        std::filesystem::temp_directory_path() /
        ("qcae-validation-binding-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        check(std::filesystem::create_directory(directory), "Cannot create private test directory");
        retains_validation_state();
        invokes_registered_validation(directory);
        std::filesystem::remove_all(directory);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
