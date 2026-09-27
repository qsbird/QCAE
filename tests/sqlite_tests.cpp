#include "qcae/sqlite_store.hpp"

#include <sqlite3.h>

#ifdef NDEBUG
#undef NDEBUG // These test checks must run in Release builds too.
#endif
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace {
namespace fs = std::filesystem;
std::string test_executable;

struct Sandbox {
    fs::path path;
    Sandbox() {
        std::string pattern = (fs::temp_directory_path() / "qcae-sqlite-test-XXXXXX").string();
        char* result = ::mkdtemp(pattern.data());
        assert(result);
        path = result;
    }
    ~Sandbox() {
        fs::remove_all(path);
    }
};

bool throws(const std::function<void()>& action) {
    try {
        action();
    } catch (const qcae::StorageError&) {
        return true;
    }
    return false;
}

bool uncertain(const std::function<void()>& action) {
    try {
        action();
    } catch (const qcae::StorageError& error) {
        return error.uncertain();
    }
    return false;
}

void child_must_succeed(const char* label, std::vector<std::string> arguments) {
    arguments.insert(arguments.begin(), test_executable);
    std::vector<char*> argv;
    for (auto& argument : arguments)
        argv.push_back(argument.data());
    argv.push_back(nullptr);
    pid_t child{};
    // SQLite and macOS logging are not safe to initialize in a fork-only child.
    // A fresh executable also drops our CLOEXEC descriptors while the parent
    // keeps holding the locks under test.
    const auto spawned =
        ::posix_spawn(&child, test_executable.c_str(), nullptr, nullptr, argv.data(), environ);
    assert(spawned == 0);
    int status = 0;
    pid_t waited{};
    do {
        waited = ::waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    assert(waited == child);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        std::fprintf(stderr,
                     "%s: child status=%d (exit=%d, signal=%d)\n",
                     label,
                     status,
                     WIFEXITED(status) ? WEXITSTATUS(status) : -1,
                     WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

int child_mode(int argc, char** argv) {
    if (argc == 4 && std::string(argv[1]) == "--expect-lock-failure") {
        try {
            qcae::SqliteWorkspaceStore second(argv[2]);
            if (*argv[3])
                second.acquire_project(argv[3]);
        } catch (const qcae::StorageError& error) {
            if (std::string(error.what()).find("already in use") != std::string::npos)
                return 0;
            std::fprintf(stderr, "Unexpected lock-probe failure: %s\n", error.what());
        }
        return 1;
    }
    const bool workspace = argc == 4 && std::string(argv[1]) == "--crash-workspace";
    const bool project = argc == 5 && std::string(argv[1]) == "--crash-project";
    if (!workspace && !project)
        return 2;
    const std::string milestone = argv[project ? 4 : 3];
    qcae::StoreOptions options;
    options.fault = [milestone](const std::string& point) {
        if (point == milestone)
            _exit(0);
    };
    qcae::SqliteWorkspaceStore store(argv[2], options);
    if (workspace)
        store.commit(1, "new");
    else {
        const auto path = store.acquire_project(argv[3]);
        store.publish_project(path, {"new-token", "new"});
    }
    return 1; // A crash-window probe must terminate at its exact fault milestone.
}

void set_pragma(sqlite3* db, const char* statement) {
    assert(sqlite3_exec(db, statement, nullptr, nullptr, nullptr) == SQLITE_OK);
}

void test_workspace() {
    Sandbox sandbox;
    const auto path = (sandbox.path / "working.sqlite").string();
    const std::string binary("a\0b", 3);
    {
        qcae::SqliteWorkspaceStore store(path);
        assert(!store.load());
        assert(store.commit(0, binary) == 1);
        auto loaded = store.load();
        assert(loaded && loaded->generation == 1 && loaded->payload == binary);
        assert(throws([&] { store.commit(0, "stale"); }));
        assert(store.load()->payload == binary);
        child_must_succeed("workspace path", {"--expect-lock-failure", path, ""});
        const auto alias = sandbox.path / "working-alias.sqlite";
        fs::create_hard_link(path, alias);
        child_must_succeed("workspace hardlink", {"--expect-lock-failure", alias.string(), ""});
        assert(throws([&] { store.acquire_project(alias.string()); }));
        qcae::StoreOptions quota;
        quota.max_payload_bytes = 2;
        assert(throws([&] { qcae::SqliteWorkspaceStore other(path, quota); }));
    }
    {
        qcae::SqliteWorkspaceStore reopened(path);
        assert(reopened.load()->payload == binary);
        assert(reopened.commit(1, "next") == 2);
    }
    qcae::StoreOptions quota;
    quota.max_payload_bytes = 2;
    qcae::SqliteWorkspaceStore limited(path, quota);
    assert(throws([&] { limited.load(); }));
    assert(throws([&] { limited.commit(2, "huge"); }));
}

void test_unknown_and_corrupt() {
    Sandbox sandbox;
    const auto arbitrary = (sandbox.path / "arbitrary.sqlite").string();
    sqlite3* db = nullptr;
    assert(sqlite3_open(arbitrary.c_str(), &db) == SQLITE_OK);
    set_pragma(db, "CREATE TABLE unrelated(value TEXT)");
    sqlite3_close(db);
    const auto before = fs::file_size(arbitrary);
    assert(throws([&] { qcae::SqliteWorkspaceStore bad(arbitrary); }));
    assert(fs::file_size(arbitrary) == before);
    const auto corrupt = (sandbox.path / "corrupt.sqlite").string();
    {
        FILE* file = std::fopen(corrupt.c_str(), "wb");
        assert(file);
        assert(std::fwrite("not sqlite", 1, 10, file) == 10);
        std::fclose(file);
    }
    assert(throws([&] { qcae::SqliteWorkspaceStore bad(corrupt); }));
    const auto future = (sandbox.path / "future.sqlite").string();
    {
        qcae::SqliteWorkspaceStore store(future);
        assert(store.commit(0, "future") == 1);
    }
    assert(sqlite3_open(future.c_str(), &db) == SQLITE_OK);
    set_pragma(db, "PRAGMA user_version=99");
    sqlite3_close(db);
    assert(throws([&] { qcae::SqliteWorkspaceStore bad(future); }));
}

void test_commit_faults() {
    Sandbox sandbox;
    const auto path = (sandbox.path / "working.sqlite").string();
    {
        qcae::StoreOptions options;
        options.fault = [](const std::string& point) {
            if (point == "before_db_commit")
                throw std::runtime_error("before");
        };
        qcae::SqliteWorkspaceStore store(path, options);
        assert(throws([&] { store.commit(0, "uncommitted"); }));
        assert(!store.load());
    }
    {
        qcae::StoreOptions options;
        options.fault = [](const std::string& point) {
            if (point == "after_db_commit")
                throw std::runtime_error("after");
        };
        qcae::SqliteWorkspaceStore store(path, options);
        assert(uncertain([&] { store.commit(0, "committed"); }));
        assert(store.load()->payload == "committed");
    }
    {
        qcae::SqliteWorkspaceStore store(path);
        assert(store.load()->generation == 1);
    }
}

void test_project_publish() {
    Sandbox sandbox;
    const auto working = (sandbox.path / "working.sqlite").string();
    const auto project = (sandbox.path / "project.qcae").string();
    {
        qcae::SqliteWorkspaceStore store(working);
        const auto canonical = store.acquire_project(project);
        assert(canonical == (fs::canonical(sandbox.path) / "project.qcae").string());
        try {
            (void)store.read_project(canonical);
            assert(false);
        } catch (const qcae::StorageError& error) {
            assert(std::string(error.what()) == "project_not_found");
        }
        store.publish_project(canonical, {"token-1", std::string("x\0y", 3)});
        assert(store.read_project(canonical).save_token == "token-1");
        assert(store.read_project(canonical).payload == std::string("x\0y", 3));
        const auto alias = sandbox.path / "project-alias.qcae";
        fs::create_symlink(project, alias);
        assert(store.acquire_project(alias.string()) == canonical);
        const auto hardlink = sandbox.path / "project-hardlink.qcae";
        fs::create_hard_link(project, hardlink);
        child_must_succeed(
            "project hardlink",
            {"--expect-lock-failure", (sandbox.path / "other.sqlite").string(), hardlink.string()});
        child_must_succeed(
            "project symlink",
            {"--expect-lock-failure", (sandbox.path / "other.sqlite").string(), alias.string()});
        store.publish_project(canonical, {"token-2", "new"});
        assert(store.read_project(canonical).payload == "new");
        child_must_succeed("project hardlink after save",
                           {"--expect-lock-failure",
                            (sandbox.path / "after-save.sqlite").string(),
                            hardlink.string()});
    }
    {
        qcae::SqliteWorkspaceStore store(working);
        const auto canonical = store.acquire_project(project);
        assert(store.read_project(canonical).save_token == "token-2");
    }
}

void test_project_rejects_unrelated_target() {
    Sandbox sandbox;
    const auto working = (sandbox.path / "working.sqlite").string();
    const auto unrelated = (sandbox.path / "other.qcae").string();
    sqlite3* db = nullptr;
    assert(sqlite3_open(unrelated.c_str(), &db) == SQLITE_OK);
    set_pragma(db, "CREATE TABLE unrelated(value TEXT)");
    sqlite3_close(db);
    const auto before = fs::file_size(unrelated);
    qcae::SqliteWorkspaceStore store(working);
    const auto path = store.acquire_project(unrelated);
    assert(throws([&] { store.read_project(path); }));
    assert(throws([&] { store.publish_project(path, {"token", "new"}); }));
    assert(fs::file_size(unrelated) == before);
    const auto symlink = sandbox.path / "redirect.qcae";
    fs::create_symlink(unrelated, symlink);
    assert(store.acquire_project(symlink.string()) == path);
    assert(throws([&] { store.publish_project(path, {"token", "new"}); }));
    qcae::StoreOptions options;
    options.max_payload_bytes = 2;
    const auto small_working = (sandbox.path / "small.sqlite").string();
    qcae::SqliteWorkspaceStore limited(small_working, options);
    const auto fresh = limited.acquire_project((sandbox.path / "fresh.qcae").string());
    assert(throws([&] { limited.publish_project(fresh, {"token", "oversized"}); }));
    assert(!fs::exists(fresh));
}

void test_lock_symlink_rejected() {
    Sandbox sandbox;
    const auto victim = sandbox.path / "victim";
    {
        FILE* file = std::fopen(victim.c_str(), "wb");
        assert(file);
        assert(std::fwrite("safe", 1, 4, file) == 4);
        std::fclose(file);
    }
    const auto working = sandbox.path / "working.sqlite";
    fs::create_symlink(victim, working.string() + ".lock");
    assert(throws([&] { qcae::SqliteWorkspaceStore store(working.string()); }));
    assert(fs::file_size(victim) == 4);
    assert(!fs::exists(working));
}

void test_project_faults() {
    Sandbox sandbox;
    const auto working = (sandbox.path / "working.sqlite").string();
    const auto project = (sandbox.path / "project.qcae").string();
    {
        qcae::SqliteWorkspaceStore store(working);
        const auto path = store.acquire_project(project);
        store.publish_project(path, {"old", "old data"});
    }
    {
        qcae::StoreOptions options;
        options.fault = [](const std::string& point) {
            if (point == "before_project_publish")
                throw std::runtime_error("before");
        };
        qcae::SqliteWorkspaceStore store(working, options);
        auto path = store.acquire_project(project);
        assert(throws([&] { store.publish_project(path, {"new", "new data"}); }));
        assert(store.read_project(path).payload == "old data");
    }
    {
        qcae::StoreOptions options;
        options.fault = [](const std::string& point) {
            if (point == "after_project_publish")
                throw std::runtime_error("after");
        };
        qcae::SqliteWorkspaceStore store(working, options);
        auto path = store.acquire_project(project);
        assert(uncertain([&] { store.publish_project(path, {"new", "new data"}); }));
        assert(store.read_project(path).payload == "new data");
    }
}

void test_crash_windows() {
    Sandbox sandbox;
    const auto working = (sandbox.path / "working.sqlite").string();
    const auto project = (sandbox.path / "project.qcae").string();
    {
        qcae::SqliteWorkspaceStore store(working);
        assert(store.commit(0, "old") == 1);
        const auto path = store.acquire_project(project);
        store.publish_project(path, {"old-token", "old"});
    }
    for (const auto* milestone : {"before_db_commit", "after_db_commit"}) {
        child_must_succeed(milestone, {"--crash-workspace", working, milestone});
        qcae::SqliteWorkspaceStore recovered(working);
        assert(recovered.load()->payload ==
               (milestone == std::string("before_db_commit") ? "old" : "new"));
    }
    for (const auto* milestone : {"before_project_publish", "after_project_publish"}) {
        child_must_succeed(milestone, {"--crash-project", working, project, milestone});
        qcae::SqliteWorkspaceStore recovered(working);
        const auto path = recovered.acquire_project(project);
        assert(recovered.read_project(path).payload ==
               (milestone == std::string("before_project_publish") ? "old" : "new"));
    }
}
} // namespace

int main(int argc, char** argv) {
    test_executable = fs::absolute(argv[0]).string();
    if (argc > 1) {
        try {
            return child_mode(argc, argv);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "Child storage probe failed: %s\n", error.what());
            return 2;
        }
    }
    test_workspace();
    test_unknown_and_corrupt();
    test_commit_faults();
    test_project_publish();
    test_project_rejects_unrelated_target();
    test_lock_symlink_rejected();
    test_project_faults();
    test_crash_windows();
}
