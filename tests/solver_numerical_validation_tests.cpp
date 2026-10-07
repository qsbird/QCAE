#include "solver_validation_store.hpp"
#include "solver_result_store.hpp"
#include "typed_json.hpp"
#include "qcae/nastran_codec.hpp"
#include "qcae/records.hpp"
#include "qcae/records_model_bridge.hpp"
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>

namespace {
using namespace qcae;
using namespace qcae::ipc;
namespace detail = qcae::ipc::detail;
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
template <class T> T good(Result<T> result) {
    require(result.ok(), result.error ? result.error->message.c_str() : "Missing value");
    return std::move(*result.value);
}
StoredSolverResult synthetic_published(std::string task_id = "synthetic-unit",
                                       bool mystran = false) {
    NastranCodec codec;
    ImportRequest request;
    request.root_resource = "cantilever.bdf";
    request.source_profile = codec.definition().reference;
    request.source_model_id = "synthetic-validation-unit";
    request.unit_system = "mm-N-MPa";
    for (const char* name : {"cantilever.bdf", "nodes.bdf", "properties.bdf", "beams.bdf"}) {
        std::ifstream stream(std::string(QCAE_SOURCE_DIR) +
                             "/tests/fixtures/nastran-real-benchmark-v3/" + name);
        require(stream.good(), "Preregistered fixture is missing");
        request.resources.push_back({name, {std::istreambuf_iterator<char>(stream), {}}});
    }
    const auto imported = codec.decode(request);
    require(imported.candidate && imported.report.complete && imported.report.issues.empty(),
            "Original benchmark must pass the production import codec");
    const auto& model = *imported.candidate;
    const auto exported = codec.encode(model, model.analyses.front().id, request.source_profile);
    require(exported.artifact && exported.report.complete,
            "Original benchmark needs its production frozen export mapping");
    const auto view = records_from_model(
        model,
        make_record_registry(),
        {{DocumentId("original-unit-document"), DocumentEpoch("original-unit-epoch")}, 20});
    SolverOwnedRun origin;
    origin.principal = "explicit-synthetic-validation-unit";
    origin.idempotency_key = "synthetic-start";
    origin.test_only = true;
    origin.startup_intent_persisted = true;
    origin.source_input = good(features::analysis::freeze_analysis_input(
        view, model.analyses.front().id, request.source_profile, exported.artifact->identities));
    const auto& input = origin.source_input;
    auto& run = origin.run;
    run.request.task_id = std::move(task_id);
    run.request.run_id = "run-" + run.request.task_id;
    run.request.run_directory = "/private/tmp/not-an-executed-solver/" + run.request.run_id;
    run.request.input = {input.version.document.id,
                         input.version.document.epoch,
                         input.version.revision,
                         input.analysis,
                         input.target.profile,
                         "artifact-synthetic-unit",
                         artifact_sha256("explicit synthetic input manifest"),
                         artifact_sha256(input.input_signature),
                         solver_export_identity_digest(input),
                         "synthetic-unit-export",
                         "qcae.nastran.static-f06.v1",
                         features::analysis::encode_frozen_analysis_input(input)};
    run.request.configuration = {"synthetic-unit",
                                 "/not-executed/test-only",
                                 {},
                                 "test-only",
                                 "test-only-process",
                                 "test-only-1",
                                 "explicit synthetic unit, no OS execution observation",
                                 artifact_sha256("synthetic config"),
                                 {"result.f06"}};
    origin.signature = solver_run_signature(input.analysis,
                                            run.request.input.artifact_id,
                                            run.request.configuration.id,
                                            run.request.configuration.configuration_digest);
    run.sequence = 3;
    run.execution = SolverExecutionState::exited;
    run.process = SolverProcessIdentity{1, "synthetic-record-only-not-a-real-process"};
    run.exit_code = 0;
    run.output_state = SolverOutputState::collected;
    origin.result_reader = SolverResultReaderConfiguration{};
    origin.result_reader->resource = "result.f06";
    std::ostringstream f06;
    f06.precision(17);
    f06 << "1 EXPLICIT SYNTHETIC UNIT PAGE 1\nSUBCASE 1\n"
           "D I S P L A C E M E N T V E C T O R\nPOINT ID. TYPE T1 T2 T3 R1 R2 R3\n";
    std::uint64_t support{};
    for (const auto& node : model.nodes) {
        const auto mapping = std::find_if(input.identities.begin(),
                                          input.identities.end(),
                                          [&](const auto& item) { return item.entity == node.id; });
        require(mapping != input.identities.end(), "Synthetic GRID lost its original mapping");
        const double x = node.position.x;
        f06 << mapping->number << " G 0 " << -x * x * (3000 - x) / (6 * 210000 * 833.333)
            << " 0 0 0 " << -x * (2000 - x) / (2 * 210000 * 833.333) << '\n';
        if (x == 0)
            support = mapping->number;
    }
    f06 << "F O R C E S O F S I N G L E - P O I N T C O N S T R A I N T\n"
           "POINT ID. TYPE T1 T2 T3 R1 R2 R3\n"
        << support << " G 0 1 0 0 0 1000\n";
    std::vector<TextResource> resources{{"result.f06", f06.str()},
                                        {"runner.stdout", "explicit synthetic unit\n"},
                                        {"runner.stderr", ""}};
    if (mystran) {
        // Fabricated external-classified fields exercise binding/codec only. The false
        // synthetic flag never comes from a probe, and this helper never runs a process.
        origin.test_only = false;
        origin.principal = "explicit-synthetic-mystran-validation-unit-no-process-proof";
        run.request.input.result_reader_version = "qcae.mystran.static-f06.v1";
        run.request.configuration = {"synthetic-mystran-validation-unit",
                                     "/not-executed/synthetic-mystran-validation-program",
                                     {"cantilever.bdf"},
                                     "MYSTRAN",
                                     "MYSTRAN",
                                     "19.0.0",
                                     "fabricated version fields, not a process observation",
                                     artifact_sha256("synthetic config before binding"),
                                     {"work/cantilever.F06", "work/cantilever.ERR"}};
        SolverVersionEvidence version;
        version.protocol = "mystran.version.v1";
        version.reported_version = "19.0.0";
        version.executable.inode = 1;
        version.executable.byte_length = 1;
        version.executable.sha256 = artifact_sha256("synthetic executable identity");
        version.process = {1, "synthetic-version-identity-not-an-observed-process"};
        version.stdout_bytes = 1;
        version.stdout_sha256 = artifact_sha256("synthetic version bytes");
        version.stderr_sha256 = artifact_sha256("");
        version.synthetic = false;
        origin.version_probe = version;
        origin.declaration_digest = artifact_sha256("synthetic version declaration");
        run.request.configuration.configuration_digest =
            solver_version_configuration_digest(origin.declaration_digest, version);
        origin.signature = solver_run_signature(input.analysis,
                                                run.request.input.artifact_id,
                                                run.request.configuration.id,
                                                run.request.configuration.configuration_digest);
        origin.result_reader->reader_version = "qcae.mystran.static-f06.v1";
        origin.result_reader->resource = "work/cantilever.F06";
        std::ifstream reference(std::string(QCAE_SOURCE_DIR) +
                                "/tests/fixtures/mystran/cantilever-19.0.0.F06");
        require(reference.good(), "Sanitized MYSTRAN parser reference is missing");
        resources = {{"work/cantilever.F06", {std::istreambuf_iterator<char>(reference), {}}},
                     {"work/cantilever.ERR", "explicit synthetic diagnostics, no process proof\n"},
                     {"runner.stdout", "explicit synthetic process record, no execution proof\n"},
                     {"runner.stderr", ""}};
    }
    for (const auto& resource : resources)
        run.outputs.push_back(
            {resource.path, resource.text.size(), artifact_sha256(resource.text)});
    auto parsed = good(detail::prepare_solver_result(origin, *origin.result_reader, resources));
    auto artifact = detail::solver_result_artifact(
        parsed, std::filesystem::path(run.request.run_directory) / ".qcae-result");
    // This is a synthetic representation used to test bindings/codec, not actual publication.
    return {std::move(parsed), std::move(artifact), true};
}
detail::SolverValidationFact complete_fact(const StoredSolverResult& stored, std::string key) {
    auto fact =
        good(detail::prepare_solver_validation(stored, NastranCodec{}.definition().reference));
    fact.idempotency_key = std::move(key);
    fact.ordinal = 1;
    const auto& profile = fact.registered_profile;
    fact.signature = record_wire::strings(std::array<std::string, 8>{"request-document",
                                                                     "request-epoch",
                                                                     "21",
                                                                     fact.run_id,
                                                                     fact.principal,
                                                                     profile.profile_id,
                                                                     profile.profile_version,
                                                                     profile.definition_digest});
    return fact;
}
// Keep this test projection equal to production validation_summary's fields. It
// verifies linked report encoding/byte accounting; public get_result's overflow
// guard is separately exercised by the carried-project IPC regression.
operations::Value summary_projection(const detail::SolverValidationFact& fact,
                                     std::string_view identity) {
    using operations::Value;
    const auto mismatched = std::count_if(fact.report.components.begin(),
                                          fact.report.components.end(),
                                          [](const auto& component) { return !component.matched; });
    return Value(Value::Object{
        {"validation_id", Value(std::string(identity))},
        {"ordinal", Value(std::to_string(fact.ordinal))},
        {"run_id", Value(fact.run_id)},
        {"result_id", Value(fact.result_id)},
        {"task_id", Value(fact.task_id)},
        {"source_kind", Value(fact.test_only ? "test_process" : "external_solver")},
        {"check_kind", Value(fact.test_only ? "synthetic_comparison" : "numerical_validation")},
        {"numerical_stage", Value(detail::solver_validation_numerical_stage(fact))},
        {"comparison", Value(fact.report.matched ? "matched" : "mismatch")},
        {"component_count", Value(std::to_string(fact.report.components.size()))},
        {"mismatched_components", Value(std::to_string(mismatched))},
        {"reference_id", Value(fact.report.reference_id)},
        {"reference_input", Value(fact.report.reference_input)},
        {"original_parsed_sha256", Value(fact.parsed_sha256)},
        {"original_result_manifest_sha256", Value(fact.manifest_sha256)}});
}
} // namespace
int main() {
    try {
        unsigned count{};
        const auto check = [&](bool value, const char* message) {
            require(value, message);
            ++count;
        };
        const auto rejects = [&](auto action) {
            try {
                action();
                return false;
            } catch (const std::exception&) {
                return true;
            }
        };
        const auto stored = synthetic_published();
        const auto profile = NastranCodec{}.definition().reference;
        const auto fact = complete_fact(stored, "comparison");
        check(fact.report.matched && fact.report.components.size() == 132 &&
                  detail::solver_validation_numerical_stage(fact) == "not_run",
              "A matched synthetic report cannot become engineering acceptance");
        const auto image = detail::solver_validation_row(fact);
        check(image->payload->size() <= 65536 && image->key.identity.size() == 108,
              "The complete 132-component fact must fit its declared bounds");
        const auto decoded = detail::decode_solver_validation_row(*image);
        detail::validate_solver_validation_link(decoded, stored, profile);
        check(*detail::solver_validation_row(decoded)->payload == *image->payload,
              "A full report must retain exact canonical bytes");
        const auto mystran_stored = synthetic_published("synthetic-mystran-unit", true);
        const auto mystran_fact = complete_fact(mystran_stored, "mystran-codec-comparison");
        const auto mystran_image = detail::solver_validation_row(mystran_fact);
        const auto mystran_decoded = detail::decode_solver_validation_row(*mystran_image);
        detail::validate_solver_validation_link(mystran_decoded, mystran_stored, profile);
        check(
            mystran_decoded.report.matched && mystran_decoded.report.components.size() == 132 &&
                mystran_decoded.report.reference_input == "nastran-real-benchmark-v3" &&
                mystran_decoded.reader.reader_version == "qcae.mystran.static-f06.v1" &&
                mystran_decoded.reader.resource == "work/cantilever.F06" &&
                mystran_decoded.frozen_input_sha256 ==
                    artifact_sha256(features::analysis::encode_frozen_analysis_input(
                        mystran_stored.parsed.origin.source_input)) &&
                *detail::solver_validation_row(mystran_decoded)->payload == *mystran_image->payload,
            "MYSTRAN numerical fact codec lost its reader, full report or original input binding");
        check(mystran_stored.parsed.origin.run.numerical_validation ==
                  SolverNumericalState::not_run,
              "A separate MYSTRAN report mutated the original unvalidated execution fact");
        for (unsigned variant = 0; variant < 4; ++variant) {
            auto altered = mystran_fact;
            if (variant == 0)
                altered.reader.reader_version = "qcae.nastran.static-f06.v1";
            else if (variant == 1)
                altered.frozen_input_sha256 = artifact_sha256("different synthetic frozen input");
            else if (variant == 2)
                altered.parsed_sha256 = artifact_sha256("different synthetic MYSTRAN fields");
            else {
                auto& component = altered.report.components.back();
                component.actual = component.expected + std::max(1.0, component.tolerance * 4);
                component.absolute_error = std::abs(component.actual - component.expected);
                component.matched = false;
                altered.report.matched = false;
            }
            const auto self_consistent =
                detail::decode_solver_validation_row(*detail::solver_validation_row(altered));
            check(rejects([&] {
                      detail::validate_solver_validation_link(
                          self_consistent, mystran_stored, profile);
                  }),
                  "A coherent MYSTRAN report changed reader, input, parsed source or components");
        }
        auto mystran_invalid = mystran_fact;
        mystran_invalid.reader.reader_version = "qcae.mystran.static-f06.v2";
        check(rejects([&] { (void)detail::solver_validation_row(mystran_invalid); }),
              "An unknown MYSTRAN numerical reader version crossed the persisted codec");
        auto altered_mystran_source = mystran_stored;
        altered_mystran_source.parsed.origin.run.numerical_validation =
            SolverNumericalState::passed;
        check(!detail::prepare_solver_validation(altered_mystran_source, profile).ok(),
              "A numerical report rewrote the immutable MYSTRAN execution observation");
        for (std::size_t component = 0; component < 132; ++component) {
            auto changed = fact;
            auto& item = changed.report.components[component];
            item.actual = item.expected + std::max(1.0, item.tolerance * 4);
            item.absolute_error = std::abs(item.actual - item.expected);
            item.matched = false;
            changed.report.matched = false;
            const auto self_consistent =
                detail::decode_solver_validation_row(*detail::solver_validation_row(changed));
            check(rejects([&] {
                      detail::validate_solver_validation_link(self_consistent, stored, profile);
                  }),
                  "A self-consistent changed component escaped original-source revalidation");
        }
        for (unsigned variant = 0; variant < 10; ++variant) {
            auto changed = fact;
            switch (variant) {
            case 0:
                changed.test_only = false;
                break;
            case 1:
                changed.origin_sha256 = artifact_sha256("wrong origin");
                break;
            case 2:
                changed.parsed_sha256 = artifact_sha256("wrong qcr");
                break;
            case 3:
                changed.manifest_sha256 = artifact_sha256("wrong manifest");
                break;
            case 4:
                changed.frozen_input_sha256 = artifact_sha256("wrong input");
                break;
            case 5:
                changed.result_id += "-wrong";
                break;
            case 6:
                changed.task_id += "-wrong";
                break;
            case 7:
                changed.reader.resource = "wrong.f06";
                break;
            case 8:
                changed.report.components[0].tolerance *= 2;
                break;
            case 9:
                changed.report.components[0].expected += .000001;
                changed.report.components[0].absolute_error = std::abs(
                    changed.report.components[0].actual - changed.report.components[0].expected);
                break;
            }
            check(
                rejects([&] { detail::validate_solver_validation_link(changed, stored, profile); }),
                "Validation accepted a changed binding, tolerance or expected value");
        }
        auto changed = fact;
        changed.report.components[0] = changed.report.components[1];
        check(rejects([&] { (void)detail::solver_validation_row(changed); }),
              "A duplicate six-component key was accepted");
        changed = fact;
        changed.report.components[0].actual = std::numeric_limits<double>::infinity();
        check(rejects([&] { (void)detail::solver_validation_row(changed); }),
              "A non-finite component was accepted");
        changed = fact;
        changed.report.components[0].unit = "N";
        check(rejects([&] { (void)detail::solver_validation_row(changed); }),
              "A wrong displacement unit was accepted");
        auto unconfigured = stored;
        unconfigured.published = false;
        check(!detail::prepare_solver_validation(unconfigured, profile).ok(),
              "An unpublished parsed intent became numerical acceptance");
        auto wrong_profile = profile;
        wrong_profile.definition_digest = artifact_sha256("unregistered profile");
        check(!detail::prepare_solver_validation(stored, wrong_profile).ok(),
              "A matching profile id/version with wrong digest was accepted");
        detail::SolverValidationIndex index{fact.run_id, fact.principal, {image->key.identity}};
        const auto head = detail::solver_validation_index_row(index);
        check(head->key.identity != image->key.identity &&
                  detail::decode_solver_validation_index(*head).report_rows == index.report_rows,
              "Index and report identities must remain distinct and reversible");
        for (unsigned position = 1; position < 16; ++position)
            index.report_rows.push_back(detail::solver_validation_identity(
                fact.run_id, {fact.principal}, "key-" + std::to_string(position)));
        check(detail::solver_validation_index_row(index)->payload->size() <= 8192,
              "Sixteen index entries exceed their declared byte bound");
        index.report_rows.push_back(
            detail::solver_validation_identity(fact.run_id, {fact.principal}, "seventeenth"));
        check(rejects([&] { (void)detail::solver_validation_index_row(index); }),
              "The seventeenth report escaped the bounded index");
        index.report_rows.pop_back();
        index.report_rows[1] = index.report_rows[0];
        check(rejects([&] { (void)detail::solver_validation_index_row(index); }),
              "Duplicate index references were accepted");
        RecordApplicationOptions options;
        options.registry = make_record_registry();
        options.owned_row_handlers = {detail::solver_validation_row_handler()};
        RecordApplication app(options);
        const Caller caller{fact.principal};
        const auto created = good(app.create_document(caller, "CAS synthetic report", "create"));
        const std::array<OwnedRowUpdate, 2> first{{{image->key, {}, image}, {head->key, {}, head}}};
        good(app.update_owned_rows(caller, created.document, first));
        const auto competing = complete_fact(stored, "competing");
        const auto competing_row = detail::solver_validation_row(competing);
        const auto competing_head = detail::solver_validation_index_row(
            {fact.run_id, fact.principal, {competing_row->key.identity}});
        const std::array<OwnedRowUpdate, 2> second{
            {{competing_row->key, {}, competing_row}, {competing_head->key, {}, competing_head}}};
        check(!app.update_owned_rows(caller, created.document, second).ok(),
              "Two writers sharing an empty head both claimed ordinal one");
        const auto rows =
            good(app.owned_rows_with_prefix(created.document,
                                            StoreSpace::artifact_record,
                                            detail::solver_validation_prefix(fact.run_id),
                                            17));
        check(rows.rows.size() == 2 && !rows.overflow &&
                  good(app.current_document()).revision == created.revision,
              "Losing report CAS left a partial row or changed model revision");
        check(!app.update_owned_rows(caller, created.document, first).ok(),
              "Direct duplicate insertion bypassed expected-row CAS");
        RecordApplication mystran_app(options);
        const Caller mystran_caller{mystran_fact.principal};
        const auto mystran_document =
            good(mystran_app.create_document(mystran_caller, "Synthetic MYSTRAN codec", "create"));
        const auto mystran_head = detail::solver_validation_index_row(
            {mystran_fact.run_id, mystran_fact.principal, {mystran_image->key.identity}});
        const std::array<OwnedRowUpdate, 2> mystran_updates{
            {{mystran_image->key, {}, mystran_image}, {mystran_head->key, {}, mystran_head}}};
        good(mystran_app.update_owned_rows(
            mystran_caller, mystran_document.document, mystran_updates));
        const auto mystran_rows = good(mystran_app.owned_rows_with_prefix(
            mystran_document.document,
            StoreSpace::artifact_record,
            detail::solver_validation_prefix(mystran_fact.run_id),
            17));
        const auto persisted_mystran =
            std::find_if(mystran_rows.rows.begin(), mystran_rows.rows.end(), [&](const auto& row) {
                return row->key == mystran_image->key;
            });
        check(mystran_rows.rows.size() == 2 && !mystran_rows.overflow &&
                  persisted_mystran != mystran_rows.rows.end() &&
                  *(*persisted_mystran)->payload == *mystran_image->payload &&
                  good(mystran_app.current_document()).revision == mystran_document.revision,
              "MYSTRAN immutable report/index persistence lost bytes or changed model revision");
        detail::validate_solver_validation_link(
            detail::decode_solver_validation_row(**persisted_mystran), mystran_stored, profile);
        for (const bool controls : {false, true}) {
            const std::string task_id(100, controls ? '\x01' : 'a');
            const auto linked_source = synthetic_published(task_id);
            operations::Value::Array summaries;
            for (std::uint64_t ordinal = 1; ordinal <= 16; ++ordinal) {
                auto linked = complete_fact(linked_source, "wire-" + std::to_string(ordinal));
                linked.ordinal = ordinal;
                const auto linked_row = detail::solver_validation_row(linked);
                const auto decoded_linked = detail::decode_solver_validation_row(*linked_row);
                detail::validate_solver_validation_link(decoded_linked, linked_source, profile);
                check(decoded_linked.test_only && decoded_linked.report.components.size() == 132 &&
                          detail::solver_validation_numerical_stage(decoded_linked) == "not_run",
                      "Wire quota samples must remain fully linked synthetic comparisons");
                summaries.push_back(summary_projection(decoded_linked, linked_row->key.identity));
            }
            const auto wire = detail::typed_json_array(summaries);
            const auto bytes = QJsonDocument(wire).toJson(QJsonDocument::Compact);
            const auto canonical = good(operations::canonical_value(operations::Value(summaries)));
            check(detail::typed_json_array_bytes(summaries) ==
                      static_cast<std::size_t>(bytes.size()),
                  "Quota accounting differs from the production compact JSON serializer");
            check(wire.first().toObject()["task_id"].toString().toStdString() == task_id,
                  "JSON encoding changed a legal original task identity");
            check(controls ? bytes.size() > 32768 : bytes.size() < 32768,
                  "Sixteen legal summaries did not cross the intended actual JSON byte boundary");
            check(canonical.size() < 32768,
                  "The former binary count no longer demonstrates JSON escaping expansion");
            std::cout << "Linked wire samples: controls=" << controls
                      << "; reports=" << summaries.size() << "; JSON_bytes=" << bytes.size()
                      << "; canonical_bytes=" << canonical.size() << '\n';
        }
        std::cout << "PASS " << count
                  << " checks; synthetic comparison/codec/CAS only; real_solver=false; "
                     "numerical_acceptance=false\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
