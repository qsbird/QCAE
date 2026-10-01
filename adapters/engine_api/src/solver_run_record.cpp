#include "qcae/solver_run_record.hpp"
#include "qcae/artifacts_local.hpp"
#include "qcae/operation_registry.hpp"
#include "solver_version_probe.hpp"
#include "solver_result_store.hpp"
#include <algorithm>
#include <charconv>
#include <csignal>
#include <filesystem>
#include <set>

namespace qcae::ipc {
namespace {
constexpr std::string_view owner = "qcae.solver.run";
void check(bool value, const char* message) {
    if (!value)
        throw RecordError(ErrorCode::schema_unsupported, message, "solver_run");
}
bool bounded(std::string_view text, std::size_t limit = 1024) {
    return !text.empty() && text.size() <= limit && text.find('\0') == std::string_view::npos;
}
bool digest(std::string_view text) {
    return text.size() == 64 && std::all_of(text.begin(), text.end(), [](char ch) {
               return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
           });
}
bool absolute(std::string_view text) {
    const std::filesystem::path path(text);
    return bounded(text, 4096) && path.is_absolute() && path.lexically_normal() == path;
}
bool output_path(std::string_view text) {
    const std::filesystem::path path(text);
    if (!bounded(text) || path.is_absolute() || path.lexically_normal() != path ||
        !path.has_filename())
        return false;
    for (const auto& part : path)
        if (part.empty() || part == "." || part == ".." || part == "input")
            return false;
    return text != "runner.stdout" && text != "runner.stderr";
}
void validate(const SolverOwnedRun& fact) {
    const auto& record = fact.run;
    const auto& request = record.request;
    const auto& provenance = request.input;
    const auto& config = request.configuration;
    check(bounded(fact.principal) && bounded(fact.idempotency_key) && !fact.signature.empty() &&
              fact.signature.size() <= 32768 && bounded(request.task_id, 128) &&
              request.run_id == "run-" + request.task_id && absolute(request.run_directory) &&
              std::filesystem::path(request.run_directory).filename() == request.run_id &&
              record.sequence > 0,
          "Malformed solver run ownership or identity");
    const auto current =
        features::analysis::decode_frozen_analysis_input(provenance.frozen_analysis_input);
    check(provenance.document == current.version.document.id &&
              provenance.epoch == current.version.document.epoch &&
              provenance.revision == current.version.revision &&
              provenance.analysis == current.analysis &&
              provenance.profile == current.target.profile && bounded(provenance.artifact_id) &&
              digest(provenance.manifest_sha256) &&
              provenance.input_fingerprint == artifact_sha256(current.input_signature) &&
              provenance.export_identity_digest == solver_export_identity_digest(current) &&
              bounded(provenance.export_rule_version) &&
              provenance.result_reader_version ==
                  (fact.result_reader ? fact.result_reader->reader_version : "unconfigured") &&
              fact.source_input.input_signature == current.input_signature &&
              fact.source_input.analysis == current.analysis &&
              fact.source_input.target == current.target &&
              solver_export_identity_digest(fact.source_input) == provenance.export_identity_digest,
          "Run provenance differs from frozen analysis or source export map");
    check(bounded(config.id) && absolute(config.executable) && bounded(config.solver_family) &&
              bounded(config.dialect) && bounded(config.solver_version) &&
              bounded(config.version_evidence) && digest(config.configuration_digest) &&
              config.argv.size() <= 64 && !config.expected_outputs.empty() &&
              config.expected_outputs.size() <= 32 && config.max_wall_time_ms > 0 &&
              config.max_wall_time_ms <= 86400000 && config.cancel_grace_ms <= 5000 &&
              config.max_output_bytes > 0 && config.max_output_bytes <= 16777216,
          "Malformed frozen process configuration");
    check(fact.signature == solver_run_signature(provenance.analysis,
                                                 provenance.artifact_id,
                                                 config.id,
                                                 config.configuration_digest),
          "Run task signature differs from analysis, artifact or configuration digest");
    if (fact.version_probe) {
        check(!fact.test_only && digest(fact.declaration_digest) &&
                  detail::solver_version_allows_execution(*fact.version_probe,
                                                          config.solver_version) &&
                  config.configuration_digest == solver_version_configuration_digest(
                                                     fact.declaration_digest, *fact.version_probe),
              "Run version observation is synthetic, unverified or differs from configuration");
    } else {
        check(fact.declaration_digest.empty(), "Legacy run contains unbound version evidence");
    }
    check(fact.test_only
              ? config.solver_family == "test-only"
              : config.solver_family == "Nastran" && config.dialect == "MSC" &&
                    (config.solver_version == "2022.1" || config.solver_version == "2024.1"),
          "Unsupported or falsely classified solver family/version");
    std::size_t argument_bytes{};
    for (const auto& argument : config.argv) {
        check(argument.size() <= 4096 && argument.find('\0') == std::string::npos,
              "Malformed process argument");
        argument_bytes += argument.size();
    }
    check(argument_bytes <= 65536, "Process argument budget exceeded");
    std::set<std::string> paths;
    for (const auto& output : config.expected_outputs)
        check(output_path(output) && paths.insert(output).second,
              "Invalid declared run output path");
    check(record.numerical_validation == SolverNumericalState::not_run &&
              (fact.result_reader || record.parsing == SolverParsingState::not_run),
          "Legacy or unconfigured runs cannot certify parsing; numerical validation is not run");
    if (fact.result_reader) {
        detail::validate_solver_result_reader(*fact.result_reader, config);
        check(record.parsing == SolverParsingState::not_run ||
                  record.parsing == SolverParsingState::parsed ||
                  record.parsing == SolverParsingState::failed,
              "Unknown parsing state");
        if (record.parsing != SolverParsingState::not_run)
            check(record.execution == SolverExecutionState::exited && record.exit_code == 0 &&
                      !record.termination_signal &&
                      record.output_state == SolverOutputState::collected,
                  "Parsing fact lacks complete normally exited output provenance");
        check(fact.result_artifact.has_value() == (record.parsing == SolverParsingState::parsed),
              "Parsed run lacks a durable result publication intent");
        if (fact.result_artifact)
            detail::validate_solver_result_intent(
                *fact.result_artifact, record, *fact.result_reader);
        check(!fact.result_published || fact.result_artifact.has_value(),
              "Result publication lacks its durable intent");
    } else {
        check(!fact.result_artifact && !fact.result_published,
              "Legacy run contains unexpected result publication facts");
    }
    if (record.process)
        check(record.process->pid > 0 && bounded(record.process->start_identity, 128),
              "Malformed OS process creation identity");
    if (!fact.startup_intent_persisted)
        check(record.sequence == 1 && record.execution == SolverExecutionState::startup_intent &&
                  !record.process && !record.exit_code && !record.termination_signal &&
                  !record.cancellation_requested && record.outputs.empty() &&
                  record.output_state == SolverOutputState::not_collected,
              "Queued snapshot falsely claims external execution");
    switch (record.execution) {
    case SolverExecutionState::startup_intent:
        check(!record.process && !record.exit_code && !record.termination_signal,
              "Startup intent contains an exit or process identity");
        break;
    case SolverExecutionState::running:
    case SolverExecutionState::cancel_requested:
        check(record.process && !record.exit_code && !record.termination_signal &&
                  record.cancellation_requested ==
                      (record.execution == SolverExecutionState::cancel_requested),
              "Running process contains inconsistent cancellation/exit facts");
        break;
    case SolverExecutionState::exited:
    case SolverExecutionState::cancelled:
        check(record.process &&
                  record.exit_code.has_value() != record.termination_signal.has_value(),
              "Exited process lacks a unique exit observation");
        if (record.exit_code)
            check(*record.exit_code >= 0 && *record.exit_code <= 255, "Invalid process exit code");
        if (record.termination_signal)
            check(*record.termination_signal > 0 && *record.termination_signal <= 255,
                  "Invalid process termination signal");
        if (record.execution == SolverExecutionState::cancelled)
            check(record.cancellation_requested && (record.termination_signal == SIGTERM ||
                                                    record.termination_signal == SIGKILL),
                  "Cancelled run lacks observed cancellation termination");
        break;
    case SolverExecutionState::launch_failed:
    case SolverExecutionState::outcome_unknown:
        break;
    default:
        check(false, "Unknown execution state");
    }
    if (record.output_state == SolverOutputState::collected) {
        check((record.execution == SolverExecutionState::exited ||
               record.execution == SolverExecutionState::cancelled) &&
                  record.outputs.size() == config.expected_outputs.size() + 2,
              "Raw outputs have no matching terminal observation");
        auto expected = config.expected_outputs;
        expected.insert(expected.end(), {"runner.stdout", "runner.stderr"});
        std::uint64_t bytes{};
        for (std::size_t i = 0; i < expected.size(); ++i) {
            const auto& file = record.outputs[i];
            check(file.path == expected[i] && digest(file.sha256) &&
                      file.byte_length <= config.max_output_bytes - bytes &&
                      (file.byte_length > 0 || i >= config.expected_outputs.size()),
                  "Raw output list differs from frozen path/digest/byte budget");
            bytes += file.byte_length;
        }
    } else {
        check((record.output_state == SolverOutputState::not_collected ||
               record.output_state == SolverOutputState::incomplete) &&
                  record.outputs.empty(),
              "Malformed incomplete raw output list");
    }
    check(record.detail.size() <= 4096 && record.detail.find('\0') == std::string::npos,
          "Malformed run diagnostic");
}
class Fields {
  public:
    explicit Fields(std::vector<std::string> values) : values_(std::move(values)) {}
    std::string take() {
        check(index_ < values_.size(), "Truncated solver run fact");
        return values_[index_++];
    }
    std::uint64_t number() {
        const auto text = take();
        std::uint64_t value{};
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        check(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size(),
              "Malformed solver run integer");
        return value;
    }
    bool boolean() {
        const auto value = number();
        check(value <= 1, "Malformed solver run boolean");
        return value != 0;
    }
    std::optional<int> optional_int() {
        const auto has = boolean();
        const auto value = number();
        check(value <= 255 && (has || value == 0), "Malformed optional exit observation");
        return has ? std::optional<int>(static_cast<int>(value)) : std::nullopt;
    }
    void finish() const {
        check(index_ == values_.size(), "Trailing solver run fields");
    }

