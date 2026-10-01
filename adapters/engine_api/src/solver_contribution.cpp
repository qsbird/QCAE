#include "qcae/solver_contribution.hpp"
#include "qcae/solver_run_record.hpp"
#include "qcae/nastran_package.hpp"
#include "qcae/operation_inputs.hpp"
#include "typed_values.hpp"
#include "typed_json.hpp"
#include "solver_version_probe.hpp"
#include "solver_result_store.hpp"
#include "solver_validation_store.hpp"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <chrono>
#include <charconv>
#include <fstream>
#include <mutex>
#include <thread>

namespace qcae::ipc {
namespace {
using namespace operations;
using FrozenInput = features::analysis::FrozenAnalysisInput;
constexpr std::string_view run_owner = "qcae.solver.run";
constexpr std::string_view task_owner = "qcae.runtime.task";
constexpr std::string_view reconcile_owner = "qcae.solver.reconcile";
template <class T> Result<T> good(T value) {
    return {Status::success, std::move(value), {}};
}
template <class T> Result<T> bad(ErrorCode code, std::string message) {
    return {code == ErrorCode::revision_conflict || code == ErrorCode::idempotency_key_conflict
                ? Status::conflict
                : Status::failed,
            {},
            Diagnostic{code, std::move(message), "solver_run"}};
}
template <class T, class U> Result<T> publication_uncertain(const Result<U>& result) {
    auto diagnostic = result.error.value_or(
        Diagnostic{ErrorCode::storage_uncertain, "No database acknowledgement", "solver_run"});
    diagnostic.message = "Result files are published; database confirmation failed [" +
                         std::string(error_name(diagnostic.code)) + "]: " + diagnostic.message;
    diagnostic.code = ErrorCode::storage_uncertain;
    return {Status::failed, {}, std::move(diagnostic)};
}
void check(bool condition, const char* message) {
    if (!condition)
        throw RecordError(ErrorCode::invalid_input, message, "solver_config");
}
std::string required_string(const QJsonObject& object, const char* key) {
    const auto value = object.value(QLatin1String(key));
    check(value.isString() && !value.toString().isEmpty() && value.toString().size() <= 4096,
          "Configuration requires a bounded string field");
    auto text = value.toString().toStdString();
    check(text.find('\0') == std::string::npos, "Configuration string contains NUL");
    return text;
}
std::vector<std::string>
string_array(const QJsonObject& object, const char* key, qsizetype limit, bool allow_empty) {
    const auto value = object.value(QLatin1String(key));
    check(value.isArray() && value.toArray().size() <= limit &&
              (allow_empty || !value.toArray().isEmpty()),
          "Configuration requires a bounded array");
    std::vector<std::string> result;
    for (const auto& item : value.toArray()) {
        check(item.isString() && item.toString().size() <= 4096,
              "Configuration array contains an invalid string");
        auto text = item.toString().toStdString();
        check(text.find('\0') == std::string::npos, "Configuration argument contains NUL");
        result.push_back(std::move(text));
    }
    return result;
}
std::uint64_t
budget(const QJsonObject& object, const char* key, std::uint64_t maximum, bool allow_zero = false) {
    const auto text = required_string(object, key);
    std::uint64_t value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    check(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && value <= maximum &&
              (allow_zero || value > 0),
          "Configuration budget must be an exact decimal string");
    return value;
}
std::string pending_key(const Caller& caller, std::string_view key) {
    return caller.principal + '\0' + std::string(key);
}
std::shared_ptr<const OwnedRowImage> task_row(const TaskRecord& task) {
    return std::make_shared<const OwnedRowImage>(
        OwnedRowImage{{StoreSpace::task_record, task.id},
                      std::string(task_owner),
                      1,
                      std::make_shared<const std::string>(encode_task_record(task)),
                      {}});
}
bool terminal(SolverExecutionState state) {
    return state == SolverExecutionState::exited || state == SolverExecutionState::cancelled ||
           state == SolverExecutionState::launch_failed ||
           state == SolverExecutionState::outcome_unknown;
}
const char* execution_name(SolverExecutionState state) {
    switch (state) {
    case SolverExecutionState::startup_intent:
        return "startup_intent";
    case SolverExecutionState::running:
        return "running";
    case SolverExecutionState::cancel_requested:
        return "cancel_requested";
    case SolverExecutionState::exited:
        return "exited";
    case SolverExecutionState::cancelled:
        return "cancelled";
    case SolverExecutionState::launch_failed:
        return "launch_failed";
    case SolverExecutionState::outcome_unknown:
        return "outcome_unknown";
    }
    return "outcome_unknown";
}
struct FrozenRun {
    SolverOwnedRun fact;
    LocalArtifactIntent artifact;
};
SolverRunConfiguration expanded_configuration(const LocalSolverConfiguration& config,
                                              const std::string& directory,
                                              const std::string& root_resource) {
    auto run = config.run;
    const auto input = (std::filesystem::path(directory) / "input" / root_resource).string();
    for (auto& argument : run.argv) {
        constexpr std::string_view token = "{input_root}";
        std::size_t offset{};
        while ((offset = argument.find(token, offset)) != std::string::npos) {
            argument.replace(offset, token.size(), input);
            offset += input.size();
        }
    }
    return run;
}
struct SolverPayload final : TaskPayload {
    TaskCompletion result;
    explicit SolverPayload(TaskCompletion value) : result(std::move(value)) {}
    std::optional<TaskCompletion> completion() const override {
        return result;
    }
};
struct SolverResultPayload final : TaskPayload {
    StoredSolverResult result;
    explicit SolverResultPayload(StoredSolverResult value) : result(std::move(value)) {}
};
void verify_result_artifact(const StoredSolverResult& result) {
    (void)detail::solver_result_row(result);
    const auto copied = LocalArtifactStore{}.verify(result.artifact);
    std::vector<TextResource> raw;
    bool parsed_copy{};
    const auto encoded = encode_solver_parsed_result(result.parsed);
    for (const auto& resource : copied) {
        if (resource.path == "parsed-result.qcr") {
            check(resource.text == encoded,
                  "Published parsed bytes differ from their durable fields");
            parsed_copy = true;
        } else {
            raw.push_back(resource);
        }
    }
    const auto readback =
        detail::prepare_solver_result(result.parsed.origin, result.parsed.reader, raw);
    check(parsed_copy && readback.ok() && encode_solver_parsed_result(*readback.value) == encoded,
          "Published F06 readback differs from parsed values or original provenance");
}
std::string reconcile_identity(const Caller& caller, std::string_view key) {
    return "solver-reconcile-" + artifact_sha256(record_wire::strings(std::array<std::string, 2>{
                                     caller.principal, std::string(key)}));
}
OwnedRowHandler reconcile_row_handler() {
    return {
        StoreSpace::artifact_record,
        std::string(reconcile_owner),
        [](const OwnedRowImage& image) {
            if (image.owner != reconcile_owner || image.schema_version != 1 ||
                image.key.space != StoreSpace::artifact_record || !image.payload ||
                image.payload->size() > 65536)
                throw RecordError(ErrorCode::schema_unsupported,
                                  "Invalid solver reconcile owner/schema");
            const auto fields = record_wire::read_strings(*image.payload);
            const auto bounded = [](std::string_view value, std::size_t limit) {
                return !value.empty() && value.size() <= limit && value.find('\0') == value.npos;
            };
            if (fields.size() != 7 || fields[0] != "QCAE-SOLVER-RECONCILE-1" ||
                !bounded(fields[1], 4096) || !bounded(fields[2], 4096) || fields[3].empty() ||
                !bounded(fields[4], 128) || !bounded(fields[5], 128) || fields[6].size() != 64 ||
                fields[6].find_first_not_of("0123456789abcdef") != fields[6].npos ||
                image.key.identity != reconcile_identity({fields[1]}, fields[2]))
                throw RecordError(ErrorCode::schema_unsupported,
                                  "Malformed solver reconcile request fact");
            const auto signature = record_wire::read_strings(fields[3]);
            if (signature.size() != 5 || !bounded(signature[0], 128) ||
                !bounded(signature[1], 128) || signature[2].empty() || signature[3] != fields[4] ||
                signature[4] != fields[1])
                throw RecordError(ErrorCode::schema_unsupported,
                                  "Solver reconcile signature differs from its request");
            std::uint64_t revision{};
            const auto parsed = std::from_chars(
                signature[2].data(), signature[2].data() + signature[2].size(), revision);
            if (parsed.ec != std::errc{} ||
                parsed.ptr != signature[2].data() + signature[2].size() ||
                std::to_string(revision) != signature[2])
                throw RecordError(ErrorCode::schema_unsupported,
                                  "Malformed solver reconcile revision");
        },
        {},
        {}};
}
std::shared_ptr<const TaskPayload>
completion(TaskState state, ErrorCode code, std::string message) {
    return std::make_shared<const SolverPayload>(
        TaskCompletion{state, Diagnostic{code, std::move(message), "solver_run"}});
}
Value version_value(const std::optional<SolverVersionEvidence>& evidence) {
    if (!evidence)
        return Value(Value::Object{});
    const auto& fact = *evidence;
    return Value(Value::Object{{"protocol", Value(fact.protocol)},
                               {"reported_version", Value(fact.reported_version)},
                               {"executable_sha256", Value(fact.executable.sha256)},
                               {"stdout_bytes", Value(std::to_string(fact.stdout_bytes))},
                               {"stderr_bytes", Value(std::to_string(fact.stderr_bytes))},
                               {"stdout_sha256", Value(fact.stdout_sha256)},
                               {"stderr_sha256", Value(fact.stderr_sha256)},
                               {"exit_code", Value(std::to_string(fact.exit_code))},
                               {"synthetic", Value(fact.synthetic)}});
}
Value validation_summary(const detail::SolverValidationFact& fact, std::string_view identity) {
    std::uint64_t mismatched{};
    for (const auto& component : fact.report.components)
        if (!component.matched)
            ++mismatched;
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
Value validation_components(const detail::SolverValidationFact& fact) {
    Value::Array components;
    for (const auto& item : fact.report.components)
        components.emplace_back(
            Value::Object{{"entity_id", Value(item.entity.value)},
                          {"solver_number", Value(std::to_string(item.solver_number))},
                          {"quantity_id", Value(item.quantity)},
                          {"component_id", Value(item.component)},
                          {"unit", Value(item.unit)},
                          {"actual", Value(item.actual)},
                          {"expected", Value(item.expected)},
                          {"absolute_error", Value(item.absolute_error)},
                          {"tolerance", Value(item.tolerance)},
                          {"matched", Value(item.matched)}});
    return Value(std::move(components));
}
bool original_input_current(const RecordApplication& app,
                            const DocumentRef& document,
                            const SolverOwnedRun& fact) {
    const auto snapshot = app.snapshot(document);
    if (!snapshot.ok())
        return false;
    const auto now = features::analysis::freeze_analysis_input(snapshot.value->records,
                                                               fact.run.request.input.analysis,
                                                               fact.run.request.input.profile,
                                                               fact.source_input.identities);
    return now.ok() &&
           artifact_sha256(now.value->input_signature) == fact.run.request.input.input_fingerprint;
}
struct ValidationHistory {
    std::shared_ptr<const OwnedRowImage> index;
    std::vector<std::pair<detail::SolverValidationFact, std::shared_ptr<const OwnedRowImage>>>
        reports;
    bool overflow{};
};
Result<ValidationHistory> validation_history(const RecordApplication& app,
                                             const DocumentRef& document,
                                             const StoredSolverResult& stored,
                                             const ProfileRef& registered_profile) {
    const auto& origin = stored.parsed.origin;
    const auto prefix = detail::solver_validation_prefix(origin.run.request.run_id);
    const auto page = app.owned_rows_with_prefix(
        document, StoreSpace::artifact_record, prefix, detail::solver_validation_history_limit + 1);
    if (!page.ok())
        return {page.status, {}, page.error};
    ValidationHistory history;
    for (const auto& image : page.value->rows)
        check(image->owner == detail::solver_validation_owner,
              "Validation prefix contains a row owned by another service");
    history.overflow = page.value->overflow;
    if (history.overflow || page.value->rows.empty())
        return good(std::move(history));
    for (const auto& image : page.value->rows)
        if (image->key.identity == prefix + "head")
            history.index = image;
    check(static_cast<bool>(history.index), "Validation history lacks its CAS index");
    const auto index = detail::decode_solver_validation_index(*history.index);
    check(index.run_id == origin.run.request.run_id && index.principal == origin.principal &&
              index.report_rows.size() + 1 == page.value->rows.size(),
          "Validation history index differs from its original run or row count");
    for (std::size_t position = 0; position < index.report_rows.size(); ++position) {
        const auto found =
            std::find_if(page.value->rows.begin(), page.value->rows.end(), [&](const auto& image) {
                return image->key.identity == index.report_rows[position];
            });
        check(found != page.value->rows.end(), "Validation history references a missing report");
        auto fact = detail::decode_solver_validation_row(**found);
        check(fact.ordinal == position + 1, "Validation history ordinals are not continuous");
        detail::validate_solver_validation_link(fact, stored, registered_profile);
        history.reports.emplace_back(std::move(fact), *found);
    }
    return good(std::move(history));
}
Value numerical_checks(const ValidationHistory& history, bool current, bool files_verified) {
    Value::Array summaries;
    for (const auto& entry : history.reports)
        summaries.push_back(validation_summary(entry.first, entry.second->key.identity));
    Value latest;
    if (!history.overflow && !history.reports.empty())
        latest = validation_summary(history.reports.back().first,
                                    history.reports.back().second->key.identity);
    constexpr std::size_t summary_byte_limit = 32768;
    const bool byte_overflow = detail::typed_json_array_bytes(summaries) > summary_byte_limit;
    const bool overflow = history.overflow || byte_overflow;
    if (overflow) {
        summaries.clear();
        latest = Value();
    }
    const bool applicable = !overflow && !history.reports.empty();
    return Value(Value::Object{
        {"original_run_validation_stage", Value("not_run")},
        {"original_source_state", Value(current ? "current" : "stale")},
        {"current_files_verified", Value(files_verified)},
        {"latest_applicable_summary", std::move(latest)},
        {"has_applicable_validation", Value(applicable)},
        {"applies_to_current_input", Value(applicable && current)},
        {"immutable_history", Value(std::move(summaries))},
        {"history_limit", Value(std::to_string(detail::solver_validation_history_limit))},
        {"history_summary_byte_limit", Value(std::to_string(summary_byte_limit))},
        {"history_overflow", Value(overflow)},
        {"history_overflow_reason",
         Value(history.overflow ? "row_count"
               : byte_overflow  ? "summary_bytes"
                                : "")}});
}
} // namespace

Result<LocalSolverConfiguration>
load_local_solver_configuration(const std::filesystem::path& path) {
    try {
        check(path.is_absolute() && path.lexically_normal() == path &&
                  !std::filesystem::is_symlink(std::filesystem::symlink_status(path)) &&
                  std::filesystem::is_regular_file(path) &&
                  std::filesystem::file_size(path) <= 16384,
              "Solver configuration must be an explicit bounded local regular file");
        std::ifstream file(path, std::ios::binary);
        const std::string bytes{std::istreambuf_iterator<char>(file),
                                std::istreambuf_iterator<char>()};
        check(file.good() || file.eof(), "Solver configuration could not read");
        QJsonParseError parse;
        const auto document = QJsonDocument::fromJson(QByteArray::fromStdString(bytes), &parse);
        check(parse.error == QJsonParseError::NoError && document.isObject(),
              "Solver configuration JSON is invalid");
        const auto object = document.object();
        QStringList keys{"schema_version",
                         "run_config_id",
                         "executable",
                         "argv",
                         "solver_family",
                         "dialect",
                         "solver_version",
                         "version_evidence",
                         "expected_outputs",
                         "run_root",
                         "test_only",
                         "max_wall_time_ms",
                         "cancel_grace_ms",
                         "max_output_bytes"};
        const auto schema = required_string(object, "schema_version");
        check(schema == "qcae.local-solver-config.v1" || schema == "qcae.local-solver-config.v2",
              "Unsupported solver configuration schema");
        if (schema == "qcae.local-solver-config.v2")
            keys.push_back("result_reader");
        check(object.size() == keys.size(), "Solver configuration has missing or unknown fields");
        for (const auto& key : object.keys())
            check(keys.contains(key), "Solver configuration has an unknown field");
        check(object.value("test_only").isBool(), "Unsupported solver configuration schema");
        LocalSolverConfiguration config;
        config.test_only = object.value("test_only").toBool();
        auto& run = config.run;
        run.id = required_string(object, "run_config_id");
        run.executable = required_string(object, "executable");
        run.argv = string_array(object, "argv", 64, true);
        run.solver_family = required_string(object, "solver_family");
        run.dialect = required_string(object, "dialect");
        run.solver_version = required_string(object, "solver_version");
        run.version_evidence = required_string(object, "version_evidence");
        run.expected_outputs = string_array(object, "expected_outputs", 32, false);
        if (schema == "qcae.local-solver-config.v2") {
            check(object.value("result_reader").isObject(), "Explicit reader object is required");
            const auto selected = object.value("result_reader").toObject();
            const QStringList reader_keys{
                "reader_version", "resource", "subcase", "unit_system", "coordinate_basis"};
            check(selected.size() == reader_keys.size(), "Reader fields are missing or unknown");
            for (const auto& key : selected.keys())
                check(reader_keys.contains(key), "Reader contains an unknown field");
            SolverResultReaderConfiguration reader;
            reader.reader_version = required_string(selected, "reader_version");
            reader.resource = required_string(selected, "resource");
            reader.subcase = budget(selected, "subcase", 1);
            reader.unit_system = required_string(selected, "unit_system");
            reader.coordinate_basis = required_string(selected, "coordinate_basis");
            detail::validate_solver_result_reader(reader, run);
            config.result_reader = std::move(reader);
        }
        run.max_wall_time_ms = budget(object, "max_wall_time_ms", 86400000);
        run.cancel_grace_ms = budget(object, "cancel_grace_ms", 5000, true);
        run.max_output_bytes = budget(object, "max_output_bytes", 16777216);
        config.run_root = required_string(object, "run_root");
        check(std::filesystem::path(run.executable).is_absolute() &&
                  std::filesystem::is_regular_file(run.executable) &&
                  config.run_root.is_absolute() &&
                  config.run_root.lexically_normal() == config.run_root &&
                  std::filesystem::is_directory(config.run_root),
              "Configured executable and run root must be absolute local resources");
        check(config.test_only
                  ? run.solver_family == "test-only"
                  : run.solver_family == "Nastran" && run.dialect == "MSC" &&
                        (run.solver_version == "2022.1" || run.solver_version == "2024.1"),
              "Configured solver family/dialect/version is outside the controlled subset");
        run.configuration_digest =
            artifact_sha256(QJsonDocument(object).toJson(QJsonDocument::Compact).toStdString());
        config.declaration_digest = run.configuration_digest;
        // The configured path alone authorizes this fixed information command, never a solve.
        // Declaration strings and JSON flags cannot certify an installed version.
        config.version_validated = false;
        if (!config.test_only) {
            const auto probe =
                detail::probe_solver_version(run.executable, config.run_root, run.solver_version);
            if (probe.ok()) {
                config.version_probe = *probe.value;
                config.version_validated =
                    detail::solver_version_allows_execution(*probe.value, run.solver_version);
                run.configuration_digest =
                    solver_version_configuration_digest(config.declaration_digest, *probe.value);
                if (!config.version_validated)
                    config.version_probe_failure =
                        "Version observation is unmatched, nonzero or explicitly synthetic";
            } else {
                config.version_probe_failure =
                    probe.error ? probe.error->message
                                : "Version probe failed within its controlled limits";
            }
        }
        return good(std::move(config));
    } catch (const std::exception& error) {
        return bad<LocalSolverConfiguration>(ErrorCode::invalid_input, error.what());
    }
}

struct SolverCoordinator::State {
    std::optional<LocalSolverConfiguration> config;
    SolverArtifactResolver resolve;
    NastranCodec codec;
    RecordApplication* app{};
    std::unique_ptr<LocalSolverRunner> runner;
    std::mutex mutex;
    std::map<std::string, std::shared_ptr<FrozenRun>> pending;
    bool ready() const {
        return app && app->durable() && config &&
               (config->test_only || (config->version_validated && config->version_probe &&
                                      detail::solver_version_allows_execution(
                                          *config->version_probe, config->run.solver_version)));
    }
    bool executable_matches_probe() const {
        if (!config || config->test_only)
            return config.has_value();
        if (!config->version_probe)
            return false;
        const auto observed = detail::solver_executable_identity(config->run.executable);
        return observed.ok() && *observed.value == config->version_probe->executable;
    }
    Result<std::pair<StoredSolverResult, std::shared_ptr<const OwnedRowImage>>>
    find_result(const DocumentRef& document, std::string_view id) const {
        const auto rows =
            app->owned_rows(document, StoreSpace::artifact_record, "qcae.solver.result");
        if (!rows.ok())
            return {rows.status, {}, rows.error};
        for (const auto& image : *rows.value)
            if (image->key.identity == id)
                return good(std::make_pair(detail::decode_solver_result_row(*image), image));
        return bad<std::pair<StoredSolverResult, std::shared_ptr<const OwnedRowImage>>>(
            ErrorCode::entity_not_found, "Parsed solver result does not exist");
    }
    Result<std::pair<SolverOwnedRun, std::shared_ptr<const OwnedRowImage>>>
    find(const DocumentRef& document, std::string_view id) const {
        const auto rows = app->owned_rows(document, StoreSpace::artifact_record, run_owner);
        if (!rows.ok())
            return {rows.status, {}, rows.error};
        for (const auto& image : *rows.value)
            if (image->key.identity == id)
                return good(std::make_pair(decode_solver_owned_run(*image), image));
        return bad<std::pair<SolverOwnedRun, std::shared_ptr<const OwnedRowImage>>>(
            ErrorCode::entity_not_found, "Analysis run does not exist");
    }
    Result<std::pair<TaskRecord, std::shared_ptr<const OwnedRowImage>>>
    find_task(const SolverOwnedRun& fact, const DocumentRef& document) const {
        const auto rows = app->owned_rows(document, StoreSpace::task_record, task_owner);
        if (!rows.ok())
            return {rows.status, {}, rows.error};
        for (const auto& image : *rows.value) {
            if (image->key.identity != fact.run.request.task_id)
                continue;
            const auto task = decode_task_record(*image->payload);
            validate_solver_task(fact, task);
            return good(std::make_pair(task, image));
        }
        return bad<std::pair<TaskRecord, std::shared_ptr<const OwnedRowImage>>>(
            ErrorCode::schema_unsupported, "Analysis run task fact is missing");
    }
    Result<bool> validate_link(const SolverOwnedRun& fact, const DocumentRef& document) const {
        const auto task = find_task(fact, document);
        if (!task.ok())
            return {task.status, {}, task.error};
        if (fact.result_artifact) {
            const auto result = find_result(document, fact.result_artifact->artifact_id);
            if (!result.ok())
                return {result.status, {}, result.error};
            detail::validate_solver_result_link(result.value->first.parsed, fact);
            check(result.value->first.artifact.manifest == fact.result_artifact->manifest &&
                      result.value->first.artifact.directory == fact.result_artifact->directory &&
                      result.value->first.artifact.root_resource ==
                          fact.result_artifact->root_resource &&
                      result.value->first.artifact.files == fact.result_artifact->files &&
                      result.value->first.published == fact.result_published,
                  "Result/run publication cross-row facts differ");
        }
        return good(true);
    }
    std::shared_ptr<const TaskPayload> prepare_result(std::string_view id,
                                                      const TaskControl& control) const {
        const auto document = app->current_document();
        if (!document.ok())
            return completion(TaskState::outcome_unknown,
                              ErrorCode::storage_uncertain,
                              "Result preparation lost its original document session");
        const auto found = find(document.value->document, id);
        if (!found.ok())
            return completion(TaskState::outcome_unknown,
                              ErrorCode::storage_uncertain,
                              "Original run fact is unavailable for result preparation");
        auto fact = found.value->first;
        check(fact.result_reader.has_value(), "Result preparation lacks its frozen reader");
        if (fact.run.request.input.document != document.value->document.id ||
            fact.run.request.input.epoch != document.value->document.epoch)
            return completion(TaskState::outcome_unknown,
                              ErrorCode::document_epoch_expired,
                              "Result preparation cannot write a reopened document");
        const auto linked = validate_link(fact, document.value->document);
        if (!linked.ok())
            return completion(TaskState::failed,
                              ErrorCode::schema_unsupported,
                              linked.error ? linked.error->message : "Run/task linkage is invalid");
        const auto source =
            resolve({fact.principal}, document.value->document, fact.run.request.input.artifact_id);
        if (!source.ok())
            return completion(TaskState::failed,
                              ErrorCode::invalid_input,
                              "Original input artifact is no longer verified");
        check(features::analysis::encode_frozen_analysis_input(source.value->first) ==
                      features::analysis::encode_frozen_analysis_input(fact.source_input) &&
                  artifact_sha256(source.value->second.manifest) ==
                      fact.run.request.input.manifest_sha256,
              "Original source artifact changed before result parsing");
        control.checkpoint();
        const auto outputs = detail::read_verified_solver_outputs(fact.run);
        const auto parsed =
            outputs.ok() ? detail::prepare_solver_result(fact, *fact.result_reader, *outputs.value)
                         : Result<SolverParsedResult>{outputs.status, {}, outputs.error};
        if (!parsed.ok()) {
            check(fact.run.sequence != UINT64_MAX, "Result sequence exceeds quota");
            ++fact.run.sequence;
            fact.run.parsing = SolverParsingState::failed;
            fact.run.detail = parsed.error ? parsed.error->message : "Result parsing failed";
            const auto next = solver_owned_row(fact);
            const OwnedRowUpdate update{next->key, found.value->second->payload, next};
            const auto saved = app->update_owned_rows(
                {fact.principal}, document.value->document, std::span(&update, 1));
            return completion(saved.ok() ? TaskState::failed : TaskState::outcome_unknown,
                              saved.ok() ? ErrorCode::invalid_input : ErrorCode::storage_uncertain,
                              fact.run.detail);
        }
        control.checkpoint();
        StoredSolverResult result{
            *parsed.value,
            detail::solver_result_artifact(*parsed.value,
                                           std::filesystem::path(fact.run.request.run_directory) /
                                               ".qcae-result"),
            false};
        auto resources = detail::solver_result_resources(result.parsed, *outputs.value);
        check(fact.run.sequence != UINT64_MAX, "Result sequence exceeds quota");
        ++fact.run.sequence;
        fact.run.parsing = SolverParsingState::parsed;
        fact.result_artifact = result.artifact;
        const auto next_run = solver_owned_row(fact);
        const auto next_result = detail::solver_result_row(result);
        const std::array<OwnedRowUpdate, 2> updates{
            {{next_run->key, found.value->second->payload, next_run},
             {next_result->key, {}, next_result}}};
        const auto saved =
            app->update_owned_rows({fact.principal}, document.value->document, updates);
        if (!saved.ok())
            return completion(TaskState::outcome_unknown,
                              ErrorCode::storage_uncertain,
                              "Result startup intent did not reach confirmed persistence");
        try {
            (void)LocalArtifactStore{}.stage(
                result.artifact, resources, [&] { control.checkpoint(); });
        } catch (const std::exception& error) {
            if (control.cancellation_requested())
                control.checkpoint();
            return completion(TaskState::failed, ErrorCode::storage_failure, error.what());
        }
        control.checkpoint();
        return std::make_shared<const SolverResultPayload>(std::move(result));
    }
};
SolverCoordinator::SolverCoordinator(std::optional<LocalSolverConfiguration> config,
                                     SolverArtifactResolver resolver)
    : state_(std::make_shared<State>()) {
    check(static_cast<bool>(resolver), "Solver coordinator requires a trusted artifact resolver");
    state_->config = std::move(config);
    state_->resolve = std::move(resolver);
}
SolverCoordinator::~SolverCoordinator() = default;
OwnedRowHandler SolverCoordinator::row_handler() {
    return solver_run_row_handler();
}
TaskPublisher SolverCoordinator::wrap_publisher(RecordApplication& app, TaskPublisher base) {
    state_->app = &app;
    const auto state = state_;
    const std::weak_ptr<State> weak = state;
    LocalSolverHost host;
    host.input_artifact = [weak](const SolverRunRequest& request) -> Result<LocalArtifactIntent> {
        const auto self = weak.lock();
        const DocumentRef document{request.input.document, request.input.epoch};
        const auto run = self->find(document, request.run_id);
        if (!run.ok())
            return {run.status, {}, run.error};
        const auto artifact =
            self->resolve({run.value->first.principal}, document, request.input.artifact_id);
        if (!artifact.ok())
            return {artifact.status, {}, artifact.error};
        return good(artifact.value->second);
    };
    host.validate_start = [weak](const SolverRunRequest& request,
                                 const LocalArtifactIntent& artifact) {
        const auto self = weak.lock();
        const DocumentRef document{request.input.document, request.input.epoch};
        const auto run = self->find(document, request.run_id);
        if (!run.ok())
            return Result<bool>{run.status, {}, run.error};
        const auto linked = self->validate_link(run.value->first, document);
        if (!linked.ok())
            return linked;
        const auto source =
            self->resolve({run.value->first.principal}, document, request.input.artifact_id);
        if (!source.ok())
            return Result<bool>{source.status, {}, source.error};
        return good(
            self->ready() && self->executable_matches_probe() &&
            run.value->first.version_probe == self->config->version_probe &&
            run.value->first.result_reader == self->config->result_reader &&
            run.value->first.run.request == request &&
            artifact.manifest == source.value->second.manifest &&
            features::analysis::encode_frozen_analysis_input(source.value->first) ==
                features::analysis::encode_frozen_analysis_input(run.value->first.source_input) &&
            request.input.profile == self->codec.definition().reference &&
            request.run_directory == (self->config->run_root / request.run_id).string() &&
            request.configuration == expanded_configuration(*self->config,
                                                            request.run_directory,
                                                            source.value->second.root_resource));
    };
    host.load = [weak](std::string_view id) -> Result<std::optional<SolverRunRecord>> {
        const auto self = weak.lock();
        const auto info = self->app->current_document();
        if (!info.ok())
            return {info.status, {}, info.error};
        const auto found = self->find(info.value->document, id);
        if (!found.ok())
            return {found.status, {}, found.error};
        return good(found.value->first.startup_intent_persisted
                        ? std::optional<SolverRunRecord>(found.value->first.run)
                        : std::nullopt);
    };
    host.persist = [weak](const SolverRunRecord& next,
                          const std::optional<SolverRunRecord>& expected) {
        const auto self = weak.lock();
        const DocumentRef document{next.request.input.document, next.request.input.epoch};
        const auto found = self->find(document, next.request.run_id);
        if (!found.ok())
            return Result<bool>{found.status, {}, found.error};
        auto fact = found.value->first;
        const auto linked = self->validate_link(fact, document);
        if (!linked.ok())
            return linked;
        auto expected_fact = fact;
        if (expected) {
            expected_fact.run = *expected;
            check(fact.startup_intent_persisted &&
                      encode_solver_owned_run(expected_fact) == *found.value->second->payload,
                  "Run execution CAS expected fact differs");
        } else {
            check(!fact.startup_intent_persisted && fact.run.request == next.request &&
                      next.sequence == 1 && next.execution == SolverExecutionState::startup_intent,
                  "Startup intent cannot replace an existing execution fact");
        }
        fact.startup_intent_persisted = true;
        fact.run = next;
        const auto row = solver_owned_row(fact);
        const OwnedRowUpdate update{row->key, found.value->second->payload, row};
        return self->app->update_owned_rows({fact.principal}, document, std::span(&update, 1));
    };
    state->runner = std::make_unique<LocalSolverRunner>(std::move(host));
    auto publisher = base;
    publisher.persist = [state, base, &app](const TaskRecord& task,
                                            const std::optional<TaskRecord>& previous) {
        if (task.operation != "analysis.start" || previous)
            return base.persist(task, previous);
        try {
            std::shared_ptr<FrozenRun> frozen;
            {
                std::lock_guard lock(state->mutex);
                const auto found =
                    state->pending.find(pending_key(task.caller, task.idempotency_key));
                check(found != state->pending.end(),
                      "Admitted analysis frozen snapshot is missing");
                frozen = found->second;
            }
            auto& fact = frozen->fact;
            auto& request = fact.run.request;
            request.task_id = task.id;
            request.run_id = "run-" + task.id;
            request.run_directory = (state->config->run_root / request.run_id).string();
            request.configuration = expanded_configuration(
                *state->config, request.run_directory, frozen->artifact.root_resource);
            validate_solver_task(fact, task);
            const auto run_row = solver_owned_row(fact), next_task = task_row(task);
            const std::array<OwnedRowUpdate, 2> updates{
                {{next_task->key, {}, next_task}, {run_row->key, {}, run_row}}};
            return app.update_owned_rows(task.caller, task.input.document, updates);
        } catch (const std::exception& error) {
            return bad<bool>(ErrorCode::invalid_input, error.what());
        }
    };
    publisher.publish = [state, base, &app](const std::shared_ptr<const TaskPayload>& payload,
                                            const TaskRecord& previous,
                                            TaskRecord complete) -> Result<TaskRecord> {
        const auto candidate = std::dynamic_pointer_cast<const SolverResultPayload>(payload);
        if (!candidate)
            return base.publish(payload, previous, std::move(complete));
        bool filesystem_started{};
        try {
            const auto session = base.validate_session(previous.input.document);
            if (!session.ok())
                return {session.status, {}, session.error};
            const auto run = state->find(previous.input.document, "run-" + previous.id);
            if (!run.ok())
                return {run.status, {}, run.error};
            auto fact = run.value->first;
            validate_solver_task(fact, previous);
            const auto linked = state->validate_link(fact, previous.input.document);
            if (!linked.ok())
                return {linked.status, {}, linked.error};
            const auto saved =
                state->find_result(previous.input.document, candidate->result.artifact.artifact_id);
            if (!saved.ok())
                return {saved.status, {}, saved.error};
            check(!saved.value->first.published &&
                      *detail::solver_result_row(candidate->result)->payload ==
                          *saved.value->second->payload,
                  "Result payload differs from its durable publication intent");
            const auto source = state->resolve(
                previous.caller, previous.input.document, fact.run.request.input.artifact_id);
            if (!source.ok())
                return {source.status, {}, source.error};
            check(features::analysis::encode_frozen_analysis_input(source.value->first) ==
                          features::analysis::encode_frozen_analysis_input(fact.source_input) &&
                      artifact_sha256(source.value->second.manifest) ==
                          fact.run.request.input.manifest_sha256,
                  "Result publication source provenance changed");
            filesystem_started = true;
            const auto& artifact = candidate->result.artifact;
            LocalArtifactStore store;
            store.publish(artifact);
            verify_result_artifact(candidate->result);
            auto published = candidate->result;
            published.published = true;
            check(fact.run.sequence != UINT64_MAX, "Result publication sequence exceeds quota");
            ++fact.run.sequence;
            fact.result_published = true;
            complete.artifact_receipt = TaskArtifactReceipt{artifact.artifact_id,
                                                            fact.run.request.input.revision,
                                                            artifact_sha256(artifact.manifest)};
            validate_solver_task(fact, complete);
            detail::validate_solver_result_link(published.parsed, fact);
            const auto next_task = task_row(complete), next_run = solver_owned_row(fact),
                       next_result = detail::solver_result_row(published);
            const std::array<OwnedRowUpdate, 3> updates{
                {{next_task->key, task_row(previous)->payload, next_task},
                 {next_run->key, run.value->second->payload, next_run},
                 {next_result->key, saved.value->second->payload, next_result}}};
            const auto committed =
                app.update_owned_rows(previous.caller, previous.input.document, updates);
            if (!committed.ok())
                return publication_uncertain<TaskRecord>(committed);
            return good(std::move(complete));
        } catch (const std::exception& error) {
            return bad<TaskRecord>(filesystem_started ? ErrorCode::storage_uncertain
                                                      : ErrorCode::schema_unsupported,
                                   error.what());
        }
    };
    return publisher;
}

Result<bool> SolverCoordinator::register_operations(OperationRegistry& registry,
                                                    RecordApplication& app,
                                                    std::function<TaskService&()> tasks) {
    state_->app = &app;
    const auto state = state_;
    auto result = registry.register_typed<SolverConfigurationInput>(
        InputTraits<SolverConfigurationInput>::definition(),
        [state](const OperationContext&, const SolverConfigurationInput&) {
            const bool executable_current = state->executable_matches_probe();
            return good(Value(Value::Object{
                {"configured", Value(state->config.has_value())},
                {"validated",
                 Value(state->ready() && executable_current && !state->config->test_only)},
                {"test_only", Value(state->config && state->config->test_only)},
                {"process_adapter_ready", Value(state->ready() && executable_current)},
                {"run_config_id", Value(state->config ? state->config->run.id : "")},
                {"configuration_digest",
                 Value(state->config ? state->config->run.configuration_digest : "")},
                {"version_probe",
                 version_value(state->config ? state->config->version_probe : std::nullopt)},
                {"version_probe_available",
                 Value(state->config && state->config->version_probe.has_value())},
                {"version_probe_failure",
                 Value(state->config ? state->config->version_probe_failure : "")},
                {"executable_identity_current", Value(executable_current)},
                {"result_reader_available",
                 Value(state->config && state->config->result_reader.has_value())},
                {"numerical_comparison_available", Value(true)},
                {"numerical_validation_available",
                 Value(state->ready() && executable_current && !state->config->test_only &&
                       state->config->result_reader.has_value())}}));
        });
    if (!result.ok())
        return result;
    result = registry.register_typed<AnalysisValidateResultInput>(
        InputTraits<AnalysisValidateResultInput>::definition(),
        [state, &app](const OperationContext& context,
                      const AnalysisValidateResultInput& input) -> Result<Value> {
            try {
                const auto& profile = state->codec.definition().reference;
                if (*context.expected_profile != profile)
                    return bad<Value>(
                        ErrorCode::invalid_input,
                        "Numerical reference requires the registered Nastran profile");
                const auto run = state->find(*context.document, input.run_id);
                if (!run.ok())
                    return {run.status, {}, run.error};
                const auto& fact = run.value->first;
                check(fact.principal == context.caller.principal,
                      "Numerical result belongs to another principal");
                const auto linked = state->validate_link(fact, *context.document);
                if (!linked.ok())
                    return {linked.status, {}, linked.error};
                if (!fact.result_published || !fact.result_artifact)
                    return bad<Value>(
                        ErrorCode::missing_input,
                        "Numerical checks require a complete published parsed result");
                const auto stored =
                    state->find_result(*context.document, fact.result_artifact->artifact_id);
                if (!stored.ok())
                    return {stored.status, {}, stored.error};
                const auto source = state->resolve(
                    context.caller, *context.document, fact.run.request.input.artifact_id);
                if (!source.ok())
                    return {source.status, {}, source.error};
                check(features::analysis::encode_frozen_analysis_input(source.value->first) ==
                              features::analysis::encode_frozen_analysis_input(fact.source_input) &&
                          artifact_sha256(source.value->second.manifest) ==
                              fact.run.request.input.manifest_sha256,
                      "Numerical source artifact differs from the original frozen input");
                const auto history =
                    validation_history(app, *context.document, stored.value->first, profile);
                if (!history.ok())
                    return {history.status, {}, history.error};
                if (history.value->overflow)
                    return bad<Value>(ErrorCode::resource_limit,
                                      "Numerical validation history exceeds its bounded index");
                const auto signature = record_wire::strings(
                    std::array<std::string, 8>{context.document->id.value,
                                               context.document->epoch.value,
                                               std::to_string(*context.expected_revision),
                                               input.run_id,
                                               context.caller.principal,
                                               profile.profile_id,
                                               profile.profile_version,
                                               profile.definition_digest});
                const auto identity = detail::solver_validation_identity(
                    input.run_id, context.caller, context.idempotency_key);
                const detail::SolverValidationFact* replay{};
                for (const auto& entry : history.value->reports)
                    if (entry.second->key.identity == identity) {
                        if (entry.first.signature != signature)
                            return bad<Value>(
                                ErrorCode::idempotency_key_conflict,
                                "Numerical validation key belongs to another request");
                        replay = &entry.first;
                        break;
                    }
                bool files_verified{};
                try {
                    verify_result_artifact(stored.value->first);
                    files_verified = true;
                } catch (const std::exception&) {
                    if (!replay)
                        return bad<Value>(
                            ErrorCode::invalid_input,
                            "Numerical admission requires verified original result files");
                }
                detail::SolverValidationFact checked;
                if (replay) {
                    checked = *replay;
                } else {
                    const auto snapshot = app.snapshot(*context.document);
                    if (!snapshot.ok())
                        return {snapshot.status, {}, snapshot.error};
                    if (snapshot.value->info.revision != *context.expected_revision)
                        return bad<Value>(ErrorCode::revision_conflict,
                                          "Numerical validation revision is stale");
                    if (history.value->reports.size() >= detail::solver_validation_history_limit)
                        return bad<Value>(ErrorCode::resource_limit,
                                          "Numerical validation history has reached 16 reports");
                    const auto prepared =
                        detail::prepare_solver_validation(stored.value->first, profile);
                    if (!prepared.ok())
                        return {prepared.status, {}, prepared.error};
                    checked = *prepared.value;
                    checked.idempotency_key = context.idempotency_key;
                    checked.signature = signature;
                    checked.ordinal = history.value->reports.size() + 1;
                    const auto next_report = detail::solver_validation_row(checked);
                    detail::SolverValidationIndex index{input.run_id, context.caller.principal, {}};
                    if (history.value->index)
                        index = detail::decode_solver_validation_index(*history.value->index);
                    index.report_rows.push_back(identity);
                    const auto next_index = detail::solver_validation_index_row(index);
                    const std::array<OwnedRowUpdate, 2> updates{
                        {{next_report->key, {}, next_report},
                         {next_index->key,
                          history.value->index ? history.value->index->payload : nullptr,
                          next_index}}};
                    const auto committed = app.update_owned_rows(
                        context.caller,
                        WriteContext{*context.document, *context.expected_revision},
                        updates);
                    // This CAS has no filesystem side effect. Definite rollback retains
                    // its original storage failure; only Core can report unknown storage.
                    if (!committed.ok())
                        return {committed.status, {}, committed.error};
                }
                return good(Value(Value::Object{
                    {"summary", validation_summary(checked, identity)},
                    {"components", validation_components(checked)},
                    {"original_run_validation_stage", Value("not_run")},
                    {"original_source_state",
                     Value(original_input_current(app, *context.document, fact) ? "current"
                                                                                : "stale")},
                    {"current_files_verified", Value(files_verified)},
                    {"replayed", Value(replay != nullptr)}}));
            } catch (const RecordError& error) {
                return bad<Value>(error.code(), error.what());
            } catch (const std::exception& error) {
                return bad<Value>(ErrorCode::schema_unsupported, error.what());
            }
        });
    if (!result.ok())
        return result;
    result = registry.register_typed<AnalysisReconcileResultInput>(
        InputTraits<AnalysisReconcileResultInput>::definition(),
        [state, &app, tasks](const OperationContext& context,
                             const AnalysisReconcileResultInput& input) -> Result<Value> {
            try {
                const auto signature = record_wire::strings(
                    std::array<std::string, 5>{context.document->id.value,
                                               context.document->epoch.value,
                                               std::to_string(*context.expected_revision),
                                               input.run_id,
                                               context.caller.principal});
                const auto fact_id = reconcile_identity(context.caller, context.idempotency_key);
                const auto prior =
                    app.owned_rows(*context.document, StoreSpace::artifact_record, reconcile_owner);
                if (!prior.ok())
                    return {prior.status, {}, prior.error};
                std::optional<std::vector<std::string>> replay;
                for (const auto& image : *prior.value)
                    if (image->key.identity == fact_id) {
                        const auto saved = record_wire::read_strings(*image->payload);
                        if (saved[3] != signature)
                            return bad<Value>(ErrorCode::idempotency_key_conflict,
                                              "Result reconcile key belongs to another request");
                        replay = saved;
                        break;
                    }
                if (!replay) {
                    const auto snapshot = app.snapshot(*context.document);
                    if (!snapshot.ok())
                        return {snapshot.status, {}, snapshot.error};
                    if (snapshot.value->info.revision != *context.expected_revision)
                        return bad<Value>(ErrorCode::revision_conflict,
                                          "Result reconcile revision is stale");
                    if (tasks().active_workers() || tasks().queued_tasks())
                        return bad<Value>(ErrorCode::invalid_input,
                                          "Result reconcile requires quiescent tasks");
                }
                const auto found = state->find(*context.document, input.run_id);
                if (!found.ok())
                    return {found.status, {}, found.error};
                auto fact = found.value->first;
                check(fact.principal == context.caller.principal,
                      "Result reconcile belongs to another principal");
                const auto linked = state->validate_link(fact, *context.document);
                if (!linked.ok())
                    return {linked.status, {}, linked.error};
                if (!fact.result_artifact)
                    return bad<Value>(ErrorCode::missing_input,
                                      "Result reconcile requires a durable parsed intent");
                const auto stored =
                    state->find_result(*context.document, fact.result_artifact->artifact_id);
                if (!stored.ok())
                    return {stored.status, {}, stored.error};
                auto published = stored.value->first;
                const auto source = state->resolve(
                    context.caller, *context.document, fact.run.request.input.artifact_id);
                if (!source.ok())
                    return {source.status, {}, source.error};
                check(features::analysis::encode_frozen_analysis_input(source.value->first) ==
                              features::analysis::encode_frozen_analysis_input(fact.source_input) &&
                          artifact_sha256(source.value->second.manifest) ==
                              fact.run.request.input.manifest_sha256,
                      "Result reconcile source differs from the original frozen input");
                const auto manifest_hash = artifact_sha256(published.artifact.manifest);
                const auto response = [&](bool replayed, bool currently_verified) {
                    return good(Value(Value::Object{
                        {"run_id", Value(input.run_id)},
                        {"result_id", Value(published.artifact.artifact_id)},
                        {"task_id", Value(fact.run.request.task_id)},
                        {"task_state", Value("succeeded")},
                        {"source_kind", Value(fact.test_only ? "test_process" : "external_solver")},
                        {"test_only", Value(fact.test_only)},
                        {"parsing", Value("parsed")},
                        {"numerical_validation", Value("not_run")},
                        {"input_revision", Value(std::to_string(fact.run.request.input.revision))},
                        {"result_manifest_sha256", Value(manifest_hash)},
                        {"replayed", Value(replayed)},
                        {"verified_at_reconcile", Value(true)},
                        {"current_files_verified", Value(currently_verified)}}));
                };
                if (replay) {
                    check(fact.result_published && published.published &&
                              (*replay)[4] == input.run_id &&
                              (*replay)[5] == published.artifact.artifact_id &&
                              (*replay)[6] == manifest_hash,
                          "Result reconcile fact differs from completed publication");
                    bool verified{};
                    try {
                        verify_result_artifact(published);
                        verified = true;
                    } catch (const std::exception&) {
                    }
                    return response(true, verified);
                }
                const auto task = state->find_task(fact, *context.document);
                if (!task.ok())
                    return {task.status, {}, task.error};
                auto complete = task.value->first;
                if (complete.state != TaskState::succeeded &&
                    complete.state != TaskState::interrupted &&
                    complete.state != TaskState::outcome_unknown)
                    return bad<Value>(ErrorCode::invalid_input,
                                      "Only completed or unknown/interrupted result publication "
                                      "can be reconciled; failed/cancelled tasks cannot succeed");
                verify_result_artifact(published);
                std::vector<OwnedRowUpdate> updates;
                if (!fact.result_published) {
                    check(fact.run.sequence != UINT64_MAX,
                          "Result reconcile sequence exceeds quota");
                    ++fact.run.sequence;
                    fact.result_published = true;
                    published.published = true;
                    const auto next_run = solver_owned_row(fact),
                               next_result = detail::solver_result_row(published);
                    updates.push_back({next_run->key, found.value->second->payload, next_run});
                    updates.push_back(
                        {next_result->key, stored.value->second->payload, next_result});
                }
                if (complete.state != TaskState::succeeded) {
                    complete.state = TaskState::succeeded;
                    complete.progress = 1;
                    complete.diagnostic.reset();
                    complete.artifact_receipt = TaskArtifactReceipt{published.artifact.artifact_id,
                                                                    fact.run.request.input.revision,
                                                                    manifest_hash};
                    check(!complete.events.empty() &&
                              complete.events.back().sequence != UINT64_MAX &&
                              complete.events.size() < 128,
                          "Result reconcile task event quota exceeded");
                    complete.events.push_back(
                        {complete.events.back().sequence + 1, complete.state, complete.progress});
                    const auto next_task = task_row(complete);
                    updates.push_back({next_task->key, task.value->second->payload, next_task});
                }
                validate_solver_task(fact, complete);
                detail::validate_solver_result_link(published.parsed, fact);
                const auto receipt = std::make_shared<const OwnedRowImage>(
                    OwnedRowImage{{StoreSpace::artifact_record, fact_id},
                                  std::string(reconcile_owner),
                                  1,
                                  std::make_shared<const std::string>(record_wire::strings(
                                      std::array<std::string, 7>{"QCAE-SOLVER-RECONCILE-1",
                                                                 context.caller.principal,
                                                                 context.idempotency_key,
                                                                 signature,
                                                                 input.run_id,
                                                                 published.artifact.artifact_id,
                                                                 manifest_hash})),
                                  {}});
                updates.push_back({receipt->key, {}, receipt});
                const auto committed =
                    app.update_owned_rows(context.caller, *context.document, updates);
                if (!committed.ok())
                    return publication_uncertain<Value>(committed);
                const auto reloaded = tasks().reconcile();
                if (!reloaded.ok())
                    return {reloaded.status, {}, reloaded.error};
                return response(false, true);
            } catch (const std::exception& error) {
                return bad<Value>(ErrorCode::storage_failure, error.what());
            }
        });
    if (!result.ok())
        return result;
    result = registry.register_typed<AnalysisGetResultInput>(
        InputTraits<AnalysisGetResultInput>::definition(),
        [state, &app](const OperationContext& context,
                      const AnalysisGetResultInput& input) -> Result<Value> {
            try {
                const auto run = state->find(*context.document, input.run_id);
                if (!run.ok())
                    return {run.status, {}, run.error};
                const auto& fact = run.value->first;
                check(fact.principal == context.caller.principal,
                      "Solver result belongs to another principal");
                const auto linked = state->validate_link(fact, *context.document);
                if (!linked.ok())
                    return {linked.status, {}, linked.error};
                if (!fact.result_published || !fact.result_artifact)
                    return bad<Value>(ErrorCode::missing_input,
                                      "A complete published parsed result is not available");
                const auto stored =
                    state->find_result(*context.document, fact.result_artifact->artifact_id);
                if (!stored.ok())
                    return {stored.status, {}, stored.error};
                const auto& parsed = stored.value->first.parsed;
                const auto source = state->resolve(
                    context.caller, *context.document, fact.run.request.input.artifact_id);
                if (!source.ok())
                    return {source.status, {}, source.error};
                check(features::analysis::encode_frozen_analysis_input(source.value->first) ==
                              features::analysis::encode_frozen_analysis_input(fact.source_input) &&
                          artifact_sha256(source.value->second.manifest) ==
                              fact.run.request.input.manifest_sha256,
                      "Parsed result source artifact differs from its frozen input");
                bool current{};
                const auto snapshot = app.snapshot(*context.document);
                if (snapshot.ok()) {
                    const auto now =
                        features::analysis::freeze_analysis_input(snapshot.value->records,
                                                                  fact.run.request.input.analysis,
                                                                  fact.run.request.input.profile,
                                                                  fact.source_input.identities);
                    current = now.ok() && artifact_sha256(now.value->input_signature) ==
                                              fact.run.request.input.input_fingerprint;
                }
                bool raw_verified{};
                try {
                    verify_result_artifact(stored.value->first);
                    raw_verified = true;
                } catch (const std::exception&) {
                    // The database retains parsed fields/provenance. Missing or damaged
                    // external bytes are explicitly unavailable and never declared verified.
                }
                const auto history = validation_history(app,
                                                        *context.document,
                                                        stored.value->first,
                                                        state->codec.definition().reference);
                if (!history.ok())
                    return {history.status, {}, history.error};
                auto field_value = [](std::string quantity,
                                      const std::array<std::string, 6>& units,
                                      const std::vector<NastranStaticGridValues>& rows) {
                    Value::Array component_names, unit_values, values;
                    for (const auto* name : {"T1", "T2", "T3", "R1", "R2", "R3"})
                        component_names.emplace_back(name);
                    for (const auto& unit : units)
                        unit_values.emplace_back(unit);
                    for (const auto& row : rows) {
                        Value::Array components;
                        for (const auto component : row.components)
                            components.emplace_back(component);
                        values.emplace_back(Value::Object{
                            {"entity_id", Value(row.entity.value)},
                            {"solver_number", Value(std::to_string(row.solver_number))},
                            {"components", Value(std::move(components))}});
                    }
                    return Value(
                        Value::Object{{"quantity_id", Value(std::move(quantity))},
                                      {"location", Value("node")},
                                      {"component_names", Value(std::move(component_names))},
                                      {"component_units", Value(std::move(unit_values))},
                                      {"values", Value(std::move(values))}});
                };
                Value::Array fields;
                fields.push_back(field_value(
                    "displacement", parsed.fields.displacement_units, parsed.fields.displacements));
                fields.push_back(field_value(
                    "spc_reaction", parsed.fields.reaction_units, parsed.fields.spc_reactions));
                return good(Value(Value::Object{
                    {"run_id", Value(input.run_id)},
                    {"result_id", Value(stored.value->first.artifact.artifact_id)},
                    {"task_id", Value(fact.run.request.task_id)},
                    {"test_only", Value(fact.test_only)},
                    {"source_kind", Value(fact.test_only ? "test_process" : "external_solver")},
                    {"parsing", Value("parsed")},
                    {"numerical_validation", Value("not_run")},
                    {"numerical_checks", numerical_checks(*history.value, current, raw_verified)},
                    {"input_current", Value(current)},
                    {"input_revision", Value(std::to_string(fact.run.request.input.revision))},
                    {"reader_version", Value(parsed.reader.reader_version)},
                    {"subcase", Value(std::to_string(parsed.reader.subcase))},
                    {"unit_system", Value(parsed.reader.unit_system)},
                    {"coordinate_basis", Value(parsed.reader.coordinate_basis)},
                    {"raw_resources_verified", Value(raw_verified)},
                    {"result_manifest_sha256",
                     Value(artifact_sha256(stored.value->first.artifact.manifest))},
                    {"version_probe", version_value(fact.version_probe)},
                    {"fields", Value(std::move(fields))}}));
            } catch (const std::exception& error) {
                return bad<Value>(ErrorCode::schema_unsupported, error.what());
            }
        });
    if (!result.ok())
        return result;
    result = registry.register_typed<AnalysisGetRunInput>(
        InputTraits<AnalysisGetRunInput>::definition(),
        [state, &app](const OperationContext& context,
                      const AnalysisGetRunInput& input) -> Result<Value> {
            try {
                const auto found = state->find(*context.document, input.run_id);
                if (!found.ok())
                    return {found.status, {}, found.error};
                const auto& fact = found.value->first;
                check(fact.principal == context.caller.principal,
                      "Analysis run belongs to another principal");
                const auto& record = fact.run;
                // Cross-row facts are checked in their original context; carried facts retain it.
                const auto linked = state->validate_link(fact, *context.document);
                if (!linked.ok())
                    return {linked.status, {}, linked.error};
                const auto source = state->resolve(
                    context.caller, *context.document, record.request.input.artifact_id);
                if (!source.ok())
                    return {source.status, {}, source.error};
                check(features::analysis::encode_frozen_analysis_input(source.value->first) ==
                              features::analysis::encode_frozen_analysis_input(fact.source_input) &&
                          artifact_sha256(source.value->second.manifest) ==
                              record.request.input.manifest_sha256,
                      "Analysis run source artifact provenance differs");
                bool current{};
                const auto snapshot = app.snapshot(*context.document);
                if (snapshot.ok()) {
                    const auto input_now =
                        features::analysis::freeze_analysis_input(snapshot.value->records,
                                                                  record.request.input.analysis,
                                                                  record.request.input.profile,
                                                                  fact.source_input.identities);
                    current = input_now.ok() && artifact_sha256(input_now.value->input_signature) ==
                                                    record.request.input.input_fingerprint;
                }
                Value::Array outputs;
                for (const auto& file : record.outputs)
                    outputs.emplace_back(
                        Value::Object{{"path", Value(file.path)},
                                      {"byte_length", Value(std::to_string(file.byte_length))},
                                      {"sha256", Value(file.sha256)}});
                return good(Value(Value::Object{
                    {"run_id", Value(input.run_id)},
                    {"task_id", Value(record.request.task_id)},
                    {"execution",
                     Value(fact.startup_intent_persisted ? execution_name(record.execution)
                                                         : "queued")},
                    {"parsing",
                     Value(record.parsing == SolverParsingState::parsed   ? "parsed"
                           : record.parsing == SolverParsingState::failed ? "failed"
                                                                          : "not_run")},
                    {"numerical_validation", Value("not_run")},
                    {"results_available", Value(fact.result_published)},
                    {"result_id",
                     Value(fact.result_artifact ? fact.result_artifact->artifact_id : "")},
                    {"test_only", Value(fact.test_only)},
                    {"source_kind",
                     Value(fact.test_only ? "test_process" : "external_process_observation")},
                    {"input_current", Value(current)},
                    {"input_revision", Value(std::to_string(record.request.input.revision))},
                    {"artifact_id", Value(record.request.input.artifact_id)},
                    {"version_probe", version_value(fact.version_probe)},
                    {"version_probe_available", Value(fact.version_probe.has_value())},
                    {"outputs", Value(std::move(outputs))},
                    {"exit_code", Value(record.exit_code ? std::to_string(*record.exit_code) : "")},
                    {"termination_signal",
                     Value(record.termination_signal ? std::to_string(*record.termination_signal)
                                                     : "")},
                    {"cancellation_requested", Value(record.cancellation_requested)},
                    {"detail", Value(record.detail)}}));
            } catch (const std::exception& error) {
                return bad<Value>(ErrorCode::schema_unsupported, error.what());
            }
        });
    if (!result.ok())
        return result;
    if (!state->ready())
        return registry.declare_unavailable(
            InputTraits<AnalysisStartInput>::definition(),
            "A trusted local solver configuration and actual version verification are required");
    return registry.register_typed<AnalysisStartInput>(
        InputTraits<AnalysisStartInput>::definition(),
        [state, &app, tasks](const OperationContext& context,
                             const AnalysisStartInput& input) -> Result<Value> {
            try {
                check(input.run_config_id == state->config->run.id,
                      "Requested local solver configuration is not installed");
                const auto signature =
                    solver_run_signature(input.analysis_id,
                                         input.artifact_id,
                                         input.run_config_id,
                                         state->config->run.configuration_digest);
                const TaskInputContext task_input{
                    *context.document, *context.expected_revision, *context.expected_profile};
                const auto prior =
                    app.owned_rows(*context.document, StoreSpace::task_record, task_owner);
                if (!prior.ok())
                    return {prior.status, {}, prior.error};
                for (const auto& image : *prior.value) {
                    const auto task = decode_task_record(*image->payload);
                    if (task.caller.principal == context.caller.principal &&
                        task.idempotency_key == context.idempotency_key) {
                        if (task.operation != "analysis.start" || task.signature != signature ||
                            task.input.document.id != task_input.document.id ||
                            task.input.document.epoch != task_input.document.epoch ||
                            task.input.revision != task_input.revision ||
                            task.input.profile != task_input.profile)
                            return bad<Value>(ErrorCode::idempotency_key_conflict,
                                              "Analysis key belongs to a different input");
                        const auto run = state->find(*context.document, "run-" + task.id);
                        if (!run.ok())
                            return {run.status, {}, run.error};
                        validate_solver_task(run.value->first, task);
                        return detail::converted(tasks().query(context.caller, task.id),
                                                 detail::task_value);
                    }
                }
                const auto snapshot = app.snapshot(*context.document);
                if (!snapshot.ok())
                    return {snapshot.status, {}, snapshot.error};
                if (snapshot.value->info.revision != *context.expected_revision)
                    return bad<Value>(ErrorCode::revision_conflict,
                                      "Analysis start revision is stale");
                check(state->executable_matches_probe(),
                      "Executable identity differs from the actual version probe");
                const auto source =
                    state->resolve(context.caller, *context.document, input.artifact_id);
                if (!source.ok())
                    return {source.status, {}, source.error};
                const auto frozen =
                    features::analysis::freeze_analysis_input(snapshot.value->records,
                                                              input.analysis_id,
                                                              *context.expected_profile,
                                                              source.value->first.identities);
                if (!frozen.ok())
                    return {frozen.status, {}, frozen.error};
                check(frozen.value->input_signature == source.value->first.input_signature &&
                          frozen.value->analysis == source.value->first.analysis &&
                          frozen.value->target == source.value->first.target,
                      "Export artifact does not describe the requested current analysis input");
                const auto plan = validate_nastran_export(
                    snapshot.value->records, input.analysis_id, state->codec);
                if (!plan.ok())
                    return {plan.status, {}, plan.error};
                verify_nastran_readback(
                    *plan.value, LocalArtifactStore{}.verify(source.value->second), state->codec);
                const auto work = std::make_shared<FrozenRun>();
                work->artifact = source.value->second;
                auto& fact = work->fact;
                fact.principal = context.caller.principal;
                fact.idempotency_key = context.idempotency_key;
                fact.signature = signature;
                fact.source_input = source.value->first;
                fact.test_only = state->config->test_only;
                fact.result_reader = state->config->result_reader;
                if (!fact.test_only) {
                    fact.version_probe = state->config->version_probe;
                    fact.declaration_digest = state->config->declaration_digest;
                }
                fact.run.request.configuration = state->config->run;
                fact.run.request.input = {
                    context.document->id,
                    context.document->epoch,
                    *context.expected_revision,
                    input.analysis_id,
                    *context.expected_profile,
                    input.artifact_id,
                    artifact_sha256(work->artifact.manifest),
                    artifact_sha256(frozen.value->input_signature),
                    solver_export_identity_digest(*frozen.value),
                    state->codec.definition().implementation_fingerprint,
                    fact.result_reader ? fact.result_reader->reader_version : "unconfigured",
                    features::analysis::encode_frozen_analysis_input(*frozen.value)};
                {
                    std::lock_guard lock(state->mutex);
                    state->pending[pending_key(context.caller, context.idempotency_key)] = work;
                }
                TaskRequest request{
                    context.caller,
                    context.idempotency_key,
                    "analysis.start",
                    signature,
                    task_input,
                    [state,
                     work](const TaskControl& control) -> std::shared_ptr<const TaskPayload> {
                        control.checkpoint();
                        const auto started = state->runner->start(work->fact.run.request);
                        if (!started.ok())
                            return completion(
                                started.error && started.error->code == ErrorCode::storage_uncertain
                                    ? TaskState::outcome_unknown
                                    : TaskState::failed,
                                started.error ? started.error->code : ErrorCode::storage_uncertain,
                                started.error ? started.error->message
                                              : "Local process admission failed");
                        const auto id = work->fact.run.request.run_id;
                        unsigned observation_errors{}, cancellation_errors{};
                        for (;;) {
                            if (control.cancellation_requested()) {
                                const auto cancelled = state->runner->cancel(id);
                                if (!cancelled.ok()) {
                                    if (++cancellation_errors >= 20)
                                        return completion(TaskState::outcome_unknown,
                                                          ErrorCode::storage_uncertain,
                                                          "External cancellation could not be "
                                                          "verified or persisted");
                                } else {
                                    cancellation_errors = 0;
                                }
                            }
                            const auto observed = state->runner->query(id);
                            if (!observed.ok()) {
                                if (++observation_errors >= 20)
                                    return completion(
                                        TaskState::outcome_unknown,
                                        ErrorCode::storage_uncertain,
                                        "External execution observation could not be persisted");
                            } else if (terminal(observed.value->execution)) {
                                const auto& run = *observed.value;
                                if (run.execution == SolverExecutionState::outcome_unknown)
                                    return completion(
                                        TaskState::outcome_unknown,
                                        ErrorCode::storage_uncertain,
                                        "External process outcome requires reconciliation");
                                if (run.execution == SolverExecutionState::cancelled)
                                    return completion(
                                        TaskState::cancelled,
                                        ErrorCode::invalid_input,
                                        "Verified owned process group cancellation completed");
                                if (work->fact.result_reader) {
                                    if (run.execution != SolverExecutionState::exited ||
                                        run.exit_code != 0 ||
                                        run.output_state != SolverOutputState::collected)
                                        return completion(
                                            TaskState::failed,
                                            ErrorCode::invalid_input,
                                            "A complete normal exit is required before parsing");
                                    return state->prepare_result(id, control);
                                }
                                return completion(TaskState::failed,
                                                  ErrorCode::unsupported_capability,
                                                  "Execution observed; real result parsing and "
                                                  "numerical validation are not configured");
                            } else {
                                observation_errors = 0;
                            }
                            std::this_thread::sleep_for(std::chrono::milliseconds(10));
                        }
                    }};
                const auto accepted = tasks().start(std::move(request));
                {
                    std::lock_guard lock(state->mutex);
                    state->pending.erase(pending_key(context.caller, context.idempotency_key));
                }
                return detail::converted(accepted, detail::task_value);
            } catch (const std::exception& error) {
                return bad<Value>(ErrorCode::invalid_input, error.what());
            }
        });
}
EngineContribution
solver_engine_contribution(const std::shared_ptr<SolverCoordinator>& coordinator) {
    check(static_cast<bool>(coordinator), "Solver contribution requires a coordinator");
    return {"qcae.solver.local",
            {},
            [coordinator](OperationRegistry& registry,
                          RecordApplication& app,
                          std::function<TaskService&()> tasks) {
                return coordinator->register_operations(registry, app, std::move(tasks));
            },
            [] {
                return std::vector<OwnedRowHandler>{SolverCoordinator::row_handler(),
                                                    detail::solver_result_row_handler(),
                                                    detail::solver_validation_row_handler(),
                                                    reconcile_row_handler()};
            },
            {}};
}
} // namespace qcae::ipc
