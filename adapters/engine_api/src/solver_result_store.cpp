#include "solver_result_store.hpp"
#include "solver_version_probe.hpp"
#include "qcae/record_registry.hpp"
#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <fcntl.h>
#include <limits>
#include <map>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

namespace qcae::ipc {
namespace {
constexpr std::string_view parsed_resource = "parsed-result.qcr";
constexpr std::size_t max_result_bytes = 1048576;
namespace fs = std::filesystem;
void require(bool value, const char* message) {
    if (!value)
        throw RecordError(ErrorCode::schema_unsupported, message, "solver_result");
}
bool digest(std::string_view text) {
    return text.size() == 64 && text.find_first_not_of("0123456789abcdef") == text.npos;
}
bool resource_path(std::string_view text) {
    const fs::path path(text);
    if (text.empty() || text.size() > 1024 || text.find('\0') != text.npos || path.is_absolute() ||
        path.lexically_normal() != path || !path.has_filename())
        return false;
    for (const auto& item : path)
        if (item.empty() || item == "." || item == ".." || item == "input" ||
            item == ".qcae-result" || item == ".qcae-stage" || item == "manifest.json" ||
            item == ".manifest.part")
            return false;
    return true;
}
void validate_reader(const SolverResultReaderConfiguration& reader) {
    require(reader.reader_version == "qcae.nastran.static-f06.v1" && reader.subcase == 1 &&
                reader.unit_system == "mm-N-MPa" && reader.coordinate_basis == "basic" &&
                resource_path(reader.resource) && reader.resource != parsed_resource &&
                reader.resource != "runner.stdout" && reader.resource != "runner.stderr",
            "The controlled result reader requires explicit F06/SUBCASE1/mm-N-MPa/basic");
}
std::vector<ArtifactFileDigest> output_manifest(const SolverRunRecord& run) {
    require(run.execution == SolverExecutionState::exited && run.exit_code == 0 &&
                !run.termination_signal && run.process &&
                run.output_state == SolverOutputState::collected &&
                !run.request.configuration.expected_outputs.empty() &&
                run.request.configuration.expected_outputs.size() <= 32 &&
                run.request.configuration.max_output_bytes > 0 &&
                run.request.configuration.max_output_bytes <= 16777216,
            "Complete outputs require an observed normal zero exit and frozen output limits");
    std::set<std::string> expected{"runner.stdout", "runner.stderr"};
    for (const auto& name : run.request.configuration.expected_outputs)
        require(resource_path(name) && name != parsed_resource && expected.insert(name).second,
                "Declared result output is unsafe, duplicated or reserved");
    require(run.outputs.size() == expected.size(), "Run output manifest is incomplete");
    std::vector<ArtifactFileDigest> files;
    std::uint64_t total{};
    for (const auto& file : run.outputs) {
        require(
            resource_path(file.path) && expected.erase(file.path) == 1 && digest(file.sha256) &&
                file.byte_length <= run.request.configuration.max_output_bytes - total &&
                (file.path == "runner.stdout" || file.path == "runner.stderr" || file.byte_length),
            "Run output manifest is inconsistent with frozen declarations");
        total += file.byte_length;
        files.push_back({file.path, file.byte_length, file.sha256});
    }
    require(expected.empty(), "Run output manifest omits an expected resource");
    return files;
}
bool same_files(std::span<const ArtifactFileDigest> left,
                std::span<const ArtifactFileDigest> right) {
    return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin());
}
bool same_intent(const LocalArtifactIntent& left, const LocalArtifactIntent& right) {
    return left.artifact_id == right.artifact_id && left.task_id == right.task_id &&
           left.directory == right.directory && left.root_resource == right.root_resource &&
           left.manifest == right.manifest && same_files(left.files, right.files);
}
void verify_resources(std::span<const ArtifactFileDigest> files,
                      std::span<const TextResource> resources) {
    require(files.size() == resources.size(), "Verified output resource list is incomplete");
    std::map<std::string, const ArtifactFileDigest*> expected;
    for (const auto& file : files)
        require(expected.emplace(file.path, &file).second, "Duplicate verified output resource");
    for (const auto& resource : resources) {
        const auto found = expected.find(resource.path);
        require(found != expected.end() && resource.text.size() == found->second->byte_length &&
                    artifact_sha256(resource.text) == found->second->sha256,
                "Readback bytes differ from the frozen run output manifest");
        expected.erase(found);
    }
    require(expected.empty(), "Verified output bytes omit an expected file");
}
void validate(const SolverParsedResult& result) {
    (void)encode_solver_owned_run(result.origin);
    validate_reader(result.reader);
    const auto& origin = result.origin;
    const auto& request = origin.run.request;
    // The adapter persists startup, process identity and exit before parse intent/publication.
    require(origin.run.sequence >= 3 &&
                origin.run.sequence <= std::numeric_limits<std::uint64_t>::max() - 2,
            "Parsed origin lacks the completed execution chain or future publication sequence");
    require(origin.startup_intent_persisted && origin.result_reader == result.reader &&
                request.input.result_reader_version == result.reader.reader_version &&
                origin.run.parsing == SolverParsingState::not_run &&
                origin.run.numerical_validation == SolverNumericalState::not_run &&
                !origin.result_artifact && !origin.result_published &&
                same_files(result.raw_outputs, output_manifest(origin.run)),
            "Parsed result does not retain its pre-parse run and complete output manifest");
    require(origin.test_only || (origin.version_probe &&
                                 detail::solver_version_allows_execution(
                                     *origin.version_probe, request.configuration.solver_version)),
            "An external result requires the original trusted version observation");
    const auto input =
        features::analysis::decode_frozen_analysis_input(request.input.frozen_analysis_input);
    const auto& fields = result.fields;
    require(fields.reader_version == result.reader.reader_version &&
                fields.subcase == result.reader.subcase &&
                fields.coordinate_basis == result.reader.coordinate_basis &&
                fields.displacement_units ==
                    std::array<std::string, 6>{"mm", "mm", "mm", "rad", "rad", "rad"} &&
                fields.reaction_units ==
                    std::array<std::string, 6>{"N", "N", "N", "N*mm", "N*mm", "N*mm"},
            "Parsed field units, case, basis or reader differ from the frozen context");
    std::map<std::uint64_t, EntityId> nodes;
    std::set<EntityId> entities;
    for (const auto& identity : input.identities)
        if (identity.name_space == "GRID")
            require(identity.number && !identity.entity.value.empty() &&
                        nodes.emplace(identity.number, identity.entity).second &&
                        entities.insert(identity.entity).second,
                    "The original GRID map is ambiguous");
    require(!nodes.empty() && fields.displacements.size() == nodes.size() &&
                !fields.spc_reactions.empty() && fields.spc_reactions.size() <= nodes.size(),
            "Parsed nodal fields are incomplete");
    for (const auto* values : {&fields.displacements, &fields.spc_reactions}) {
        std::set<std::uint64_t> seen;
        for (const auto& row : *values) {
            const auto found = nodes.find(row.solver_number);
            require(found != nodes.end() && found->second == row.entity &&
                        seen.insert(row.solver_number).second &&
                        std::all_of(row.components.begin(),
                                    row.components.end(),
                                    [](double value) { return std::isfinite(value); }),
                    "Parsed nodal values differ from the original GRID map or are nonfinite");
        }
    }
}
class Fields {
  public:
    explicit Fields(std::vector<std::string> values) : values_(std::move(values)) {}
    std::string take() {
        require(at_ < values_.size(), "Truncated parsed result");
        return values_[at_++];
    }
    std::uint64_t number() {
        const auto value = take();
        std::uint64_t result{};
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
        require(parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size(),
                "Malformed parsed result integer");
        return result;
    }
    void finish() const {
        require(at_ == values_.size(), "Trailing parsed result fields");
    }

