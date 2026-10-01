#include "solver_version_probe.hpp"
#include "qcae/artifacts_local.hpp"
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>
#include <unistd.h>

namespace {
namespace fs = std::filesystem;
using namespace qcae;
using namespace qcae::ipc;
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
int child(std::string_view mode) {
    require(std::getenv("QCAE_VERSION_TEST_CANARY") == nullptr,
            "Probe inherited test-only host canary environment");
    std::cout << "QCAE SYNTHETIC VERSION PROBE\n";
    if (mode == "synthetic-descendant") {
        int barrier[2];
        require(::pipe(barrier) == 0, "Synthetic descendant barrier failed");
        const auto descendant = ::fork();
        require(descendant >= 0, "Synthetic descendant fork failed");
        if (descendant == 0) {
            ::close(barrier[0]);
            ::close(STDOUT_FILENO);
            ::close(STDERR_FILENO);
            (void)::write(barrier[1], "Y", 1);
            ::close(barrier[1]);
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            std::ofstream("descendant.finished") << "Explicit bounded test-only descendant";
            ::_exit(0);
        }
        ::close(barrier[1]);
        char ready{};
        require(::read(barrier[0], &ready, 1) == 1 && ready == 'Y',
                "Synthetic descendant did not reach its real barrier");
        ::close(barrier[0]);
    }
    if (mode == "synthetic-timeout") {
        std::signal(SIGTERM, SIG_IGN);
        for (;;)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (mode == "synthetic-output") {
        std::cout << std::string(70000, 'x') << std::flush;
        return 0;
    }
    if (mode == "synthetic-aggregate-output") {
        std::cout << std::string(40000, 'x') << std::flush;
        std::cerr << std::string(30000, 'y') << std::flush;
        return 0;
    }
    if (mode == "synthetic-noheader")
        std::cout << "Compatible solver 2024.1\n";
    else if (mode == "synthetic-substring")
        std::cout << "Advertisement MSC Nastran 2024.1\n";
    else if (mode == "synthetic-many")
        std::cout << "MSC Nastran 2024.1\nMSC Nastran 2022.1\n";
    else if (mode == "synthetic-patch")
        std::cout << "MSC Nastran 2024.1.9\n";
    else if (mode == "synthetic-2022")
        std::cout << "MSC Nastran V2022.1\n";
    else if (mode == "synthetic-nul")
        std::cout << std::string("MSC Nastran 2024.1\n\0", 20);
    else
        std::cout << "MSC Nastran 2024.1\n";
    std::cerr << "Explicit test-only version output, not an installed Nastran\n";
    return mode == "synthetic-nonzero" ? 7 : 0;
}
} // namespace

int main(int argc, char** argv) {
    fs::path directory;
    try {
        if (argc == 2 && std::string_view(argv[1]) == "help")
            return child(fs::path(argv[0]).filename().string());
        require(argc == 1, "Unexpected test arguments");
        directory = fs::canonical(fs::temp_directory_path()) /
                    ("qcae-version-test-" + std::to_string(::getpid()));
        require(fs::create_directory(directory), "Test directory already exists");
        require(::setenv("QCAE_VERSION_TEST_CANARY", "nonsecret-test-only", 1) == 0,
                "Test canary failed");
        auto executable = [&](std::string name) {
            const auto path = directory / name;
            fs::copy_file(fs::canonical(argv[0]), path);
            fs::permissions(path,
                            fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec);
            return path;
        };
        unsigned checks{};
        auto check = [&](bool valid, const char* message) {
            require(valid, message);
            ++checks;
        };
        const auto matching_path = executable("synthetic-matching");
        const auto matching = detail::probe_solver_version(matching_path, directory, "2024.1");
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
