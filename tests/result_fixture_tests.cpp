#include "qcae/result_fixture.hpp"
#include "qcae/result_service.hpp"
#include "qcae/analysis_features.hpp"
#include "qcae/records.hpp"
#include <algorithm>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace qcae;
namespace analysis = qcae::features::analysis;
namespace results = qcae::features::results;
void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
template <class T> T good(Result<T> result) {
    if (!result.ok())
        throw std::runtime_error(result.error ? result.error->message : "Missing result");
    return std::move(*result.value);
}
EntityId node(unsigned index) {
    return EntityId("node-" + std::to_string(index));
}
const ProfileRef profile{"nastran-fixture", "1", "explicit-test-profile"};
const Caller caller{"result-fixture-tests"};
struct ModelFixture {
    RecordApplication app{[] {
        RecordApplicationOptions options;
        options.registry = make_record_registry();
        options.owned_row_handlers.push_back(results::result_row_handler());
        return options;
    }()};
    ModelFixture() {
        good(app.create_document(caller, "M", "create"));
        change("seed", [](EditSession& edit) {
            edit.put(records::Material{EntityId("material"), "Steel", 210000, .3});
            edit.put(records::BeamSection{EntityId("section"),
                                          "M section",
                                          EntityId("material"),
                                          100,
                                          833.333,
                                          833.333,
                                          1400});
            std::vector<EntityId> members;
            for (unsigned index = 0; index <= 10; ++index)
                edit.put(records::Node{node(index), {double(index * 100), 0, 0}, {}});
            for (unsigned index = 0; index < 10; ++index) {
                const EntityId id("beam-" + std::to_string(index));
                edit.put(records::Beam{
                    id, EntityId("section"), {node(index), node(index + 1)}, {0, 1, 0}, {}});
                members.push_back(id);
            }
            edit.put(records::NodalForce{EntityId("force"), node(10), {0, -1, 0}});
            edit.put(records::Constraint{EntityId("constraint"), {node(0)}, "123456"});
            edit.put(records::LoadCase{
                EntityId("case"), "LC1", {EntityId("force")}, {EntityId("constraint")}});
            edit.put(records::AnalysisDefinition{EntityId("analysis"),
                                                 "M static",
                                                 {profile, "linear_static"},
                                                 {},
                                                 {},
                                                 std::vector<EntityId>{EntityId("case")}});
            edit.put(records::Part{EntityId("part"), "Part", members});
            edit.put(records::Assembly{EntityId("assembly"), "Assembly", {EntityId("part")}});
            edit.put(records::EntitySet{EntityId("fixed"), "Fixed", {node(0)}});
            edit.put(records::EntitySet{EntityId("loaded"), "Loaded", {node(10)}});
        });
    }
    DocumentView view() {
        return good(app.snapshot(good(app.current_document()).document)).records;
    }
    template <class Edit> void change(std::string key, Edit action) {
        const auto info = good(app.current_document());
        good(app.execute(
            caller,
            {info.document, info.revision},
            "test.change",
            key,
            [action, key](const DocumentView& view, const RecordIdentityAllocator&) {
                EditSession edit(view);
                action(edit);
                return Result<RecordPreparedOperation>{
                    Status::success,
                    RecordPreparedOperation{
                        edit.prepare(), "Fixture edit", EntityId{}, key, 0, false},
                    {}};
            },
            key));
    }
    void undo(std::string key) {
        const auto info = good(app.current_document());
        good(app.undo(caller, {info.document, info.revision}, key));
    }
    std::vector<ExportIdentifier> mapping() {
        std::vector<ExportIdentifier> result;
        const auto snapshot = view();
        for (const auto& [type, name_space] :
             {std::pair{RecordTraits<records::Node>::type_id, "GRID"},
              std::pair{RecordTraits<records::Material>::type_id, "MAT1"},
              std::pair{RecordTraits<records::BeamSection>::type_id, "PBAR"},
              std::pair{RecordTraits<records::Beam>::type_id, "CBAR"},
              std::pair{RecordTraits<records::NodalForce>::type_id, "FORCE"},
              std::pair{RecordTraits<records::Constraint>::type_id, "SPC1"}}) {
            std::uint64_t number = 1000;
            snapshot.visit(type, [&](const Record& record) {
                result.push_back({EntityId(record->key().identity), name_space, ++number});
            });
        }
        std::reverse(result.begin(), result.end());
        return result;
    }
    analysis::FrozenAnalysisInput freeze() {
        return good(
            analysis::freeze_analysis_input(view(), EntityId("analysis"), profile, mapping()));
    }
};
std::uint64_t solver_number(const analysis::FrozenAnalysisInput& input, const EntityId& entity) {
    for (const auto& item : input.identities)
        if (item.entity == entity && item.name_space == "GRID")
            return item.number;
    throw std::runtime_error("Fixture node has no frozen GRID number");
}
results::ResultFixture fixture(const analysis::FrozenAnalysisInput& input) {
    return {
        analysis::physical_signature_hex(input),
        input.identities,
        "displacement",
        "mm",
        {"X", "Y", "Z"},
        "node",
        "global",
        "LC1",
        "fixture",
        0,
        {{solver_number(input, node(0)), {0, 0, 0}}, {solver_number(input, node(10)), {0, -1, 0}}}};
}
void read_mapping_and_metadata() {
    ModelFixture model;
    const auto frozen = model.freeze();
    const auto data = fixture(frozen);
    const auto result = good(results::FixtureResultReader{}.read(frozen, data));
    check(result.field.values.size() == 2 && result.field.values[0].entity == node(0) &&
              result.field.values[0].value == std::array<double, 3>{0, 0, 0} &&
              result.field.values[1].entity == node(10) &&
              result.field.values[1].value == std::array<double, 3>{0, -1, 0},
          "BP-19 R values map through frozen GRID numbers to exact M entities");
    check(result.field.quantity == "displacement" && result.field.unit == "mm" &&
              result.field.components == std::vector<std::string>{"X", "Y", "Z"} &&
              result.field.location == "node" && result.field.coordinate_basis == "global" &&
              result.field.case_label == "LC1" && result.field.frame == 0 &&
              result.source_kind == "fixture" &&
              result.reader_version == "qcae.fixture.displacement.v1",
          "BP-19 all required quantity/location/unit/case/source metadata remain explicit");
    check(results::result_state(result, model.view()) == results::ResultState::current,
          "R matches current physical input");
    ModelFixture separate;
    check(results::result_state(result, separate.view()) == results::ResultState::stale,
          "Identical physical values and IDs in a different document remain stale");
    const auto decoded_input =
        analysis::decode_frozen_analysis_input(analysis::encode_frozen_analysis_input(frozen));
    check(decoded_input.input_signature == frozen.input_signature &&
              analysis::physical_signature_hex(decoded_input) == data.input_fingerprint,
          "binary frozen input survives codec without UTF8 loss");
    const auto decoded_result =
        results::decode_result_bundle(results::encode_result_bundle(result));
    check(decoded_result.field.values[1].entity == node(10) &&
              decoded_result.source_kind == "fixture" &&
              decoded_result.input.input_signature == frozen.input_signature,
          "persistent result codec retains fixture source and original physical input");
    auto corrupted = frozen;
    corrupted.identities.front().entity = EntityId("different-model-entity");
    check(!results::FixtureResultReader{}.read(corrupted, fixture(corrupted)).ok(),
          "Frozen metadata map must cover the signature's exact physical entity IDs");
    auto remapped = frozen.identities;
    for (auto& identity : remapped)
        identity.number += 200;
    const auto another_map = good(
        analysis::freeze_analysis_input(model.view(), EntityId("analysis"), profile, remapped));
    check(another_map.input_signature == frozen.input_signature &&
              good(results::FixtureResultReader{}.read(another_map, fixture(another_map)))
                      .field.values[1]
                      .entity == node(10),
          "solver numbering permutation does not alter physical input or entity interpretation");
}
void negative_metadata_and_provenance() {
    ModelFixture model;
    const auto frozen = model.freeze();
    const auto original = fixture(frozen);
    for (unsigned repeat = 0; repeat < 10; ++repeat) {
        auto wrong = original;
        wrong.input_fingerprint[0] = wrong.input_fingerprint[0] == '0' ? '1' : '0';
        check(!results::FixtureResultReader{}.read(frozen, wrong).ok(),
              "R negative fingerprint rejected");
        wrong = original;
        wrong.location = "integration_point";
        check(!results::FixtureResultReader{}.read(frozen, wrong).ok(),
              "F22 different result location rejected");
        wrong = original;
        wrong.identities.front().number += 1;
        check(!results::FixtureResultReader{}.read(frozen, wrong).ok(),
              "F22 different frozen map rejected");
        wrong = original;
        wrong.source_kind = "external_solver";
        check(!results::FixtureResultReader{}.read(frozen, wrong).ok(),
              "F22 fixture cannot be relabeled as actual solver execution");
    }
    std::vector<results::ResultFixture> missing;
    auto value = original;
    value.unit.clear();
    missing.push_back(value);
    value = original;
    value.components.clear();
    missing.push_back(value);
    value = original;
    value.coordinate_basis.clear();
    missing.push_back(value);
    value = original;
    value.case_label.clear();
    missing.push_back(value);
    value = original;
    value.frame.reset();
    missing.push_back(value);
    value = original;
    value.values.clear();
    missing.push_back(value);
    value = original;
    value.values[0].solver_number = UINT64_MAX;
    missing.push_back(value);
    value = original;
    value.values.push_back(value.values[0]);
    missing.push_back(value);
    value = original;
    value.values[0].value[0] = std::numeric_limits<double>::infinity();
    missing.push_back(value);
    for (const auto& negative : missing)
        check(!results::FixtureResultReader{}.read(frozen, negative).ok(),
              "missing or invalid result metadata rejected");
}
void physical_staleness_and_organization() {
    ModelFixture model;
    const auto result =
        good(results::FixtureResultReader{}.read(model.freeze(), fixture(model.freeze())));
    model.change("organization", [](EditSession& edit) {
        edit.update<records::Part>(EntityId("part"), [](auto& value) {
            value.name = "Renamed";
            value.members.push_back(node(5));
        });
        edit.update<records::EntitySet>(EntityId("loaded"),
                                        [](auto& value) { value.members = {node(5), node(10)}; });
        edit.update<records::LoadCase>(EntityId("case"),
                                       [](auto& value) { value.name = "Renamed case"; });
        edit.update<records::AnalysisDefinition>(
            EntityId("analysis"), [](auto& value) { value.name = "Renamed analysis"; });
        edit.put(records::IncludeDocument{EntityId("include"), "main.bdf", {}, {node(0)}});
        edit.put(records::SourceIdentifier{EntityId("source"),
                                           node(0),
                                           "source-model",
                                           EntityId("include"),
                                           profile,
                                           "GRID",
                                           999});
    });
    check(results::result_state(result, model.view()) == results::ResultState::current,
          "organization, names and source numbering changes do not stale the result");
    model.change("node", [](EditSession& edit) {
        edit.update<records::Node>(node(10), [](auto& value) { value.position[0] = 1100; });
    });
    check(results::result_state(result, model.view()) == results::ResultState::stale,
          "BP-19 node physics change stales original R");
    model.undo("undo-node");
    check(results::result_state(result, model.view()) == results::ResultState::current,
          "undo restoring physical input permits R despite its different document revision");
    model.change("force", [](EditSession& edit) {
        edit.update<records::NodalForce>(EntityId("force"),
                                         [](auto& value) { value.force_n[1] = -2; });
    });
    check(results::result_state(result, model.view()) == results::ResultState::stale,
          "load physics change stales original R");
    model.undo("undo-force");
    model.change("material", [](EditSession& edit) {
        edit.update<records::Material>(EntityId("material"),
                                       [](auto& value) { value.young_modulus_mpa = 200000; });
    });
    check(results::result_state(result, model.view()) == results::ResultState::stale,
          "material physics change stales original R");
    model.undo("undo-material");
    model.change("missing-load", [](EditSession& edit) {
        edit.update<records::LoadCase>(EntityId("case"), [](auto& value) { value.forces.clear(); });
    });
    check(results::result_state(result, model.view()) == results::ResultState::stale,
          "incomplete analysis cannot claim an old result current");
}
void result_fact_and_replay() {
    ModelFixture model;
    const auto input = model.freeze();
    const auto data = fixture(input);
    const auto before = good(model.app.current_document());
    const auto history = good(model.app.history(before.document));
    unsigned resolutions{};
    results::FixtureResultService service(
        model.app,
        [&](const Caller& actor, const DocumentRef& document, std::string_view artifact) {
            ++resolutions;
            if (actor.principal != caller.principal || document.id != before.document.id ||
                document.epoch != before.document.epoch || artifact != "published-M")
                return Result<analysis::FrozenAnalysisInput>{
                    Status::failed,
                    {},
                    Diagnostic{ErrorCode::entity_not_found, "Published input is missing", {}}};
            return Result<analysis::FrozenAnalysisInput>{Status::success, input, {}};
        });
    const WriteContext context{before.document, before.revision};
    const auto retained = good(service.read(caller, context, "published-M", data, "read-R"));
    check(good(model.app.current_document()).revision == before.revision &&
              good(model.app.history(before.document)).items.size() == history.items.size() &&
              good(model.app.history(before.document)).cursor == history.cursor,
          "Result auxiliary persistence leaves document revision and history unchanged");
    const auto rows = good(
        model.app.owned_rows(before.document, StoreSpace::artifact_record, "qcae.results.fixture"));
    check(rows.size() == 1, "One immutable result fact is persisted");
    results::result_row_handler().validate(*rows.front());
    check(good(service.read(caller, context, "published-M", data, "read-R")).id == retained.id &&
              resolutions == 1,
          "Exact result retry returns retained fact without re-resolving external input");
    auto changed = data;
    changed.values.back().value[1] = -2;
    const auto conflict = service.read(caller, context, "published-M", changed, "read-R");
    check(!conflict.ok() && conflict.error->code == ErrorCode::idempotency_key_conflict,
          "Same result key cannot replace accepted values");
    const auto other = service.get(Caller{"other"}, before.document, retained.id);
    check(!other.ok() && other.error->code == ErrorCode::entity_not_found,
          "Result facts are caller scoped");
    check(!service.read(Caller{"other"}, context, "published-M", data, "foreign-artifact").ok(),
          "Result reads carry caller identity through the published input resolver");
    changed = data;
    changed.location = "integration_point";
    check(!service.read(caller, context, "published-M", changed, "bad-location").ok() &&
              !service.read(caller, context, "missing", data, "missing-artifact").ok() &&
              good(model.app.owned_rows(
                       before.document, StoreSpace::artifact_record, "qcae.results.fixture"))
                      .size() == 1,
          "Rejected fixture or unresolved input cannot publish a result fact");
    model.change("change-physics", [](EditSession& edit) {
        edit.update<records::Node>(node(10), [](auto& value) { value.position[0] += 1; });
    });
    const auto current = good(model.app.current_document());
    const auto old = good(service.get(caller, current.document, retained.id));
    check(old.bundle.input.version.revision == input.version.revision &&
              old.bundle.input.version.document.epoch == input.version.document.epoch &&
              results::result_state(old.bundle, model.view()) == results::ResultState::stale,
          "Physical edits retain original result provenance and mark it stale");
    check(good(service.read(caller, context, "published-M", data, "read-R")).id == retained.id,
          "Exact retry preserves original fact after document advances");
    const auto late = service.read(caller, context, "published-M", data, "late-new-key");
    check(!late.ok() && late.error->code == ErrorCode::revision_conflict,
          "New result writes reject stale request revisions");
}
} // namespace
int main() {
    try {
        read_mapping_and_metadata();
        negative_metadata_and_provenance();
        physical_staleness_and_organization();
        result_fact_and_replay();
        std::cout << "PASS: BP-19 R fixture values/provenance and F22 rejection; physical-only "
                     "stale state\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