  private:
    std::vector<std::string> values_;
    std::size_t index_{};
};
} // namespace
std::string solver_export_identity_digest(const features::analysis::FrozenAnalysisInput& input) {
    std::vector<std::string> fields;
    for (const auto& item : input.identities)
        fields.insert(fields.end(),
                      {item.entity.value, item.name_space, std::to_string(item.number)});
    return artifact_sha256(record_wire::strings(fields));
}
std::string solver_run_signature(const EntityId& analysis,
                                 std::string_view artifact_id,
                                 std::string_view configuration_id,
                                 std::string_view configuration_digest) {
    using operations::Value;
    const auto parameters = operations::canonical_value(
        Value(Value::Object{{"analysis_id", Value(analysis.value)},
                            {"artifact_id", Value(artifact_id)},
                            {"run_config_id", Value(configuration_id)}}));
    check(parameters.ok(), "Run signature parameters cannot be encoded");
    return record_wire::strings(
        std::array<std::string, 2>{*parameters.value, std::string(configuration_digest)});
}
std::string encode_solver_owned_run(const SolverOwnedRun& fact) {
    validate(fact);
    const auto& record = fact.run;
    const auto& request = record.request;
    const auto& input = request.input;
    const auto& config = request.configuration;
    std::vector<std::string> fields{
        fact.result_reader   ? "QCAE-SOLVER-RUN-3"
        : fact.version_probe ? "QCAE-SOLVER-RUN-2"
                             : "QCAE-SOLVER-RUN-1",
        fact.principal,
        fact.idempotency_key,
        fact.signature,
        features::analysis::encode_frozen_analysis_input(fact.source_input),
        fact.test_only ? "1" : "0",
        fact.startup_intent_persisted ? "1" : "0",
        request.run_id,
        request.task_id,
        request.run_directory,
        input.document.value,
        input.epoch.value,
        std::to_string(input.revision),
        input.analysis.value,
        record_wire::profile(input.profile),
        input.artifact_id,
        input.manifest_sha256,
        input.input_fingerprint,
        input.export_identity_digest,
        input.export_rule_version,
        input.result_reader_version,
        input.frozen_analysis_input,
        config.id,
        config.executable,
        config.solver_family,
        config.dialect,
        config.solver_version,
        config.version_evidence,
        config.configuration_digest,
        std::to_string(config.max_wall_time_ms),
        std::to_string(config.cancel_grace_ms),
        std::to_string(config.max_output_bytes),
        std::to_string(config.argv.size())};
    fields.insert(fields.end(), config.argv.begin(), config.argv.end());
    fields.push_back(std::to_string(config.expected_outputs.size()));
    fields.insert(fields.end(), config.expected_outputs.begin(), config.expected_outputs.end());
    fields.insert(fields.end(),
                  {std::to_string(record.sequence),
                   std::to_string(static_cast<unsigned>(record.execution)),
                   std::to_string(static_cast<unsigned>(record.parsing)),
                   std::to_string(static_cast<unsigned>(record.numerical_validation)),
                   std::to_string(static_cast<unsigned>(record.output_state)),
                   record.cancellation_requested ? "1" : "0",
                   record.process ? "1" : "0"});
    if (record.process)
        fields.insert(fields.end(),
                      {std::to_string(record.process->pid), record.process->start_identity});
    fields.insert(fields.end(),
                  {record.exit_code ? "1" : "0",
                   std::to_string(record.exit_code.value_or(0)),
                   record.termination_signal ? "1" : "0",
                   std::to_string(record.termination_signal.value_or(0)),
                   std::to_string(record.outputs.size())});
    for (const auto& file : record.outputs)
        fields.insert(fields.end(), {file.path, std::to_string(file.byte_length), file.sha256});
    fields.push_back(record.detail);
    if (fact.version_probe || fact.result_reader) {
        fields.push_back(fact.version_probe ? encode_solver_version_evidence(*fact.version_probe)
                                            : "");
        fields.push_back(fact.declaration_digest);
    }
    if (fact.result_reader) {
        const auto& reader = *fact.result_reader;
        fields.insert(fields.end(),
                      {reader.reader_version,
                       reader.resource,
                       std::to_string(reader.subcase),
                       reader.unit_system,
                       reader.coordinate_basis,
                       fact.result_artifact ? "1" : "0"});
        if (fact.result_artifact) {
            const auto& artifact = *fact.result_artifact;
            fields.insert(fields.end(),
                          {artifact.artifact_id,
                           artifact.task_id,
                           artifact.directory.string(),
                           artifact.root_resource,
                           artifact.manifest,
                           std::to_string(artifact.files.size())});
            for (const auto& file : artifact.files)
                fields.insert(fields.end(),
                              {file.path, std::to_string(file.byte_length), file.sha256});
        }
        fields.push_back(fact.result_published ? "1" : "0");
    }
    auto encoded = record_wire::strings(fields);
    check(encoded.size() <= 65536, "Solver run fact exceeds owned-row budget");
    return encoded;
}
SolverOwnedRun decode_solver_owned_run(const OwnedRowImage& image) {
    check(image.owner == owner && image.schema_version >= 1 && image.schema_version <= 3 &&
              image.key.space == StoreSpace::artifact_record && image.payload &&
              image.payload->size() <= 65536,
          "Unsupported solver run owned-row envelope");
    Fields fields(record_wire::read_strings(*image.payload));
    check(fields.take() == (image.schema_version == 1   ? "QCAE-SOLVER-RUN-1"
                            : image.schema_version == 2 ? "QCAE-SOLVER-RUN-2"
                                                        : "QCAE-SOLVER-RUN-3"),
          "Unsupported solver run schema");
    SolverOwnedRun fact;
    fact.principal = fields.take();
    fact.idempotency_key = fields.take();
    fact.signature = fields.take();
    fact.source_input = features::analysis::decode_frozen_analysis_input(fields.take());
    fact.test_only = fields.boolean();
    fact.startup_intent_persisted = fields.boolean();
    auto& record = fact.run;
    auto& request = record.request;
    request.run_id = fields.take();
    request.task_id = fields.take();
    request.run_directory = fields.take();
    auto& input = request.input;
    input.document = DocumentId(fields.take());
    input.epoch = DocumentEpoch(fields.take());
    input.revision = fields.number();
    input.analysis = EntityId(fields.take());
    input.profile = record_wire::read_profile(fields.take());
    for (auto* value : {&input.artifact_id,
                        &input.manifest_sha256,
                        &input.input_fingerprint,
                        &input.export_identity_digest,
                        &input.export_rule_version,
                        &input.result_reader_version,
                        &input.frozen_analysis_input})
        *value = fields.take();
    auto& config = request.configuration;
    for (auto* value : {&config.id,
                        &config.executable,
                        &config.solver_family,
                        &config.dialect,
                        &config.solver_version,
                        &config.version_evidence,
                        &config.configuration_digest})
        *value = fields.take();
    config.max_wall_time_ms = fields.number();
    config.cancel_grace_ms = fields.number();
    config.max_output_bytes = fields.number();
    auto count = fields.number();
    check(count <= 64, "Solver argument count exceeds budget");
    while (count-- > 0)
        config.argv.push_back(fields.take());
    count = fields.number();
    check(count <= 32, "Solver output count exceeds budget");
    while (count-- > 0)
        config.expected_outputs.push_back(fields.take());
    record.sequence = fields.number();
    const auto execution = fields.number(), parsing = fields.number(), numerical = fields.number(),
               output = fields.number();
    check(execution <= 6 && parsing <= 2 && numerical <= 2 && output <= 2,
          "Malformed solver stage enum");
    record.execution = static_cast<SolverExecutionState>(execution);
    record.parsing = static_cast<SolverParsingState>(parsing);
    record.numerical_validation = static_cast<SolverNumericalState>(numerical);
    record.output_state = static_cast<SolverOutputState>(output);
    record.cancellation_requested = fields.boolean();
    if (fields.boolean()) {
        const auto pid = fields.number();
        check(pid <= INT32_MAX, "Malformed OS PID");
        record.process = SolverProcessIdentity{static_cast<std::int64_t>(pid), fields.take()};
    }
    record.exit_code = fields.optional_int();
    record.termination_signal = fields.optional_int();
    count = fields.number();
    check(count <= 34, "Observed output count exceeds budget");
    while (count-- > 0) {
        auto path = fields.take();
        const auto bytes = fields.number();
        record.outputs.push_back({std::move(path), bytes, fields.take()});
    }
    record.detail = fields.take();
    if (image.schema_version >= 2) {
        const auto probe = fields.take();
        if (!probe.empty())
            fact.version_probe = decode_solver_version_evidence(probe);
        check(image.schema_version == 3 || fact.version_probe.has_value(),
              "Run v2 lacks its frozen version observation");
        fact.declaration_digest = fields.take();
    }
    if (image.schema_version == 3) {
        SolverResultReaderConfiguration reader;
        reader.reader_version = fields.take();
        reader.resource = fields.take();
        reader.subcase = fields.number();
        reader.unit_system = fields.take();
        reader.coordinate_basis = fields.take();
        fact.result_reader = std::move(reader);
        if (fields.boolean()) {
            LocalArtifactIntent artifact;
            artifact.artifact_id = fields.take();
            artifact.task_id = fields.take();
            artifact.directory = fields.take();
            artifact.root_resource = fields.take();
            artifact.manifest = fields.take();
            count = fields.number();
            check(count <= 35, "Result artifact file count exceeds quota");
            while (count-- > 0) {
                auto path = fields.take();
                const auto length = fields.number();
                artifact.files.push_back({std::move(path), length, fields.take()});
            }
            fact.result_artifact = std::move(artifact);
        }
        fact.result_published = fields.boolean();
    }
    fields.finish();
    check(request.run_id == image.key.identity && encode_solver_owned_run(fact) == *image.payload,
          "Run row identity or canonical bytes differ");
    return fact;
}
std::shared_ptr<const OwnedRowImage> solver_owned_row(const SolverOwnedRun& fact) {
    return std::make_shared<const OwnedRowImage>(
        OwnedRowImage{{StoreSpace::artifact_record, fact.run.request.run_id},
                      std::string(owner),
                      fact.result_reader   ? 3U
                      : fact.version_probe ? 2U
                                           : 1U,
                      std::make_shared<const std::string>(encode_solver_owned_run(fact)),
                      {}});
}
void validate_solver_task(const SolverOwnedRun& fact, const TaskRecord& task) {
    const auto& request = fact.run.request;
    check(task.id == request.task_id && task.caller.principal == fact.principal &&
              task.operation == "analysis.start" && task.idempotency_key == fact.idempotency_key &&
              task.signature == fact.signature &&
              task.input.document.id == request.input.document &&
              task.input.document.epoch == request.input.epoch &&
              task.input.revision == request.input.revision &&
              task.input.profile == request.input.profile && !task.receipt,
          "Analysis run/task cross-row provenance is inconsistent");
    const auto expected = fact.result_artifact
                              ? std::optional<TaskArtifactReceipt>(TaskArtifactReceipt{
                                    fact.result_artifact->artifact_id,
                                    request.input.revision,
                                    artifact_sha256(fact.result_artifact->manifest)})
                              : std::nullopt;
    check(fact.result_published
              ? task.state == TaskState::succeeded && task.artifact_receipt == expected && expected
              : task.state != TaskState::succeeded && !task.artifact_receipt,
          "Analysis run/task artifact publication facts are inconsistent");
}
OwnedRowHandler solver_run_row_handler() {
    return {StoreSpace::artifact_record,
            std::string(owner),
            [](const OwnedRowImage& image) { (void)decode_solver_owned_run(image); },
            [](const OwnedRowImage& image) -> std::shared_ptr<const OwnedRowImage> {
                auto fact = decode_solver_owned_run(image);
                const auto execution = fact.run.execution;
                if (!fact.startup_intent_persisted || execution == SolverExecutionState::exited ||
                    execution == SolverExecutionState::cancelled ||
                    execution == SolverExecutionState::launch_failed ||
                    execution == SolverExecutionState::outcome_unknown)
                    return {};
                check(fact.run.sequence != UINT64_MAX, "Run recovery sequence exceeded budget");
                ++fact.run.sequence;
                fact.run.execution = SolverExecutionState::outcome_unknown;
                fact.run.detail =
                    "External execution requires reconciliation after host restart; not rerun";
                return solver_owned_row(fact);
            },
            [](const OwnedRowImage& image) {
                const auto fact = decode_solver_owned_run(image);
                const auto execution = fact.run.execution;
                return fact.startup_intent_persisted && execution != SolverExecutionState::exited &&
                       execution != SolverExecutionState::cancelled &&
                       execution != SolverExecutionState::launch_failed;
            }};
}
} // namespace qcae::ipc
