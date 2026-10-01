#include "solver_version_probe.hpp"
#include "qcae/artifacts_local.hpp"
#include "qcae/record_registry.hpp"
#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <fstream>
#include <poll.h>
#include <set>
#include <sstream>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <libproc.h>
#endif

namespace qcae::ipc {
namespace {
namespace fs = std::filesystem;
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
bool digest(std::string_view value) {
    return value.size() == 64 && value.find_first_not_of("0123456789abcdef") == value.npos;
}
bool version(std::string_view value) {
    return value == "2022.1" || value == "2024.1";
}
void validate_evidence(const SolverVersionEvidence& fact) {
    const auto& file = fact.executable;
    require(fact.protocol == "msc.help.v1" &&
                (fact.reported_version.empty() || version(fact.reported_version)) &&
                file.inode > 0 && file.byte_length > 0 && file.byte_length <= 134217728 &&
                file.modified_nanoseconds < 1000000000 && file.changed_nanoseconds < 1000000000 &&
                digest(file.sha256) && fact.process.pid > 0 && fact.process.pid <= INT32_MAX &&
                !fact.process.start_identity.empty() && fact.process.start_identity.size() <= 128 &&
                fact.process.start_identity.find('\0') == std::string::npos &&
                fact.stdout_bytes <= 65536 && fact.stderr_bytes <= 65536 - fact.stdout_bytes &&
                digest(fact.stdout_sha256) && digest(fact.stderr_sha256) && fact.exit_code >= 0 &&
                fact.exit_code <= 255,
            "Malformed local version observation");
}
template <class T> T exact(std::string_view value) {
    T result{};
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    require(parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size(),
            "Malformed version observation integer");
    return result;
}
class Descriptor {
  public:
    explicit Descriptor(int value = -1) : value_(value) {}
    ~Descriptor() {
        reset();
    }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
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
int owned(int fd) {
    if (fd < 0 || fd >= 3)
        return fd;
    const int next = ::fcntl(fd, F_DUPFD_CLOEXEC, 3);
    ::close(fd);
    return next;
}
void pipe_pair(int (&fd)[2]) {
    require(::pipe(fd) == 0, "Version capture pipe failed");
    fd[0] = owned(fd[0]);
    fd[1] = owned(fd[1]);
    if (fd[0] < 0 || fd[1] < 0 || ::fcntl(fd[0], F_SETFD, FD_CLOEXEC) < 0 ||
        ::fcntl(fd[1], F_SETFD, FD_CLOEXEC) < 0) {
        ::close(fd[0]);
        ::close(fd[1]);
        throw std::runtime_error("Version capture descriptor setup failed");
    }
}
void no_symlinks(const fs::path& path) {
    require(path.is_absolute() && path.lexically_normal() == path, "Expected absolute local path");
    fs::path current;
    for (const auto& part : path) {
        current /= part;
        require(!fs::is_symlink(fs::symlink_status(current)),
                "Version resource contains a symlink");
    }
}
SolverExecutableIdentity metadata(const struct stat& info) {
    SolverExecutableIdentity identity;
    identity.device = static_cast<std::uint64_t>(info.st_dev);
    identity.inode = static_cast<std::uint64_t>(info.st_ino);
    identity.byte_length = static_cast<std::uint64_t>(info.st_size);
#if defined(__APPLE__)
    const auto modified = info.st_mtimespec, changed = info.st_ctimespec;
#else
    const auto modified = info.st_mtim, changed = info.st_ctim;
#endif
    identity.modified_seconds = modified.tv_sec;
    identity.modified_nanoseconds = static_cast<std::uint64_t>(modified.tv_nsec);
    identity.changed_seconds = changed.tv_sec;
    identity.changed_nanoseconds = static_cast<std::uint64_t>(changed.tv_nsec);
    return identity;
}
std::optional<std::string> start_identity(pid_t pid) {
#if defined(__APPLE__)
    proc_bsdinfo info{};
    if (::proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info, sizeof(info)) != sizeof(info))
        return {};
    return "darwin:" + std::to_string(info.pbi_start_tvsec) + ":" +
           std::to_string(info.pbi_start_tvusec);
#else
    std::ifstream file("/proc/" + std::to_string(pid) + "/stat");
    std::string line;
    if (!std::getline(file, line) || line.rfind(')') == line.npos)
        return {};
    std::istringstream fields(line.substr(line.rfind(')') + 1));
    std::string token;
    for (int field = 3; field <= 22; ++field)
        if (!(fields >> token))
            return {};
    std::ifstream boot("/proc/sys/kernel/random/boot_id");
    std::string boot_id;
    if (!std::getline(boot, boot_id) || boot_id.empty())
        return {};
    return "linux:" + boot_id + ":" + token;
#endif
}
std::string reported_version(std::string_view text) {
    std::set<std::string> versions;
    while (!text.empty()) {
        const auto end = text.find('\n');
        auto line = text.substr(0, end);
        text = end == text.npos ? std::string_view{} : text.substr(end + 1);
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
            line.remove_prefix(1);
        // Conservative wrapper contract. A matching substring in arbitrary prose is insufficient.
        constexpr std::string_view prefix = "MSC Nastran ";
        if (!line.starts_with(prefix))
            continue;
        line.remove_prefix(prefix.size());
        if (line.starts_with('V'))
            line.remove_prefix(1);
        const auto stop = line.find_first_of(" \t\r");
        const auto token = line.substr(0, stop);
        if (!version(token))
            return {};
        versions.emplace(token);
    }
    return versions.size() == 1 ? *versions.begin() : "";
}
} // namespace

std::string encode_solver_version_evidence(const SolverVersionEvidence& fact) {
    validate_evidence(fact);
    const auto& file = fact.executable;
    return record_wire::strings(
        std::array<std::string, 19>{"QCAE-SOLVER-VERSION-1",
                                    fact.protocol,
                                    fact.reported_version,
                                    std::to_string(file.device),
                                    std::to_string(file.inode),
                                    std::to_string(file.byte_length),
                                    std::to_string(file.modified_seconds),
                                    std::to_string(file.modified_nanoseconds),
                                    std::to_string(file.changed_seconds),
                                    std::to_string(file.changed_nanoseconds),
                                    file.sha256,
                                    std::to_string(fact.process.pid),
                                    fact.process.start_identity,
                                    std::to_string(fact.stdout_bytes),
                                    std::to_string(fact.stderr_bytes),
                                    fact.stdout_sha256,
                                    fact.stderr_sha256,
                                    std::to_string(fact.exit_code),
                                    fact.synthetic ? "1" : "0"});
}
SolverVersionEvidence decode_solver_version_evidence(std::string_view bytes) {
    require(bytes.size() <= 2048, "Version observation exceeds budget");
    const auto values = record_wire::read_strings(bytes);
    require(values.size() == 19 && values[0] == "QCAE-SOLVER-VERSION-1",
            "Unknown version observation");
    SolverVersionEvidence fact;
    fact.protocol = values[1];
    fact.reported_version = values[2];
    auto& file = fact.executable;
    file.device = exact<std::uint64_t>(values[3]);
    file.inode = exact<std::uint64_t>(values[4]);
    file.byte_length = exact<std::uint64_t>(values[5]);
    file.modified_seconds = exact<std::int64_t>(values[6]);
    file.modified_nanoseconds = exact<std::uint64_t>(values[7]);
    file.changed_seconds = exact<std::int64_t>(values[8]);
    file.changed_nanoseconds = exact<std::uint64_t>(values[9]);
    file.sha256 = values[10];
    fact.process = {exact<std::int64_t>(values[11]), values[12]};
    fact.stdout_bytes = exact<std::uint64_t>(values[13]);
    fact.stderr_bytes = exact<std::uint64_t>(values[14]);
    fact.stdout_sha256 = values[15];
    fact.stderr_sha256 = values[16];
    fact.exit_code = exact<int>(values[17]);
    require(values[18] == "0" || values[18] == "1", "Malformed synthetic observation flag");
    fact.synthetic = values[18] == "1";
    require(encode_solver_version_evidence(fact) == bytes, "Noncanonical version observation");
    return fact;
}
std::string solver_version_configuration_digest(std::string_view declaration_digest,
                                                const SolverVersionEvidence& evidence) {
    require(digest(declaration_digest), "Declaration digest is invalid");
    return artifact_sha256(record_wire::strings(std::array<std::string, 2>{
        std::string(declaration_digest), encode_solver_version_evidence(evidence)}));
}

namespace detail {
Result<SolverExecutableIdentity> solver_executable_identity(const fs::path& path,
                                                            std::uint64_t max_bytes) {
    try {
        no_symlinks(path);
        Descriptor file(owned(::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC)));
        struct stat before{}, after{}, current{};
        require(file.get() >= 0 && ::fstat(file.get(), &before) == 0 && S_ISREG(before.st_mode) &&
                    before.st_size > 0 && max_bytes > 0 && max_bytes <= 134217728 &&
                    static_cast<std::uint64_t>(before.st_size) <= max_bytes &&
                    ::access(path.c_str(), X_OK) == 0,
                "Version executable is unavailable or oversized");
        std::string bytes(static_cast<std::size_t>(before.st_size), '\0');
        std::size_t offset{};
        while (offset < bytes.size()) {
            const auto count = ::read(file.get(), bytes.data() + offset, bytes.size() - offset);
            if (count < 0 && errno == EINTR)
                continue;
            require(count > 0, "Version executable changed during readback");
            offset += static_cast<std::size_t>(count);
        }
        char extra{};
        require(::read(file.get(), &extra, 1) == 0 && ::fstat(file.get(), &after) == 0 &&
                    ::lstat(path.c_str(), &current) == 0 && metadata(before) == metadata(after) &&
                    metadata(before) == metadata(current),
                "Version executable identity changed during readback");
        auto identity = metadata(before);
        identity.sha256 = artifact_sha256(bytes);
        return {Status::success, std::move(identity), {}};
    } catch (const std::exception& error) {
        return {Status::failed,
                {},
                Diagnostic{ErrorCode::invalid_input, error.what(), "solver_version"}};
    }
}
bool solver_version_allows_execution(const SolverVersionEvidence& fact, std::string_view expected) {
    try {
        validate_evidence(fact);
        return version(expected) && fact.reported_version == expected && fact.exit_code == 0 &&
               !fact.synthetic;
    } catch (...) {
        return false;
    }
}
Result<SolverVersionEvidence> probe_solver_version(const fs::path& executable,
                                                   const fs::path& private_root,
                                                   std::string_view expected,
                                                   const SolverVersionProbeLimits& limits) {
    fs::path cwd;
    pid_t child = -1;
    std::optional<std::string> identity;
    bool reaped{};
    try {
        require(version(expected) && limits.wall_time_ms > 0 && limits.wall_time_ms <= 5000 &&
                    limits.max_output_bytes > 0 && limits.max_output_bytes <= 65536,
                "Version probe limits or controlled version are invalid");
        no_symlinks(private_root);
        require(fs::is_directory(private_root), "Private version probe parent is missing");
        const auto before = solver_executable_identity(executable, limits.max_executable_bytes);
        require(before.ok(), "Version executable cannot be frozen");
        auto pattern = (private_root / ".qcae-version-XXXXXX").string();
        require(::mkdtemp(pattern.data()) != nullptr, "Private version probe directory failed");
        cwd = pattern;
        int out[2], err[2], gate[2];
        pipe_pair(out);
        Descriptor out_read(out[0]), out_write(out[1]);
        pipe_pair(err);
        Descriptor err_read(err[0]), err_write(err[1]);
        require(::socketpair(AF_UNIX, SOCK_STREAM, 0, gate) == 0,
                "Version probe admission gate failed");
        gate[0] = owned(gate[0]);
        gate[1] = owned(gate[1]);
        Descriptor gate_read(gate[0]), gate_write(gate[1]);
        require(gate_read.get() >= 0 && gate_write.get() >= 0, "Version gate descriptors failed");
#if defined(__APPLE__)
        const int enabled = 1;
        require(::setsockopt(
                    gate_write.get(), SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) == 0,
                "Version gate signal protection failed");
#endif
        Descriptor input(owned(::open("/dev/null", O_RDONLY | O_CLOEXEC)));
        const auto max_fd = ::sysconf(_SC_OPEN_MAX);
        require(input.get() >= 0 && max_fd > 0 && max_fd <= 1048576,
                "Version descriptor bound is invalid");
        char help[] = "help", locale[] = "LANG=C", all[] = "LC_ALL=C";
        const auto executable_text = executable.string(), cwd_text = cwd.string();
        char* argv[] = {const_cast<char*>(executable_text.c_str()), help, nullptr};
        char* environment[] = {locale, all, nullptr};
        child = ::fork();
        require(child >= 0, "Version probe fork failed");
        if (child == 0) {
            if (::setpgid(0, 0) != 0 || ::chdir(cwd_text.c_str()) != 0 ||
                ::dup2(input.get(), STDIN_FILENO) < 0 ||
                ::dup2(out_write.get(), STDOUT_FILENO) < 0 ||
                ::dup2(err_write.get(), STDERR_FILENO) < 0)
                ::_exit(126);
            for (int fd = 3; fd < max_fd; ++fd)
                if (fd != gate_read.get())
                    ::close(fd);
            struct rlimit zero{0, 0}, file_limit{65536, 65536}, cpu_limit{2, 2};
            if (::setrlimit(RLIMIT_CORE, &zero) != 0 ||
                ::setrlimit(RLIMIT_FSIZE, &file_limit) != 0 ||
                ::setrlimit(RLIMIT_CPU, &cpu_limit) != 0)
                ::_exit(126);
            char token{};
            ssize_t count;
            do {
                count = ::read(gate_read.get(), &token, 1);
            } while (count < 0 && errno == EINTR);
            if (count != 1 || token != 'Y')
                ::_exit(126);
            ::close(gate_read.get());
            ::execve(executable_text.c_str(), argv, environment);
            ::_exit(127);
        }
        out_write.reset();
        err_write.reset();
        gate_read.reset();
        identity = start_identity(child);
        require(identity.has_value(), "Version probe creation identity unavailable");
#if defined(__APPLE__)
        constexpr int flags = 0;
#else
        constexpr int flags = MSG_NOSIGNAL;
#endif
        require(::send(gate_write.get(), "Y", 1, flags) == 1, "Version probe admission failed");
        gate_write.reset();
        require(::fcntl(out_read.get(), F_SETFL, O_NONBLOCK) == 0 &&
                    ::fcntl(err_read.get(), F_SETFL, O_NONBLOCK) == 0,
                "Version capture nonblocking setup failed");
        std::string stdout_text, stderr_text;
        bool out_end{}, err_end{};
        int status{};
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(limits.wall_time_ms);
        auto capture = [&](Descriptor& fd, std::string& text, bool& end) {
            char buffer[8192];
            while (!end) {
                const auto count = ::read(fd.get(), buffer, sizeof(buffer));
                if (count < 0 && errno == EINTR)
                    continue;
                if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                    return;
                require(count >= 0, "Version capture failed");
                if (count == 0) {
                    end = true;
                    return;
                }
                require(static_cast<std::uint64_t>(count) <=
                            limits.max_output_bytes - stdout_text.size() - stderr_text.size(),
                        "Version output budget exceeded");
                text.append(buffer, static_cast<std::size_t>(count));
            }
        };
        while (!reaped || !out_end || !err_end) {
            capture(out_read, stdout_text, out_end);
            capture(err_read, stderr_text, err_end);
            if (!reaped) {
                pid_t waited;
                do {
                    waited = ::waitpid(child, &status, WNOHANG);
                } while (waited < 0 && errno == EINTR);
                require(waited >= 0, "Version waitpid observation unavailable");
                reaped = waited == child;
            }
            require(std::chrono::steady_clock::now() < deadline, "Version probe timeout");
            if (!reaped || !out_end || !err_end) {
                struct pollfd ready[2]{{out_read.get(), POLLIN, 0}, {err_read.get(), POLLIN, 0}};
                (void)::poll(ready, 2, 10);
            }
        }
        require(WIFEXITED(status), "Version probe terminated without a normal exit");
        require(::kill(-child, 0) != 0 && errno == ESRCH,
                "Version probe process group remains unresolved");
        const auto after = solver_executable_identity(executable, limits.max_executable_bytes);
        require(after.ok() && *after.value == *before.value,
                "Version executable changed during probe");
        require(stdout_text.find('\0') == stdout_text.npos &&
                    stderr_text.find('\0') == stderr_text.npos,
                "Version output contains NUL");
        SolverVersionEvidence fact;
        fact.reported_version = reported_version(stdout_text + "\n" + stderr_text);
        fact.executable = *before.value;
        fact.process = {child, *identity};
        fact.stdout_bytes = stdout_text.size();
        fact.stderr_bytes = stderr_text.size();
        fact.stdout_sha256 = artifact_sha256(stdout_text);
        fact.stderr_sha256 = artifact_sha256(stderr_text);
        fact.exit_code = WEXITSTATUS(status);
        fact.synthetic =
            (stdout_text + stderr_text).find("QCAE SYNTHETIC VERSION PROBE") != std::string::npos;
        validate_evidence(fact);
        fs::remove_all(cwd);
        return {Status::success, std::move(fact), {}};
    } catch (const std::exception& error) {
        if (child > 0 && !reaped) {
            // An unreaped owned child cannot have its PID reused. Still require creation identity.
            if (identity && start_identity(child) == identity)
                (void)::kill(-child, SIGKILL);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
            int status{};
            while (std::chrono::steady_clock::now() < deadline) {
                const auto waited = ::waitpid(child, &status, WNOHANG);
                if (waited == child || (waited < 0 && errno == ECHILD)) {
                    reaped = true;
                    break;
                }
                (void)::poll(nullptr, 0, 5);
            }
        }
        // Reaping the leader does not prove its process group has stopped. Never erase
        // a private directory that an unresolved descendant may still be using, and
        // never signal a group after its owned leader identity has been reaped.
        const bool group_gone = child < 0 || (reaped && ::kill(-child, 0) != 0 && errno == ESRCH);
        if (group_gone) {
            std::error_code ignored;
            if (!cwd.empty())
                fs::remove_all(cwd, ignored);
        }
        return {Status::failed,
                {},
                Diagnostic{ErrorCode::unsupported_capability, error.what(), "solver_version"}};
    }
}
} // namespace detail
} // namespace qcae::ipc