  private:
    std::vector<std::string> values_;
    std::size_t at_{};
};
std::string json_string(std::string_view text) {
    constexpr char hex[] = "0123456789abcdef";
    std::string result{"\""};
    for (const unsigned char ch : text) {
        if (ch == '"' || ch == '\\') {
            result += '\\';
            result += static_cast<char>(ch);
        } else if (ch < 32) {
            result += "\\u00";
            result += hex[ch >> 4];
            result += hex[ch & 15];
        } else {
            result += static_cast<char>(ch);
        }
    }
    return result + '"';
}
void no_symlinks(const fs::path& path) {
    require(path.is_absolute() && path.lexically_normal() == path,
            "Result resource requires an absolute normalized path");
    fs::path current;
    for (const auto& item : path) {
        current /= item;
        require(!fs::is_symlink(fs::symlink_status(current)), "Result resource contains a symlink");
    }
}
bool same_file(const struct stat& left, const struct stat& right) {
#if defined(__APPLE__)
    const auto lm = left.st_mtimespec, rm = right.st_mtimespec, lc = left.st_ctimespec,
               rc = right.st_ctimespec;
#else
    const auto lm = left.st_mtim, rm = right.st_mtim, lc = left.st_ctim, rc = right.st_ctim;
#endif
    return left.st_dev == right.st_dev && left.st_ino == right.st_ino &&
           left.st_size == right.st_size && left.st_mode == right.st_mode &&
           lm.tv_sec == rm.tv_sec && lm.tv_nsec == rm.tv_nsec && lc.tv_sec == rc.tv_sec &&
           lc.tv_nsec == rc.tv_nsec;
}
class Descriptor {
  public:
    explicit Descriptor(int value) : value_(value) {}
    ~Descriptor() {
        if (value_ >= 0)
            ::close(value_);
    }
    int get() const {
        return value_;
    }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;

