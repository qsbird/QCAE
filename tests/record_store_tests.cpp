#include "qcae/sqlite_store.hpp"
#ifdef QCAE_C3_SQLITE_INSTRUMENTED
#include "c3_sqlite_copy_bridge.h"
#include "qcae/operation_ledger.hpp"
#endif

#include <sqlite3.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <array>
#include <cassert>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace {
using namespace qcae;
namespace fs = std::filesystem;

struct Sandbox {
    fs::path path;
    Sandbox() {
        std::string pattern = (fs::temp_directory_path() / "qcae-record-store-XXXXXX").string();
        const char* directory = ::mkdtemp(pattern.data());
        assert(directory);
        path = directory;
    }
    ~Sandbox() {
        fs::remove_all(path);
    }
    std::string database(const std::string& name = "working.sqlite") const {
        return (path / name).string();
    }
};

SharedStoreBytes bytes(std::string value) {
    return std::make_shared<const std::string>(std::move(value));
}

RowMutation row(StoreSpace space, std::string identity, std::string value) {
    return {{space, std::move(identity)}, bytes(std::move(value))};
}

bool fails(const std::function<void()>& action, bool expected_uncertain = false) {
    try {
        action();
    } catch (const StorageError& error) {
        return error.uncertain() == expected_uncertain;
    }
    return false;
}

std::string read_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

