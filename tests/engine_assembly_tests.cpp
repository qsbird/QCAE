#include "engine_test_contribution.hpp"
#include "qcae/core.hpp"
#include "qcae/ipc_api.hpp"
#include "qcae/nastran_contribution.hpp"
#include "qcae/records_model_bridge.hpp"
#include <QJsonArray>
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace {
struct IncompleteCodec final : qcae::IModelCodec, qcae::IProfileProvider {
    qcae::ProfileDefinition profile;
    const qcae::ProfileDefinition& definition() const noexcept override {
        return profile;
    }
    qcae::ImportOutcome decode(const qcae::ImportRequest&) const override {
        return {};
    }
    qcae::ExportOutcome
    encode(const qcae::Model&, const qcae::EntityId&, const qcae::ProfileRef&) const override {
        return {};
    }
};
void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
void rejects_duplicate_contribution() {
    auto contributions = qcae::ipc::default_engine_contributions();
    contributions.push_back(contributions.front());
    bool rejected{};
    try {
        (void)qcae::ipc::assemble_engine(contributions);
    } catch (const qcae::RecordError&) {
        rejected = true;
    }
    check(rejected, "duplicate contribution must be rejected");
}
void rejects_duplicate_operation() {
    auto contributions = qcae::ipc::default_engine_contributions();
    const auto extra = contribution_test::contribution();
    contributions.push_back(extra);
    contributions.push_back({"test.duplicate-operation", {}, extra.operations});
    auto assembly = qcae::ipc::assemble_engine(contributions);
    qcae::MemoryApplication facade({}, {}, {}, {}, assembly.records);
    bool rejected{};
    try {
        qcae::ipc::TypedHost host(facade.record_application(), {}, assembly.operations);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    check(rejected, "duplicate operation must prevent host publication");
}
void rejects_reserved_operations() {
    using namespace qcae;
    using namespace qcae::operations;
    MemoryApplication facade;
    for (const auto* id : {"entity.fields",
                           "task.status",
                           "task.cancel",
                           "task.reconcile",
                           "project.create",
                           "entity.query",
                           "runtime.handshake"}) {
        bool rejected{};
        try {
            ipc::TypedHost host(
                facade.record_application(),
                {},
                [id](OperationRegistry& registry,
                     RecordApplication&,
                     std::function<TaskService&()>) {
                    return registry.declare_unavailable(
                        {id, 1, "test.reserved.v1", OperationEffect::read_only, {}, {}},
                        "Reserved-name probe");
                });
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        check(rejected, "contributed declarations must not shadow existing host operations");
    }
}
void freezes_registry_and_shares_application() {
    auto contributions = qcae::ipc::default_engine_contributions();
    contributions.push_back(contribution_test::contribution());
    auto assembly = qcae::ipc::assemble_engine(contributions);
    check(assembly.records->frozen(), "assembled records must be frozen");
    check(assembly.records->find(qcae::RecordTraits<contribution_test::Relation>::type_id),
          "test record must reach assembled registry");
    qcae::MemoryApplication facade({}, {}, {}, {}, assembly.records);
    const auto created = facade.create_document({"test"}, "Assembled", "create");
    check(created.ok(), "common project operation must remain usable");
    const auto view = facade.record_application().snapshot(created.value->document);
    check(view.ok() && view.value->records.registry() == assembly.records,
          "compatibility facade must use exactly the assembled registry");
    qcae::ipc::TypedHost host(facade.record_application(), {}, std::move(assembly.operations));
    check(host.supports("test.relation.create"), "contributed operation must reach typed host");
    const auto standard = qcae::ipc::assemble_engine(qcae::ipc::default_engine_contributions());
    check(!standard.records->find(qcae::RecordTraits<contribution_test::Relation>::type_id),
          "production records must exclude test contribution");
    qcae::ipc::TypedHost without(facade.record_application(), {}, standard.operations);
    check(!without.supports("test.relation.create"), "disabled operation must be absent");
}
void selects_only_installed_services() {
    using namespace qcae;
    const auto defaults = ipc::default_engine_contributions();
    const std::array platform{defaults.front()};
    const auto common = ipc::assemble_engine(platform);
    check(!common.model_codec() && !common.profile_definition() &&
              common.profile_codec_owner.empty() && common.render_owner.empty(),
          "platform-only assembly must not inherit compiled Nastran services");
    check(common.render.frozen() && common.render.find(RecordTraits<records::Tri3>::type_id),
          "platform-only assembly must retain the generic render factory");
    MemoryApplication facade({}, {}, {}, {}, common.records);
    ipc::TypedHost host(facade.record_application(), {}, common.operations);
    const auto catalog = ipc::dispatch(facade,
                                       {{"api_version", "1.1"},
                                        {"request_id", "platform-catalog"},
                                        {"operation", "capabilities.list"},
                                        {"parameters", QJsonObject{}}},
                                       {"test"},
                                       common.model_codec(),
                                       common.profile_definition(),
                                       nullptr,
                                       &host);
    check(
        catalog.value("status").toString() == "success" &&
            catalog.value("data").toObject().value("declared_solver_profiles").toArray().isEmpty(),
        "discovery must use the selected assembly's profile binding");
    check(host.supports("geometry.create_line") && !host.supports("nastran.ui"),
          "common operations must remain installed without Nastran UI");
    const auto normal = ipc::assemble_engine(defaults);
    const bool enabled = ipc::nastran_package_enabled();
    check((normal.model_codec() != nullptr) == enabled &&
              (normal.profile_definition() != nullptr) == enabled,
          "default ON/OFF codec selection changed");
    check(normal.render.entries().size() == (enabled ? 3u : 4u) &&
              normal.render_owner == (enabled ? "qcae.nastran" : ""),
          "default ON/OFF render factory or ownership changed");
    const auto coordinator = std::make_shared<ipc::NastranArtifactCoordinator>();
    ipc::EngineContribution codec_only;
    codec_only.id = "test.codec-only";
    codec_only.profile_codec = coordinator->codec_binding();
    const auto codec_selected = ipc::assemble_engine(std::array{codec_only});
    check(codec_selected.model_codec() == &coordinator->codec() &&
              codec_selected.profile_codec_owner == codec_only.id &&
              codec_selected.render_owner.empty(),
          "a callable codec binding is a valid standalone contribution");
    ipc::EngineContribution render_only;
    render_only.id = "test.render-only";
    render_only.render_factory = [] { return ipc::nastran_render_contributions(); };
    const auto render_selected = ipc::assemble_engine(std::array{render_only});
    check(!render_selected.model_codec() && render_selected.render_owner == render_only.id &&
              render_selected.render.entries().size() == 3,
          "a callable render factory is a valid standalone contribution");
}
void keeps_selected_codec_alive() {
    using namespace qcae;
    ipc::EngineAssembly selected;
    const IModelCodec* actual{};
    const IProfileProvider* actual_profile{};
    std::weak_ptr<ipc::NastranArtifactCoordinator> released;
    {
        auto coordinator = std::make_shared<ipc::NastranArtifactCoordinator>();
        released = coordinator;
        actual = &coordinator->codec();
        actual_profile = &coordinator->codec();
        const std::array contribution{ipc::nastran_engine_contribution(coordinator)};
        selected = ipc::assemble_engine(contribution);
    }
    // Remove other coordinator-owning callbacks so this checks the binding's own lifetime.
    selected.operations = {};
    selected.publisher_factory = {};
    selected.catalog.reset();
    check(released.expired() && selected.model_codec() == actual &&
              selected.profile_codec->profile.get() == actual_profile &&
              selected.profile_codec_owner == "qcae.nastran",
          "selected binding must retain the actual codec after coordinator wrapper release");
    const auto profile = selected.profile_definition()->reference;
    ImportRequest input{"cantilever.bdf", {}, profile, "engine-binding-test", "mm-N-MPa"};
    const auto fixtures = std::filesystem::path(__FILE__).parent_path() / "fixtures/nastran";
    for (const auto* path :
         {"cantilever.bdf", "mesh/nodes.bdf", "mesh/beams.bdf", "properties.bdf"}) {
        std::ifstream file(fixtures / path, std::ios::binary);
        check(static_cast<bool>(file), "codec binding fixture is missing");
        input.resources.push_back(
            {path, {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()}});
    }
    const auto decoded = selected.model_codec()->decode(input);
    check(decoded.report.complete && decoded.candidate && !decoded.candidate->analyses.empty(),
          "retained registered codec must perform a real import");
    const auto encoded = selected.model_codec()->encode(
        *decoded.candidate, decoded.candidate->analyses.front().id, profile);
    check(encoded.report.complete && encoded.artifact && encoded.artifact->profile == profile,
          "retained registered codec and definition must perform a real export");
    const auto registry = make_record_registry();
    const auto record = registry->make(records::Node{EntityId("projection"), {1, 2, 3}, {}});
    const auto projection = selected.render.find(RecordTraits<records::Node>::type_id);
    check(projection &&
              std::get<RenderPoint>(projection->project(record)).entity == EntityId("projection"),
          "registered render factory must retain a real callable projection");
}
void rejects_invalid_services() {
    using namespace qcae;
    const auto coordinator = std::make_shared<ipc::NastranArtifactCoordinator>();
    const auto valid = ipc::nastran_engine_contribution(coordinator);
    const auto rejects = [](std::vector<ipc::EngineContribution> contributions) {
        try {
            (void)ipc::assemble_engine(contributions);
        } catch (const RecordError& error) {
            return error.code() == ErrorCode::invalid_input && error.field() == "contribution";
        }
        return false;
    };
    auto invalid = valid;
    invalid.profile_codec->codec.reset();
    check(rejects({invalid}), "null codec binding must be rejected");
    invalid = valid;
    invalid.profile_codec->profile.reset();
    check(rejects({invalid}), "null profile binding must be rejected");
    invalid = valid;
    invalid.profile_codec->profile = std::make_shared<NastranCodec>();
    check(rejects({invalid}), "unrelated profile/codec lifetimes must be rejected");
    invalid = valid;
    invalid.profile_codec = ipc::EngineCodecBinding{
        std::shared_ptr<const IModelCodec>(std::shared_ptr<const IModelCodec>{},
                                           &coordinator->codec()),
        std::shared_ptr<const IProfileProvider>(std::shared_ptr<const IProfileProvider>{},
                                                &coordinator->codec())};
    check(rejects({invalid}), "borrowed aliases without an owner must be rejected");
    invalid = valid;
    const auto incomplete = std::make_shared<IncompleteCodec>();
    invalid.profile_codec = ipc::EngineCodecBinding{incomplete, incomplete};
    check(rejects({invalid}), "incomplete profile definition must be rejected");
    invalid = valid;
    invalid.id = "test.second-profile";
    check(rejects({valid, invalid}), "distinct contribution IDs cannot install two codecs");
    invalid.profile_codec.reset();
    check(rejects({valid, invalid}),
          "distinct contribution IDs cannot install two render factories");
    invalid = valid;
    invalid.render_factory = [] { return RenderContributions{}; };
    check(rejects({invalid}), "unfrozen renderer must be rejected");
    invalid.render_factory = [] {
        RenderContributions empty;
        empty.freeze();
        return empty;
    };
    check(rejects({invalid}), "empty renderer must be rejected");
}
void invokes_discovered_package_contributions() {
    using namespace qcae;
    auto assembly = ipc::assemble_engine(ipc::default_engine_contributions());
    bool premature_rejected{};
    try {
        (void)assembly.catalog->describe();
    } catch (const std::logic_error&) {
        premature_rejected = true;
    }
    check(premature_rejected, "discovery must wait for actual operation registration");
    MemoryApplication facade({}, {}, {}, {}, assembly.records);
    ipc::TypedHost host(facade.record_application(), {}, assembly.operations);
    const auto& registered = assembly.catalog->entries();
    const auto package = std::find_if(registered.begin(), registered.end(), [](const auto& entry) {
        return entry.id == "qcae.nastran";
    });
    if (!ipc::nastran_package_enabled()) {
        check(package == registered.end(), "disabled package must have no registration group");
        check(!assembly.catalog->describe().isEmpty(), "common contributions must remain listed");
        return;
    }
    check(package != registered.end() && package->core_rules.size() == 1 &&
              !package->operations.empty() && package->profile_codec &&
              package->validators.size() == 1 && package->ui_operations.size() == 1 &&
              package->render.size() == 3,
          "registered package must expose actual bindings for all six categories");
    const auto& ports = *package->profile_codec;
    check(ports.codec.get() == assembly.model_codec() &&
              ports.profile.get() == assembly.profile_codec->profile.get(),
          "discovered codec must be exactly the selected production ports");
    const auto profile = ports.profile->definition().reference;
    ImportRequest input{"cantilever.bdf", {}, profile, "contribution-fixture", "mm-N-MPa"};
    const auto fixtures = std::filesystem::path(__FILE__).parent_path() / "fixtures/nastran";
    for (const auto* path :
         {"cantilever.bdf", "mesh/nodes.bdf", "mesh/beams.bdf", "properties.bdf"}) {
        std::ifstream file(fixtures / path, std::ios::binary);
        check(static_cast<bool>(file), "package contribution fixture is missing");
        input.resources.push_back(
            {path, {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()}});
    }
    const auto decoded = ports.codec->decode(input);
    check(decoded.report.complete && decoded.candidate && !decoded.candidate->analyses.empty(),
          "discovered codec must actually import engineering records");
    const auto& model = *decoded.candidate;
    const auto analysis = model.analyses.front().id;
    const auto view = records_from_model(model, assembly.records);
    const auto encoded = ports.codec->encode(model, analysis, profile);
    const auto plan = package->validators.front().validate_export(view, analysis);
    check(encoded.report.complete && encoded.artifact && plan.ok() &&
              plan.value->identities.size() == encoded.artifact->identities.size() &&
              plan.value->resources.size() == encoded.artifact->resources.size(),
          "discovered codec and validator must produce the same controlled export");
    for (std::size_t index = 0; index < plan.value->identities.size(); ++index) {
        const auto& actual = plan.value->identities[index];
        const auto& expected = encoded.artifact->identities[index];
        check(actual.entity == expected.entity && actual.name_space == expected.name_space &&
                  actual.number == expected.number,
              "discovered validator must preserve the codec's engineering number map");
    }
    const auto& rule = package->core_rules.front();
    check(rule.id == "qcae.nastran.controlled_subset", "core rule must retain its stable identity");
    assembly.records->rules().at(rule.rule_index)(view);
    auto unsupported = model;
    unsupported.analyses.front().target.analysis_kind = "unsupported";
    const auto invalid = records_from_model(unsupported, make_record_registry());
    bool rule_rejected{};
    try {
        assembly.records->rules().at(rule.rule_index)(invalid);
    } catch (const RecordError& error) {
        rule_rejected = error.code() == ErrorCode::invalid_input;
    }
    check(rule_rejected, "enumerated core rule must reject a real unsupported analysis");
    const auto ui =
        host.dispatch({{"api_version", "1.1"},
                       {"request_id", "discovered-ui"},
                       {"operation", QString::fromStdString(package->ui_operations.front())},
                       {"requested_version", 1},
                       {"parameters", QJsonObject{}}},
                      {"contribution-test"});
    check(ui.value("status").toString() == "success" &&
              ui.value("data").toObject().value("operation").toString() == "model.export",
          "discovered UI reference must invoke the actual registered handler");
    const auto described = host.capabilities();
    for (const auto& operation : package->operations) {
        bool matched{};
        for (const auto& value : described) {
            const auto descriptor = value.toObject();
            if (descriptor.value("name").toString().toStdString() ==
                operation.definition.operation_id) {
                matched = descriptor.value("available").toBool() == operation.available &&
                          descriptor.value("version").toInteger() ==
                              static_cast<qint64>(operation.definition.version) &&
                          descriptor.value("schema_id").toString().toStdString() ==
                              operation.definition.schema_id;
                break;
            }
        }
        check(matched, "operation ownership must come from actual installed descriptors");
    }
    for (const auto& projection : package->render) {
        if (projection.record_type == RecordTraits<records::GeometryLine>::type_id) {
            const auto record = assembly.records->make(records::GeometryLine{
                records::GeometryId("discovered-line"), {0, 0, 0}, {2, 0, 0}, 1});
            check(std::get<RenderGeometryLine>(projection.project(record)).entity ==
                      EntityId("discovered-line"),
                  "enumerated geometry projector must actually preserve its identity");
        } else {
            Record record;
            view.visit(projection.record_type, [&](const Record& candidate) {
                if (!record)
                    record = candidate;
            });
            check(static_cast<bool>(record), "discovered projector fixture is missing");
            const auto projected = projection.project(record);
            const auto identity =
                std::visit([](const auto& value) { return value.entity.value; }, projected);
            check(identity == record->key().identity,
                  "enumerated projector must preserve the record identity");
        }
    }
}
void rejects_invalid_discovery_bindings() {
    using namespace qcae;
    const auto rejects = [](ipc::EngineContribution contribution) {
        try {
            (void)ipc::assemble_engine(std::array{std::move(contribution)});
        } catch (const RecordError& error) {
            return error.code() == ErrorCode::invalid_input && error.field() == "contribution";
        }
        return false;
    };
    auto invalid = contribution_test::contribution();
    invalid.core_rule_ids = {"one", "two"};
    check(rejects(invalid), "named rule counts must match real registrations");
    invalid.core_rule_ids = {""};
    check(rejects(invalid), "empty rule identity must be rejected");
    invalid = contribution_test::contribution();
    invalid.validators = {{"validator", 1, {}}};
    check(rejects(invalid), "empty validator callable must be rejected");
    invalid = contribution_test::contribution();
    invalid.validators = {{"validator", 0, [](const DocumentView&, const EntityId&) {
                               return Result<ArtifactPlan>{};
                           }}};
    check(rejects(invalid), "zero validator version must be rejected");
    auto named_rule = contribution_test::contribution();
    named_rule.core_rule_ids = {"qcae.model.relations"};
    bool duplicate_rule_rejected{};
    try {
        (void)ipc::assemble_engine(
            std::array{ipc::default_engine_contributions().front(), named_rule});
    } catch (const RecordError& error) {
        duplicate_rule_rejected = error.code() == ErrorCode::invalid_input;
    }
    check(duplicate_rule_rejected, "different contributors must not reuse a named rule identity");
    ipc::EngineContribution validator_only;
    validator_only.id = "validator-owner";
    validator_only.validators = {{"same-validator", 1, [](const DocumentView&, const EntityId&) {
                                      return Result<ArtifactPlan>{};
                                  }}};
    auto second_validator = validator_only;
    second_validator.id = "other-validator-owner";
    bool duplicate_validator_rejected{};
    try {
        (void)ipc::assemble_engine(std::array{validator_only, second_validator});
    } catch (const RecordError& error) {
        duplicate_validator_rejected = error.code() == ErrorCode::invalid_input;
    }
    check(duplicate_validator_rejected,
          "different contributors must not reuse a validation binding identity");
    for (const auto* reference : {"missing.operation", "test.relation.create"}) {
        auto contribution = contribution_test::contribution();
        contribution.ui_operations = {reference};
        const auto platform = ipc::default_engine_contributions().front();
        auto assembly = ipc::assemble_engine(std::array{platform, std::move(contribution)});
        MemoryApplication facade({}, {}, {}, {}, assembly.records);
        bool rejected{};
        try {
            ipc::TypedHost host(facade.record_application(), {}, assembly.operations);
        } catch (const RecordError& error) {
            rejected = error.code() == ErrorCode::invalid_input;
        }
        check(rejected,
              "UI references must be available read-only registrations of their contributor");
        bool catalog_hidden{};
        try {
            (void)assembly.catalog->entries();
        } catch (const std::logic_error&) {
            catalog_hidden = true;
        }
        check(catalog_hidden, "failed registration must not publish a partial catalog");
    }
}
} // namespace
int main() {
    try {
        rejects_duplicate_contribution();
        rejects_duplicate_operation();
        rejects_reserved_operations();
        freezes_registry_and_shares_application();
        selects_only_installed_services();
        keeps_selected_codec_alive();
        rejects_invalid_services();
        invokes_discovered_package_contributions();
        rejects_invalid_discovery_bindings();
        std::cout << "PASS: static engine assembly, duplicate rejection and shared authority\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