  private:
    int value_;
};
} // namespace

std::string encode_solver_parsed_result(const SolverParsedResult& result) {
    validate(result);
    const auto origin = solver_owned_row(result.origin);
    std::vector<std::string> fields{"QCAE-SOLVER-PARSED-RESULT-1",
                                    origin->key.identity,
                                    std::to_string(origin->schema_version),
                                    *origin->payload,
                                    result.reader.reader_version,
                                    result.reader.resource,
                                    std::to_string(result.reader.subcase),
                                    result.reader.unit_system,
                                    result.reader.coordinate_basis,
                                    std::to_string(result.raw_outputs.size())};
    for (const auto& file : result.raw_outputs)
        fields.insert(fields.end(), {file.path, std::to_string(file.byte_length), file.sha256});
    fields.push_back(std::to_string(result.fields.subcase));
    fields.push_back(result.fields.coordinate_basis);
    fields.push_back(result.fields.reader_version);
    for (const auto& unit : result.fields.displacement_units)
        fields.push_back(unit);
    for (const auto& unit : result.fields.reaction_units)
        fields.push_back(unit);
    for (const auto* rows : {&result.fields.displacements, &result.fields.spc_reactions}) {
        fields.push_back(std::to_string(rows->size()));
        for (const auto& row : *rows) {
            fields.push_back(row.entity.value);
            fields.push_back(std::to_string(row.solver_number));
            for (const auto value : row.components)
                fields.push_back(record_wire::real(value));
        }
    }
    auto encoded = record_wire::strings(fields);
    require(encoded.size() <= max_result_bytes, "Parsed result exceeds its owned-row quota");
    return encoded;
}
SolverParsedResult decode_solver_parsed_result(std::string_view bytes) {
    require(bytes.size() <= max_result_bytes, "Parsed result exceeds its owned-row quota");
    Fields fields(record_wire::read_strings(bytes));
    require(fields.take() == "QCAE-SOLVER-PARSED-RESULT-1", "Unknown parsed result schema");
    auto run_id = fields.take();
    const auto schema = fields.number();
    require(schema >= 1 && schema <= 3, "Unknown original run schema");
    auto payload = std::make_shared<const std::string>(fields.take());
    SolverParsedResult result;
    result.origin = decode_solver_owned_run({{StoreSpace::artifact_record, std::move(run_id)},
                                             "qcae.solver.run",
                                             static_cast<std::uint32_t>(schema),
                                             std::move(payload)});
    result.reader.reader_version = fields.take();
    result.reader.resource = fields.take();
    result.reader.subcase = fields.number();
    result.reader.unit_system = fields.take();
    result.reader.coordinate_basis = fields.take();
    auto count = fields.number();
    require(count <= 34, "Parsed raw-output count exceeds its quota");
    while (count-- > 0) {
        auto path = fields.take();
        const auto length = fields.number();
        result.raw_outputs.push_back({std::move(path), length, fields.take()});
    }
    result.fields.subcase = fields.number();
    result.fields.coordinate_basis = fields.take();
    result.fields.reader_version = fields.take();
    for (auto& unit : result.fields.displacement_units)
        unit = fields.take();
    for (auto& unit : result.fields.reaction_units)
        unit = fields.take();
    for (auto* rows : {&result.fields.displacements, &result.fields.spc_reactions}) {
        count = fields.number();
        require(count <= 10000, "Parsed GRID row count exceeds its quota");
        while (count-- > 0) {
            NastranStaticGridValues row{EntityId(fields.take()), fields.number(), {}};
            for (auto& value : row.components)
                value = record_wire::read_real(fields.take());
            rows->push_back(std::move(row));
        }
    }
    fields.finish();
    require(encode_solver_parsed_result(result) == bytes, "Noncanonical parsed result");
    return result;
}

