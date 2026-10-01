#include "qcae/analysis_features.hpp"
#include "qcae/core.hpp"
#include "qcae/quantities.hpp"
#include "qcae/records_model_bridge.hpp"
#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace qcae;
using namespace qcae::operations;
namespace analysis_features = qcae::features::analysis;
void check(bool condition, const char* message) {
    if (!condition)
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
analysis_features::CheckReport conclusion(Result<analysis_features::CheckReport> result,
                                          Status status) {
    check(result.value.has_value() && result.status == status && result.value->outcome == status,
          "scenario conclusion preserves its report and explicit outcome");
    check(status == Status::success || (result.error && !result.error->field.empty()),
          "non-success conclusion retains its structured error");
    return std::move(*result.value);
}
const ProfileRef profile{"nastran-test", "1", "analysis-check-test-v1"};
const Caller caller{"analysis-tests"};
WriteContext at(const DocumentInfo& info) {
    return {info.document, info.revision};
}
struct Fixture {
    MemoryApplication compatible{{},
                                 {},
                                 [](const auto& value) { return value == profile; },
                                 {analysis_features::check_row_handler()}};
    RecordApplication& app = compatible.record_application();
    OperationRegistry registry;
    explicit Fixture(bool seed = true) {
        good(analysis_features::register_handlers(registry, app));
        const auto document = good(app.create_document(caller, "Analysis", "create"));
        if (!seed)
            return;
        good(app.execute(
            caller,
            at(document),
            "test.seed",
            "seed-node",
            [](const DocumentView& view, const RecordIdentityAllocator&) {
                EditSession edit(view);
                edit.put(records::Node{EntityId("node"), {0, 0, 0}, {}});
                edit.put(records::Node{EntityId("middle"), {500, 0, 0}, {}});
                edit.put(records::Node{EntityId("tip"), {1000, 0, 0}, {}});
                edit.put(records::Material{EntityId("material"), "Steel", 210000, .3});
                edit.put(records::BeamSection{EntityId("section"),
                                              "Section",
                                              EntityId("material"),
                                              100,
                                              833.333,
                                              1666.666,
                                              1400});
                edit.put(records::Beam{EntityId("beam-a"),
                                       EntityId("section"),
                                       {EntityId("node"), EntityId("middle")},
                                       {0, 1, 0},
                                       {}});
                edit.put(records::Beam{EntityId("beam-b"),
                                       EntityId("section"),
                                       {EntityId("middle"), EntityId("tip")},
                                       {0, 1, 0},
                                       {}});
                return Result<RecordPreparedOperation>{
                    Status::success,
                    RecordPreparedOperation{
                        edit.prepare(), "Seed node", EntityId("node"), "seed-node", 0, true},
                    {}};
            },
            "seed"));
    }
    OperationContext context(std::string key) {
        const auto info = good(app.current_document());
        return {caller, info.document, info.revision, std::move(key), profile, "test", 1};
    }
    DocumentView view() {
        return good(app.snapshot(good(app.current_document()).document)).records;
    }
    template <class Input> Result<Value> invoke(const Input& input, const OperationContext& ctx) {
        return registry.invoke(
            InputTraits<Input>::operation_id, ctx, InputTraits<Input>::to_value(input));
    }
    template <class Input> EntityId create(const Input& input, std::string key) {
        const auto result = good(invoke(input, context(std::move(key))));
        return EntityId(
            std::get<std::string>(std::get<Value::Object>(result.data).at("entity_id").data));
    }
    template <class Action> void change(std::string key, Action action) {
        const auto info = good(app.current_document());
        good(app.execute(
            caller,
            at(info),
            "test.change",
            key,
            [action, key](const DocumentView& view, const RecordIdentityAllocator&) {
                EditSession edit(view);
                action(edit, view);
                return Result<RecordPreparedOperation>{
                    Status::success,
                    RecordPreparedOperation{
                        edit.prepare(), "Scenario fixture", EntityId{}, key, 0, false},
                    {}};
            },
            key));
    }
};
struct Case {
    EntityId force, constraint, load_case, analysis;
};
Case create_case(Fixture& fixture) {
    Case result;
    result.force =
        fixture.create(ForceCreateInput{EntityId("tip"), {0, "N"}, {0, "N"}, {-1, "kN"}}, "force");
    result.constraint =
        fixture.create(ConstraintCreateInput{{EntityId("node")}, "123456"}, "constraint");
    result.load_case = fixture.create(
        LoadCaseCreateInput{"Cantilever", {result.force}, {result.constraint}}, "load-case");
    result.analysis = fixture.create(AnalysisCreateInput{"Static", {result.load_case}}, "analysis");
    return result;
}
void references_and_rule_lifecycle() {
    Fixture fixture;
    const auto data = create_case(fixture);
    auto view = fixture.view();
    const auto& analysis =
        view.find<records::AnalysisDefinition>(data.analysis)->get<records::AnalysisDefinition>();
    check(analysis.load_cases == std::vector<EntityId>{data.load_case} && analysis.forces.empty() &&
              analysis.constraints.empty(),
          "BP-11 analysis references one authoritative load case");
    const auto& load_case = view.find<records::LoadCase>(data.load_case)->get<records::LoadCase>();
    check(load_case.forces == std::vector<EntityId>{data.force} &&
              load_case.constraints == std::vector<EntityId>{data.constraint},
          "BP-11 load case references exact force and constraint");
    check(view.find<records::NodalForce>(data.force)->get<records::NodalForce>().force_n[2] ==
              -1000,
          "force components reuse shared signed kN normalization");
    analysis_features::CheckService checks(fixture.app);
    const auto initial = good(fixture.app.current_document());
    const auto ready = good(checks.run(caller, at(initial), data.analysis, "initial-check"));
    check(ready.issues.empty() &&
              analysis_features::issue_state(ready, view) == analysis_features::IssueState::current,
          "initial complete analysis has no completeness issues");
    check(good(fixture.app.current_document()).revision == initial.revision &&
              good(fixture.app.history(initial.document)).items.size() == 5,
          "auxiliary check does not create a model revision or undo entry");
    const auto deletion =
        fixture.compatible.preview_edit(caller, at(initial), DeleteEntity{data.force});
    bad(deletion, ErrorCode::invalid_input, "BP-11 deleting a referenced force must be rejected");
    check(good(fixture.app.current_document()).revision == initial.revision,
          "failed deletion leaves authority unchanged");
    auto mixed = analysis;
    mixed.forces = {data.force};
    EditSession invalid(view);
    invalid.put(mixed);
    try {
        invalid.prepare();
        throw std::runtime_error("mixed analysis reference paths were accepted");
    } catch (const RecordError& error) {
        check(error.code() == ErrorCode::invalid_input,
              "mixed paths return structured input error");
    }
    good(fixture.invoke(LoadCaseSetReferencesInput{data.load_case, {}, {data.constraint}},
                        fixture.context("remove-load")));
    const auto missing_input = good(fixture.app.current_document());
    const auto missing = conclusion(
        checks.run(caller, at(missing_input), data.analysis, "missing-check"), Status::needs_input);
    check(missing.issues.size() == 1 && missing.issues[0].rule_id == "missing-load" &&
              missing.issues[0].rule_version == 1 && missing.issues[0].severity == "error" &&
              missing.issues[0].entity ==
                  RecordKey{RecordTraits<records::AnalysisDefinition>::type_id,
                            data.analysis.value} &&
              same_record_version(missing.issues[0].input, fixture.view().version()),
          "BP-14 exactly one missing-load issue locates the frozen analysis");
    good(fixture.invoke(LoadCaseSetReferencesInput{data.load_case, {data.force}, {data.constraint}},
                        fixture.context("repair")));
    const auto repaired = good(fixture.app.current_document());
    const auto obsolete = good(checks.get(caller, repaired.document, missing.id));
    check(obsolete.issues.size() == 1 && analysis_features::issue_state(obsolete, fixture.view()) ==
                                             analysis_features::IssueState::stale,
          "BP-14 old issue is stale after transaction repair");
    const auto rechecked = good(checks.run(caller, at(repaired), data.analysis, "recheck"));
    check(rechecked.issues.empty(), "BP-14 repaired missing-load rule yields zero issues");
    good(fixture.app.undo(caller, at(repaired), "undo-repair"));
    const auto undone = good(fixture.app.current_document());
    check(
        conclusion(checks.run(caller, at(undone), data.analysis, "undo-check"), Status::needs_input)
                .issues.size() == 1,
        "undo restores incomplete case through the same history");
    good(fixture.app.redo(caller, at(undone), "redo-repair"));
    const auto redone = good(fixture.app.current_document());
    check(good(checks.run(caller, at(redone), data.analysis, "redo-check")).issues.empty(),
          "redo restores repaired case through the same history");
    const auto frozen_replay = conclusion(
        checks.run(caller, at(missing_input), data.analysis, "missing-check"), Status::needs_input);
    check(frozen_replay.id == missing.id && frozen_replay.issues.size() == 1 &&
              analysis_features::issue_state(frozen_replay, fixture.view()) ==
                  analysis_features::IssueState::stale,
          "idempotent old check returns its original stale input fact");
    bad(checks.run(caller, at(redone), data.analysis, "missing-check"),
        ErrorCode::idempotency_key_conflict,
        "same check key with different input cannot replace retained report");
    bad(checks.run(caller, at(missing_input), data.analysis, "fresh-stale"),
        ErrorCode::revision_conflict,
        "new check refuses a stale input revision");
    bad(checks.get(Caller{"another-caller"}, redone.document, missing.id),
        ErrorCode::entity_not_found,
        "check report lookup is scoped to its actor");
    bad(checks.run(caller, at(redone), EntityId("node"), "not-analysis"),
        ErrorCode::entity_not_found,
        "analysis record identity cannot be replaced with another entity kind or task ID");
    const auto encoded = analysis_features::encode_check_report(missing);
    const auto decoded = analysis_features::decode_check_report(encoded);
    check(decoded.id == missing.id && decoded.issues.size() == 1 &&
              same_record_version(decoded.input, missing.input),
          "report codec preserves entity/rule/input version");
    try {
        (void)analysis_features::decode_check_report(encoded + "x");
        throw std::runtime_error("corrupt check report accepted");
    } catch (const RecordError&) {
    }
    const auto prepared = good(
        fixture.compatible.preview_edit(caller, at(redone), MoveNode{EntityId("node"), {1, 2, 3}}));
    good(fixture.compatible.commit(caller, at(redone), prepared.id, "legacy-move"));
    check(fixture.view()
                  .find<records::AnalysisDefinition>(data.analysis)
                  ->get<records::AnalysisDefinition>()
                  .load_cases == std::vector<EntityId>{data.load_case},
          "legacy record-level edits retain authoritative load case references");
    const auto projected = model_from_records(fixture.view());
    check(projected.analyses.front().forces == std::vector<EntityId>{data.force} &&
              projected.analyses.front().constraints == std::vector<EntityId>{data.constraint},
          "legacy Nastran/model projection resolves the controlled single load case");
}
void force_vector_shared_transaction() {
    Fixture fixture;
    const auto data = create_case(fixture);
    const auto original = fixture.view();
    const auto before = good(fixture.app.current_document());
    const auto history_size = good(fixture.app.history(before.document)).items.size();
    const auto old_force =
        original.find<records::NodalForce>(data.force)->get<records::NodalForce>();
    analysis_features::CheckService checks(fixture.app);
    const auto old_check = good(checks.run(caller, at(before), data.analysis, "before-force-edit"));
    const auto context = fixture.context("set-force-vector");
    const ForceSetVectorInput edit{data.force, {0, "N"}, {-2, "kN"}, {0, "N"}};
    const auto receipt = good(fixture.invoke(edit, context));
    const auto changed = good(fixture.app.current_document());
    const auto& force =
        fixture.view().find<records::NodalForce>(data.force)->get<records::NodalForce>();
    check(force.id == old_force.id && force.node == old_force.node &&
              force.force_n == std::array<double, 3>{0, -2000, 0},
          "typed force edit normalizes signed units and preserves physical identity/reference");
    check(changed.revision == before.revision + 1 &&
              good(fixture.app.history(changed.document)).items.size() == history_size + 1,
          "force vector edit is exactly one shared transaction/history entry");
    check(analysis_features::issue_state(old_check, fixture.view()) ==
              analysis_features::IssueState::stale,
          "force change makes the frozen scenario conclusion stale");
    const auto replay = good(fixture.invoke(edit, context));
    const auto same_transaction = [&](const Value& value) {
        const auto& fields = std::get<Value::Object>(value.data);
        const auto& first = std::get<Value::Object>(receipt.data);
        return fields.at("transaction_id") == first.at("transaction_id") &&
               fields.at("committed_revision") == first.at("committed_revision") &&
               fields.at("entity_id") == first.at("entity_id") &&
               std::get<bool>(fields.at("replayed").data);
    };
    check(same_transaction(replay) &&
              good(fixture.app.current_document()).revision == changed.revision,
          "same context/key force retry returns original receipt without another write");
    auto different = edit;
    different.y.value = -3;
    bad(fixture.invoke(different, context),
        ErrorCode::idempotency_key_conflict,
        "same force key with different vector cannot replace the original transaction");
    auto stale = context;
    stale.idempotency_key = "fresh-key-stale-force";
    bad(fixture.invoke(edit, stale),
        ErrorCode::revision_conflict,
        "fresh force write with old revision cannot submit");
    for (const auto& target : {EntityId("absent-force"), EntityId("material")}) {
        auto wrong = edit;
        wrong.force_id = target;
        bad(fixture.invoke(wrong, fixture.context("wrong-" + target.value)),
            ErrorCode::entity_not_found,
            "force setter refuses missing/wrong-kind identities");
    }
    for (const auto unit : {"", "mm"}) {
        auto wrong = edit;
        wrong.y.unit = unit;
        bad(fixture.invoke(wrong, fixture.context("bad-unit-" + std::string(unit))),
            std::string_view(unit).empty() ? ErrorCode::missing_input : ErrorCode::invalid_unit,
            "force setter requires an explicit force unit");
    }
    auto nonfinite = edit;
    nonfinite.y.value = std::numeric_limits<double>::infinity();
    bad(fixture.invoke(nonfinite, fixture.context("nonfinite-force")),
        ErrorCode::invalid_input,
        "force setter refuses nonfinite physical values");
    check(good(fixture.app.current_document()).revision == changed.revision &&
              good(fixture.app.history(changed.document)).items.size() == history_size + 1,
          "invalid force edits leave revision and shared history unchanged");
    good(fixture.app.undo(caller, at(changed), "undo-force-vector"));
    check(
        fixture.view().find<records::NodalForce>(data.force)->get<records::NodalForce>().force_n ==
            old_force.force_n,
        "one undo restores the complete original force vector");
    const auto undone = good(fixture.app.current_document());
    check(same_transaction(good(fixture.invoke(edit, context))) &&
              good(fixture.app.current_document()).revision == undone.revision &&
              fixture.view()
                      .find<records::NodalForce>(data.force)
                      ->get<records::NodalForce>()
                      .force_n == old_force.force_n,
          "retry after undo returns original fact without reapplying force edit");
    good(fixture.app.redo(caller, at(undone), "redo-force-vector"));
    check(
        fixture.view().find<records::NodalForce>(data.force)->get<records::NodalForce>().force_n ==
            std::array<double, 3>{0, -2000, 0},
        "redo restores the same force vector");
    check(fixture.view().find<records::LoadCase>(data.load_case)->get<records::LoadCase>().forces ==
                  std::vector<EntityId>{data.force} &&
              fixture.view()
                      .find<records::Material>(EntityId("material"))
                      ->get<records::Material>()
                      .young_modulus_mpa == 210000,
          "force edit never changes its load-case references or unrelated material");
    const ForcePreviewVectorInput preview_input{data.force, {0, "N"}, {-3, "N"}, {0, "N"}};
    const auto preview_context = fixture.context("");
    const auto preview = good(fixture.invoke(preview_input, preview_context));
    const auto& preview_fields = std::get<Value::Object>(preview.data);
    const PreviewId preview_id(std::get<std::string>(preview_fields.at("preview_id").data));
    const auto before_preview = good(fixture.app.current_document());
    const auto preview_history = good(fixture.app.history(before_preview.document)).items.size();
    check(before_preview.revision == *preview_context.expected_revision &&
              fixture.view()
                      .find<records::NodalForce>(data.force)
                      ->get<records::NodalForce>()
                      .force_n == std::array<double, 3>{0, -2000, 0} &&
              !std::get<bool>(preview_fields.at("creates_entity").data),
          "force preview reports the existing identity without changing physical state");
    good(fixture.invoke(ForceSetVectorInput{data.force, {0, "N"}, {-4, "N"}, {0, "N"}},
                        fixture.context("interleaved-force-write")));
    bad(fixture.app.commit(caller,
                           {*preview_context.document, *preview_context.expected_revision},
                           preview_id,
                           "stale-force-preview"),
        ErrorCode::revision_conflict,
        "interleaved write invalidates an old force preview");
    const auto fresh_context = fixture.context("");
    const auto fresh_preview = good(fixture.invoke(preview_input, fresh_context));
    const PreviewId fresh_id(
        std::get<std::string>(std::get<Value::Object>(fresh_preview.data).at("preview_id").data));
    const auto committed =
        good(fixture.compatible.commit(caller,
                                       {*fresh_context.document, *fresh_context.expected_revision},
                                       fresh_id,
                                       "shared-force-preview-commit"));
    check(committed.primary_entity == data.force &&
              fixture.view()
                      .find<records::NodalForce>(data.force)
                      ->get<records::NodalForce>()
                      .force_n == std::array<double, 3>{0, -3, 0} &&
              good(fixture.app.history(before_preview.document)).items.size() ==
                  preview_history + 2,
          "typed force preview commits through the same compatibility/shared history port");
    const auto after_preview_commit = good(fixture.app.current_document());
    good(fixture.app.undo(caller, at(after_preview_commit), "undo-shared-force-preview"));
    check(
        fixture.view().find<records::NodalForce>(data.force)->get<records::NodalForce>().force_n ==
            std::array<double, 3>{0, -4, 0},
        "one shared undo restores the interleaved force vector");
}
void input_and_historical_compatibility() {
    Fixture fixture;
    const Value empty_ids(Value::Array{});
    bad(wire::entity_id_array(empty_ids, "ids"),
        ErrorCode::invalid_input,
        "existing entity array contracts remain nonempty by default");
    check(good(wire::entity_id_array(empty_ids, "ids", true)).empty(),
          "explicit schema flag permits a complete empty reference list");
    bad(wire::entity_id_array(Value(true), "ids", true),
        ErrorCode::invalid_input,
        "allow-empty arrays still reject scalar wire values");
    bad(wire::entity_id_array(
            wire::to_value(std::vector<EntityId>{EntityId("a"), EntityId("a")}), "ids", true),
        ErrorCode::invalid_input,
        "allow-empty arrays retain duplicate reference rejection");
    check(InputTraits<LoadCaseSetReferencesInput>::definition().fields[1].allow_empty &&
              !InputTraits<ConstraintCreateInput>::definition().fields[0].allow_empty,
          "capability metadata distinguishes incomplete-case references from required nodes");
    const auto data = create_case(fixture);
    const auto before = good(fixture.app.current_document());
    bad(fixture.invoke(LoadCaseSetReferencesInput{data.load_case,
                                                  {EntityId("missing-force")},
                                                  {data.constraint}},
                       fixture.context("dangling")),
        ErrorCode::invalid_input,
        "dangling load reference is a commit invariant");
    bad(fixture.invoke(ConstraintCreateInput{{EntityId("node")}, "11"},
                       fixture.context("duplicate-dof")),
        ErrorCode::invalid_input,
        "constraint DOF rule is reused");
    bad(fixture.invoke(AnalysisCreateInput{"Too many cases", {data.load_case, data.load_case}},
                       fixture.context("duplicate-case")),
        ErrorCode::invalid_input,
        "duplicate case references are rejected");
    bad(analysis_features::prepare_create_force({EntityId("node"), {1, "mm"}, {0, "N"}, {0, "N"}}),
        ErrorCode::invalid_unit,
        "force dimension check reuses parameter module");
    check(good(fixture.app.current_document()).revision == before.revision,
          "input failures leave one engine revision unchanged");
    auto context = fixture.context("equivalent-force");
    const ForceCreateInput force{EntityId("node"), {1, "kN"}, {0, "N"}, {0, "N"}};
    const auto first = good(fixture.invoke(force, context));
    const auto replay = good(fixture.invoke(
        ForceCreateInput{EntityId("node"), {1000, "N"}, {0, "N"}, {0, "N"}}, context));
    check(std::get<Value::Object>(first.data).at("entity_id") ==
                  std::get<Value::Object>(replay.data).at("entity_id") &&
              std::get<bool>(std::get<Value::Object>(replay.data).at("replayed").data),
          "equivalent force units share one normalized transaction fact");
    const auto registry = fixture.view().registry();
    const auto old = registry->make(records::AnalysisDefinition{EntityId("old-analysis"),
                                                                "Legacy",
                                                                {profile, "linear_static"},
                                                                {data.force},
                                                                {data.constraint}});
    auto input = record_wire::decode(old->encoded());
    input.version = 1;
    const auto decoded = registry->from_input(input);
    check(decoded->version() == 2 && !decoded->get<records::AnalysisDefinition>().load_cases &&
              decoded->get<records::AnalysisDefinition>().forces ==
                  std::vector<EntityId>{data.force},
          "v1 analysis decodes its historical direct references without inventing cases");
    EditSession edit(fixture.view());
    edit.put(decoded);
    const auto prepared = edit.prepare();
    analysis_features::RuleCatalog rules;
    check(good(rules.check(prepared.candidate, EntityId("old-analysis"))).issues.empty(),
          "v1 analysis completeness uses its sole historical reference path");
    auto illegal_old = record_wire::decode(
        fixture.view().find<records::AnalysisDefinition>(data.analysis)->encoded());
    illegal_old.version = 1;
    try {
        registry->from_input(illegal_old);
        throw std::runtime_error("v1 record accepted a field introduced in v2");
    } catch (const RecordError& error) {
        check(error.code() == ErrorCode::invalid_input,
              "field introduction version rejects incompatible old encoding");
    }
    const auto incomplete = fixture.create(AnalysisCreateInput{"Empty", {}}, "incomplete");
    check(conclusion(rules.check(fixture.view(), incomplete), Status::needs_input).issues.size() ==
              2,
          "semantic incompleteness is allowed for the diagnostic workflow");
}
template <class T, class Action>
void modify(EditSession& edit, const DocumentView& view, std::string_view id, Action action) {
    auto value = view.find<T>(EntityId(std::string(id)))->template get<T>();
    action(value);
    edit.put(value);
}
struct Scenario {
    const char* name;
    Status expected;
    const char* rule;
    std::function<void(EditSession&, const DocumentView&, const Case&)> change;
};
void cantilever_scenarios() {
    const std::vector<Scenario> scenarios{
        {"unassigned-section",
         Status::needs_input,
         "missing-section",
         [](auto& edit, const auto& view, const auto&) {
             modify<records::Beam>(edit, view, "beam-b", [](auto& beam) { beam.section.reset(); });
         }},
        {"missing-poisson-ratio",
         Status::needs_input,
         "missing-material",
         [](auto& edit, const auto& view, const auto&) {
             modify<records::Material>(
                 edit, view, "material", [](auto& material) { material.poisson_ratio.reset(); });
         }},
        {"no-beams",
         Status::needs_input,
         "missing-beam",
         [](auto& edit, const auto&, const auto&) {
             edit.erase({RecordTraits<records::Beam>::type_id, "beam-a"});
             edit.erase({RecordTraits<records::Beam>::type_id, "beam-b"});
         }},
        {"partial-fixed-end",
         Status::needs_input,
         "missing-fixed-dofs",
         [](auto& edit, const auto& view, const auto& data) {
             modify<records::Constraint>(
                 edit, view, data.constraint.value, [](auto& support) { support.dofs = "123"; });
         }},
        {"load-at-support",
         Status::failed,
         "cantilever-tip-load",
         [](auto& edit, const auto& view, const auto& data) {
             modify<records::NodalForce>(
                 edit, view, data.force.value, [](auto& force) { force.node = EntityId("node"); });
         }},
        {"load-at-interior",
         Status::failed,
         "cantilever-tip-load",
         [](auto& edit, const auto& view, const auto& data) {
             modify<records::NodalForce>(edit, view, data.force.value, [](auto& force) {
                 force.node = EntityId("middle");
             });
         }},
        {"fixed-interior",
         Status::failed,
         "cantilever-fixed-end",
         [](auto& edit, const auto& view, const auto& data) {
             modify<records::Constraint>(edit, view, data.constraint.value, [](auto& support) {
                 support.nodes = {EntityId("middle")};
             });
         }},
        {"both-ends-fixed",
         Status::failed,
         "cantilever-fixed-end",
         [](auto& edit, const auto& view, const auto& data) {
             modify<records::Constraint>(edit, view, data.constraint.value, [](auto& support) {
                 support.nodes = {EntityId("node"), EntityId("tip")};
             });
         }},
        {"zero-force",
         Status::failed,
         "cantilever-transverse-load",
         [](auto& edit, const auto& view, const auto& data) {
             modify<records::NodalForce>(
                 edit, view, data.force.value, [](auto& force) { force.force_n = {}; });
         }},
        {"axial-force",
         Status::failed,
         "cantilever-transverse-load",
         [](auto& edit, const auto& view, const auto& data) {
             modify<records::NodalForce>(
                 edit, view, data.force.value, [](auto& force) { force.force_n = {1, 0, 0}; });
         }},
        {"mixed-axial-force",
         Status::failed,
         "cantilever-transverse-load",
         [](auto& edit, const auto& view, const auto& data) {
             modify<records::NodalForce>(
                 edit, view, data.force.value, [](auto& force) { force.force_n = {.01, 1, 0}; });
         }},
        {"multiple-forces",
         Status::failed,
         "cantilever-single-load",
         [](auto& edit, const auto& view, const auto& data) {
             edit.put(records::NodalForce{EntityId("extra-force"), EntityId("tip"), {0, 1, 0}});
             modify<records::LoadCase>(edit, view, data.load_case.value, [](auto& load_case) {
                 load_case.forces.push_back(EntityId("extra-force"));
             });
         }},
        {"multiple-constraints",
         Status::failed,
         "cantilever-single-constraint",
         [](auto& edit, const auto& view, const auto& data) {
             edit.put(records::Constraint{EntityId("extra-constraint"), {EntityId("node")}, "4"});
             modify<records::LoadCase>(edit, view, data.load_case.value, [](auto& load_case) {
                 load_case.constraints.push_back(EntityId("extra-constraint"));
             });
         }},
        {"orphan-node",
         Status::failed,
         "cantilever-connectivity",
         [](auto& edit, const auto&, const auto&) {
             edit.put(records::Node{EntityId("orphan"), {42, 8, 3}, {}});
         }},
        {"branch",
         Status::failed,
         "cantilever-connectivity",
         [](auto& edit, const auto&, const auto&) {
             edit.put(records::Node{EntityId("branch-node"), {500, 200, 0}, {}});
             edit.put(records::Beam{EntityId("branch-beam"),
                                    EntityId("section"),
                                    {EntityId("middle"), EntityId("branch-node")},
                                    {0, 0, 1},
                                    {}});
         }},
        {"disconnected-chain",
         Status::failed,
         "cantilever-connectivity",
         [](auto& edit, const auto&, const auto&) {
             edit.put(records::Node{EntityId("other-root"), {0, 200, 0}, {}});
             edit.put(records::Node{EntityId("other-tip"), {100, 200, 0}, {}});
             edit.put(records::Beam{EntityId("other-beam"),
                                    EntityId("section"),
                                    {EntityId("other-root"), EntityId("other-tip")},
                                    {0, 1, 0},
                                    {}});
         }},
        {"closed-cycle",
         Status::failed,
         "cantilever-connectivity",
         [](auto& edit, const auto&, const auto&) {
             edit.put(records::Beam{EntityId("closing-beam"),
                                    EntityId("section"),
                                    {EntityId("tip"), EntityId("node")},
                                    {0, 1, 0},
                                    {}});
         }},
        {"duplicate-edge",
         Status::failed,
         "cantilever-connectivity",
         [](auto& edit, const auto&, const auto&) {
             edit.put(records::Beam{EntityId("duplicate-beam"),
                                    EntityId("section"),
                                    {EntityId("middle"), EntityId("node")},
                                    {0, 1, 0},
                                    {}});
         }},
        {"bent-beam",
         Status::failed,
         "cantilever-straight",
         [](auto& edit, const auto& view, const auto&) {
             modify<records::Node>(edit, view, "middle", [](auto& node) { node.position[1] = 20; });
         }},
        {"backtracking-chain",
         Status::failed,
         "cantilever-straight",
         [](auto& edit, const auto& view, const auto&) {
             modify<records::Node>(
                 edit, view, "middle", [](auto& node) { node.position[0] = 1200; });
         }},
        {"varying-section",
         Status::failed,
         "cantilever-uniform-section",
         [](auto& edit, const auto& view, const auto&) {
             auto section = view.template find<records::BeamSection>(EntityId("section"))
                                ->template get<records::BeamSection>();
             section.id = EntityId("other-section");
             section.i1_mm4 *= 2;
             edit.put(section);
             modify<records::Beam>(edit, view, "beam-b", [](auto& beam) {
                 beam.section = EntityId("other-section");
             });
         }},
        {"varying-material",
         Status::failed,
         "cantilever-uniform-section",
         [](auto& edit, const auto& view, const auto&) {
             edit.put(records::Material{EntityId("other-material"), "Different E", 200000, .3});
             auto section = view.template find<records::BeamSection>(EntityId("section"))
                                ->template get<records::BeamSection>();
             section.id = EntityId("other-section");
             section.material = EntityId("other-material");
             edit.put(section);
             modify<records::Beam>(edit, view, "beam-b", [](auto& beam) {
                 beam.section = EntityId("other-section");
             });
         }},
        {"varying-orientation",
         Status::failed,
         "cantilever-orientation",
         [](auto& edit, const auto& view, const auto&) {
             modify<records::Beam>(
                 edit, view, "beam-b", [](auto& beam) { beam.orientation = {0, 0, 1}; });
         }},
        {"reversed-support-and-tip",
         Status::success,
         "",
         [](auto& edit, const auto& view, const auto& data) {
             modify<records::NodalForce>(
                 edit, view, data.force.value, [](auto& force) { force.node = EntityId("node"); });
             modify<records::Constraint>(edit, view, data.constraint.value, [](auto& support) {
                 support.nodes = {EntityId("tip")};
                 support.dofs = "654321";
             });
         }},
        {"nonuniform-discretization",
         Status::success,
         "",
         [](auto& edit, const auto& view, const auto&) {
             modify<records::Node>(edit, view, "middle", [](auto& node) { node.position[0] = 73; });
         }},
        {"single-beam",
         Status::success,
         "",
         [](auto& edit, const auto& view, const auto& data) {
             edit.erase({RecordTraits<records::Beam>::type_id, "beam-b"});
             edit.erase({RecordTraits<records::Node>::type_id, "tip"});
             modify<records::NodalForce>(edit, view, data.force.value, [](auto& force) {
                 force.node = EntityId("middle");
             });
         }},
        {"twenty-segment-chain",
         Status::success,
         "",
         [](auto& edit, const auto&, const auto&) {
             edit.erase({RecordTraits<records::Beam>::type_id, "beam-a"});
             edit.erase({RecordTraits<records::Beam>::type_id, "beam-b"});
             auto node_id = [](unsigned index) {
                 return index == 0    ? EntityId("node")
                        : index == 10 ? EntityId("middle")
                        : index == 20 ? EntityId("tip")
                                      : EntityId("node-" + std::to_string(index));
             };
             for (unsigned index = 1; index < 20; ++index)
                 if (index != 10)
                     edit.put(records::Node{node_id(index), {double(index * 50), 0, 0}, {}});
             for (unsigned index = 0; index < 20; ++index)
                 edit.put(records::Beam{EntityId("beam-" + std::to_string(index)),
                                        EntityId("section"),
                                        {node_id(index), node_id(index + 1)},
                                        {0, 1, 0},
                                        {}});
         }},
        {"equivalent-property-identities",
         Status::success,
         "",
         [](auto& edit, const auto& view, const auto&) {
             edit.put(records::Material{EntityId("equivalent-material"), "Renamed", 210000, .3});
             auto section = view.template find<records::BeamSection>(EntityId("section"))
                                ->template get<records::BeamSection>();
             section.id = EntityId("equivalent-section");
             section.name = "Equivalent";
             section.material = EntityId("equivalent-material");
             edit.put(section);
             modify<records::Beam>(edit, view, "beam-b", [](auto& beam) {
                 beam.section = EntityId("equivalent-section");
                 beam.orientation = {0, -2, 0};
                 std::swap(beam.nodes[0], beam.nodes[1]);
             });
         }},
        {"rotated-translated-coordinate-frame",
         Status::success,
         "",
         [](auto& edit, const auto& view, const auto& data) {
             const double a = 1 / std::sqrt(3.), b = 1 / std::sqrt(2.);
             for (const auto id : {"node", "middle", "tip"})
                 modify<records::Node>(edit, view, id, [a](auto& node) {
                     const auto x = node.position[0];
                     node.position = {43 + a * x, -83 + a * x, 12 + a * x};
                 });
             for (const auto id : {"beam-a", "beam-b"})
                 modify<records::Beam>(
                     edit, view, id, [b](auto& beam) { beam.orientation = {b, -b, 0}; });
             modify<records::NodalForce>(edit, view, data.force.value, [b](auto& force) {
                 force.force_n = {-1000 * b, 1000 * b, 0};
             });
         }},
        {"organization-variant", Status::success, "", [](auto& edit, const auto&, const auto&) {
             edit.put(records::Part{
                 EntityId("part"), "Any name", {EntityId("beam-a"), EntityId("beam-b")}});
             edit.put(records::Assembly{EntityId("assembly"), "Nested", {EntityId("part")}});
             edit.put(records::EntitySet{EntityId("fixed-set"), "Support", {EntityId("node")}});
             edit.put(records::IncludeDocument{EntityId("include"),
                                               "arbitrary.bdf",
                                               {},
                                               {EntityId("material"),
                                                EntityId("section"),
                                                EntityId("beam-a"),
                                                EntityId("beam-b")}});
         }}};
    for (const auto& scenario : scenarios) {
        Fixture fixture;
        const auto data = create_case(fixture);
        fixture.change(scenario.name,
                       [&](auto& edit, const auto& view) { scenario.change(edit, view, data); });
        const auto before = good(fixture.app.current_document());
        const auto history = good(fixture.app.history(before.document)).items.size();
        const auto snapshot = fixture.view();
        const auto report = conclusion(
            analysis_features::RuleCatalog{}.check(snapshot, data.analysis), scenario.expected);
        check(report.catalog_version == analysis_features::RuleCatalog::version &&
                  same_record_version(report.input, snapshot.version()),
              "scenario report records the exact input and catalog");
        if (scenario.expected == Status::success)
            check(report.issues.empty(), scenario.name);
        else {
            const auto issue =
                std::find_if(report.issues.begin(), report.issues.end(), [&](const auto& value) {
                    return value.rule_id == scenario.rule;
                });
            check(issue != report.issues.end() && issue->rule_version == 1 &&
                      issue->severity == "error" && !issue->entity.identity.empty() &&
                      !issue->field.empty() && !issue->actual.empty() && !issue->expected.empty() &&
                      same_record_version(issue->input, snapshot.version()),
                  scenario.name);
        }
        check(good(fixture.app.current_document()).revision == before.revision &&
                  good(fixture.app.history(before.document)).items.size() == history,
              "scenario checks never change authoritative physics or history");
    }
    Fixture empty(false);
    const auto empty_analysis =
        empty.create(AnalysisCreateInput{"No physical facts", {}}, "empty-analysis");
    const auto incomplete = conclusion(
        analysis_features::RuleCatalog{}.check(empty.view(), empty_analysis), Status::needs_input);
    check(incomplete.issues.size() == 3,
          "empty input cannot become a solvable scene from reference counts");
    std::cout << "PASS: TST-F07/TST-I08 bounded cantilever scenarios " << scenarios.size() + 1
              << '\n';
}

const Value::Object& object(const Result<Value>& result) {
    check(result.value.has_value(), "operation preserves the diagnostic report data");
    return std::get<Value::Object>(result.value->data);
}
void non_success_report_persistence() {
    Fixture fixture;
    const auto data = create_case(fixture);
    good(fixture.invoke(LoadCaseSetReferencesInput{data.load_case, {}, {data.constraint}},
                        fixture.context("remove-load")));
    const auto context = fixture.context("needs-input-report");
    const auto before = good(fixture.app.current_document());
    const auto history = good(fixture.app.history(before.document)).items.size();
    const auto response = fixture.invoke(AnalysisCheckInput{data.analysis}, context);
    check(response.status == Status::needs_input && response.error &&
              response.error->code == ErrorCode::missing_input,
          "registry returns needs_input with structured issues");
    const auto& body = object(response);
    check(std::get<std::string>(body.at("outcome").data) == "needs_input" &&
              std::get<std::string>(body.at("check_execution").data) == "completed" &&
              std::get<Value::Array>(body.at("issues").data).size() == 1,
          "execution completion and scenario conclusion remain distinct");
    const auto id = std::get<std::string>(body.at("check_id").data);
    analysis_features::CheckService service(fixture.app);
    const auto retained = good(service.get(caller, before.document, id));
    check(retained.outcome == Status::needs_input && retained.issues.size() == 1,
          "non-success report is retained in the same application side-row port");
    const auto replay = fixture.invoke(AnalysisCheckInput{data.analysis}, context);
    check(replay.status == Status::needs_input && object(replay) == body,
          "identical non-success retry returns the exact report");
    check(good(fixture.app.current_document()).revision == before.revision &&
              good(fixture.app.history(before.document)).items.size() == history,
          "persisting a diagnostic report does not create a model/history commit");
    fixture.change("wrong-but-complete", [&](auto& edit, const auto& view) {
        modify<records::LoadCase>(edit, view, data.load_case.value, [&](auto& load_case) {
            load_case.forces = {data.force};
        });
        modify<records::NodalForce>(
            edit, view, data.force.value, [](auto& force) { force.node = EntityId("node"); });
    });
    const auto wrong =
        fixture.invoke(AnalysisCheckInput{data.analysis}, fixture.context("wrong-report"));
    check(wrong.status == Status::failed && wrong.error &&
              wrong.error->code == ErrorCode::invalid_input &&
              std::get<std::string>(object(wrong).at("outcome").data) == "failed",
          "a complete but wrong scenario returns failed with report data");
    const auto wrong_id = std::get<std::string>(object(wrong).at("check_id").data);
    const auto saved = good(service.get(caller, before.document, wrong_id));
    check(saved.outcome == Status::failed &&
              saved.issues.front().rule_id == "cantilever-tip-load" &&
              analysis_features::issue_state(saved, fixture.view()) ==
                  analysis_features::IssueState::current,
          "wrong scenario issues retain their physical entity and current input provenance");
    const auto old = good(service.get(caller, before.document, id));
    check(old.outcome == Status::needs_input && old.issues.size() == 1 &&
              analysis_features::issue_state(old, fixture.view()) ==
                  analysis_features::IssueState::stale,
          "new checks preserve old input evidence without overwriting or refreshing it");
    const auto bytes = analysis_features::encode_check_report(saved);
    check(record_wire::read_strings(bytes).front() == "QCAE-ANALYSIS-CHECK-2" &&
              analysis_features::decode_check_report(bytes).outcome == Status::failed,
          "v2 report wire stores failed outcome explicitly");
    auto forged = record_wire::read_strings(bytes);
    forged[13] = "success";
    try {
        (void)analysis_features::decode_check_report(record_wire::strings(forged));
        throw std::runtime_error("report accepted a forged pass with failed rule issues");
    } catch (const RecordError& error) {
        check(error.code() == ErrorCode::schema_unsupported, "forged report outcome is rejected");
    }
}

void legacy_report_compatibility() {
    Fixture fixture;
    const auto data = create_case(fixture);
    const auto info = good(fixture.app.current_document());
    auto report = good(analysis_features::RuleCatalog{}.check(fixture.view(), data.analysis));
    const auto identity =
        good(canonical_value(Value(Value::Object{{"caller", Value(caller.principal)},
                                                 {"document", Value(info.document.id.value)},
                                                 {"key", Value("legacy-check")}})));
    report.id = "qcae:analysis-check:" + identity;
    report.principal = caller.principal;
    report.catalog_version = analysis_features::RuleCatalog::legacy_version;
    report.signature = record_wire::strings(std::array{info.document.id.value,
                                                       info.document.epoch.value,
                                                       std::to_string(info.revision),
                                                       data.analysis.value,
                                                       report.catalog_version});
    const auto rule = analysis_features::RuleCatalog{}.definitions().front();
    report.issues.push_back(
        {std::string(rule.id),
         rule.version,
         "error",
         {RecordTraits<records::AnalysisDefinition>::type_id, data.analysis.value},
         report.input,
         "forces",
         std::string(rule.description),
         "0",
         ">=1"});
    const std::vector<std::string> fields{
        "QCAE-ANALYSIS-CHECK-1",
        report.id,
        report.principal,
        report.signature,
        info.document.id.value,
        info.document.epoch.value,
        std::to_string(info.revision),
        data.analysis.value,
        profile.profile_id,
        profile.profile_version,
        profile.definition_digest,
        "linear_static",
        "qcae.analysis.rules.v1",
        "1",
        "missing-load",
        "1",
        "error",
        std::to_string(RecordTraits<records::AnalysisDefinition>::type_id.value),
        data.analysis.value,
        "forces",
        std::string(rule.description),
        "0",
        ">=1"};
    const auto golden = record_wire::strings(fields);
    check(analysis_features::encode_check_report(report) == golden,
          "historical v1 report bytes and execution-success semantics remain unchanged");
    const auto decoded = analysis_features::decode_check_report(golden);
    check(decoded.catalog_version == analysis_features::RuleCatalog::legacy_version &&
              decoded.outcome == Status::success && decoded.issues.size() == 1 &&
              analysis_features::issue_state(decoded, fixture.view()) ==
                  analysis_features::IssueState::stale,
          "old completeness report never claims a current cantilever scenario check");
    const auto row = std::make_shared<const OwnedRowImage>(
        OwnedRowImage{{StoreSpace::artifact_record, report.id},
                      "qcae.analysis.check",
                      1,
                      std::make_shared<const std::string>(golden),
                      {}});
    const OwnedRowUpdate update{row->key, {}, row};
    good(fixture.app.update_owned_rows(caller, info.document, std::span(&update, 1)));
    analysis_features::CheckService service(fixture.app);
    check(good(service.get(caller, info.document, report.id)).catalog_version ==
              analysis_features::RuleCatalog::legacy_version,
          "legacy schema1 report remains readable through the installed row handler");
    bad(service.run(caller, at(info), data.analysis, "legacy-check"),
        ErrorCode::idempotency_key_conflict,
        "new catalog cannot overwrite an old check key with a different rule signature");
    const auto retained = good(
        fixture.app.owned_rows(info.document, StoreSpace::artifact_record, "qcae.analysis.check"));
    check(retained.size() == 1 && *retained.front()->payload == golden &&
              !analysis_features::check_row_handler().recover(*retained.front()),
          "legacy report is preserved byte-for-byte across recovery validation");
    auto illegal = fields;
    illegal[12] = std::string(analysis_features::RuleCatalog::version);
    try {
        (void)analysis_features::decode_check_report(record_wire::strings(illegal));
        throw std::runtime_error("v1 report falsely declared the v2 catalog");
    } catch (const RecordError& error) {
        check(error.code() == ErrorCode::schema_unsupported, "report wire and catalog must agree");
    }
}
void authoritative_record_guards() {
    Fixture fixture;
    const auto data = create_case(fixture);
    const auto info = good(fixture.app.current_document());
    const auto view = fixture.view();
    for (const auto field : {"young_modulus", "section_inertia", "parallel_orientation"}) {
        try {
            EditSession edit(view);
            if (std::string_view(field) == "young_modulus")
                modify<records::Material>(
                    edit, view, "material", [](auto& value) { value.young_modulus_mpa = 0; });
            else if (std::string_view(field) == "section_inertia")
                modify<records::BeamSection>(
                    edit, view, "section", [](auto& value) { value.i1_mm4 = 0; });
            else
                modify<records::Beam>(
                    edit, view, "beam-a", [](auto& value) { value.orientation = {1, 0, 0}; });
            (void)edit.prepare();
            throw std::runtime_error("invalid required physical record was accepted");
        } catch (const RecordError& error) {
            check(error.code() == ErrorCode::invalid_input,
                  "scenario checks consume committed finite/positive physical field invariants");
        }
    }
    check(good(fixture.app.current_document()).revision == info.revision &&
              good(analysis_features::RuleCatalog{}.check(fixture.view(), data.analysis))
                  .issues.empty(),
          "rejected invalid record candidates preserve the original valid scenario");
}
} // namespace
int main() {
    try {
        references_and_rule_lifecycle();
        force_vector_shared_transaction();
        input_and_historical_compatibility();
        cantilever_scenarios();
        non_success_report_persistence();
        legacy_report_compatibility();
        authoritative_record_guards();
        std::cout
            << "PASS: BP-11/14 load case references, versioned issues and shared transactions\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