std::string scalar_text(const std::string& path, const char* sql) {
    sqlite3* db = nullptr;
    assert(sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
    sqlite3_stmt* statement = nullptr;
    assert(sqlite3_prepare_v2(db, sql, -1, &statement, nullptr) == SQLITE_OK);
    assert(sqlite3_step(statement) == SQLITE_ROW);
    const auto* text = sqlite3_column_text(statement, 0);
    assert(text);
    std::string result(reinterpret_cast<const char*>(text));
    sqlite3_finalize(statement);
    sqlite3_close(db);
    return result;
}

void test_rows_and_generation() {
    Sandbox sandbox;
    const auto path = sandbox.database();
    SharedStoreBytes retained;
    {
        SqliteWorkspaceStore store(path);
        assert(!store.load());
        const auto empty = store.load_rows();
        assert(empty.generation == 0 && empty.rows.empty() && !empty.legacy);
        assert(scalar_text(path, "PRAGMA user_version") == "1");
        StoreBatch initial{0,
                           "initial",
                           {row(StoreSpace::document_record, "node/1", std::string("a\0b", 3)),
                            row(StoreSpace::history_entry, "1", "history"),
                            row(StoreSpace::operation_fact, "edit/1", "receipt"),
                            row(StoreSpace::host_operation_fact, "create", "host"),
                            row(StoreSpace::document_metadata, "document", "metadata"),
                            row(StoreSpace::task_record, "task/1", ""),
                            row(StoreSpace::artifact_record, "artifact/1", "artifact")}};
        const auto receipt = store.commit_rows(initial);
        assert(receipt.generation == 1 && receipt.rows_written == 7 && receipt.payload_bytes == 37);
        assert(scalar_text(path, "PRAGMA user_version") == "2");
        assert(scalar_text(path, "SELECT magic FROM record_state") == "QCAE-ROWS");
        const auto loaded = store.load_rows();
        assert(loaded.generation == 1 && loaded.rows.size() == 7 && !loaded.legacy);
        retained = loaded.rows[0].value;
        assert(*retained == std::string("a\0b", 3));
        assert(loaded.rows[5].value && loaded.rows[5].value->empty());
        assert(fails([&] { store.load(); }));
        assert(fails([&] { store.commit(0, "must not write stale legacy data"); }));
        assert(fails([&] { store.commit_rows(initial); }));
        StoreBatch changed{1,
                           "change",
                           {row(StoreSpace::document_record, "node/1", "replacement"),
                            {{StoreSpace::history_entry, "1"}, nullptr},
                            {{StoreSpace::history_entry, "absent"}, nullptr},
                            row(StoreSpace::operation_fact, "edit/2", "new fact")}};
        const auto updated = store.commit_rows(changed);
        assert(updated.generation == 2 && updated.rows_written == 3 && updated.payload_bytes == 19);
        assert(*retained == std::string("a\0b", 3));
        assert(store.load_rows().rows.size() == 7);
        assert(scalar_text(path, "SELECT transaction_id FROM record_state") == "change");
    }
    SqliteWorkspaceStore reopened(path);
    assert(reopened.load_rows().generation == 2);
    assert(*reopened.load_rows().rows[0].value == "replacement");
    assert(fails([&] { reopened.load(); }));
}

void test_large_history_is_not_a_search_key() {
    Sandbox sandbox;
    const auto path = sandbox.database();
    const std::string history(2 * 1024 * 1024, 'h');
    SqliteWorkspaceStore store(path);
    store.commit_rows({0,
                       "seed",
                       {row(StoreSpace::history_entry, "old-seed", history),
                        row(StoreSpace::document_record, "node/1", "before")}});
    // Warm the exact prepared statements and cursor paths before observation.
    store.commit_rows({1, "warm", {row(StoreSpace::document_record, "node/1", "warm")}});
#ifdef QCAE_C3_SQLITE_INSTRUMENTED
    auto operation = std::make_shared<qcae::ledger::OperationLedger>(
        qcae::ledger::Identity{"large-history-key", {}, {}, 0});
    qcae::ledger::activate(operation);
    assert(qcae_c3_sqlite_observer_begin());
#endif
    const auto receipt =
        store.commit_rows({2,
                           "local",
                           {row(StoreSpace::document_record, "node/1", "after"),
                            row(StoreSpace::operation_fact, "new-fact", "receipt")}});
    assert(receipt.rows_written == 2 && receipt.payload_bytes == 12);
#ifdef QCAE_C3_SQLITE_INSTRUMENTED
    qcae::ledger::activate({});
    const auto copied =
        operation->snapshot()
            .values[static_cast<std::size_t>(qcae::ledger::Stage::sqlite)]
                   [static_cast<std::size_t>(qcae::ledger::Metric::driver_internal_copy_bytes)];
    assert(copied && *copied < history.size() / 2);
    std::cout << "Observed local-store driver copies with retained 2MiB history: " << *copied
              << " bytes\n";
#endif
    const auto loaded = store.load_rows();
    assert(loaded.rows.size() == 3 && loaded.generation == 3);
    for (const auto& item : loaded.rows)
        if (item.key.space == StoreSpace::history_entry)
            assert(item.key.identity == "old-seed" && *item.value == history);
}

void test_without_rowid_store_remains_supported() {
    Sandbox sandbox;
    const auto path = sandbox.database();
    {
        SqliteWorkspaceStore store(path);
        store.commit_rows({0,
                           "old-physical-layout",
                           {row(StoreSpace::document_record, "node/1", "old bytes"),
                            row(StoreSpace::history_entry, "1", "unchanged history")}});
    }
    // This unit fixture exercises both supported physical table layouts; it
    // is not represented as a frozen legacy-encoder acceptance fixture.
    sqlite3* database{};
    assert(sqlite3_open(path.c_str(), &database) == SQLITE_OK);
    assert(sqlite3_exec(database,
                        "BEGIN;ALTER TABLE store_rows RENAME TO original_rows;"
                        "CREATE TABLE store_rows(space INTEGER NOT NULL CHECK(space BETWEEN 1 AND "
                        "7),identity TEXT NOT NULL CHECK(length(identity)>0),value BLOB NOT NULL,"
                        "PRIMARY KEY(space,identity)) WITHOUT ROWID;"
                        "INSERT INTO store_rows SELECT space,identity,value FROM original_rows;"
                        "DROP TABLE original_rows;COMMIT",
                        nullptr,
                        nullptr,
                        nullptr) == SQLITE_OK);
    sqlite3_close(database);
    SqliteWorkspaceStore reopened(path);
    const auto loaded = reopened.load_rows();
    assert(loaded.generation == 1 && loaded.rows.size() == 2);
    assert(*loaded.rows[0].value == "old bytes" && *loaded.rows[1].value == "unchanged history");
    reopened.commit_rows(
        {1, "old-layout-edit", {row(StoreSpace::document_record, "node/1", "new bytes")}});
    const auto updated = reopened.load_rows();
    assert(updated.generation == 2 && updated.rows.size() == 2);
    assert(*updated.rows[0].value == "new bytes" && *updated.rows[1].value == "unchanged history");
}

void test_legacy_detection_is_read_only() {
    Sandbox sandbox;
    const auto path = sandbox.database();
    const std::string payload("QCAE-WORKSPACE\0legacy-exact-bytes", 33);
    {
        SqliteWorkspaceStore legacy(path);
        assert(legacy.commit(0, payload) == 1);
    }
    const auto original = read_file(path);
    {
        SqliteWorkspaceStore legacy(path);
        const auto loaded = legacy.load_rows();
        assert(loaded.generation == 1 && loaded.rows.empty() && loaded.legacy);
        assert(loaded.legacy->payload == payload && loaded.legacy->generation == 1);
        assert(fails([&] {
            legacy.commit_rows(
                {1, "implicit-migration", {row(StoreSpace::document_record, "1", "x")}});
        }));
        assert(legacy.load()->payload == payload);
        assert(scalar_text(path, "PRAGMA user_version") == "1");
        assert(scalar_text(path,
                           "SELECT count(*) FROM sqlite_master WHERE name IN "
                           "('record_state','store_rows')") == "0");
    }
    assert(read_file(path) == original);
}

void test_invalid_batches_and_quotas() {
    Sandbox sandbox;
    StoreOptions options;
    options.max_rows = 2;
    options.max_batch_rows = 3;
    options.max_payload_bytes = 8;
    options.max_key_bytes = 8;
    SqliteWorkspaceStore store(sandbox.database(), options);
    auto invalid = [&](StoreBatch batch) {
        assert(fails([&] { store.commit_rows(batch); }));
        assert(store.load_rows().generation == 0);
        assert(scalar_text(sandbox.database(), "PRAGMA user_version") == "1");
    };
    invalid({0,
             "dup",
             {row(StoreSpace::document_record, "same", "a"),
              row(StoreSpace::document_record, "same", "b")}});
    invalid({0, "badkey", {row(StoreSpace::document_record, "", "a")}});
    invalid({0, "badspace", {row(static_cast<StoreSpace>(0), "key", "a")}});
    invalid({0, "badspace", {row(static_cast<StoreSpace>(8), "key", "a")}});
    invalid({0, "nulkey", {row(StoreSpace::document_record, std::string("a\0b", 3), "a")}});
    invalid({0, "longkey", {row(StoreSpace::document_record, "123456789", "a")}});
    invalid({0, "", {row(StoreSpace::document_record, "key", "a")}});
    invalid({1, "stale", {row(StoreSpace::document_record, "key", "a")}});
    invalid({0, "bytes", {row(StoreSpace::document_record, "key", "123456789")}});
    invalid({0,
             "rows",
             {row(StoreSpace::document_record, "a", "1"),
              row(StoreSpace::document_record, "b", "2"),
              row(StoreSpace::document_record, "c", "3")}});
    invalid({0,
             "batch",
             {{{StoreSpace::document_record, "a"}, nullptr},
              {{StoreSpace::document_record, "b"}, nullptr},
              {{StoreSpace::document_record, "c"}, nullptr},
              {{StoreSpace::document_record, "d"}, nullptr}}});
    assert(store
               .commit_rows({0,
                             "valid",
                             {row(StoreSpace::document_record, "a", "1234"),
                              row(StoreSpace::document_record, "b", "5678")}})
               .generation == 1);
    assert(fails([&] {
        store.commit_rows({1, "overflow", {row(StoreSpace::document_record, "a", "12345")}});
    }));
    assert(store.load_rows().generation == 1);
    assert(*store.load_rows().rows[0].value == "1234");
    const auto replaced = store.commit_rows({1,
                                             "replace",
                                             {row(StoreSpace::document_record, "c", "ABCD"),
                                              {{StoreSpace::document_record, "a"}, nullptr}}});
    assert(replaced.generation == 2 && replaced.rows_written == 2 && replaced.payload_bytes == 4);
    const auto namespaces = store.commit_rows({2,
                                               "spaces",
                                               {{{StoreSpace::document_record, "b"}, nullptr},
                                                row(StoreSpace::history_entry, "c", "EFGH")}});
    assert(namespaces.generation == 3 && store.load_rows().rows.size() == 2);
}

void test_failure_windows() {
    Sandbox sandbox;
    const auto path = sandbox.database();
    std::string fault;
    StoreOptions options;
    options.fault = [&](const std::string& point) {
        if (point == fault) {
            fault.clear();
            throw std::runtime_error("injected " + point);
        }
    };
    {
        SqliteWorkspaceStore store(path, options);
        const StoreBatch initial{0, "initial", {row(StoreSpace::document_record, "a", "before")}};
        fault = "before_db_commit";
        assert(fails([&] { store.commit_rows(initial); }));
        assert(store.load_rows().generation == 0 && !store.load());
        assert(scalar_text(path, "PRAGMA user_version") == "1");
        assert(store.commit_rows(initial).generation == 1);
        const StoreBatch changed{1,
                                 "atomic",
                                 {row(StoreSpace::document_record, "a", "after"),
                                  row(StoreSpace::history_entry, "1", "delta"),
                                  row(StoreSpace::document_metadata, "revision", "2"),
                                  row(StoreSpace::operation_fact, "atomic", "receipt")}};
        fault = "before_db_commit";
        assert(fails([&] { store.commit_rows(changed); }));
        assert(store.load_rows().generation == 1 && store.load_rows().rows.size() == 1);
        assert(*store.load_rows().rows[0].value == "before");
        fault = "after_db_commit";
        assert(fails([&] { store.commit_rows(changed); }, true));
    }
    SqliteWorkspaceStore recovered(path);
    const auto loaded = recovered.load_rows();
    assert(loaded.generation == 2 && loaded.rows.size() == 4);
    assert(*loaded.rows[0].value == "after");
    assert(*loaded.rows[1].value == "delta");
    assert(*loaded.rows[2].value == "receipt");
    assert(*loaded.rows[3].value == "2");
}

void test_one_row_update_has_bounded_payload() {
    for (std::uint64_t count : {1000, 10000}) {
        Sandbox sandbox;
        SqliteWorkspaceStore store(sandbox.database());
        StoreBatch initial{0, "seed", {}};
        const auto shared = bytes(std::string(128, 'x'));
        for (std::uint64_t index = 0; index < count; ++index)
            initial.mutations.push_back(
                {{StoreSpace::document_record, std::to_string(index)}, shared});
        const auto seeded = store.commit_rows(initial);
        assert(seeded.rows_written == count && seeded.payload_bytes == count * 128);
        const auto updated = store.commit_rows(
            {1, "one-row", {row(StoreSpace::document_record, "500", std::string(128, 'y'))}});
        assert(updated.rows_written == 1 && updated.payload_bytes == 128 &&
               updated.generation == 2);
        assert(store.load_rows().rows.size() == count);
    }
}
void test_unsupported_record_version() {
    Sandbox sandbox;
    const auto path = sandbox.database();
    {
        SqliteWorkspaceStore store(path);
        store.commit_rows({0, "initial", {row(StoreSpace::document_record, "a", "value")}});
    }
    sqlite3* db = nullptr;
    assert(sqlite3_open(path.c_str(), &db) == SQLITE_OK);
    assert(
        sqlite3_exec(
            db, "UPDATE record_state SET record_version=4294967297", nullptr, nullptr, nullptr) ==
        SQLITE_OK);
    sqlite3_close(db);
    assert(fails([&] { SqliteWorkspaceStore rejected(path); }));
}

int child_commit(const std::string& path, const std::string& milestone) {
    try {
        if (milestone == "recent_wal") {
            SqliteWorkspaceStore writer(path);
            writer.commit(1, "recent WAL payload");
            ::_exit(23); // Preserve the committed WAL without a SQLite close/checkpoint.
        }
        if (milestone != "before_db_commit" && milestone != "after_db_commit")
            throw std::invalid_argument("Unknown child commit milestone");
        StoreOptions options;
        options.fault = [&](const std::string& point) {
            if (point == milestone)
                ::_exit(17);
        };
        SqliteWorkspaceStore store(path, options);
        store.commit_rows({1,
                           "crash",
                           {row(StoreSpace::document_record, "a", "after"),
                            row(StoreSpace::history_entry, "1", "delta"),
                            row(StoreSpace::operation_fact, "crash", "receipt")}});
        ::_exit(18); // Reaching normal completion means the requested crash window was missed.
    } catch (const std::exception& error) {
        std::cerr << "Store child " << milestone << ": " << error.what() << '\n';
        return 19;
    }
}

void run_child_commit(const std::string& executable,
                      const std::string& path,
                      const std::string& milestone,
                      int expected_exit) {
    // Start a fresh process image: system SQLite/logging state is unsafe to reuse in a
    // forked child before exec. The child still terminates abruptly at the exact fault hook.
    std::array<std::string, 4> values{executable, "--store-child", path, milestone};
    std::array<char*, 5> arguments{
        values[0].data(), values[1].data(), values[2].data(), values[3].data(), nullptr};
    pid_t child{};
    const auto spawned =
        ::posix_spawnp(&child, executable.c_str(), nullptr, nullptr, arguments.data(), environ);
    if (spawned != 0)
        throw std::runtime_error("Cannot spawn store child: " +
                                 std::string(std::strerror(spawned)));
    int status{};
    pid_t waited{};
    do {
        waited = ::waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited != child)
        throw std::runtime_error("Cannot wait for store child: " +
                                 std::string(std::strerror(errno)));
    if (!WIFEXITED(status) || WEXITSTATUS(status) != expected_exit) {
        std::cerr << "Store child " << child << " milestone=" << milestone
                  << " expected_exit=" << expected_exit << " status=" << status;
        if (WIFEXITED(status))
            std::cerr << " exit=" << WEXITSTATUS(status);
        if (WIFSIGNALED(status))
            std::cerr << " signal=" << WTERMSIG(status);
        std::cerr << '\n';
    }
    assert(WIFEXITED(status) && WEXITSTATUS(status) == expected_exit);
}

void test_process_termination_windows(const std::string& executable) {
    for (const std::string milestone : {"before_db_commit", "after_db_commit"}) {
        Sandbox sandbox;
        const auto path = sandbox.database();
        {
            SqliteWorkspaceStore initial(path);
            initial.commit_rows({0, "initial", {row(StoreSpace::document_record, "a", "before")}});
        }
        run_child_commit(executable, path, milestone, 17);
        SqliteWorkspaceStore recovered(path);
        const auto loaded = recovered.load_rows();
        const bool committed = milestone == "after_db_commit";
        assert(loaded.generation == (committed ? 2 : 1));
        assert(loaded.rows.size() == (committed ? 3 : 1));
        assert(*loaded.rows[0].value == (committed ? "after" : "before"));
    }
}

void test_delete_mode_detection_preserves_source() {
    Sandbox sandbox;
    const auto path = sandbox.database();
    {
        SqliteWorkspaceStore initial(path);
        initial.commit(0, "legacy DELETE source");
    }
    sqlite3* db = nullptr;
    assert(sqlite3_open(path.c_str(), &db) == SQLITE_OK);
    assert(sqlite3_exec(db, "PRAGMA journal_mode=DELETE", nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(db);
    const auto original = read_file(path);
    {
        SqliteWorkspaceStore probe(path);
        const auto loaded = probe.load_rows();
        assert(loaded.legacy && loaded.legacy->payload == "legacy DELETE source");
        assert(fails([&] {
            probe.commit_rows({1, "no-migrate", {row(StoreSpace::document_record, "a", "x")}});
        }));
        assert(scalar_text(path, "PRAGMA journal_mode") == "delete");
    }
    assert(read_file(path) == original);
    const auto source = read_legacy_workspace_readonly(path);
    assert(source.generation == 1 && source.payload == "legacy DELETE source");
    assert(read_file(path) == original);
    {
        SqliteWorkspaceStore writer(path);
        writer.commit(1, "authorized legacy edit");
        assert(scalar_text(path, "PRAGMA journal_mode") == "wal");
    }
}

void test_readonly_source_with_recent_wal_commit(const std::string& executable) {
    Sandbox sandbox;
    const auto path = sandbox.database();
    {
        SqliteWorkspaceStore initial(path);
        initial.commit(0, "prior payload");
    }
    run_child_commit(executable, path, "recent_wal", 23);
    assert(fs::exists(path + "-wal") && fs::file_size(path + "-wal") > 0);
    const auto original = read_file(path);
    const auto wal = read_file(path + "-wal");
    const auto shm = read_file(path + "-shm");
    const auto source = read_legacy_workspace_readonly(path);
    assert(source.generation == 2 && source.payload == "recent WAL payload");
    assert(read_file(path) == original);
    assert(read_file(path + "-wal") == wal);
    assert(read_file(path + "-shm") == shm);

    // SQLite must recover the copied WAL even when its source has no SHM.
    assert(fs::remove(path + "-shm"));
    const auto no_shm = read_legacy_workspace_readonly(path);
    assert(no_shm.generation == 2 && no_shm.payload == "recent WAL payload");
    assert(!fs::exists(path + "-shm"));
    assert(read_file(path) == original && read_file(path + "-wal") == wal);
}

void test_readonly_permissions_without_sidecars() {
    Sandbox sandbox;
    const auto path = sandbox.database();
    {
        SqliteWorkspaceStore initial(path);
        initial.commit(0, "protected source");
    }
    // Some SQLite builds retain an empty WAL and SHM after the final close.
    // Establish this fixture's no-sidecar condition explicitly, after verifying
    // there are no uncheckpointed frames to discard. Only our temporary source
    // is changed here; the read-only migration call below must preserve it.
    if (fs::exists(path + "-wal")) {
        assert(fs::file_size(path + "-wal") == 0);
        assert(fs::remove(path + "-wal"));
    }
    if (fs::exists(path + "-shm"))
        assert(fs::remove(path + "-shm"));
    assert(!fs::exists(path + "-wal") && !fs::exists(path + "-shm"));
    fs::permissions(path, fs::perms::owner_read);
    const auto permissions = fs::status(path).permissions();
    const auto original = read_file(path);
    const auto source = read_legacy_workspace_readonly(path);
    assert(source.generation == 1 && source.payload == "protected source");
    assert(read_file(path) == original && fs::status(path).permissions() == permissions);
    assert(!fs::exists(path + "-wal") && !fs::exists(path + "-shm"));
}

void test_frozen_sources_unchanged(const fs::path& directory) {
    for (const char* name : {"saved-empty.qcae",
                             "saved-beam.qcae",
                             "saved-organization.qcae",
                             "all-supported-entities.qcae",
                             "unsaved-workspace.sqlite",
                             "redo-workspace.sqlite"}) {
        const auto path = (directory / name).string();
        const auto original = read_file(path);
        const bool had_lock = fs::exists(path + ".lock");
        try {
            const auto source = read_legacy_workspace_readonly(path);
            assert(fs::path(path).extension() == ".sqlite");
            assert(source.generation > 0 && !source.payload.empty());
        } catch (const StorageError& error) {
            assert(std::string(error.what()).starts_with("legacy_readonly_unavailable:"));
        }
        assert(read_file(path) == original);
        assert(fs::exists(path + ".lock") == had_lock);
    }
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 4 && std::string(argv[1]) == "--store-child")
        return child_commit(argv[2], argv[3]);
    test_rows_and_generation();
    test_large_history_is_not_a_search_key();
    test_without_rowid_store_remains_supported();
    test_legacy_detection_is_read_only();
    test_invalid_batches_and_quotas();
    test_failure_windows();
    test_one_row_update_has_bounded_payload();
    test_unsupported_record_version();
    test_process_termination_windows(argv[0]);
    test_delete_mode_detection_preserves_source();
    test_readonly_source_with_recent_wal_commit(argv[0]);
    test_readonly_permissions_without_sidecars();
    if (argc == 2)
        test_frozen_sources_unchanged(argv[1]);
}
