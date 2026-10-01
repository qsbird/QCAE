#include "solver_result_store.hpp"
#include "qcae/record_application.hpp"
#include "qcae/records.hpp"
#include <csignal>
#include <fstream>
#include <iostream>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>

namespace {
namespace fs = std::filesystem;
using namespace qcae;
using namespace qcae::ipc;
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
template <class T> T good(Result<T> result) {
    require(result.ok(), result.error ? result.error->message.c_str() : "Missing result");
    return std::move(*result.value);
}
features::analysis::FrozenAnalysisInput frozen_input() {
    RecordApplicationOptions options;
    options.registry = make_record_registry();
    RecordApplication app(options);
    const Caller caller{"explicit-test-only-result-unit"};
    const auto created = good(app.create_document(caller, "Synthetic result unit", "create"));
    const ProfileRef profile{"explicit-test-only-result-profile", "1", "not-a-vendor-proof"};
    good(app.execute(
        caller,
        {created.document, created.revision},
        "test.seed",
        "seed",
        [profile](const DocumentView& view, const RecordIdentityAllocator&) {
            EditSession edit(view);
            edit.put(records::Material{EntityId("material"), "Unit fixture", 210000, .3});
            edit.put(records::BeamSection{
                EntityId("section"), "Unit", EntityId("material"), 100, 833.333, 833.333, 1400});
            edit.put(records::Node{EntityId("root-stable"), {0, 0, 0}, {}});
            edit.put(records::Node{EntityId("tip-stable"), {1000, 0, 0}, {}});
            edit.put(records::Beam{EntityId("beam"),
                                   EntityId("section"),
                                   {EntityId("root-stable"), EntityId("tip-stable")},
                                   {0, 1, 0},
                                   {}});
            edit.put(records::NodalForce{EntityId("force"), EntityId("tip-stable"), {0, -1, 0}});
            edit.put(
                records::Constraint{EntityId("constraint"), {EntityId("root-stable")}, "123456"});
            edit.put(records::LoadCase{
                EntityId("case"), "LC1", {EntityId("force")}, {EntityId("constraint")}});
            edit.put(records::AnalysisDefinition{EntityId("analysis"),
                                                 "Synthetic static",
                                                 {profile, "linear_static"},
                                                 {},
                                                 {},
                                                 std::vector<EntityId>{EntityId("case")}});
            return Result<RecordPreparedOperation>{
                Status::success,
                RecordPreparedOperation{
                    edit.prepare(), "Explicit test-only fixture", EntityId{}, "seed", 0, false},
                {}};
        },
        "seed"));
    const auto snapshot = good(app.snapshot(good(app.current_document()).document));
    const std::vector<ExportIdentifier> mapping{{EntityId("root-stable"), "GRID", 91},
                                                {EntityId("tip-stable"), "GRID", 7},
                                                {EntityId("material"), "MAT1", 1},
                                                {EntityId("section"), "PBAR", 1},
                                                {EntityId("beam"), "CBAR", 1},
                                                {EntityId("force"), "FORCE", 1},
                                                {EntityId("constraint"), "SPC1", 1}};
    return good(features::analysis::freeze_analysis_input(
        snapshot.records, EntityId("analysis"), profile, mapping));
}
const std::string synthetic = "1 EXPLICIT SYNTHETIC RESULT UNIT PAGE 1\nSUBCASE 1\n"
                              "D I S P L A C E M E N T V E C T O R\n"
                              "POINT ID. TYPE T1 T2 T3 R1 R2 R3\n"
                              "91 G 0 0 0 0 0 0\n7 G 0 -1.904763 0 0 0 -0.002857144\n"
                              "F O R C E S O F S I N G L E - P O I N T C O N S T R A I N T\n"
                              "POINT ID. TYPE T1 T2 T3 R1 R2 R3\n91 G 0 1 0 0 0 1000\n";
void write(const fs::path& path, std::string_view bytes) {
    std::ofstream output(path, std::ios::binary);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    require(output.good(), "Synthetic unit output write failed");
}
SolverOwnedRun origin(const fs::path& directory) {
    SolverOwnedRun fact;
    fact.principal = "explicit-test-only-result-unit";
    fact.idempotency_key = "unit-key";
    fact.test_only = true;
    fact.startup_intent_persisted = true;
    fact.source_input = frozen_input();
    const auto& input = fact.source_input;
    auto& request = fact.run.request;
    request.run_id = "run-unit";
    request.task_id = "unit";
    request.run_directory = directory.string();
    request.input = {input.version.document.id,
                     input.version.document.epoch,
                     input.version.revision,
                     input.analysis,
                     input.target.profile,
                     "artifact-unit",
                     artifact_sha256("explicit test-only input manifest"),
                     artifact_sha256(input.input_signature),
                     solver_export_identity_digest(input),
                     "test-only-export",
                     "qcae.nastran.static-f06.v1",
                     features::analysis::encode_frozen_analysis_input(input)};
    request.configuration = {"test-only",
                             "/not-executed/test-only-program",
                             {},
                             "test-only",
                             "test-only-process",
                             "test-only-1",
                             "explicit file/format unit, not a process execution proof",
                             artifact_sha256("explicit test-only configuration"),
                             {"result.f06"}};
    fact.signature = solver_run_signature(input.analysis,
                                          request.input.artifact_id,
                                          request.configuration.id,
                                          request.configuration.configuration_digest);
    fact.run.sequence = 3;
    fact.run.execution = SolverExecutionState::exited;
    fact.run.process = SolverProcessIdentity{1, "explicit-synthetic-record-not-an-executed-solver"};
    fact.run.exit_code = 0;
    fact.run.output_state = SolverOutputState::collected;
    fact.result_reader = SolverResultReaderConfiguration{};
    fact.result_reader->resource = "result.f06";
    for (const auto& file :
         std::vector<TextResource>{{"result.f06", synthetic},
                                   {"runner.stdout", "explicit synthetic fixture\n"},
                                   {"runner.stderr", ""}}) {
        write(directory / file.path, file.text);
        fact.run.outputs.push_back({file.path, file.text.size(), artifact_sha256(file.text)});
    }
    return fact;
}
} // namespace

