#include "qcae/material_operations.hpp"
#include "qcae/mesh_editing_operations.hpp"
#include "qcae/records.hpp"
#include "qcae/core.hpp"
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace qcae;
using namespace qcae::operations;
namespace material_features = qcae::features::materials;
namespace mesh_features = qcae::features::mesh_editing;

namespace {
void check(bool value, const std::string& message) {
    if (!value)
        throw std::runtime_error(message);
}
template <class T> T good(Result<T> value, const std::string& message) {
    check(value.ok(), message + (value.error ? ": " + value.error->message : ""));
    return std::move(*value.value);
}
template <class T> void bad(const Result<T>& result, ErrorCode code, const std::string& message) {
    const auto status = code == ErrorCode::missing_input ? Status::needs_input
                        : code == ErrorCode::idempotency_key_conflict ||
                                code == ErrorCode::revision_conflict ||
                                code == ErrorCode::document_epoch_expired
                            ? Status::conflict
                            : Status::failed;
    check(!result.ok() && !result.value && result.status == status && result.error &&
              result.error->code == code,
          message);
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
        check(batch.expected_generation == generation, "row generation agrees with coordinator");
        auto candidate = rows;
        std::uint64_t bytes{};
        for (const auto& row : batch.mutations) {
            if (row.after) {
                candidate[row.key] = row.after;
                bytes += row.after->size();
            } else
                candidate.erase(row.key);
        }
        rows.swap(candidate);
        return {++generation, batch.mutations.size(), bytes};
    }
};
RecordApplicationOptions options(const std::shared_ptr<MemoryRows>& rows) {
    RecordApplicationOptions options;
    options.registry = make_record_registry();
    options.records = rows;
    return options;
}
struct Fixture {
    std::shared_ptr<MemoryRows> rows = std::make_shared<MemoryRows>();
    RecordApplication app{options(rows)};
    OperationRegistry registry;
    Caller caller{"model-operation-tests"};
    DocumentInfo initial;
    Fixture() {
        initial = good(app.create_document(caller, "Typed feature operations", "create-doc"),
                       "create document");
        good(material_features::register_handlers(registry, app),
             "register material/section handlers");
        good(mesh_features::register_handlers(registry, app), "register node/beam handlers");
    }
    OperationContext context(std::string key) {
        const auto info = good(app.current_document(), "current document");
        return {caller, info.document, info.revision, std::move(key), std::nullopt, "request-1"};
    }
    RecordSnapshot snapshot() {
        return good(app.snapshot(initial.document), "record snapshot");
    }
    Value invoke(std::string_view name, const OperationContext& context, Value input) {
        return good(registry.invoke(name, 1, context, input), "invoke " + std::string(name));
    }
};
const Value::Object& object(const Value& value) {
    return std::get<Value::Object>(value.data);
}
std::string transaction(const Value& value) {
    return std::get<std::string>(object(value).at("transaction_id").data);
}
Revision revision(const Value& value, std::string_view field) {
    return std::stoull(std::get<std::string>(object(value).at(std::string(field)).data));
}
bool replayed(const Value& value) {
    return std::get<bool>(object(value).at("replayed").data);
}
template <class T> EntityId only_id(const DocumentView& view) {
    EntityId id;
    std::size_t count{};
    view.visit(RecordTraits<T>::type_id, [&](const auto& record) {
        id = record->template get<T>().id;
        ++count;
    });
    check(count == 1, "query exactly one created record instead of fabricating a replay ID");
    return id;
}
void canonical_signatures() {
    Value::Object first;
    first.emplace("b", Value(1.25));
    first.emplace("a", Value(1));
    Value::Object second;
    second.emplace("a", Value(1.0));
    second.emplace("b", Value(1.25));
    check(good(canonical_value(Value(first)), "canonical object") ==
              good(canonical_value(Value(second)), "canonical reordered object"),
          "object insertion order and equivalent int/double do not change signature");
    check(good(canonical_value(Value(-0.0)), "negative zero") ==
              good(canonical_value(Value(0)), "zero"),
          "signed zero normalizes");
    check(good(canonical_value(Value(true)), "boolean") !=
              good(canonical_value(Value(1)), "integer"),
          "boolean is distinct from a numeric one");
    check(good(canonical_value(Value(std::int64_t{9007199254740993})), "large exact integer") !=
              good(canonical_value(Value(9007199254740992.0)), "rounded double"),
          "int64 identity is not lost through double conversion");
    check(
        good(canonical_value(Value(std::numeric_limits<std::int64_t>::max())), "maximum integer") !=
            good(canonical_value(Value(9223372036854775808.0)), "outside signed range"),
        "double boundary conversion does not overflow int64");
    check(good(canonical_value(Value(Value::Array{Value("a"), Value("bc")})), "array strings") !=
              good(canonical_value(Value(Value::Array{Value("ab"), Value("c")})), "other strings"),
          "length prefixes prevent concatenation collisions");
    check(good(canonical_value(Value(Value::Array{Value(1), Value(2)})), "ordered array") !=
              good(canonical_value(Value(Value::Array{Value(2), Value(1)})), "reversed array"),
          "generic array ordering remains semantic");
    bad(canonical_value(Value(std::numeric_limits<double>::infinity())),
        ErrorCode::invalid_input,
        "nonfinite canonical value rejected");
    Value nested(1);
    for (int i = 0; i < 66; ++i)
        nested = Value(Value::Array{std::move(nested)});
    bad(canonical_value(nested),
        ErrorCode::resource_limit,
        "bounded recursive encoding rejects excessive nesting");
}
void canonical_owned_boundaries() {
    Value::Object fields;
    fields.emplace("a", Value(std::int64_t{-9223372036854775807LL - 1}));
    fields.emplace("b", Value(1.25));
    fields.emplace("c", Value(std::string("x\0y", 3)));
    const Value input(std::move(fields));
    const auto operation = std::make_shared<ledger::OperationLedger>(
        ledger::Identity{"canonical-builder-copies", {}, {}, 0});
    Result<std::string> encoded;
    {
        ledger::Scope scope(operation);
        encoded = canonical_value(input);
    }
    std::string expected = "QCV1:o3:s1:an20:-9223372036854775808s1:bn4:1.25s1:cs3:";
    expected.append("x\0y", 3);
    check(
        encoded.ok() && *encoded.value == expected,
        "Canonical builder preserves exact old tags, sorted fields, int64 and embedded NUL bytes");
    const auto snapshot = operation->snapshot();
    const auto& measured = snapshot.values[static_cast<std::size_t>(ledger::Stage::application)];
    check(!measured[static_cast<std::size_t>(ledger::Metric::model_copy_bytes)] &&
              measured[static_cast<std::size_t>(ledger::Metric::metadata_copy_bytes)].value_or(0) >=
                  expected.size() + 64,
          "Builder borrows the existing input and counts its own output/numeric scratch as H");

    const auto registry = make_record_registry();
    const DocumentView base(registry);
    MaterialCreateInput original{std::string(80, 's'), {210, "GPa"}, .3};
    auto plan = good(material_features::prepare_create_material(original), "owned material plan");
    original.name.assign("changed");
    original.young_modulus = {1, "MPa"};
    const auto prepared =
        good(plan.prepare(base, [] { return EntityId("material"); }), "owned material callback");
    const auto material = prepared.change.candidate.find<records::Material>(EntityId("material"));
    check(material && material->get<records::Material>().name == std::string(80, 's') &&
              material->get<records::Material>().young_modulus_mpa == 210000 &&
              prepared.signature == plan.signature,
          "Moving canonical storage into the callback retains independent input and identical "
          "signature");
}
void normalizers_and_optional_fields() {
    const auto canonical =
        good(material_features::prepare_create_material({"Steel", {210000, "MPa"}, std::nullopt}),
             "canonical material");
    for (const auto& quantity :
         std::vector<Quantity>{{210, "GPa"}, {210000000, "kPa"}, {210000000000, "Pa"}}) {
        check(good(material_features::prepare_create_material({"Steel", quantity, std::nullopt}),
                   "pressure normalization")
                      .signature == canonical.signature,
              "equivalent pressure units have one canonical operation signature");
    }
    const auto explicit_nu =
        good(material_features::prepare_create_material({"Steel", {210000, "MPa"}, .3}),
             "explicit Poisson ratio");
    check(explicit_nu.signature != canonical.signature,
          "omitted Poisson ratio is not silently defaulted");
    const auto omitted =
        good(InputTraits<MaterialCreateInput>::from_value(
                 InputTraits<MaterialCreateInput>::to_value({"Steel", {210, "GPa"}, std::nullopt})),
             "optional field omission");
    check(!omitted.poisson_ratio, "generated adapter preserves optional absence");
    const auto supplied =
        good(InputTraits<MaterialCreateInput>::from_value(
                 InputTraits<MaterialCreateInput>::to_value({"Steel", {210, "GPa"}, .3})),
             "optional field presence");
    check(supplied.poisson_ratio == .3, "generated adapter preserves explicit value");
    auto bad_nu =
        object(InputTraits<MaterialCreateInput>::to_value({"Steel", {210, "GPa"}, std::nullopt}));
    bad_nu.emplace("poisson_ratio", Value(true));
    bad(InputTraits<MaterialCreateInput>::from_value(Value(bad_nu)),
        ErrorCode::invalid_input,
        "optional number rejects a bool");
    bad(material_features::prepare_create_material({"Steel", {1, "N"}, {}}),
        ErrorCode::invalid_unit,
        "wrong material dimension");
    bad(material_features::prepare_create_material({"Steel", {1, ""}, {}}),
        ErrorCode::missing_input,
        "material unit required");
    bad(material_features::prepare_create_material({"Steel", {-1, "MPa"}, {}}),
        ErrorCode::invalid_input,
        "generated material positive rule reused");
    bad(material_features::prepare_create_material({"Steel", {210000, "MPa"}, .5}),
        ErrorCode::invalid_input,
        "generated Poisson range rule reused");
    bad(material_features::prepare_create_material({"", {210000, "MPa"}, {}}),
        ErrorCode::invalid_input,
        "generated material name rule reused");
    bad(material_features::prepare_create_section({"Section", EntityId("material"), 0, 1, 1, 1}),
        ErrorCode::invalid_input,
        "generated section positive rule reused");
    bad(material_features::prepare_create_section(
            {"Section", EntityId("material"), 1, 1, 1, std::numeric_limits<double>::infinity()}),
        ErrorCode::invalid_input,
        "section numeric range validated before staging");
    bad(mesh_features::prepare_move_node(
            {EntityId("node"), {std::numeric_limits<double>::denorm_min(), 0, 0}}),
        ErrorCode::invalid_input,
        "node canonical position rejects underflow");
    bad(mesh_features::prepare_assign_section({{}, EntityId("section")}),
        ErrorCode::invalid_input,
        "empty beam selection rejected");
    bad(mesh_features::prepare_assign_section(
            {{EntityId("beam"), EntityId("beam")}, EntityId("section")}),
        ErrorCode::invalid_input,
        "duplicate beam IDs rejected");
    const auto order_a = good(
        mesh_features::prepare_assign_section({{EntityId("b0"), EntityId("b1")}, EntityId("s")}),
        "selected beams");
    const auto order_b = good(
        mesh_features::prepare_assign_section({{EntityId("b1"), EntityId("b0")}, EntityId("s")}),
        "reordered selected beams");
    check(order_a.signature == order_b.signature, "beam assignment normalizes set ordering");
}
void material_entry_boundary_consistency() {
    for (const auto& quantity :
         std::vector<Quantity>{{210, "GPa"},
                               {210000, "MPa"},
                               {210000000, "kPa"},
                               {210000000000, "Pa"},
                               {std::numeric_limits<double>::min(), "MPa"},
                               {std::numeric_limits<double>::max(), "MPa"},
                               {1e-307, "Pa"},
                               {std::numeric_limits<double>::min() / 2, "GPa"},
                               {std::numeric_limits<double>::max(), "GPa"},
                               {0, "MPa"},
                               {-1, "MPa"},
                               {210000, ""},
                               {210000, "N"}}) {
        MemoryApplication legacy;
        const Caller caller{"entry-consistency"};
        const auto initial = good(legacy.create_document(caller, "Units", "create"), "create");
        const auto compatible = legacy.preview(
            caller, {initial.document, initial.revision}, CreateMaterial{"Steel", quantity});
        Fixture typed;
        const auto result = typed.registry.invoke(
            "material.create",
            typed.context("typed-create"),
            InputTraits<MaterialCreateInput>::to_value({"Steel", quantity, std::nullopt}));
        check(compatible.ok() == result.ok(),
              "legacy and typed entry must agree on numeric bounds");
        if (!compatible.ok()) {
            check(compatible.status == result.status && compatible.error && result.error &&
                      compatible.error->code == result.error->code,
                  "legacy and typed entry must agree on quantity rejection");
            check(good(legacy.current_document(), "unchanged legacy").revision == 0 &&
                      typed.snapshot().info.revision == 0,
                  "rejected quantities must not publish either model");
            continue;
        }
        good(legacy.commit(caller,
                           {initial.document, initial.revision},
                           compatible.value->id,
                           "legacy-commit"),
             "legacy material commit");
        const auto id = only_id<records::Material>(typed.snapshot().records);
        const auto canonical = typed.snapshot().records.find<records::Material>(id);
        check(good(legacy.snapshot(initial.document), "legacy material")
                      .materials.front()
                      .young_modulus_mpa == canonical->get<records::Material>().young_modulus_mpa,
              "both entries must commit the same canonical engineering value");
    }
}
void direct_retry_and_history() {
    Fixture fixture;
    const auto context = fixture.context("material-key");
    const auto original = fixture.invoke(
        "material.create",
        context,
        InputTraits<MaterialCreateInput>::to_value({"Steel", {210, "GPa"}, std::nullopt}));
    const auto material = only_id<records::Material>(fixture.snapshot().records);
    check(!fixture.snapshot()
               .records.find<records::Material>(material)
               ->get<records::Material>()
               .poisson_ratio,
          "authoritative material retains absent Nu");
    check(!replayed(original) && revision(original, "committed_revision") == 1,
          "first direct request commits once");
    check(std::get<std::string>(object(original).at("entity_id").data) == material.value,
          "created ID comes from the persisted application receipt");
    const auto generation = fixture.rows->generation;
    auto retry_context = context;
    retry_context.request_id = "network-retry";
    const auto retry = fixture.invoke(
        "material.create",
        retry_context,
        InputTraits<MaterialCreateInput>::to_value({"Steel", {210000, "MPa"}, std::nullopt}));
    check(std::get<std::string>(object(original).at("entity_id").data) ==
              std::get<std::string>(object(retry).at("entity_id").data),
          "F05 replay preserves the original created entity ID");
    check(transaction(original) == transaction(retry) && replayed(retry),
          "F05 same canonical parameters return original transaction");
    check(fixture.rows->generation == generation && fixture.snapshot().info.revision == 1 &&
              fixture.snapshot().records.count(RecordTraits<records::Material>::type_id) == 1,
          "F05 performs no extra persistent or model commit");
    bad(fixture.registry.invoke(
            "material.create",
            1,
            context,
            InputTraits<MaterialCreateInput>::to_value({"Steel", {200000, "MPa"}, std::nullopt})),
        ErrorCode::idempotency_key_conflict,
        "F06 same key/different canonical parameters conflict");
    check(fixture.rows->generation == generation && fixture.snapshot().info.revision == 1,
          "F06 leaves model/history/store unchanged");
    const auto fact =
        good(fixture.app.action_outcome(
                 fixture.caller, fixture.initial.document, "material.create", "material-key"),
             "query direct persisted fact");
    check(fact.transaction.value == transaction(original),
          "generic application owns direct operation ledger");
    const auto current = fixture.snapshot().info;
    good(fixture.app.undo(fixture.caller, {current.document, current.revision}, "undo-material"),
         "undo direct material create");
    check(fixture.snapshot().records.count(RecordTraits<records::Material>::type_id) == 0,
          "undo removes created record");
    const auto replay_after_undo = fixture.invoke(
        "material.create",
        retry_context,
        InputTraits<MaterialCreateInput>::to_value({"Steel", {210, "GPa"}, std::nullopt}));
    check(transaction(replay_after_undo) == transaction(original) &&
              fixture.snapshot().records.count(RecordTraits<records::Material>::type_id) == 0,
          "F07 original direct fact replay does not reapply an undone create");
    check(std::get<std::string>(object(replay_after_undo).at("entity_id").data) == material.value,
          "replay after undo still identifies the originally created entity");
    const auto undone = fixture.snapshot().info;
    good(fixture.app.redo(fixture.caller, {undone.document, undone.revision}, "redo-material"),
         "redo typed record create");
    check(fixture.snapshot().records.find<records::Material>(material) != nullptr,
          "redo restores original identity");
}
void retained_create_identity() {
    const auto rows = std::make_shared<MemoryRows>();
    const Caller caller{"recover-feature-test"};
    OperationContext original_context;
    Value original;
    {
        RecordApplication app{options(rows)};
        OperationRegistry registry;
        good(material_features::register_handlers(registry, app), "register original handlers");
        const auto document =
            good(app.create_document(caller, "Recovery", "document"), "create retained document");
        original_context = {
            caller, document.document, document.revision, "material", {}, "first-request"};
        original = good(registry.invoke("material.create",
                                        original_context,
                                        InputTraits<MaterialCreateInput>::to_value(
                                            {"Steel", {210, "GPa"}, .3})),
                        "create retained material");
    }
    RecordApplication restored{options(rows)};
    const auto document =
        good(restored.recover_document(caller, "recover"), "recover retained rows");
    const auto receipt =
        good(restored.action_outcome(
                 caller, document.document, "material.create", original_context.idempotency_key),
             "query restored create fact");
    const EntityId material(std::get<std::string>(object(original).at("entity_id").data));
    check(receipt.primary_entity == material && receipt.transaction.value == transaction(original),
          "retained operation receipt preserves the original stable entity and transaction");
    check(good(restored.snapshot(document.document), "restored snapshot")
                  .records.find<records::Material>(material) != nullptr,
          "receipt identity refers to the retained material record");
    OperationRegistry registry;
    good(material_features::register_handlers(registry, restored), "register restored handlers");
    bad(registry.invoke("material.create",
                        original_context,
                        InputTraits<MaterialCreateInput>::to_value({"Steel", {210, "GPa"}, .3})),
        ErrorCode::document_epoch_expired,
        "recovery expires the previous document epoch before a handler can replay");
}
void edits_sections_and_reference_failures() {
    Fixture fixture;
    fixture.invoke("material.create",
                   fixture.context("material"),
                   InputTraits<MaterialCreateInput>::to_value({"Steel", {210, "GPa"}, .3}));
    const auto material = only_id<records::Material>(fixture.snapshot().records);
    fixture.invoke("material.set_young_modulus",
                   fixture.context("set-material"),
                   InputTraits<MaterialSetYoungModulusInput>::to_value({material, {200, "GPa"}}));
    check(fixture.snapshot()
                  .records.find<records::Material>(material)
                  ->get<records::Material>()
                  .young_modulus_mpa == 200000,
          "setter normalizes pressure through shared parameters");
    const auto seed_context = fixture.context("seed");
    good(fixture.app.execute(
             fixture.caller,
             {*seed_context.document, *seed_context.expected_revision},
             "test.seed",
             "seed",
             [](const DocumentView& view,
                const RecordIdentityAllocator&) -> Result<RecordPreparedOperation> {
                 EditSession edit(view);
                 edit.put(records::Node{EntityId("n0"), {0, 0, 0}, std::nullopt});
                 edit.put(records::Node{EntityId("n1"), {1, 0, 0}, std::nullopt});
                 edit.put(records::Node{EntityId("n2"), {2, 0, 0}, std::nullopt});
                 edit.put(records::Beam{EntityId("b0"),
                                        std::nullopt,
                                        {EntityId("n0"), EntityId("n1")},
                                        {0, 1, 0},
                                        std::nullopt});
                 edit.put(records::Beam{EntityId("b1"),
                                        std::nullopt,
                                        {EntityId("n1"), EntityId("n2")},
                                        {0, 1, 0},
                                        std::nullopt});
                 return {Status::success,
                         RecordPreparedOperation{edit.prepare(),
                                                 "Seed nodes and unassigned beams",
                                                 EntityId("n0"),
                                                 "seed",
                                                 0,
                                                 false},
                         std::nullopt};
             },
             "seed"),
         "seed representative mesh");
    fixture.invoke("node.move",
                   fixture.context("move"),
                   InputTraits<NodeMoveInput>::to_value({EntityId("n1"), {1, 1, 0}}));
    check(fixture.snapshot()
                  .records.find<records::Node>(EntityId("n1"))
                  ->get<records::Node>()
                  .position == std::array<double, 3>{1, 1, 0},
          "node move updates typed record");
    const auto section_context = fixture.context("section");
    const auto section_input = InputTraits<SectionCreateInput>::to_value(
        {"Beam section", material, 100, 833.333, 833.333, 1400});
    const auto created_section = fixture.invoke("section.create", section_context, section_input);
    const auto section = only_id<records::BeamSection>(fixture.snapshot().records);
    const auto section_retry = fixture.invoke("section.create", section_context, section_input);
    check(transaction(section_retry) == transaction(created_section) && replayed(section_retry) &&
              std::get<std::string>(object(section_retry).at("entity_id").data) == section.value,
          "section create retry retains its original transaction and stable identity");
    const auto before_assignment = fixture.snapshot();
    check(!before_assignment.records.find<records::Beam>(EntityId("b0"))
               ->get<records::Beam>()
               .section,
          "unassigned beam has no implicit section");
    const auto assignment_context = fixture.context("assign");
    const auto assignment = fixture.invoke(
        "beam.assign_section",
        assignment_context,
        InputTraits<BeamAssignSectionInput>::to_value({{EntityId("b1"), EntityId("b0")}, section}));
    const auto after_assignment = fixture.snapshot();
    const auto generation_after_assignment = fixture.rows->generation;
    const auto assignment_retry = fixture.invoke(
        "beam.assign_section",
        assignment_context,
        InputTraits<BeamAssignSectionInput>::to_value({{EntityId("b0"), EntityId("b1")}, section}));
    check(transaction(assignment_retry) == transaction(assignment) && replayed(assignment_retry) &&
              fixture.rows->generation == generation_after_assignment,
          "reordered beam IDs replay the same compound assignment without another commit");
    check(after_assignment.records.find<records::Beam>(EntityId("b0"))
                      ->get<records::Beam>()
                      .section == section &&
              after_assignment.records.find<records::Beam>(EntityId("b1"))
                      ->get<records::Beam>()
                      .section == section,
          "both beams reference the existing section");
    check(after_assignment.records.count(RecordTraits<records::Node>::type_id) == 3 &&
              after_assignment.records.count(RecordTraits<records::BeamSection>::type_id) == 1,
          "assignment creates no duplicate nodes or properties");
    for (const auto id : {"n0", "n1", "n2"})
        check(before_assignment.records.find<records::Node>(EntityId(id)) ==
                  after_assignment.records.find<records::Node>(EntityId(id)),
              "assignment preserves immutable node allocations");
    const auto revision_before = after_assignment.info.revision;
    const auto generation_before = fixture.rows->generation;
    bad(fixture.registry.invoke("node.move",
                                fixture.context("wrong-node-type"),
                                InputTraits<NodeMoveInput>::to_value({material, {1, 2, 3}})),
        ErrorCode::entity_not_found,
        "wrong node record type rejected");
    bad(fixture.registry.invoke("section.create",
                                fixture.context("wrong-material-ref"),
                                InputTraits<SectionCreateInput>::to_value(
                                    {"Invalid section", EntityId("n0"), 1, 1, 1, 1})),
        ErrorCode::invalid_input,
        "wrong material reference type rejected by generated reference rules");
    bad(fixture.registry.invoke("beam.assign_section",
                                fixture.context("wrong-section-ref"),
                                InputTraits<BeamAssignSectionInput>::to_value(
                                    {{EntityId("b0"), EntityId("b1")}, material})),
        ErrorCode::invalid_input,
        "wrong section reference rejects complete assignment");
    bad(fixture.registry.invoke("beam.assign_section",
                                fixture.context("missing-beam"),
                                InputTraits<BeamAssignSectionInput>::to_value(
                                    {{EntityId("b0"), EntityId("missing")}, section})),
        ErrorCode::entity_not_found,
        "missing second beam rejects entire edit");
    check(fixture.snapshot().info.revision == revision_before &&
              fixture.rows->generation == generation_before,
          "invalid references change no revision or persistent rows");
    check(fixture.snapshot().records.find<records::Beam>(EntityId("b0")) ==
              after_assignment.records.find<records::Beam>(EntityId("b0")),
          "failed compound assignment does not partially publish first beam");
    auto stale = fixture.context("stale-move");
    --*stale.expected_revision;
    bad(fixture.registry.invoke(
            "node.move", stale, InputTraits<NodeMoveInput>::to_value({EntityId("n0"), {-1, 0, 0}})),
        ErrorCode::revision_conflict,
        "actual expected revision is checked by application");
}
} // namespace
int main() {
    try {
        canonical_signatures();
        canonical_owned_boundaries();
        normalizers_and_optional_fields();
        material_entry_boundary_consistency();
        direct_retry_and_history();
        retained_create_identity();
        edits_sections_and_reference_failures();
        std::cout << "PASS: reusable record feature operations, normalization, references and "
                     "direct retries\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
