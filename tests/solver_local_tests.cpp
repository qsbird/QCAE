#include "qcae/solver_local.hpp"
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unistd.h>

namespace {
using namespace qcae;
namespace fs = std::filesystem;
const std::string frozen_input_bytes =
    std::string("test-only") + '\0' + "frozen analysis/map/profile record";
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
template <class T> Result<T> success(T value) {
    return {Status::success, std::move(value), std::nullopt};
}
template <class T> Result<T> failed() {
    return {Status::failed,
            std::nullopt,
            Diagnostic{ErrorCode::storage_failure, "test-only injected failure", "test"}};
}
void string_value(std::ostream& out, const std::string& value) {
    out << std::quoted(value) << '\n';
}
void string_value(std::istream& in, std::string& value) {
    in >> std::quoted(value);
}
void strings(std::ostream& out, const std::vector<std::string>& values) {
    out << values.size() << '\n';
    for (const auto& value : values)
        string_value(out, value);
}
void strings(std::istream& in, std::vector<std::string>& values) {
    std::size_t size{};
    in >> size;
    require(size <= 64, "Test persistence vector is malformed");
    values.resize(size);
    for (auto& value : values)
        string_value(in, value);
}
void encode_request(std::ostream& out, const SolverRunRequest& request) {
    string_value(out, request.run_id);
    string_value(out, request.task_id);
    string_value(out, request.run_directory);
    const auto& input = request.input;
    for (const auto* value : {&input.document.value,
                              &input.epoch.value,
                              &input.analysis.value,
                              &input.profile.profile_id,
                              &input.profile.profile_version,
                              &input.profile.definition_digest,
                              &input.artifact_id,
                              &input.manifest_sha256,
                              &input.input_fingerprint,
                              &input.export_identity_digest,
                              &input.export_rule_version,
                              &input.result_reader_version,
                              &input.frozen_analysis_input})
        string_value(out, *value);
    out << input.revision << '\n';
    const auto& config = request.configuration;
    for (const auto* value : {&config.id,
                              &config.executable,
                              &config.solver_family,
                              &config.dialect,
                              &config.solver_version,
                              &config.version_evidence,
                              &config.configuration_digest})
        string_value(out, *value);
    strings(out, config.argv);
    strings(out, config.expected_outputs);
    out << config.max_wall_time_ms << ' ' << config.cancel_grace_ms << ' '
        << config.max_output_bytes << '\n';
}
void decode_request(std::istream& in, SolverRunRequest& request) {
    string_value(in, request.run_id);
    string_value(in, request.task_id);
    string_value(in, request.run_directory);
    auto& input = request.input;
    for (auto* value : {&input.document.value,
                        &input.epoch.value,
                        &input.analysis.value,
                        &input.profile.profile_id,
                        &input.profile.profile_version,
                        &input.profile.definition_digest,
                        &input.artifact_id,
                        &input.manifest_sha256,
                        &input.input_fingerprint,
                        &input.export_identity_digest,
                        &input.export_rule_version,
                        &input.result_reader_version,
                        &input.frozen_analysis_input})
        string_value(in, *value);
    in >> input.revision;
    auto& config = request.configuration;
    for (auto* value : {&config.id,
                        &config.executable,
                        &config.solver_family,
                        &config.dialect,
                        &config.solver_version,
                        &config.version_evidence,
                        &config.configuration_digest})
        string_value(in, *value);
    strings(in, config.argv);
    strings(in, config.expected_outputs);
    in >> config.max_wall_time_ms >> config.cancel_grace_ms >> config.max_output_bytes;
}
std::string encode(const SolverRunRecord& record) {
    std::ostringstream out;
    encode_request(out, record.request);
    out << record.sequence << ' ' << static_cast<int>(record.execution) << ' '
        << static_cast<int>(record.parsing) << ' ' << static_cast<int>(record.numerical_validation)
        << ' ' << static_cast<int>(record.output_state) << '\n';
    out << record.cancellation_requested << '\n';
    out << record.process.has_value() << '\n';
    if (record.process) {
        out << record.process->pid << '\n';
        string_value(out, record.process->start_identity);
    }
    out << record.exit_code.has_value() << ' ' << record.exit_code.value_or(0) << ' '
        << record.termination_signal.has_value() << ' ' << record.termination_signal.value_or(0)
        << '\n';
    out << record.outputs.size() << '\n';
    for (const auto& file : record.outputs) {
        string_value(out, file.path);
        out << file.byte_length << '\n';
        string_value(out, file.sha256);
    }
    string_value(out, record.detail);
    return out.str();
}
SolverRunRecord decode(std::istream& in) {
    SolverRunRecord record;
    decode_request(in, record.request);
    int execution{}, parsing{}, numerical{}, output{};
    in >> record.sequence >> execution >> parsing >> numerical >> output;
    require(execution >= 0 && execution <= 6 && parsing >= 0 && parsing <= 2 && numerical >= 0 &&
                numerical <= 2 && output >= 0 && output <= 2,
            "Test persistence state is malformed");
    record.execution = static_cast<SolverExecutionState>(execution);
    record.parsing = static_cast<SolverParsingState>(parsing);
    record.numerical_validation = static_cast<SolverNumericalState>(numerical);
    record.output_state = static_cast<SolverOutputState>(output);
    in >> record.cancellation_requested;
    bool has_process{}, has_exit{}, has_signal{};
    int exit{}, signal{};
    in >> has_process;
    if (has_process) {
        SolverProcessIdentity identity;
        in >> identity.pid;
        string_value(in, identity.start_identity);
        record.process = std::move(identity);
    }
    in >> has_exit >> exit >> has_signal >> signal;
    if (has_exit)
        record.exit_code = exit;
    if (has_signal)
        record.termination_signal = signal;
    std::size_t count{};
    in >> count;
    require(count <= 34, "Test persistence output count is malformed");
    record.outputs.resize(count);
    for (auto& file : record.outputs) {
        string_value(in, file.path);
        in >> file.byte_length;
        string_value(in, file.sha256);
    }
    string_value(in, record.detail);
    require(!in.fail(), "Test persistence record is truncated");
    in >> std::ws;
    require(in.eof(), "Test persistence record has trailing bytes");
    return record;
}
// Explicit test-only durable hook implementation; production must use the engine's workspace.
struct DurableFacts {
    fs::path directory;
    std::optional<SolverExecutionState> fail_state;
    bool fail_after_commit{};
    std::optional<SolverRunRecord> load(std::string_view id) const {
        std::ifstream in(directory / (std::string(id) + ".fact"));
        if (!in)
            return std::nullopt;
        return decode(in);
    }
    Result<bool> persist(const SolverRunRecord& next,
                         const std::optional<SolverRunRecord>& expected) {
        const auto current = load(next.request.run_id);
        require(current.has_value() == expected.has_value() &&
                    (!current || encode(*current) == encode(*expected)),
                "Test persistence CAS mismatch");
        const bool inject = fail_state == next.execution;
        if (inject && !fail_after_commit)
            return failed<bool>();
        const auto path = directory / (next.request.run_id + ".fact");
        auto part = path;
        part += ".part";
        const int fd = ::open(part.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_NOFOLLOW, 0600);
        require(fd >= 0, "Test persistence open failed");
        const auto bytes = encode(next);
        const auto written = ::write(fd, bytes.data(), bytes.size());
        const bool synced = written == static_cast<ssize_t>(bytes.size()) && ::fsync(fd) == 0;
        ::close(fd);
        require(synced, "Test persistence write/sync failed");
        fs::rename(part, path);
        const int parent = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY);
        require(parent >= 0, "Test persistence parent open failed");
        const bool parent_synced = ::fsync(parent) == 0;
        ::close(parent);
        require(parent_synced, "Test persistence directory sync failed");
        return inject ? failed<bool>() : success(true);
    }
};
LocalSolverHost hooks(DurableFacts& facts, const LocalArtifactIntent& artifact, bool& compatible) {
    return {[artifact](const SolverRunRequest&) { return success(artifact); },
            [&compatible](const SolverRunRequest& request, const LocalArtifactIntent&) {
                return success(compatible &&
                               request.input.frozen_analysis_input == frozen_input_bytes);
            },
            [&facts](std::string_view id) { return success(facts.load(id)); },
            [&facts](const SolverRunRecord& next, const std::optional<SolverRunRecord>& expected) {
                return facts.persist(next, expected);
            }};
}
bool finished(SolverExecutionState state) {
    return state == SolverExecutionState::exited || state == SolverExecutionState::cancelled ||
           state == SolverExecutionState::launch_failed ||
           state == SolverExecutionState::outcome_unknown;
}
SolverRunRecord wait(LocalSolverRunner& runner, std::string_view id) {
    for (unsigned attempt = 0; attempt < 400; ++attempt) {
        const auto result = runner.query(id);
        require(result.ok(), "Actual process query failed");
        if (finished(result.value->execution))
            return *result.value;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    throw std::runtime_error("Test process did not reach a terminal state");
}
void await_child(const SolverRunRequest& request) {
    for (unsigned i = 0; i < 200; ++i) {
        if (fs::exists(fs::path(request.run_directory) / "child.started"))
            return;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    throw std::runtime_error("Test child did not install its signal behavior");
}
void crash_on_term(int) {
    // ASan owns SIGSEGV and may convert it to SIGABRT. Request the precise OS signal
    // under test directly, so the expected waitpid fact is identical in both builds.
    (void)::kill(::getpid(), SIGABRT);
}
void exit_on_term(int) {
    ::_exit(0);
}
class ClosedStandardInput {
  public:
    ClosedStandardInput() : saved_(::fcntl(STDIN_FILENO, F_DUPFD_CLOEXEC, 3)) {
        if (saved_ >= 0)
            ::close(STDIN_FILENO);
    }
    ~ClosedStandardInput() {
        if (saved_ >= 0) {
            (void)::dup2(saved_, STDIN_FILENO);
            ::close(saved_);
        }
    }

  private:
    int saved_;
};
int child(int argc, char** argv) {
    require(argc == 5, "Test-only child arguments differ");
    const std::string mode(argv[2]);
    if (mode == "ignore-term")
        std::signal(SIGTERM, SIG_IGN);
    if (mode == "crash-on-term")
        std::signal(SIGTERM, crash_on_term);
    if (mode == "exit-on-term")
        std::signal(SIGTERM, exit_on_term);
    std::ofstream("child.started") << "test-only process executed\n";
    if (mode == "missing")
        return 0;
    if (mode == "symlink") {
        fs::create_symlink("input/model.bdf", argv[3]);
        return 0;
    }
    if (mode == "tamper-input")
        std::ofstream("input/model.bdf") << "damaged frozen copy";
    std::ofstream output(argv[3], std::ios::binary);
    if (mode != "empty")
        output << (mode == "corrupt" ? "TEST_ONLY_CORRUPT_RESULT\n" : "TEST_ONLY_OUTPUT\n")
               << argv[4] << '\n';
    output.close();
    if (mode == "many-files")
        for (unsigned i = 0; i < 3; ++i)
            std::ofstream("extra-" + std::to_string(i)) << std::string(700000, 'x');
    std::cout << "test-only execution; no Nastran solver or engineering result\n";
    if (mode == "sleep" || mode == "ignore-term" || mode == "many-files" ||
        mode == "crash-on-term" || mode == "exit-on-term")
        for (unsigned i = 0; i < 1000; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    fs::path directory;
    try {
        if (argc > 1 && std::string_view(argv[1]) == "--test-child")
            return child(argc, argv);
        directory = fs::canonical(fs::temp_directory_path()) /
                    ("qcae-solver-test-" + std::to_string(::getpid()));
        require(fs::create_directory(directory), "Test directory already exists");
        fs::create_directory(directory / "facts");
        fs::create_directory(directory / "runs");
        ArtifactPlan plan{"model.bdf",
                          {{"model.bdf", "TEST_ONLY_INPUT\nINCLUDE 'parts/nodes.bdf'\n"},
                           {"parts/nodes.bdf", "TEST_ONLY_INCLUDE\n"}},
                          {},
                          EntityId("analysis-test"),
                          {"test-only", "1", "test-definition"}};
        LocalArtifactIntent artifact{"artifact-test",
                                     "export-task-test",
                                     directory / "artifact",
                                     plan.root_resource,
                                     artifact_file_digests(plan),
                                     "test-only frozen artifact manifest\n"};
        LocalArtifactStore artifact_store;
        (void)artifact_store.stage(artifact, plan.resources, [] {});
        artifact_store.publish(artifact);
        DurableFacts facts{directory / "facts", std::nullopt, false};
        bool compatible = true;
        LocalSolverRunner runner(hooks(facts, artifact, compatible), {1});
        auto request = [&](std::string id, std::string mode = "success") {
            SolverRunRequest value;
            value.run_id = std::move(id);
            value.task_id = "solver-task-test";
            value.run_directory = (directory / "runs" / value.run_id).string();
            value.input = {DocumentId("doc-test"),
                           DocumentEpoch("epoch-test"),
                           7,
                           EntityId("analysis-test"),
                           plan.profile,
                           artifact.artifact_id,
                           artifact_sha256(artifact.manifest),
                           "test-only physical signature",
                           artifact_sha256("test-only frozen export map"),
                           "test-only.export.v1",
                           "test-only.reader.v1",
                           frozen_input_bytes};
            value.configuration = {"test-only-config",
                                   fs::canonical(argv[0]).string(),
                                   {"--test-child", std::move(mode), "result.bin", "literal"},
                                   "test-only",
                                   "test-only-process-v1",
                                   "test-only-1",
                                   "test-only executable, not an installed Nastran",
                                   artifact_sha256("test-only explicit config"),
                                   {"result.bin"},
                                   2000,
                                   30,
                                   1048576};
            return value;
        };
        unsigned cases{};
        auto check = [&](bool value, const char* message) {
            require(value, message);
            ++cases;
        };
        auto normal = request("literal");
        normal.configuration.argv.back() = "$(touch shell-must-not-run); literal with spaces";
        const auto started = runner.start(normal);
        check(started.ok() && started.value->process &&
                  !started.value->process->start_identity.empty(),
              "Process started without its persisted OS identity");
        const auto done = wait(runner, normal.run_id);
        check(done.request.input.frozen_analysis_input == frozen_input_bytes &&
                  done.request.input.frozen_analysis_input.find('\0') != std::string::npos,
              "Opaque frozen input encoding was rejected or changed across persistence");
        std::ifstream literal(directory / "runs/literal/result.bin");
        std::ostringstream contents;
        contents << literal.rdbuf();
        check(done.execution == SolverExecutionState::exited && done.exit_code == 0 &&
                  done.output_state == SolverOutputState::collected && done.outputs.size() == 3 &&
                  contents.str().find(normal.configuration.argv.back()) != std::string::npos &&
                  !fs::exists(directory / "runs/literal/shell-must-not-run"),
              "Literal argv, actual waitpid or output observation failed");
        check(done.parsing == SolverParsingState::not_run &&
                  done.numerical_validation == SolverNumericalState::not_run,
              "Exit zero falsely became parsed or numerically verified");
        check(runner.start(normal).value->sequence == done.sequence,
              "Same run identity executed twice");
        auto changed = normal;
        changed.configuration.argv.back() = "changed";
        check(runner.start(changed).status == Status::conflict,
              "Same run identity accepted different configuration");
        {
            ClosedStandardInput closed_input;
            auto closed = request("closed-stdin");
            check(runner.start(closed).ok() && wait(runner, closed.run_id).exit_code == 0,
                  "Closed host stdin broke process descriptors or admission gate");
        }
        for (const auto& mode : {"corrupt", "missing", "empty", "symlink", "tamper-input"}) {
            auto value = request(mode, mode);
            check(runner.start(value).ok(), "Output fault test did not execute");
            const auto observed = wait(runner, value.run_id);
            check(observed.execution == SolverExecutionState::exited && observed.exit_code == 0 &&
                      observed.parsing == SolverParsingState::not_run &&
                      observed.numerical_validation == SolverNumericalState::not_run &&
                      (mode == std::string("corrupt")
                           ? observed.output_state == SolverOutputState::collected
                           : observed.output_state == SolverOutputState::incomplete),
                  "Damaged or missing output became an engineering success");
        }
        check(artifact_store.verify(artifact).front().text == plan.resources.front().text,
              "Solver modified the original frozen artifact");
        compatible = false;
        const auto incompatible = request("incompatible");
        check(!runner.start(incompatible).ok() && !facts.load(incompatible.run_id) &&
                  !fs::exists(incompatible.run_directory),
              "Host-rejected configuration created an intent or process");
        compatible = true;
        {
            std::ofstream broken(artifact.directory / "model.bdf");
            broken << "corrupted input";
        }
        const auto broken = request("broken-input");
        check(!runner.start(broken).ok() && !facts.load(broken.run_id),
              "Damaged completed input reached process admission");
        std::ofstream(artifact.directory / "model.bdf") << plan.resources.front().text;
        auto wrong_manifest = artifact;
        wrong_manifest.manifest += "damaged";
        LocalSolverRunner wrong_runner(hooks(facts, wrong_manifest, compatible));
        check(!wrong_runner.start(request("wrong-manifest")).ok(),
              "Wrong manifest digest reached process admission");
        for (const bool uncertain : {false, true}) {
            const auto id = uncertain ? "uncertain-identity" : "failed-identity";
            const auto value = request(id);
            facts.fail_state = SolverExecutionState::running;
            facts.fail_after_commit = uncertain;
            check(!runner.start(value).ok() &&
                      !fs::exists(fs::path(value.run_directory) / "child.started"),
                  "Child executed before durable identity acknowledgement");
            facts.fail_state.reset();
            facts.fail_after_commit = false;
            LocalSolverRunner restored(hooks(facts, artifact, compatible));
            const auto recovered = restored.query(id);
            check(recovered.ok() &&
                      recovered.value->execution == SolverExecutionState::outcome_unknown &&
                      restored.start(value).value->execution ==
                          SolverExecutionState::outcome_unknown &&
                      !fs::exists(fs::path(value.run_directory) / "child.started"),
                  "Uncertain launch recovery blindly reran the process");
        }
        facts.fail_state = SolverExecutionState::startup_intent;
        const auto no_intent = request("failed-intent");
        check(!runner.start(no_intent).ok() && !fs::exists(no_intent.run_directory),
              "Failed startup persistence mutated process files");
        facts.fail_state.reset();
        facts.fail_state = SolverExecutionState::startup_intent;
        facts.fail_after_commit = true;
        const auto uncertain_intent = request("uncertain-intent");
        check(!runner.start(uncertain_intent).ok() && !fs::exists(uncertain_intent.run_directory),
              "Uncertain startup persistence admitted a process");
        facts.fail_state.reset();
        facts.fail_after_commit = false;
        LocalSolverRunner intent_recovery(hooks(facts, artifact, compatible));
        check(intent_recovery.start(uncertain_intent).value->execution ==
                  SolverExecutionState::outcome_unknown,
              "Persisted uncertain startup intent was executed on retry");
        const auto malformed_executable = directory / "not-an-executable-format";
        std::ofstream(malformed_executable) << "TEST_ONLY_INVALID_EXECUTABLE_FORMAT\n";
        fs::permissions(malformed_executable, fs::perms::owner_all);
        auto exec_failure = request("exec-failure");
        exec_failure.configuration.executable = malformed_executable.string();
        const auto failed_exec = runner.start(exec_failure);
        check(failed_exec.ok() &&
                  failed_exec.value->execution == SolverExecutionState::launch_failed &&
                  !fs::exists(fs::path(exec_failure.run_directory) / "child.started"),
              "execve failure was reported as execution success");
        auto slow = request("cancel", "ignore-term");
        check(runner.start(slow).ok(), "Cancellation test did not start");
        await_child(slow);
        check(!runner.start(request("quota")).ok() && !facts.load("quota"),
              "Active process quota admitted another child");
        facts.fail_state = SolverExecutionState::cancel_requested;
        check(!runner.cancel(slow.run_id).ok() &&
                  runner.query(slow.run_id).value->execution == SolverExecutionState::running,
              "Cancellation sent a signal before durable intent");
        facts.fail_state.reset();
        const auto cancelled = runner.cancel(slow.run_id);
        check(cancelled.ok() && cancelled.value->accepted,
              "Actual cancellation request was not accepted");
        const auto stopped = wait(runner, slow.run_id);
        check(stopped.execution == SolverExecutionState::cancelled &&
                  stopped.termination_signal == SIGKILL && stopped.cancellation_requested &&
                  stopped.parsing == SolverParsingState::not_run,
              "Verified process group cancellation did not converge");
        for (const auto& mode : {"crash-on-term", "exit-on-term"}) {
            auto value = request(mode, mode);
            check(runner.start(value).ok(), "Cancellation race child did not start");
            await_child(value);
            check(runner.cancel(value.run_id).value->accepted,
                  "Cancellation race intent was not accepted");
            const auto observed = wait(runner, value.run_id);
            check(observed.execution == SolverExecutionState::exited &&
                      observed.cancellation_requested &&
                      (mode == std::string("crash-on-term") ? observed.termination_signal == SIGABRT
                                                            : observed.exit_code == 0),
                  "Cancellation replaced a natural failure or successful exit fact");
        }
        check(!runner.cancel(normal.run_id).value->accepted &&
                  runner.query(normal.run_id).value->exit_code == 0,
              "Late cancellation overwrote an already observed exit");
        auto completion = request("completion-persist");
        check(runner.start(completion).ok(), "Completion persistence test did not start");
        facts.fail_state = SolverExecutionState::exited;
        bool failed_completion{};
        for (unsigned i = 0; i < 200; ++i) {
            if (!runner.query(completion.run_id).ok()) {
                failed_completion = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        check(failed_completion, "Completion persistence failure was not injected");
        facts.fail_state.reset();
        check(wait(runner, completion.run_id).exit_code == 0,
              "Observed waitpid fact was lost after persistence failure");
        auto reused = request("reused-pid");
        SolverRunRecord old;
        old.request = reused;
        old.execution = SolverExecutionState::running;
        old.process = SolverProcessIdentity{::getpid(), "test-only wrong start identity"};
        require(facts.persist(old, std::nullopt).ok(), "Old identity fact did not persist");
        LocalSolverRunner restored(hooks(facts, artifact, compatible));
        const auto unknown = restored.cancel(reused.run_id);
        check(unknown.ok() && !unknown.value->accepted &&
                  unknown.value->run.execution == SolverExecutionState::outcome_unknown &&
                  ::kill(::getpid(), 0) == 0 && !fs::exists(reused.run_directory),
              "Recovered PID was signalled or blindly restarted");
        auto timeout = request("timeout", "ignore-term");
        timeout.configuration.max_wall_time_ms = 40;
        check(runner.start(timeout).ok(), "Wall time test did not start");
        const auto timed_out = wait(runner, timeout.run_id);
        check(timed_out.execution == SolverExecutionState::exited &&
                  timed_out.termination_signal.has_value() &&
                  timed_out.detail.find("Wall time") != std::string::npos,
              "Query-enforced wall time budget did not terminate the child");
        auto disk = request("disk-budget", "many-files");
        check(runner.start(disk).ok(), "Aggregate output budget child did not start");
        const auto disk_failure = wait(runner, disk.run_id);
        check(disk_failure.execution == SolverExecutionState::exited &&
                  disk_failure.termination_signal.has_value() &&
                  disk_failure.detail.find("byte budget") != std::string::npos &&
                  disk_failure.output_state == SolverOutputState::incomplete,
              "Aggregate output budget did not terminate or reject partial output");
        fs::remove_all(directory);
        std::cout
            << "PASS: " << cases
            << " local process checks; all children test-only, no Nastran/numerical acceptance\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        if (!directory.empty())
            std::cerr << "Test-only evidence retained at " << directory << '\n';
        return 1;
    }
}
