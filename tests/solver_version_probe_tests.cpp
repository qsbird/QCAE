#include "solver_version_probe.hpp"
#include "qcae/artifacts_local.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <sys/resource.h>
#include <thread>
#include <vector>
#include <unistd.h>

namespace {
namespace fs = std::filesystem;
using namespace qcae;
using namespace qcae::ipc;
class InheritedDescriptorCanary {
  public:
    InheritedDescriptorCanary() : fd_(::open("/dev/null", O_RDONLY)) {
        if (fd_ < 0)
            throw std::runtime_error("Descriptor canary source is unavailable");
        const auto next = ::fcntl(fd_, F_DUPFD, QCAE_SOLVER_VERSION_FD_CANARY);
        ::close(fd_);
        fd_ = next;
        if (fd_ != QCAE_SOLVER_VERSION_FD_CANARY) {
            if (fd_ >= 0)
                ::close(fd_);
            fd_ = -1;
            throw std::runtime_error("Descriptor canary slot is unavailable");
        }
    }
    ~InheritedDescriptorCanary() {
        ::close(fd_);
    }
    InheritedDescriptorCanary(const InheritedDescriptorCanary&) = delete;
    InheritedDescriptorCanary& operator=(const InheritedDescriptorCanary&) = delete;
    bool inheritable() const {
        const auto flags = ::fcntl(fd_, F_GETFD);
        return flags >= 0 && (flags & FD_CLOEXEC) == 0;
    }

  private:
    int fd_;
};
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
} // namespace