int main() {
    fs::path root;
    try {
        root = fs::canonical(fs::temp_directory_path()) /
               ("qcae-result-unit-" + std::to_string(::getpid()));
        require(fs::create_directory(root) && fs::create_directory(root / "run-unit"),
                "Synthetic result unit directory already exists");
        auto fact = origin(root / "run-unit");
        unsigned count{};
        auto check = [&](bool value, const char* message) {
            require(value, message);
            ++count;
        };
        auto rejects = [&](auto action) {
            try {
                action();
                return false;
            } catch (const std::exception&) {
                return true;
            }
        };
        const auto resources = good(detail::read_verified_solver_outputs(fact.run));
        check(resources.size() == 3, "Actual file readback omitted an output/log");
        auto damaged = fact.run;
        damaged.exit_code = 1;
        check(!detail::read_verified_solver_outputs(damaged).ok(),
              "A nonzero exit exposed verified result bytes");
        damaged = fact.run;
        damaged.output_state = SolverOutputState::incomplete;
        check(!detail::read_verified_solver_outputs(damaged).ok(),
              "Incomplete output observations exposed result bytes");
        damaged = fact.run;
        damaged.outputs.pop_back();
        check(!detail::read_verified_solver_outputs(damaged).ok(),
              "A missing empty stderr escaped complete manifest checks");
        damaged = fact.run;
        damaged.outputs[2] = damaged.outputs[1];
        check(!detail::read_verified_solver_outputs(damaged).ok(),
              "A duplicate file replaced another manifest entry");
        damaged = fact.run;
        damaged.request.configuration.max_output_bytes = synthetic.size();
        check(!detail::read_verified_solver_outputs(damaged).ok(),
              "Raw outputs escaped the frozen aggregate quota");
        write(root / "run-unit/result.f06", std::string(synthetic.size(), 'x'));
        check(!detail::read_verified_solver_outputs(fact.run).ok(),
              "Same-length raw corruption escaped SHA256 readback");
        write(root / "run-unit/result.f06", synthetic);
        fs::rename(root / "run-unit/result.f06", root / "original.f06");
        fs::create_symlink(root / "original.f06", root / "run-unit/result.f06");
        check(!detail::read_verified_solver_outputs(fact.run).ok(),
              "Symlink output crossed the readback boundary");
        fs::remove(root / "run-unit/result.f06");
        require(::mkfifo((root / "run-unit/result.f06").c_str(), 0600) == 0,
                "Synthetic FIFO failed");
        check(!detail::read_verified_solver_outputs(fact.run).ok(),
              "A nonregular output crossed the readback boundary");
        fs::remove(root / "run-unit/result.f06");
        fs::rename(root / "original.f06", root / "run-unit/result.f06");
        SolverResultReaderConfiguration reader;
        reader.resource = "result.f06";
        const auto parsed = good(detail::prepare_solver_result(fact, reader, resources));
        check(parsed.origin.test_only &&
                  parsed.fields.displacements[1].entity == EntityId("tip-stable") &&
                  parsed.fields.displacements[1].solver_number == 7 &&
                  parsed.fields.displacements[1].components[1] == -1.904763 &&
                  parsed.fields.spc_reactions[0].components[5] == 1000 &&
                  parsed.fields.displacement_units[3] == "rad" &&
                  parsed.fields.reaction_units[5] == "N*mm",
              "Parsed fields lost the original map or component units");
        const auto encoded = encode_solver_parsed_result(parsed);
        check(encode_solver_parsed_result(decode_solver_parsed_result(encoded)) == encoded,
              "Parsed codec did not retain complete original run provenance");
        auto wrong_reader = reader;
        wrong_reader.subcase = 2;
        check(!detail::prepare_solver_result(fact, wrong_reader, resources).ok(),
              "Unconfigured subcase silently reinterpreted the run");
        auto wrong_resources = resources;
        wrong_resources.front().text.front() = 'x';
        check(!detail::prepare_solver_result(fact, reader, wrong_resources).ok(),
              "Unverified in-memory bytes reached the F06 reader");
        auto wrong = parsed;
        wrong.fields.displacements[1].components[0] = std::numeric_limits<double>::quiet_NaN();
        check(rejects([&] { (void)encode_solver_parsed_result(wrong); }),
              "Nonfinite stored component crossed the parsed codec");
        wrong = parsed;
        wrong.origin.test_only = false;
        check(rejects([&] { (void)encode_solver_parsed_result(wrong); }),
              "A test-only process was relabeled as an external solver");
        const auto intent = detail::solver_result_artifact(parsed, root / "run-unit/.qcae-result");
        auto pending = fact;
        ++pending.run.sequence;
        pending.run.parsing = SolverParsingState::parsed;
        pending.result_artifact = intent;
        auto changed_run = pending;
        changed_run.run.request.input.export_identity_digest = artifact_sha256("changed map");
        check(rejects([&] { detail::validate_solver_result_link(parsed, changed_run); }),
              "Result query trusted a mismatched run map");
        changed_run = pending;
        changed_run.run.cancellation_requested = !fact.run.cancellation_requested;
        (void)encode_solver_owned_run(changed_run);
        check(rejects([&] { detail::validate_solver_result_link(parsed, changed_run); }),
              "Result query trusted a changed original cancellation observation");
        check(!rejects([&] { detail::validate_solver_result_link(parsed, pending); }),
              "The durable parsed intent lost its original run link");
        auto published = pending;
        ++published.run.sequence;
        published.result_published = true;
        check(!rejects([&] { detail::validate_solver_result_link(parsed, published); }),
              "A published result lost its original run link");
        auto reconciled = published;
        reconciled.run.sequence += 3;
        check(!rejects([&] { detail::validate_solver_result_link(parsed, reconciled); }),
              "Additional reconciliation observations invalidated a published result");
        for (const auto sequence : {std::uint64_t{1}, fact.run.sequence}) {
            changed_run = pending;
            changed_run.run.sequence = sequence;
            (void)encode_solver_owned_run(changed_run);
            check(rejects([&] { detail::validate_solver_result_link(parsed, changed_run); }),
                  "A pending result accepted a regressed or unchanged run sequence");
        }
        changed_run = published;
        changed_run.run.sequence = pending.run.sequence;
        (void)encode_solver_owned_run(changed_run);
        check(rejects([&] { detail::validate_solver_result_link(parsed, changed_run); }),
              "A publication reused the pending parse-intent sequence");
        for (const auto phase : {SolverParsingState::not_run, SolverParsingState::failed}) {
            changed_run = fact;
            changed_run.run.sequence = published.run.sequence;
            changed_run.run.parsing = phase;
            (void)encode_solver_owned_run(changed_run);
            check(rejects([&] { detail::validate_solver_result_link(parsed, changed_run); }),
                  "A result linked to an unparsed or failed current run");
        }
        for (const bool change_manifest : {false, true}) {
            changed_run = pending;
            if (change_manifest)
                changed_run.result_artifact->manifest += " ";
            else
                changed_run.result_artifact->files.back().sha256 = artifact_sha256("different");
            (void)encode_solver_owned_run(changed_run);
            check(rejects([&] { detail::validate_solver_result_link(parsed, changed_run); }),
                  "A current run accepted a noncanonical parsed artifact intent");
        }
        auto replace_origin = [&](const SolverOwnedRun& replacement) {
            auto fields = record_wire::read_strings(encoded);
            const auto image = solver_owned_row(replacement);
            fields[1] = image->key.identity;
            fields[2] = std::to_string(image->schema_version);
            fields[3] = *image->payload;
            return record_wire::strings(fields);
        };
        for (const auto sequence : {std::uint64_t{1},
                                    std::uint64_t{2},
                                    std::numeric_limits<std::uint64_t>::max() - 1,
                                    std::numeric_limits<std::uint64_t>::max()}) {
            wrong = parsed;
            wrong.origin.run.sequence = sequence;
            const auto forged = replace_origin(wrong.origin);
            check(rejects([&] { (void)encode_solver_parsed_result(wrong); }),
                  "An impossible original sequence was encoded as parsed provenance");
            check(rejects([&] { (void)decode_solver_parsed_result(forged); }),
                  "A serialized impossible original sequence escaped result validation");
            check(!detail::prepare_solver_result(wrong.origin, reader, resources).ok(),
                  "Result preparation accepted an origin without two future sequence values");
        }
        for (const auto& replacement : {pending, published}) {
            const auto forged = replace_origin(replacement);
            check(rejects([&] { (void)decode_solver_parsed_result(forged); }),
                  "Parsed provenance recursively embedded a parsed or published origin");
        }
        for (unsigned state = 0; state < 4; ++state) {
            auto replacement = fact;
            if (state == 0)
                replacement.run.exit_code = 1;
            else if (state == 1) {
                replacement.run.output_state = SolverOutputState::incomplete;
                replacement.run.outputs.clear();
            } else if (state == 2) {
                replacement.run.execution = SolverExecutionState::cancelled;
                replacement.run.cancellation_requested = true;
                replacement.run.exit_code.reset();
                replacement.run.termination_signal = SIGTERM;
            } else
                replacement.run.parsing = SolverParsingState::failed;
            const auto forged = replace_origin(replacement);
            check(!detail::prepare_solver_result(replacement, reader, resources).ok(),
                  "Result preparation accepted a nonzero, incomplete, cancelled or parsed origin");
            check(rejects([&] { (void)decode_solver_parsed_result(forged); }),
                  "Stored provenance accepted an invalid original execution/output/parse phase");
        }
        wrong = parsed;
        wrong.origin.run.sequence = std::numeric_limits<std::uint64_t>::max() - 2;
        auto boundary_run = wrong.origin;
        boundary_run.run.parsing = SolverParsingState::parsed;
        boundary_run.result_artifact =
            detail::solver_result_artifact(wrong, root / "run-unit/.qcae-result");
        ++boundary_run.run.sequence;
        check(!rejects([&] { detail::validate_solver_result_link(wrong, boundary_run); }),
              "The highest valid original sequence lost its pending transition");
        ++boundary_run.run.sequence;
        boundary_run.result_published = true;
        check(!rejects([&] { detail::validate_solver_result_link(wrong, boundary_run); }),
              "The highest valid original sequence overflowed its publication transition");
        for (const bool is_published : {false, true}) {
            const auto row = detail::solver_result_row({parsed, intent, is_published});
            const auto decoded = detail::decode_solver_result_row(*row);
            check(decoded.published == is_published &&
                      encode_solver_parsed_result(decoded.parsed) == encoded,
                  "Stored pending/published result altered immutable origin provenance");
            auto fields = record_wire::read_strings(*row->payload);
            fields[2] = replace_origin(pending);
            auto altered = *row;
            altered.payload = std::make_shared<const std::string>(record_wire::strings(fields));
            check(rejects([&] { (void)detail::decode_solver_result_row(altered); }),
                  "A stored result row trusted an already-parsed original snapshot");
        }
        auto legacy = fact;
        legacy.result_reader.reset();
        legacy.run.request.input.result_reader_version = "unconfigured";
        legacy.run.sequence = 1;
        const auto legacy_row = solver_owned_row(legacy);
        check(legacy_row->schema_version == 1 && encode_solver_owned_run(decode_solver_owned_run(
                                                     *legacy_row)) == *legacy_row->payload,
              "The result-specific sequence gate changed legacy owned-run decoding");
        const auto copies = detail::solver_result_resources(parsed, resources);
        LocalArtifactStore store;
        const auto staged = store.stage(intent, copies, {});
        check(staged.size() == 4 && !fs::exists(intent.directory / "manifest.json"),
              "Staging falsely published a completed result manifest");
        store.publish(intent);
        check(store.verify(intent).size() == 4 &&
                  intent.manifest.find("\"source_kind\":\"test_process\"") !=
                      intent.manifest.npos &&
                  intent.manifest.find("\"numerical_validation\":\"not_run\"") !=
                      intent.manifest.npos,
              "Actual result publication lost synthetic/numerical provenance");
        write(root / "run-unit/result.f06", std::string(synthetic.size(), 'x'));
        check(store.verify(intent).size() == 4,
              "Changing original raw files altered their frozen published copy");
        write(intent.directory / "manifest.json", std::string(intent.manifest.size(), 'x'));
        check(rejects([&] { (void)store.verify(intent); }),
              "Corrupt final result manifest remained valid");
        fs::remove_all(root);
        std::cout
            << "PASS: " << count
            << " synthetic file/format/publication checks; no process or numerical acceptance\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\nSynthetic unit evidence retained at " << root << '\n';
        return 1;
    }
}
