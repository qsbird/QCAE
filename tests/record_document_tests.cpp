#include "qcae/edit_session.hpp"
#include "qcae/records.hpp"
#include "qcae/records_model_bridge.hpp"
#include "qcae/operation_ledger.hpp"

#include <algorithm>
#include <iostream>
#include <numeric>
#include <random>
#include <stdexcept>

namespace {
using namespace qcae;
namespace r = qcae::records;

void check(bool value, const std::string& message) {
    if (!value)
        throw std::runtime_error(message);
}
template <class Function> void rejects(ErrorCode expected, Function function, const char* message) {
    try {
        function();
    } catch (const RecordError& error) {
        check(error.code() == expected, std::string(message) + ": wrong error: " + error.what());
        return;
    }
    throw std::runtime_error(std::string(message) + ": accepted invalid input");
}
EntityId id(const std::string& value) {
    return EntityId(value);
}
RecordVersion version() {
    return {{DocumentId("doc"), DocumentEpoch("epoch")}, 7};
}
Model legacy_model() {
    Model model;
    model.materials.push_back({id("material"), "Steel", 210000, .3});
    // Deliberately not sorted: storage position must not become identity.
    model.nodes.push_back({id("node-z"), {0, 0, 0}});
    model.nodes.push_back({id("node-a"), {1000, 0, 0}});
    model.sections.push_back({id("section"), "Box", id("material"), 100, 200, 300, 400});
    model.beams.push_back({id("beam"), id("section"), {id("node-z"), id("node-a")}, {0, 1, 0}});
    model.parts.push_back({id("part"), "Part", {id("node-z"), id("node-a"), id("beam")}});
    model.assemblies.push_back({id("assembly"), "Assembly", {id("part")}});
    model.sets.push_back({id("set"), "Tip", {id("node-a")}});
    model.includes.push_back(
        {id("include"), "main.bdf", std::nullopt, {id("node-z"), id("node-a")}});
    model.forces.push_back({id("force"), id("node-a"), {0, -1, 0}});
    model.constraints.push_back({id("constraint"), {id("node-z")}, "123456"});
    model.analyses.push_back({id("analysis"),
                              "Static",
                              {{"nastran", "1", "test-semantic-digest"}, "linear_static"},
                              {id("force")},
                              {id("constraint")}});
    model.sources.push_back({id("node-z"),
                             "bdf-model",
                             id("include"),
                             {"nastran", "1", "test-semantic-digest"},
                             "GRID",
                             27});
    return model;
}

void version_copy_ledger() {
    const RecordVersion expected{
        {DocumentId(std::string(80, 'd')), DocumentEpoch(std::string(90, 'e'))}, 17};
    const DocumentView view(make_record_registry(), expected);
    const auto operation = std::make_shared<ledger::OperationLedger>(
        ledger::Identity{"record-version-copies", {}, {}, 0});
    {
        ledger::Scope scope(operation);
        auto first = view.version();
        const auto second = view.version();
        first.document.id.value.front() = 'x';
        check(second.document.id == expected.document.id &&
                  second.document.epoch == expected.document.epoch &&
                  second.revision == expected.revision,
              "version() must still return independent owned fields with the same values");
    }
    const auto counted =
        operation->snapshot().values[static_cast<std::size_t>(ledger::Stage::records)]
                                    [static_cast<std::size_t>(ledger::Metric::metadata_copy_bytes)];
    check(counted == 2 * (sizeof(RecordVersion) + 80 + 90),
          "Each actual owned RecordVersion return must be counted exactly once at source");
    check(view.version().document.id == expected.document.id,
          "Modifying a returned version must leave its immutable document unchanged");
}

void implicit_view_copy_ledger() {
    const RecordVersion expected{
        {DocumentId(std::string(80, 'd')), DocumentEpoch(std::string(90, 'e'))}, 17};
    const DocumentView view(make_record_registry(), expected);
    DocumentView assigned(make_record_registry());
    const auto operation = std::make_shared<ledger::OperationLedger>(
        ledger::Identity{"implicit-view-copies", {}, {}, 0});
    {
        ledger::Scope scope(operation);
        auto copied = view;
        assigned = view;
        const auto& same = assigned;
        assigned = same;
        auto moved = std::move(copied);
        check(moved.registry() == view.registry() && assigned.registry() == view.registry(),
              "Copies and moves must retain the same immutable record registry");
    }
    const auto counted =
        operation->snapshot().values[static_cast<std::size_t>(ledger::Stage::records)]
                                    [static_cast<std::size_t>(ledger::Metric::metadata_copy_bytes)];
    check(counted == 2 * (sizeof(RecordVersion) + 80 + 90),
          "Copy construction/assignment each clone version fields; self copy/move do not");
    check(same_record_version(assigned.version(), expected),
          "Copy assignment must preserve complete document identity, epoch and revision");
}

void borrowed_version_guard_and_replacement() {
    const RecordVersion expected{
        {DocumentId(std::string(80, 'd')), DocumentEpoch(std::string(90, 'e'))}, 17};
    const DocumentView view(make_record_registry(), expected);
    RecordVersion replacement{expected.document, 18};
    const auto operation = std::make_shared<ledger::OperationLedger>(
        ledger::Identity{"borrowed-version-guards", {}, {}, 0});
    {
        ledger::Scope scope(operation);
        check(view.matches_version(expected) && !view.matches_version(replacement),
              "Borrowed guards must still compare document, epoch and revision");
        const auto newer = view.with_version(std::move(replacement));
        check(newer.registry() == view.registry() && !newer.matches_version(expected) &&
                  view.matches_version(expected),
              "Version replacement must share records without modifying the original version");
        validate_record_candidate(view, view, {});
    }
    const auto snapshot = operation->snapshot();
    check(snapshot.values[static_cast<std::size_t>(ledger::Stage::records)]
                         [static_cast<std::size_t>(ledger::Metric::metadata_copy_bytes)] == 0,
          "Borrowed guards and moved replacement must not create an unused old version clone");
}

void edit_key_copy_ledger() {
    const auto registry = make_record_registry();
    const DocumentView base(registry, version());
    const std::string identity(80, 'n');
    const auto node = registry->make(r::Node{id(identity), {0, 0, 0}, std::nullopt});
    const auto operation =
        std::make_shared<ledger::OperationLedger>(ledger::Identity{"edit-key-copies", {}, {}, 0});
    {
        ledger::Scope scope(operation);
        EditSession edit(base);
        edit.put(node);
        edit.put(node);
        check(edit.find(RecordTraits<r::Node>::type_id, identity) == node,
              "Observed keys must preserve replacement and lookup semantics");
    }
    const auto counted =
        operation->snapshot().values[static_cast<std::size_t>(ledger::Stage::records)]
                                    [static_cast<std::size_t>(ledger::Metric::metadata_copy_bytes)];
    check(counted == sizeof(RecordVersion) + 3 + 5 + 5 * (sizeof(RecordKey) + identity.size()),
          "First put copies three keys, repeat put one key, and find one temporary key");
}

void schemas_and_registration() {
    const auto registry = make_record_registry();
    const auto material = registry->make(r::Material{id("m"), "Steel", 210000, .3});
    const auto node = registry->make(r::Node{id("n"), {1, 2, 3}, std::nullopt});
    check(registry->decode(material->encoded())->get<r::Material>() == material->get<r::Material>(),
          "typed canonical codec roundtrip");
    for (unsigned seed = 0; seed < 10; ++seed) {
        auto descriptors = generated_record_descriptors();
        std::mt19937 random(seed);
        std::shuffle(descriptors.begin(), descriptors.end(), random);
        auto shuffled = std::make_shared<RecordRegistry>();
        for (auto descriptor : descriptors)
            shuffled->add(std::move(descriptor));
        shuffled->freeze();
        check(shuffled->types() == registry->types(), "type IDs independent of registration order");
        for (const auto type : registry->types())
            check(shuffled->find(type)->fields == registry->find(type)->fields,
                  "field IDs independent of registration order");
        check(shuffled->decode(material->encoded())->encoded() == material->encoded(),
              "canonical bytes independent of registration order");
    }
    auto prior = record_wire::decode(material->encoded());
    std::erase_if(prior.fields, [](const auto& field) { return field.id == RecordFieldId{4}; });
    check(!registry->from_input(prior)->get<r::Material>().poisson_ratio,
          "absent optional field retains its schema default");

    const auto empty = DocumentView(registry, version());
    EditSession initial(empty);
    initial.put(material);
    initial.put(node);
    const auto base = initial.prepare().candidate;
    std::size_t negative_cases = 0;
    auto negative = [&](RecordInput input, ErrorCode code, const char* label) {
        rejects(code, [&] { registry->from_input(input); }, label);
        check(base.find(material->key()) == material && base.find(node->key()) == node,
              "schema failure leaves immutable model and history images unchanged");
        ++negative_cases;
    };
    auto missing = record_wire::decode(material->encoded());
    missing.fields.erase(missing.fields.begin());
    negative(missing, ErrorCode::missing_input, "missing required field");
    auto wrong_type = record_wire::decode(node->encoded());
    wrong_type.fields[0].kind = RecordFieldKind::text;
    negative(wrong_type, ErrorCode::invalid_input, "wrong field type");
    auto missing_unit = record_wire::decode(node->encoded());
    missing_unit.fields[0].unit.clear();
    negative(missing_unit, ErrorCode::invalid_unit, "missing unit");
    auto unknown_unit = record_wire::decode(node->encoded());
    unknown_unit.fields[0].unit = "furlong";
    negative(unknown_unit, ErrorCode::invalid_unit, "unknown unit");
    auto bounds = record_wire::decode(material->encoded());
    for (auto& field : bounds.fields)
        if (field.id == RecordFieldId{3})
            field.payload = record_wire::real(-1);
    negative(bounds, ErrorCode::invalid_input, "out-of-range value");
    auto wrong_reference = [&](const EntityId& target, const char* label) {
        EditSession edit(base);
        edit.put(r::BeamSection{id("s"), "Section", target, 1, 1, 1, 1});
        rejects(ErrorCode::invalid_input, [&] { edit.prepare(); }, label);
        check(base.size() == 2 && !base.find<r::BeamSection>(id("s")),
              "bad reference does not publish candidate");
        ++negative_cases;
    };
    wrong_reference(id("n"), "wrong reference target type");
    wrong_reference(id("missing"), "dangling reference");
    auto future = record_wire::decode(material->encoded());
    ++future.version;
    negative(future, ErrorCode::schema_unsupported, "unknown future record version");
    check(negative_cases == 8, "all eight schema negative cases executed");
    rejects(
        ErrorCode::invalid_input,
        [&] { registry->decode(node->encoded() + "trailing"); },
        "trailing wire bytes");
    rejects(
        ErrorCode::resource_limit,
        [&] { registry->make(r::Node{id(std::string(240, 'n')), {0, 0, 0}, std::nullopt}); },
        "node record limit includes its encoded identity and header");
}

void stable_changes_and_bridges() {
    const auto registry = make_record_registry();
    const auto original = legacy_model();
    check(validate_model(original).empty(), "legacy fixture is valid under pre-refactor rules");
    RecordStats bridge_stats;
    const auto base = records_from_model(original, registry, version(), &bridge_stats);
    check(model_from_records(base, &bridge_stats) == original,
          "all twelve legacy collections roundtrip without order or identity changes");
    check(base.count(RecordTraits<r::Mesh>::type_id) == 1, "legacy mesh ownership is explicit");
    check(bridge_stats.whole_model_materializations == 1,
          "full export materialization is instrumented");
    const auto retained = base.find<r::Node>(id("node-a"));
    EditSession edit(base);
    edit.update<r::Node>(id("node-a"), [](auto& node) { node.position[1] = 1; });
    const auto prepared = edit.prepare();
    check(same_record_version(prepared.base, version()),
          "prepared candidate carries its immutable input version");
    check(prepared.changes.records.size() == 1 &&
              prepared.changes.records[0].key.identity == "node-a",
          "change addresses stable entity key");
    check(prepared.changes.records[0].fields == std::vector<RecordFieldId>{{2}},
          "generated field IDs identify the actual modified field");
    check(retained->get<r::Node>().position[1] == 0, "old pinned read remains unchanged");
    check(prepared.candidate.find<r::Material>(id("material")) ==
              base.find<r::Material>(id("material")),
          "untouched records retain shared ownership");
    auto current = prepared.candidate;
    for (unsigned round = 0; round < 100; ++round) {
        current = apply_record_changes(current, prepared.changes, RecordDirection::reverse);
        check(diff_record_views(base, current).empty(), "undo restores all semantic records");
        current = apply_record_changes(current, prepared.changes);
        check(diff_record_views(prepared.candidate, current).empty(),
              "redo restores all semantic records");
    }
    const auto serialized = encode_record_changes(prepared.changes);
    const auto decoded = decode_record_changes(*registry, serialized);
    const auto restored = apply_record_changes(base, decoded);
    check(diff_record_views(restored, prepared.candidate).empty(),
          "history persists typed before/after images");
    rejects(
        ErrorCode::revision_conflict,
        [&] { apply_record_changes(restored, decoded); },
        "reapplying a committed delta is rejected");
    auto malformed = decoded;
    malformed.records.front().before = Record{};
    rejects(
        ErrorCode::invalid_input,
        [&] { encode_record_changes(malformed); },
        "history encoding rejects a present null image");
    EditSession coalesced(base);
    coalesced.update<r::Node>(id("node-a"), [](auto& node) { node.position[1] = 2; });
    coalesced.update<r::Node>(id("node-a"), [](auto& node) { node.position[1] = 0; });
    check(coalesced.prepare().changes.empty(), "repeated edits coalesce to semantic no-op");
    EditSession dangling(base);
    dangling.erase({RecordTraits<r::Node>::type_id, "node-a"});
    rejects(
        ErrorCode::invalid_input,
        [&] { dangling.prepare(); },
        "delete cannot leave references dangling");

    // Deleting an unreferenced record and restoring it preserves its stable slot and siblings.
    EditSession added(base);
    added.put(r::Node{id("middle"), {20, 0, 0}, std::nullopt});
    const auto with_extra = added.prepare().candidate;
    EditSession removed(with_extra);
    removed.erase({RecordTraits<r::Node>::type_id, "middle"});
    const auto removal = removed.prepare();
    const auto reverse =
        apply_record_changes(removal.candidate, removal.changes, RecordDirection::reverse);
    check(diff_record_views(with_extra, reverse).empty(),
          "deletion reverse does not shift sibling identities");
}

void geometry_and_ownership() {
    const auto registry = make_record_registry();
    EditSession build(DocumentView(registry, version()));
    const r::GeometryId geometry("geometry");
    const r::MeshId mesh("mesh");
    build.put(r::GeometryLine{geometry, {0, 0, 0}, {1000, 0, 0}, 1});
    build.put(r::Mesh{mesh, "Line mesh", "geometry", geometry, 1, false});
    build.put(r::Node{id("n"), {0, 0, 0}, mesh});
    const auto base = build.prepare().candidate;
    EditSession mixed(base);
    const r::MeshId other_mesh("other-mesh");
    mixed.put(r::Mesh{other_mesh, "Other mesh", "manual", std::nullopt, 0, false});
    mixed.put(r::Node{id("other-node"), {1, 0, 0}, other_mesh});
    mixed.put(r::Beam{
        id("cross-mesh-line"), std::nullopt, {id("n"), id("other-node")}, {0, 0, 1}, std::nullopt});
    rejects(
        ErrorCode::invalid_input,
        [&] { mixed.prepare(); },
        "unassigned element cannot bypass mesh ownership consistency");
    check(base.find<r::GeometryLine>(geometry)->key().type != base.find<r::Mesh>(mesh)->key().type,
          "geometry and mesh have distinct persistent type identities");
    EditSession stale(base);
    stale.update<r::GeometryLine>(geometry, [](auto& line) {
        line.end[0] = 1200;
        ++line.geometry_revision;
    });
    rejects(
        ErrorCode::invalid_input,
        [&] { stale.prepare(); },
        "geometry revision cannot silently invalidate binding");
    stale.update<r::Mesh>(mesh, [](auto& value) { value.stale = true; });
    check(stale.prepare().candidate.find<r::Mesh>(mesh)->get<r::Mesh>().stale,
          "explicit composite stale marking is accepted");
}

void locality() {
    const auto registry = make_record_registry();
    for (const std::size_t count : {1000u, 10000u, 100000u}) {
        Model model;
        model.materials.push_back({id("material"), "Steel", 210000, .3});
        model.sections.push_back({id("section"), "Section", id("material"), 100, 200, 200, 300});
        auto node_id = [](std::size_t index) { return id("n" + std::to_string(1000000 + index)); };
        model.nodes.reserve(count);
        model.beams.reserve(count - 1);
        for (std::size_t index = 0; index < count; ++index) {
            model.nodes.push_back({node_id(index), {static_cast<double>(index), 0, 0}});
            if (index)
                model.beams.push_back({id("b" + std::to_string(1000000 + index)),
                                       id("section"),
                                       {node_id(index - 1), node_id(index)},
                                       {0, 0, 1}});
        }
        const auto base = records_from_model(model, registry, version());
        const auto chosen = node_id(count / 2);
        const auto activity_before = record_activity_counters();
        for (unsigned run = 0; run < 20; ++run) {
            EditSession edit(base, {500000, 2000000, record_wire::maximum_record_bytes});
            const bool node_edit = run < 10;
            if (node_edit)
                edit.update<r::Node>(chosen, [](auto& value) { value.position[1] = .5; });
            else
                edit.update<r::Material>(id("material"),
                                         [](auto& value) { value.young_modulus_mpa = 205000; });
            auto prepared = edit.prepare();
            auto stats = prepared.stats;
            const auto history = encode_record_changes(prepared.changes, &stats);
            const auto bound = node_edit ? 589824u : 69632u;
            check(stats.changed_records == 1 && stats.dirty_pages == 1,
                  "locality edit touches exactly one record and page");
            check(stats.model_bytes_copied <= bound && stats.model_bytes_encoded <= bound &&
                      stats.metadata_bytes_copied <= 65536 && history.size() <= bound,
                  "local mutation and history payload stay inside the frozen bounds");
            check(stats.whole_model_serializations == 0 && stats.whole_model_materializations == 0,
                  "local edit never serializes or materializes the whole model");
            if (node_edit)
                check((*prepared.changes.records.front().after)->encoded().size() <= 256,
                      "node canonical record stays below the frozen record limit");
        }
        const auto activity_after = record_activity_counters();
        check(activity_before.whole_model_serializations ==
                      activity_after.whole_model_serializations &&
                  activity_before.whole_model_materializations ==
                      activity_after.whole_model_materializations,
              "global instrumentation confirms no hidden complete-model bridge calls");
        std::cout << "locality " << count << " nodes: 20/20 record-layer samples passed\n";
    }
}
} // namespace

int main() {
    try {
        schemas_and_registration();
        stable_changes_and_bridges();
        geometry_and_ownership();
        version_copy_ledger();
        implicit_view_copy_ledger();
        borrowed_version_guard_and_replacement();
        edit_key_copy_ledger();
        locality();
        std::cout << "PASS: typed records, schema negatives, stable-key history, legacy bridge, "
                     "geometry ownership\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