int main(int argc, char**) {
    fs::path directory;
    try {
        require(argc == 1, "Unexpected test arguments");
        directory = fs::canonical(fs::temp_directory_path()) /
                    ("qcae-version-test-" + std::to_string(::getpid()));
        require(fs::create_directory(directory), "Test directory already exists");
        require(::setenv("QCAE_VERSION_TEST_CANARY", "nonsecret-test-only", 1) == 0,
                "Test canary failed");
        auto executable = [&](std::string name) {
            const auto path = directory / name;
            fs::copy_file(fs::canonical(QCAE_SOLVER_VERSION_FIXTURE_PATH), path);
            fs::permissions(path,
                            fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec);
            return path;
        };
        unsigned checks{};
        auto check = [&](bool valid, const char* message) {
            require(valid, message);
            ++checks;
        };
        InheritedDescriptorCanary descriptor_canary;
        check(descriptor_canary.inheritable(), "High descriptor canary would close on exec");
        const auto matching_path = executable("synthetic-matching");
#if defined(__APPLE__)
        struct rlimit descriptor_limit{};
        require(::getrlimit(RLIMIT_NOFILE, &descriptor_limit) == 0,
                "Descriptor limit observation failed");
        const struct rlimit reduced_limit{128, descriptor_limit.rlim_max};
        require(::setrlimit(RLIMIT_NOFILE, &reduced_limit) == 0,
                "Descriptor limit reduction failed");
        check(::sysconf(_SC_OPEN_MAX) < QCAE_SOLVER_VERSION_FD_CANARY,
              "Inherited descriptor did not exceed the reduced descriptor bound");
#endif
        const auto matching = detail::probe_solver_version(matching_path, directory, "2024.1");
#if defined(__APPLE__)
        require(::setrlimit(RLIMIT_NOFILE, &descriptor_limit) == 0,
                "Descriptor limit restoration failed");
#endif
        check(matching.ok() && matching.value->reported_version == "2024.1" &&
                  matching.value->exit_code == 0 && matching.value->synthetic &&
                  matching.value->stdout_bytes > 0 && matching.value->stderr_bytes > 0 &&
                  !matching.value->process.start_identity.empty(),
              "Actual fixed help probe did not capture its synthetic observation");
        check(!detail::solver_version_allows_execution(*matching.value, "2024.1"),
              "Synthetic probe became validated real solver configuration");
        const auto encoded = encode_solver_version_evidence(*matching.value);
        check(decode_solver_version_evidence(encoded) == *matching.value,
              "Actual version evidence codec changed observed identity/bytes");
        const auto declaration = artifact_sha256("test-only declaration");
        const auto config_digest =
            solver_version_configuration_digest(declaration, *matching.value);
        auto changed_evidence = *matching.value;
        changed_evidence.stdout_sha256 = artifact_sha256("different test-only output");
        check(solver_version_configuration_digest(declaration, changed_evidence) != config_digest,
              "Configuration digest did not bind actual probe output");
        for (const auto& mode :
             {"synthetic-noheader", "synthetic-substring", "synthetic-many", "synthetic-patch"}) {
            const auto observed =
                detail::probe_solver_version(executable(mode), directory, "2024.1");
            check(observed.ok() && observed.value->reported_version.empty() &&
                      !detail::solver_version_allows_execution(*observed.value, "2024.1"),
                  "Unsupported or ambiguous version output was accepted");
        }
        const auto older =
            detail::probe_solver_version(executable("synthetic-2022"), directory, "2022.1");
        check(older.ok() && older.value->reported_version == "2022.1" && older.value->synthetic &&
                  !detail::solver_version_allows_execution(*older.value, "2024.1"),
              "Controlled older version or expected-version mismatch was misread");
        const auto nonzero =
            detail::probe_solver_version(executable("synthetic-nonzero"), directory, "2024.1");
        check(nonzero.ok() && nonzero.value->exit_code == 7 &&
                  !detail::solver_version_allows_execution(*nonzero.value, "2024.1"),
              "Version-looking text hid an actual nonzero exit");
        check(!detail::probe_solver_version(executable("synthetic-nul"), directory, "2024.1").ok(),
              "NUL in version output was accepted");
        detail::SolverVersionProbeLimits short_limit{100, 65536, 134217728};
        const auto started = std::chrono::steady_clock::now();
        check(!detail::probe_solver_version(
                   executable("synthetic-timeout"), directory, "2024.1", short_limit)
                      .ok() &&
                  std::chrono::steady_clock::now() - started < std::chrono::seconds(2),
              "Version timeout did not bound actual child execution");
        check(
            !detail::probe_solver_version(executable("synthetic-output"), directory, "2024.1").ok(),
            "Version stdout/stderr aggregate limit was not enforced");
        check(!detail::probe_solver_version(
                   executable("synthetic-aggregate-output"), directory, "2024.1")
                   .ok(),
              "Individually bounded stdout and stderr escaped their aggregate budget");
        check(std::none_of(fs::directory_iterator(directory),
                           fs::directory_iterator{},
                           [](const auto& entry) {
                               return entry.path().filename().string().starts_with(
                                   ".qcae-version-");
                           }),
              "Completed or killed probe retained private scratch directories");
        check(!detail::probe_solver_version(executable("synthetic-descendant"), directory, "2024.1")
                   .ok(),
              "Unresolved process group became a completed version observation");
        std::vector<fs::path> retained;
        for (const auto& entry : fs::directory_iterator(directory))
            if (entry.path().filename().string().starts_with(".qcae-version-"))
                retained.push_back(entry.path());
        check(retained.size() == 1, "Unresolved descendant scratch was erased");
        const auto descendant_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!fs::exists(retained.front() / "descendant.finished")) {
            require(std::chrono::steady_clock::now() < descendant_deadline,
                    "Bounded synthetic descendant did not finish naturally");
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        fs::remove_all(retained.front());
        const auto before = detail::solver_executable_identity(matching_path);
        const auto replacement = executable("replacement");
        fs::rename(matching_path, directory / "previous");
        fs::rename(replacement, matching_path);
        const auto replaced = detail::solver_executable_identity(matching_path);
        check(before.ok() && replaced.ok() && *before.value != *replaced.value,
              "Same-byte executable path replacement escaped inode/ctime recheck");
        auto current = *replaced.value;
        std::ofstream(matching_path, std::ios::binary | std::ios::app) << "changed";
        const auto edited = detail::solver_executable_identity(matching_path);
        check(edited.ok() && *edited.value != current && edited.value->sha256 != current.sha256,
              "Executable content modification escaped digest recheck");
        fs::create_symlink(matching_path, directory / "linked");
        check(!detail::solver_executable_identity(directory / "linked").ok(),
              "Symlink executable entered the trusted identity boundary");
        check(!detail::solver_executable_identity(matching_path, 1).ok(),
              "Executable readback budget was not enforced");
        (void)::unsetenv("QCAE_VERSION_TEST_CANARY");
        fs::remove_all(directory);
        std::cout << "PASS: " << checks
                  << " actual version-probe checks; all executables synthetic, no "
                     "Nastran/numerical acceptance\n";
        return 0;
    } catch (const std::exception& error) {
        (void)::unsetenv("QCAE_VERSION_TEST_CANARY");
        std::cerr << error.what() << "\nSynthetic evidence retained at " << directory << '\n';
        return 1;
    }
}