namespace detail {
void validate_solver_result_reader(const SolverResultReaderConfiguration& reader,
                                   const SolverRunConfiguration& config) {
    validate_reader(reader);
    require(std::find(config.expected_outputs.begin(),
                      config.expected_outputs.end(),
                      reader.resource) != config.expected_outputs.end(),
            "Configured F06 resource is not a declared frozen output");
    for (const auto& name : config.expected_outputs)
        require(resource_path(name) && name != parsed_resource,
                "A configured output conflicts with reserved result publication resources");
}
void validate_solver_result_intent(const LocalArtifactIntent& intent,
                                   const SolverRunRecord& run,
                                   const SolverResultReaderConfiguration& reader) {
    validate_solver_result_reader(reader, run.request.configuration);
    const auto raw = output_manifest(run);
    require(
        intent.artifact_id == "result-" + run.request.task_id &&
            intent.task_id == run.request.task_id &&
            intent.directory == fs::path(run.request.run_directory) / ".qcae-result" &&
            intent.root_resource == reader.resource && !intent.manifest.empty() &&
            intent.manifest.size() <= 65536 && intent.manifest.find('\0') == intent.manifest.npos &&
            intent.files.size() == raw.size() + 1 &&
            same_files(std::span(intent.files).first(raw.size()), raw) &&
            intent.files.back().path == parsed_resource && intent.files.back().byte_length > 0 &&
            intent.files.back().byte_length <= max_result_bytes &&
            digest(intent.files.back().sha256),
        "Result intent differs from the original run and complete frozen file manifest");
    std::uint64_t total{};
    for (const auto& file : intent.files) {
        require(file.byte_length <= 16777216 - total,
                "Result publication exceeds the complete artifact byte quota");
        total += file.byte_length;
    }
}
Result<std::vector<TextResource>> read_verified_solver_outputs(const SolverRunRecord& run) {
    try {
        const auto files = output_manifest(run);
        const fs::path root(run.request.run_directory);
        no_symlinks(root);
        require(fs::is_directory(root), "Original solver run directory is unavailable");
        std::vector<TextResource> resources;
        for (const auto& file : files) {
            const auto path = root / file.path;
            no_symlinks(path);
            Descriptor fd(::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
            struct stat before{}, after{}, current{};
            require(fd.get() >= 0 && ::fstat(fd.get(), &before) == 0 && S_ISREG(before.st_mode) &&
                        before.st_size >= 0 &&
                        static_cast<std::uint64_t>(before.st_size) == file.byte_length,
                    "Raw result output is missing, nonregular or has changed length");
            std::string bytes(static_cast<std::size_t>(file.byte_length), '\0');
            std::size_t offset{};
            while (offset < bytes.size()) {
                const auto count = ::read(fd.get(), bytes.data() + offset, bytes.size() - offset);
                if (count < 0 && errno == EINTR)
                    continue;
                require(count > 0, "Raw result output readback is incomplete");
                offset += static_cast<std::size_t>(count);
            }
            char extra{};
            require(::read(fd.get(), &extra, 1) == 0 && ::fstat(fd.get(), &after) == 0 &&
                        ::lstat(path.c_str(), &current) == 0 && same_file(before, after) &&
                        same_file(before, current) && artifact_sha256(bytes) == file.sha256,
                    "Raw result output identity or SHA256 changed during readback");
            resources.push_back({file.path, std::move(bytes)});
        }
        return {Status::success, std::move(resources), {}};
    } catch (const std::exception& error) {
        return {Status::failed,
                {},
                Diagnostic{ErrorCode::invalid_input, error.what(), "solver_result"}};
    }
}
Result<SolverParsedResult> prepare_solver_result(const SolverOwnedRun& run,
                                                 const SolverResultReaderConfiguration& reader,
                                                 std::span<const TextResource> resources) {
    try {
        (void)encode_solver_owned_run(run);
        validate_reader(reader);
        const auto files = output_manifest(run.run);
        verify_resources(files, resources);
        const auto found = std::find_if(resources.begin(), resources.end(), [&](const auto& item) {
            return item.path == reader.resource;
        });
        require(found != resources.end(), "The configured F06 resource is absent");
        const auto input = features::analysis::decode_frozen_analysis_input(
            run.run.request.input.frozen_analysis_input);
        const auto parsed =
            read_nastran_static_f06(found->text,
                                    {reader.subcase, reader.unit_system, reader.coordinate_basis},
                                    input.identities);
        if (!parsed.ok())
            return {parsed.status, {}, parsed.error};
        SolverParsedResult result{run, reader, files, *parsed.value};
        (void)encode_solver_parsed_result(result);
        return {Status::success, std::move(result), {}};
    } catch (const std::exception& error) {
        return {Status::failed,
                {},
                Diagnostic{ErrorCode::invalid_input, error.what(), "solver_result"}};
    }
}
LocalArtifactIntent solver_result_artifact(const SolverParsedResult& result,
                                           const fs::path& directory) {
    validate(result);
    require(directory.is_absolute() && directory.lexically_normal() == directory,
            "Result publication directory must be an absolute trusted path");
    require(directory == fs::path(result.origin.run.request.run_directory) / ".qcae-result",
            "Result publication directory differs from its original run");
    LocalArtifactIntent intent;
    intent.artifact_id = "result-" + result.origin.run.request.task_id;
    intent.task_id = result.origin.run.request.task_id;
    intent.directory = directory;
    intent.root_resource = result.reader.resource;
    intent.files = result.raw_outputs;
    const auto encoded = encode_solver_parsed_result(result);
    intent.files.push_back(
        {std::string(parsed_resource), encoded.size(), artifact_sha256(encoded)});
    const auto& input = result.origin.run.request.input;
    const auto& config = result.origin.run.request.configuration;
    intent.manifest =
        "{\"schema_version\":\"qcae.solver-result.v1\",\"artifact_id\":" +
        json_string(intent.artifact_id) + ",\"task_id\":" + json_string(intent.task_id) +
        ",\"run_id\":" + json_string(result.origin.run.request.run_id) + ",\"source_kind\":" +
        json_string(result.origin.test_only ? "test_process" : "external_solver") +
        ",\"input_fingerprint\":" + json_string(input.input_fingerprint) +
        ",\"export_identity_digest\":" + json_string(input.export_identity_digest) +
        ",\"source_manifest_sha256\":" + json_string(input.manifest_sha256) +
        ",\"profile_id\":" + json_string(input.profile.profile_id) +
        ",\"profile_version\":" + json_string(input.profile.profile_version) +
        ",\"profile_definition_digest\":" + json_string(input.profile.definition_digest) +
        ",\"frozen_input_sha256\":" + json_string(artifact_sha256(input.frozen_analysis_input)) +
        ",\"configuration_digest\":" + json_string(config.configuration_digest) +
        ",\"solver_family\":" + json_string(config.solver_family) +
        ",\"dialect\":" + json_string(config.dialect) +
        ",\"solver_version\":" + json_string(config.solver_version) +
        ",\"reader_version\":" + json_string(result.reader.reader_version) +
        ",\"subcase\":\"1\",\"unit_system\":\"mm-N-MPa\",\"coordinate_basis\":\"basic\"," +
        "\"numerical_validation\":\"not_run\",\"parsed_resource\":" + json_string(parsed_resource) +
        ",\"origin_run_sha256\":" +
        json_string(artifact_sha256(encode_solver_owned_run(result.origin))) + ",\"files\":[";
    for (std::size_t index = 0; index < intent.files.size(); ++index) {
        if (index)
            intent.manifest += ',';
        const auto& file = intent.files[index];
        intent.manifest += "{\"path\":" + json_string(file.path) +
                           ",\"byte_length\":" + json_string(std::to_string(file.byte_length)) +
                           ",\"sha256\":" + json_string(file.sha256) + '}';
    }
    intent.manifest += "]}\n";
    return intent;
}
std::vector<TextResource> solver_result_resources(const SolverParsedResult& result,
                                                  std::span<const TextResource> verified_outputs) {
    validate(result);
    verify_resources(result.raw_outputs, verified_outputs);
    std::vector<TextResource> resources(verified_outputs.begin(), verified_outputs.end());
    resources.push_back({std::string(parsed_resource), encode_solver_parsed_result(result)});
    return resources;
}
void validate_solver_result_link(const SolverParsedResult& result, const SolverOwnedRun& current) {
    validate(result);
    (void)encode_solver_owned_run(current);
    const auto& origin = result.origin;
    require(current.run.parsing == SolverParsingState::parsed && current.result_artifact &&
                current.run.numerical_validation == SolverNumericalState::not_run &&
                current.run.sequence >= origin.run.sequence + (current.result_published ? 2 : 1),
            "Current result run lacks its durable parsed/publication sequence transition");
    const auto expected = solver_result_artifact(result, current.result_artifact->directory);
    require(same_intent(expected, *current.result_artifact),
            "Current run result intent differs from its canonical parsed provenance");
    require(origin.principal == current.principal &&
                origin.idempotency_key == current.idempotency_key &&
                origin.signature == current.signature && origin.test_only == current.test_only &&
                origin.version_probe == current.version_probe &&
                origin.result_reader == current.result_reader &&
                origin.declaration_digest == current.declaration_digest &&
                origin.run.request == current.run.request &&
                origin.run.process == current.run.process &&
                origin.run.exit_code == current.run.exit_code &&
                origin.run.termination_signal == current.run.termination_signal &&
                origin.run.execution == current.run.execution &&
                origin.run.cancellation_requested == current.run.cancellation_requested &&
                features::analysis::encode_frozen_analysis_input(origin.source_input) ==
                    features::analysis::encode_frozen_analysis_input(current.source_input) &&
                same_files(result.raw_outputs, output_manifest(current.run)),
            "Parsed result/run cross-row provenance differs");
}
std::shared_ptr<const OwnedRowImage> solver_result_row(const StoredSolverResult& result) {
    const auto expected = solver_result_artifact(result.parsed, result.artifact.directory);
    require(same_intent(expected, result.artifact),
            "Result artifact intent differs from parsed provenance and complete file manifest");
    auto payload =
        record_wire::strings(std::array<std::string, 5>{"QCAE-SOLVER-RESULT-1",
                                                        result.published ? "1" : "0",
                                                        encode_solver_parsed_result(result.parsed),
                                                        result.artifact.directory.string(),
                                                        result.artifact.manifest});
    require(payload.size() <= 2097152, "Result owned-row exceeds its quota");
    return std::make_shared<const OwnedRowImage>(
        OwnedRowImage{{StoreSpace::artifact_record, result.artifact.artifact_id},
                      "qcae.solver.result",
                      1,
                      std::make_shared<const std::string>(std::move(payload)),
                      {}});
}
StoredSolverResult decode_solver_result_row(const OwnedRowImage& image) {
    require(image.owner == "qcae.solver.result" && image.schema_version == 1 &&
                image.key.space == StoreSpace::artifact_record && image.payload &&
                image.payload->size() <= 2097152,
            "Unknown solver result owned-row envelope");
    const auto fields = record_wire::read_strings(*image.payload);
    require(fields.size() == 5 && fields[0] == "QCAE-SOLVER-RESULT-1" &&
                (fields[1] == "0" || fields[1] == "1"),
            "Malformed solver result owned-row");
    StoredSolverResult result;
    result.parsed = decode_solver_parsed_result(fields[2]);
    result.artifact = solver_result_artifact(result.parsed, fields[3]);
    result.published = fields[1] == "1";
    require(result.artifact.artifact_id == image.key.identity &&
                result.artifact.manifest == fields[4] &&
                *solver_result_row(result)->payload == *image.payload,
            "Solver result owned-row identity or manifest differs");
    return result;
}
OwnedRowHandler solver_result_row_handler() {
    return {StoreSpace::artifact_record,
            "qcae.solver.result",
            [](const OwnedRowImage& image) { (void)decode_solver_result_row(image); },
            {},
            {}};
}
} // namespace detail
} // namespace qcae::ipc
