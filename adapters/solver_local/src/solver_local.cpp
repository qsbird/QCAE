#include "qcae/solver_local.hpp"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <libproc.h>
#elif !defined(__linux__)
#error "solver_local currently supports POSIX macOS and Linux process identities only"
#endif

namespace qcae {
namespace {
using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;
constexpr std::uint64_t max_bytes = 16777216;
class FileDescriptor {
  public:
    explicit FileDescriptor(int value = -1) : value_(value) {}
    ~FileDescriptor() {
        reset();
    }
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    int get() const {
        return value_;
    }
    void reset(int value = -1) {
        if (value_ >= 0)
            ::close(value_);
        value_ = value;
    }

  private:
    int value_;
};
template <class T> Result<T> success(T value) {
    return {Status::success, std::move(value), std::nullopt};
}
template <class T>
Result<T> failure(ErrorCode code, std::string message, Status status = Status::failed) {
    return {status, std::nullopt, Diagnostic{code, std::move(message), "solver_run"}};
}
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
bool text_valid(std::string_view text, std::size_t limit = 1024) {
    return !text.empty() && text.size() <= limit && text.find('\0') == std::string_view::npos;
}
bool digest_valid(std::string_view text) {
    return text.size() == 64 && std::all_of(text.begin(), text.end(), [](char ch) {
               return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
           });
}
bool relative_file(std::string_view text) {
    if (!text_valid(text))
        return false;
    fs::path path(text);
    if (path.is_absolute() || path.lexically_normal() != path || !path.has_filename())
        return false;
    for (const auto& part : path)
        if (part == "." || part == ".." || part.empty())
            return false;
    return true;
}
void no_symlinks(const fs::path& path) {
    require(path.is_absolute() && path.lexically_normal() == path,
            "Path must be absolute and normalized");
    fs::path current;
    for (const auto& part : path) {
        current /= part;
        require(!fs::is_symlink(fs::symlink_status(current)), "Symlink path is unsupported");
    }
}
void validate_request(const SolverRunRequest& request) {
    require(text_valid(request.run_id, 128) && text_valid(request.task_id, 128),
            "Run and task identities are required");
    require(std::all_of(request.run_id.begin(),
                        request.run_id.end(),
                        [](char ch) {
                            return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                                   (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
                        }),
            "Run identity must be a safe directory name");
    require(text_valid(request.run_directory, 4096), "Run directory is required");
    fs::path directory(request.run_directory);
    no_symlinks(directory);
    require(directory.filename() == request.run_id, "Run directory must belong to run_id");
    require(fs::is_directory(directory.parent_path()), "Run parent directory is missing");
    const auto& input = request.input;
    require(
        text_valid(input.document.value) && text_valid(input.epoch.value) &&
            text_valid(input.analysis.value) && text_valid(input.profile.profile_id) &&
            text_valid(input.profile.profile_version) &&
            text_valid(input.profile.definition_digest) && text_valid(input.artifact_id) &&
            digest_valid(input.manifest_sha256) && text_valid(input.input_fingerprint, max_bytes) &&
            digest_valid(input.export_identity_digest) && text_valid(input.export_rule_version) &&
            text_valid(input.result_reader_version) && !input.frozen_analysis_input.empty() &&
            input.frozen_analysis_input.size() <= max_bytes,
        "Frozen analysis provenance is incomplete");
    const auto& config = request.configuration;
    require(text_valid(config.id) && text_valid(config.executable, 4096) &&
                text_valid(config.solver_family) && text_valid(config.dialect) &&
                text_valid(config.solver_version) && text_valid(config.version_evidence) &&
                digest_valid(config.configuration_digest),
            "Explicit solver configuration and version evidence are required");
    no_symlinks(fs::path(config.executable));
    require(fs::is_regular_file(config.executable) &&
                ::access(config.executable.c_str(), X_OK) == 0,
            "Configured executable is unavailable");
    require(config.argv.size() <= 64, "Too many process arguments");
    std::size_t argument_bytes{};
    for (const auto& argument : config.argv) {
        require(argument.size() <= 4096 && argument.find('\0') == std::string::npos,
                "Process argument is invalid");
        argument_bytes += argument.size();
    }
    require(argument_bytes <= 65536, "Process arguments exceed budget");
    require(config.max_wall_time_ms > 0 && config.max_wall_time_ms <= 86400000 &&
                config.cancel_grace_ms <= 5000 && config.max_output_bytes > 0 &&
                config.max_output_bytes <= max_bytes,
            "Process budget is invalid");
    require(!config.expected_outputs.empty() && config.expected_outputs.size() <= 32,
            "Explicit bounded output paths are required");
    std::set<std::string> paths;
    for (const auto& output : config.expected_outputs) {
        require(relative_file(output) && fs::path(output).begin()->string() != "input" &&
                    output != "runner.stdout" && output != "runner.stderr" &&
                    paths.insert(output).second,
                "Output path is invalid or duplicated");
    }
}
bool terminal(SolverExecutionState state) {
    return state == SolverExecutionState::exited || state == SolverExecutionState::cancelled ||
           state == SolverExecutionState::launch_failed ||
           state == SolverExecutionState::outcome_unknown;
}
std::optional<std::string> process_start_identity(pid_t pid) {
#if defined(__APPLE__)
    proc_bsdinfo info{};
    if (::proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info, sizeof(info)) != sizeof(info))
        return std::nullopt;
    return "darwin:" + std::to_string(info.pbi_start_tvsec) + ":" +
           std::to_string(info.pbi_start_tvusec);
#else
    std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
    std::string line;
    if (!std::getline(stat, line))
        return std::nullopt;
    const auto end = line.rfind(')');
    if (end == std::string::npos)
        return std::nullopt;
    std::istringstream fields(line.substr(end + 1));
    std::string value;
    for (int field = 3; field <= 22; ++field)
        if (!(fields >> value))
            return std::nullopt;
    std::ifstream boot("/proc/sys/kernel/random/boot_id");
    std::string boot_id;
    if (!std::getline(boot, boot_id) || boot_id.empty())
        return std::nullopt;
    return "linux:" + boot_id + ":" + value;
#endif
}
void sync_directory(const fs::path& path) {
    FileDescriptor fd(::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    require(fd.get() >= 0 && ::fsync(fd.get()) == 0, "Run directory sync failed");
}
int owned_descriptor(int fd) {
    if (fd < 0 || fd >= 3)
        return fd;
    const int duplicate = ::fcntl(fd, F_DUPFD_CLOEXEC, 3);
    ::close(fd);
    return duplicate;
}
void make_pipe(int (&fds)[2]) {
    require(::pipe(fds) == 0, "Process control pipe failed");
    fds[0] = owned_descriptor(fds[0]);
    fds[1] = owned_descriptor(fds[1]);
    if (fds[0] < 0 || fds[1] < 0 || ::fcntl(fds[0], F_SETFD, FD_CLOEXEC) == -1 ||
        ::fcntl(fds[1], F_SETFD, FD_CLOEXEC) == -1) {
        ::close(fds[0]);
        ::close(fds[1]);
        throw std::runtime_error("Process control descriptor setup failed");
    }
}
void make_gate(int (&fds)[2]) {
    require(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "Process admission socket failed");
    fds[0] = owned_descriptor(fds[0]);
    fds[1] = owned_descriptor(fds[1]);
    bool valid = fds[0] >= 0 && fds[1] >= 0 && ::fcntl(fds[0], F_SETFD, FD_CLOEXEC) != -1 &&
                 ::fcntl(fds[1], F_SETFD, FD_CLOEXEC) != -1;
#if defined(__APPLE__)
    const int enabled = 1;
    valid = valid && ::setsockopt(fds[1], SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) == 0;
#endif
    if (!valid) {
        ::close(fds[0]);
        ::close(fds[1]);
        throw std::runtime_error("Process admission socket setup failed");
    }
}
void check_output_budget(const SolverRunRecord& record) {
    std::uint64_t total{};
    std::size_t entries{};
    const fs::path root(record.request.run_directory);
    for (fs::recursive_directory_iterator it(root), end; it != end; ++it) {
        require(++entries <= 256 && it.depth() <= 16, "Run output entry budget exceeded");
        const auto relative = it->path().lexically_relative(root);
        require(!it->is_symlink(), "Run output contains a symlink");
        if (*relative.begin() == "input") {
            if (it->is_directory())
                it.disable_recursion_pending();
            continue;
        }
        require(it->is_directory() || it->is_regular_file(), "Run output type is invalid");
        if (it->is_regular_file()) {
            const auto bytes = it->file_size();
            require(bytes <= record.request.configuration.max_output_bytes - total,
                    "Run output byte budget exceeded");
            total += bytes;
        }
    }
}
[[noreturn]] void child_fail(int error_fd, int error) {
    // Only async-signal-safe operations are allowed between fork and exec in the engine.
    ssize_t count;
    do {
        count = ::write(error_fd, &error, sizeof(error));
    } while (count < 0 && errno == EINTR);
    ::_exit(126);
}
void collect_outputs(SolverRunRecord& record, const LocalArtifactIntent& copy) {
    try {
        (void)LocalArtifactStore{}.verify(copy);
        check_output_budget(record);
        const fs::path root(record.request.run_directory);
        auto paths = record.request.configuration.expected_outputs;
        paths.push_back("runner.stdout");
        paths.push_back("runner.stderr");
        std::vector<SolverOutputFile> files;
        for (const auto& path : paths) {
            const auto full = root / path;
            no_symlinks(full);
            FileDescriptor fd(::open(full.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
            struct stat info{};
            require(fd.get() >= 0 && ::fstat(fd.get(), &info) == 0 && S_ISREG(info.st_mode) &&
                        info.st_size >= 0 &&
                        static_cast<std::uint64_t>(info.st_size) <=
                            record.request.configuration.max_output_bytes,
                    "Expected run output is missing or invalid");
            require(info.st_size != 0 || path == "runner.stdout" || path == "runner.stderr",
                    "Expected run output is empty");
            std::string bytes(static_cast<std::size_t>(info.st_size), '\0');
            std::size_t offset{};
            while (offset < bytes.size()) {
                const auto count = ::read(fd.get(), bytes.data() + offset, bytes.size() - offset);
                if (count < 0 && errno == EINTR)
                    continue;
                require(count > 0, "Run output changed during readback");
                offset += static_cast<std::size_t>(count);
            }
            char extra{};
            require(::read(fd.get(), &extra, 1) == 0, "Run output changed during readback");
            struct stat after{};
            require(::fstat(fd.get(), &after) == 0 && info.st_dev == after.st_dev &&
                        info.st_ino == after.st_ino && info.st_size == after.st_size &&
                        info.st_mtime == after.st_mtime && info.st_ctime == after.st_ctime,
                    "Run output metadata changed during readback");
            files.push_back({path, bytes.size(), artifact_sha256(bytes)});
        }
        record.outputs = std::move(files);
        record.output_state = SolverOutputState::collected;
    } catch (const std::exception& error) {
        record.outputs.clear();
        record.output_state = SolverOutputState::incomplete;
        if (!record.detail.empty())
            record.detail += "; ";
        record.detail += error.what();
    }
}
} // namespace

struct LocalSolverRunner::State {
    struct Active {
        pid_t pid{};
        std::string identity;
        SolverRunRecord persisted;
        LocalArtifactIntent input_copy;
        Clock::time_point deadline, cancel_time{};
        bool cancel_sent{}, killed{};
        std::optional<SolverRunRecord> observed;
    };
    LocalSolverHost host;
    LocalSolverLimits limits;
    std::mutex mutex;
    std::map<std::string, Active> active;

    bool persist(const SolverRunRecord& next, const std::optional<SolverRunRecord>& expected) {
        try {
            const auto result = host.persist(next, expected);
            return result.ok() && *result.value;
        } catch (...) {
            return false;
        }
    }
    Result<SolverRunRecord> refresh(std::string_view run_id) {
        const auto loaded = host.load(run_id);
        if (!loaded.ok())
            return failure<SolverRunRecord>(ErrorCode::storage_failure, "Run facts could not load");
        if (!*loaded.value)
            return failure<SolverRunRecord>(ErrorCode::invalid_input, "Run identity is unknown");
        auto record = **loaded.value;
        if (record.request.run_id != run_id || record.sequence == 0)
            return failure<SolverRunRecord>(ErrorCode::storage_uncertain,
                                            "Run facts are malformed");
        auto found = active.find(std::string(run_id));
        if (found == active.end()) {
            if (!terminal(record.execution)) {
                auto next = record;
                ++next.sequence;
                next.execution = SolverExecutionState::outcome_unknown;
                next.detail =
                    "No owned child handle after restart; execution requires reconciliation";
                if (!persist(next, record))
                    return failure<SolverRunRecord>(ErrorCode::storage_uncertain,
                                                    "Recovery fact could not persist");
                record = std::move(next);
            }
            return success(std::move(record));
        }
        auto& child = found->second;
        if (record.sequence != child.persisted.sequence ||
            record.request != child.persisted.request || record.process != child.persisted.process)
            return failure<SolverRunRecord>(ErrorCode::storage_uncertain,
                                            "Owned process facts changed unexpectedly");
        if (!child.observed) {
            int status{};
            pid_t waited;
            do {
                waited = ::waitpid(child.pid, &status, WNOHANG);
            } while (waited < 0 && errno == EINTR);
            const auto identity = waited == 0 ? process_start_identity(child.pid) : std::nullopt;
            if (waited == child.pid) {
                auto next = record;
                ++next.sequence;
                next.execution = SolverExecutionState::exited;
                if (WIFEXITED(status))
                    next.exit_code = WEXITSTATUS(status);
                if (WIFSIGNALED(status)) {
                    next.termination_signal = WTERMSIG(status);
                    if (record.cancellation_requested && child.cancel_sent &&
                        (WTERMSIG(status) == SIGTERM || WTERMSIG(status) == SIGKILL))
                        next.execution = SolverExecutionState::cancelled;
                }
                if (::kill(-child.pid, 0) == 0 || errno == EPERM) {
                    next.execution = SolverExecutionState::outcome_unknown;
                    next.detail =
                        "Leader exited while process group remains; outputs are not frozen";
                } else {
                    collect_outputs(next, child.input_copy);
                }
                child.observed = std::move(next);
            } else if (waited == 0 && !identity) {
                // macOS can stop exposing process info during exit before waitpid can reap it.
                // The owned child handle remains valid; defer signals and observe it next poll.
                return success(std::move(record));
            } else if (waited < 0 || identity != child.identity) {
                auto next = record;
                ++next.sequence;
                next.execution = SolverExecutionState::outcome_unknown;
                next.detail = "Owned child identity or exit observation is unavailable";
                child.observed = std::move(next);
            } else {
                const auto now = Clock::now();
                std::string budget_error;
                try {
                    check_output_budget(record);
                } catch (const std::exception& error) {
                    budget_error = error.what();
                }
                if (!child.cancel_sent && (now >= child.deadline || !budget_error.empty())) {
                    auto next = record;
                    ++next.sequence;
                    next.detail = budget_error.empty()
                                      ? "Wall time budget exceeded; termination requested"
                                      : budget_error;
                    if (!persist(next, record))
                        return failure<SolverRunRecord>(
                            ErrorCode::storage_uncertain,
                            "Budget termination intent could not persist");
                    child.persisted = next;
                    record = std::move(next);
                    require(::kill(-child.pid, SIGTERM) == 0 || errno == ESRCH,
                            "Owned process group termination failed");
                    child.cancel_sent = true;
                    child.cancel_time = now;
                }
                if (child.cancel_sent && !child.killed &&
                    now - child.cancel_time >=
                        std::chrono::milliseconds(record.request.configuration.cancel_grace_ms)) {
                    require(::kill(-child.pid, SIGKILL) == 0 || errno == ESRCH,
                            "Owned process group forced termination failed");
                    child.killed = true;
                }
                return success(std::move(record));
            }
        }
        if (!persist(*child.observed, record))
            return failure<SolverRunRecord>(ErrorCode::storage_uncertain,
                                            "Observed process completion could not persist");
        record = *child.observed;
        active.erase(found);
        return success(std::move(record));
    }
};

LocalSolverRunner::LocalSolverRunner(LocalSolverHost host, LocalSolverLimits limits)
    : state_(std::make_unique<State>()) {
    require(host.input_artifact && host.validate_start && host.load && host.persist,
            "Solver runner requires trusted validation and durable fact hooks");
    require(limits.max_active_processes > 0 && limits.max_active_processes <= 8,
            "Active process limit is invalid");
    state_->host = std::move(host);
    state_->limits = limits;
}
LocalSolverRunner::~LocalSolverRunner() = default;

Result<SolverRunRecord> LocalSolverRunner::start(const SolverRunRequest& request) {
    std::lock_guard lock(state_->mutex);
    try {
        require(text_valid(request.run_id, 128), "Run identity is required");
        const auto prior = state_->host.load(request.run_id);
        if (!prior.ok())
            return failure<SolverRunRecord>(ErrorCode::storage_failure, "Run facts could not load");
        if (*prior.value) {
            if ((**prior.value).request != request)
                return failure<SolverRunRecord>(ErrorCode::idempotency_key_conflict,
                                                "Run identity already has different frozen inputs",
                                                Status::conflict);
            return state_->refresh(request.run_id);
        }
        validate_request(request);
        if (state_->active.size() >= state_->limits.max_active_processes)
            return failure<SolverRunRecord>(ErrorCode::resource_limit,
                                            "Active process quota reached");
        const auto input = state_->host.input_artifact(request);
        if (!input.ok())
            return failure<SolverRunRecord>(ErrorCode::invalid_input,
                                            "Frozen artifact is unresolved");
        const auto& artifact = *input.value;
        require(artifact.artifact_id == request.input.artifact_id &&
                    artifact_sha256(artifact.manifest) == request.input.manifest_sha256,
                "Frozen artifact identity or manifest digest differs");
        const auto resources = LocalArtifactStore{}.verify(artifact);
        const auto validation = state_->host.validate_start(request, artifact);
        require(validation.ok() && *validation.value,
                "Frozen input or solver configuration is not validated by the host");
        const fs::path directory(request.run_directory);
        require(!fs::exists(directory), "Run directory already exists without matching run facts");
        SolverRunRecord record;
        record.request = request;
        if (!state_->persist(record, std::nullopt))
            return failure<SolverRunRecord>(ErrorCode::storage_uncertain,
                                            "Startup intent did not reach durable storage");
        pid_t pid = -1;
        try {
            require(fs::create_directory(directory), "Run directory could not be created");
            fs::permissions(directory, fs::perms::owner_all, fs::perm_options::replace);
            sync_directory(directory.parent_path());
            auto copy = artifact;
            copy.directory = directory / "input";
            (void)LocalArtifactStore{}.stage(copy, resources, [] {});
            LocalArtifactStore{}.publish(copy);
            FileDescriptor output(
                owned_descriptor(::open((directory / "runner.stdout").c_str(),
                                        O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                                        0600)));
            FileDescriptor errors(
                owned_descriptor(::open((directory / "runner.stderr").c_str(),
                                        O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                                        0600)));
            FileDescriptor null_input(owned_descriptor(::open("/dev/null", O_RDONLY | O_CLOEXEC)));
            require(output.get() >= 0 && errors.get() >= 0 && null_input.get() >= 0,
                    "Run log descriptors could not be created");
            sync_directory(directory);
            std::vector<char*> arguments;
            arguments.push_back(const_cast<char*>(request.configuration.executable.c_str()));
            for (const auto& argument : request.configuration.argv)
                arguments.push_back(const_cast<char*>(argument.c_str()));
            arguments.push_back(nullptr);
            char locale[] = "LANG=C", locale_all[] = "LC_ALL=C";
            char* environment[] = {locale, locale_all, nullptr};
            int gate[2], exec_error[2];
            make_gate(gate);
            FileDescriptor gate_read(gate[0]), gate_write(gate[1]);
            make_pipe(exec_error);
            FileDescriptor error_read(exec_error[0]), error_write(exec_error[1]);
            const long descriptor_limit = ::sysconf(_SC_OPEN_MAX);
            require(descriptor_limit > 0 && descriptor_limit <= 1048576,
                    "Process descriptor budget is unsupported");
            const rlimit file_limit{request.configuration.max_output_bytes,
                                    request.configuration.max_output_bytes};
            const rlimit core_limit{0, 0};
            pid = ::fork();
            require(pid >= 0, "Process fork failed");
            if (pid == 0) {
                for (int fd = 3; fd < descriptor_limit; ++fd)
                    if (fd != gate_read.get() && fd != error_write.get() && fd != output.get() &&
                        fd != errors.get() && fd != null_input.get())
                        ::close(fd);
                if (::setpgid(0, 0) != 0 || ::chdir(request.run_directory.c_str()) != 0 ||
                    ::dup2(null_input.get(), STDIN_FILENO) < 0 ||
                    ::dup2(output.get(), STDOUT_FILENO) < 0 ||
                    ::dup2(errors.get(), STDERR_FILENO) < 0 ||
                    ::setrlimit(RLIMIT_FSIZE, &file_limit) != 0 ||
                    ::setrlimit(RLIMIT_CORE, &core_limit) != 0)
                    child_fail(error_write.get(), errno);
                ::close(output.get());
                ::close(errors.get());
                ::close(null_input.get());
                char allowed{};
                ssize_t count;
                do {
                    count = ::read(gate_read.get(), &allowed, 1);
                } while (count < 0 && errno == EINTR);
                if (count != 1 || allowed != 'Y')
                    ::_exit(125);
                ::close(gate_read.get());
                ::execve(request.configuration.executable.c_str(), arguments.data(), environment);
                child_fail(error_write.get(), errno);
            }
            gate_read.reset();
            error_write.reset();
            const auto identity = process_start_identity(pid);
            require(identity.has_value(), "OS process creation identity is unavailable");
            auto running = record;
            ++running.sequence;
            running.execution = SolverExecutionState::running;
            running.process = SolverProcessIdentity{pid, *identity};
            if (!state_->persist(running, record)) {
                gate_write.reset();
                int status{};
                while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
                }
                pid = -1;
                return failure<SolverRunRecord>(ErrorCode::storage_uncertain,
                                                "Process identity did not reach durable storage");
            }
            record = running;
            state_->active.emplace(
                request.run_id,
                State::Active{pid,
                              *identity,
                              record,
                              std::move(copy),
                              Clock::now() +
                                  std::chrono::milliseconds(request.configuration.max_wall_time_ms),
                              {},
                              false,
                              false,
                              std::nullopt});
#if defined(__APPLE__)
            constexpr int send_flags = 0;
#else
            constexpr int send_flags = MSG_NOSIGNAL;
#endif
            require(::send(gate_write.get(), "Y", 1, send_flags) == 1,
                    "Process admission gate failed");
            gate_write.reset();
            int exec_errno{};
            ssize_t count;
            do {
                count = ::read(error_read.get(), &exec_errno, sizeof(exec_errno));
            } while (count < 0 && errno == EINTR);
            require(count == 0, "Configured process failed before exec completion");
            return success(std::move(record));
        } catch (const std::exception& error) {
            // Before gate release a child exits on pipe EOF. After exec failure it already exits.
            // Never signal a PID that lacks the owned-child and start-identity proof.
            if (pid > 0) {
                state_->active.erase(request.run_id);
                int status{};
                while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
                }
            }
            auto next = record;
            ++next.sequence;
            next.execution = SolverExecutionState::launch_failed;
            next.detail = error.what();
            if (!state_->persist(next, record))
                return failure<SolverRunRecord>(ErrorCode::storage_uncertain,
                                                "Launch failure could not reach durable storage");
            return success(std::move(next));
        }
    } catch (const std::exception& error) {
        return failure<SolverRunRecord>(ErrorCode::invalid_input, error.what());
    }
}

Result<SolverRunRecord> LocalSolverRunner::query(std::string_view run_id) {
    std::lock_guard lock(state_->mutex);
    try {
        return state_->refresh(run_id);
    } catch (const std::exception& error) {
        return failure<SolverRunRecord>(ErrorCode::storage_uncertain, error.what());
    }
}
Result<SolverCancelResult> LocalSolverRunner::cancel(std::string_view run_id) {
    std::lock_guard lock(state_->mutex);
    try {
        const auto current = state_->refresh(run_id);
        if (!current.ok())
            return {current.status, std::nullopt, current.error};
        auto record = *current.value;
        if (terminal(record.execution))
            return success(SolverCancelResult{std::move(record), false});
        auto found = state_->active.find(std::string(run_id));
        require(found != state_->active.end() && record.process.has_value() &&
                    process_start_identity(found->second.pid) == record.process->start_identity,
                "Cancellation has no verified owned process identity");
        auto& child = found->second;
        if (record.execution == SolverExecutionState::cancel_requested)
            return success(SolverCancelResult{std::move(record), true});
        auto next = record;
        ++next.sequence;
        next.execution = SolverExecutionState::cancel_requested;
        next.cancellation_requested = true;
        if (!state_->persist(next, record))
            return failure<SolverCancelResult>(ErrorCode::storage_uncertain,
                                               "Cancellation intent could not persist");
        child.persisted = next;
        require(::kill(-child.pid, SIGTERM) == 0 || errno == ESRCH,
                "Owned process group cancellation failed");
        child.cancel_sent = true;
        child.cancel_time = Clock::now();
        return success(SolverCancelResult{std::move(next), true});
    } catch (const std::exception& error) {
        return failure<SolverCancelResult>(ErrorCode::storage_uncertain, error.what());
    }
}
} // namespace qcae
