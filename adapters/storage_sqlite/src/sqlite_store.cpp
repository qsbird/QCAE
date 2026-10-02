#include "qcae/sqlite_store.hpp"
#include "ledger_vfs.hpp"

#include <sqlite3.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#else
#error "SqliteWorkspaceStore currently requires POSIX file locks and atomic rename"
#endif

namespace qcae {
namespace {
namespace fs = std::filesystem;
constexpr int kWorkspaceId = 0x51434157; // QCAW
constexpr int kProjectId = 0x51434150;   // QCAP
constexpr int kSchemaVersion = 1;
constexpr int kRowsSchemaVersion = 2;
constexpr int kRecordVersion = 1;
constexpr const char* kRowsMagic = "QCAE-ROWS";

[[noreturn]] void fail(const std::string& message) {
    throw StorageError(message);
}

std::string system_error(const std::string& action) {
    return action + ": " + std::strerror(errno);
}

struct File {
    int fd{-1};
    File() = default;
    explicit File(int value) : fd(value) {}
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    File(File&& other) noexcept : fd(std::exchange(other.fd, -1)) {}
    File& operator=(File&& other) noexcept {
        if (this != &other) {
            if (fd >= 0)
                ::close(fd);
            fd = std::exchange(other.fd, -1);
        }
        return *this;
    }
    ~File() {
        if (fd >= 0)
            ::close(fd);
    }
};

std::string canonical_path(const std::string& path) {
    if (path.empty())
        fail("empty storage path");
    std::error_code ec;
    fs::path input = fs::absolute(fs::path(path), ec);
    if (ec)
        fail("invalid storage path: " + ec.message());
    fs::path result = fs::weakly_canonical(input, ec);
    if (ec)
        fail("cannot canonicalize storage path: " + ec.message());
    if (!fs::is_directory(result.parent_path(), ec) || ec)
        fail("storage parent is not a directory");
    return result.string();
}

File lock_sidecar(const std::string& path) {
    const std::string lock = path + ".lock";
    File file(::open(lock.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600));
    if (file.fd < 0)
        fail(system_error("open storage lock"));
    struct stat info{};
    if (::fstat(file.fd, &info) != 0 || !S_ISREG(info.st_mode))
        fail("storage lock is not a regular file");
    if (::flock(file.fd, LOCK_EX | LOCK_NB) != 0)
        fail(system_error("storage is already in use"));
    return file;
}

struct InodeLock {
    File target;
    File guard;
};

InodeLock lock_inode_file(File target) {
    struct stat info{};
    if (::fstat(target.fd, &info) != 0 || !S_ISREG(info.st_mode))
        fail("storage target is not a regular file");
    const std::string directory = "/tmp/qcae-inode-locks-" + std::to_string(::getuid());
    if (::mkdir(directory.c_str(), 0700) != 0 && errno != EEXIST)
        fail(system_error("create inode lock directory"));
    struct stat directory_info{};
    if (::lstat(directory.c_str(), &directory_info) != 0 || !S_ISDIR(directory_info.st_mode) ||
        directory_info.st_uid != ::getuid() || (directory_info.st_mode & 0077) != 0)
        fail("unsafe inode lock directory");
    const std::string key =
        directory + "/" + std::to_string(info.st_dev) + "-" + std::to_string(info.st_ino) + ".lock";
    File guard(::open(key.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600));
    if (guard.fd < 0)
        fail(system_error("open inode lock"));
    struct stat lock_info{};
    if (::fstat(guard.fd, &lock_info) != 0 || !S_ISREG(lock_info.st_mode) ||
        lock_info.st_uid != ::getuid())
        fail("unsafe inode lock file");
    if (::flock(guard.fd, LOCK_EX | LOCK_NB) != 0)
        fail(system_error("storage inode is already in use"));
    return {std::move(target), std::move(guard)};
}

InodeLock lock_inode_if_present(const std::string& path) {
    File file(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (file.fd < 0) {
        if (errno == ENOENT)
            return {};
        fail(system_error("open storage inode"));
    }
    return lock_inode_file(std::move(file));
}

bool exists(const std::string& path) {
    struct stat info{};
    if (::lstat(path.c_str(), &info) == 0) {
        if (!S_ISREG(info.st_mode))
            fail("storage target is not a regular file");
        return true;
    }
    if (errno == ENOENT)
        return false;
    fail(system_error("stat storage target"));
}

void ensure_same_inode(const std::string& path, const InodeLock& inode) {
    struct stat current{};
    if (::lstat(path.c_str(), &current) != 0) {
        if (errno == ENOENT && inode.target.fd < 0)
            return;
        fail("storage target changed while leased");
    }
    if (!S_ISREG(current.st_mode) || inode.target.fd < 0)
        fail("storage target changed while leased");
    struct stat held{};
    if (::fstat(inode.target.fd, &held) != 0 || current.st_dev != held.st_dev ||
        current.st_ino != held.st_ino)
        fail("storage target changed while leased");
}

struct Db {
    sqlite3* db{};
    std::map<std::string, sqlite3_stmt*, std::less<>> statements;
    Db(const std::string& path, int flags) {
        const auto* observe = std::getenv("QCAE_LEDGER_VFS");
        const auto* vfs =
            observe && std::strcmp(observe, "1") == 0 ? sqlite_ledger::name() : nullptr;
        int rc = sqlite3_open_v2(
            path.c_str(), &db, flags | SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_NOFOLLOW, vfs);
        if (rc != SQLITE_OK) {
            std::string message = db ? sqlite3_errmsg(db) : "out of memory";
            if (db)
                sqlite3_close(db);
            db = nullptr;
            fail("open SQLite database: " + message);
        }
        sqlite3_busy_timeout(db, 1000);
    }
    Db(const Db&) = delete;
    Db& operator=(const Db&) = delete;
    ~Db() {
        for (const auto& [sql, statement] : statements)
            sqlite3_finalize(statement);
        if (db)
            sqlite3_close(db);
    }
};

// A read-only WAL database may still need writable shared-memory sidecars.
// Stage a quiescent legacy source while its inode lease is held so SQLite can
// reconstruct those sidecars without touching the protected source directory.
struct LegacyReadSnapshot {
    fs::path directory;
    std::string path;
    std::string source;
    struct stat main_before{}, wal_before{};
    bool had_wal{};
    static struct stat metadata(const std::string& path) {
        struct stat value{};
        if (::lstat(path.c_str(), &value) != 0 || !S_ISREG(value.st_mode))
            fail("legacy source changed while reading");
        return value;
    }
    static bool same_metadata(const struct stat& left, const struct stat& right) {
#if defined(__APPLE__)
        const auto modified_left = left.st_mtimespec, modified_right = right.st_mtimespec;
        const auto changed_left = left.st_ctimespec, changed_right = right.st_ctimespec;
#else
        const auto modified_left = left.st_mtim, modified_right = right.st_mtim;
        const auto changed_left = left.st_ctim, changed_right = right.st_ctim;
#endif
        return left.st_dev == right.st_dev && left.st_ino == right.st_ino &&
               left.st_size == right.st_size && modified_left.tv_sec == modified_right.tv_sec &&
               modified_left.tv_nsec == modified_right.tv_nsec &&
               changed_left.tv_sec == changed_right.tv_sec &&
               changed_left.tv_nsec == changed_right.tv_nsec;
    }
    static bool same_bytes(const std::string& source, const std::string& copy) {
        std::ifstream original(source, std::ios::binary), staged(copy, std::ios::binary);
        if (!original || !staged)
            return false;
        char left[65536], right[65536];
        do {
            original.read(left, sizeof(left));
            staged.read(right, sizeof(right));
            if (original.gcount() != staged.gcount() ||
                std::memcmp(left, right, static_cast<std::size_t>(original.gcount())) != 0)
                return false;
        } while (original && staged);
        return original.eof() && staged.eof();
    }
    void verify_source() const {
        if (!same_metadata(main_before, metadata(source)) || exists(source + "-wal") != had_wal ||
            (had_wal && !same_metadata(wal_before, metadata(source + "-wal"))) ||
            !same_bytes(source, path) || (had_wal && !same_bytes(source + "-wal", path + "-wal")) ||
            !same_metadata(main_before, metadata(source)) || exists(source + "-wal") != had_wal ||
            (had_wal && !same_metadata(wal_before, metadata(source + "-wal"))))
            fail("legacy source changed while reading");
    }
    explicit LegacyReadSnapshot(const std::string& source_path) : source(source_path) {
        main_before = metadata(source);
        had_wal = exists(source + "-wal");
        if (had_wal)
            wal_before = metadata(source + "-wal");
        std::string pattern = (fs::temp_directory_path() / "qcae-legacy-read-XXXXXX").string();
        const auto created = ::mkdtemp(pattern.data());
        if (!created)
            fail(system_error("create legacy read snapshot"));
        directory = fs::canonical(created);
        path = (directory / "source.sqlite").string();
        try {
            fs::copy_file(source, path);
            if (had_wal)
                fs::copy_file(source + "-wal", path + "-wal");
            verify_source();
            // A protected source may itself be mode 0444. Only its private
            // copy needs write permission for SQLite's WAL recovery.
            fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write);
            if (had_wal)
                fs::permissions(path + "-wal", fs::perms::owner_read | fs::perms::owner_write);
        } catch (const std::exception& error) {
            std::error_code unused;
            fs::remove_all(directory, unused);
            fail("stage legacy read snapshot: " + std::string(error.what()));
        }
    }
    ~LegacyReadSnapshot() {
        std::error_code unused;
        fs::remove_all(directory, unused);
    }
    LegacyReadSnapshot(const LegacyReadSnapshot&) = delete;
    LegacyReadSnapshot& operator=(const LegacyReadSnapshot&) = delete;
};

void exec(sqlite3* db, const char* sql) {
    char* message = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &message);
    if (rc != SQLITE_OK) {
        std::string detail = message ? message : sqlite3_errmsg(db);
        sqlite3_free(message);
        fail("SQLite: " + detail);
    }
}

struct Statement {
    sqlite3_stmt* stmt{};
    bool cached{};
    Statement(sqlite3* db, const char* sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
            fail("SQLite prepare: " + std::string(sqlite3_errmsg(db)));
    }
    Statement(Db& db, const char* sql) : cached(true) {
        const auto found = db.statements.find(sql);
        if (found != db.statements.end()) {
            stmt = found->second;
            return;
        }
        if (sqlite3_prepare_v3(db.db, sql, -1, SQLITE_PREPARE_PERSISTENT, &stmt, nullptr) !=
            SQLITE_OK)
            fail("SQLite prepare: " + std::string(sqlite3_errmsg(db.db)));
        try {
            db.statements.emplace(sql, stmt);
        } catch (...) {
            sqlite3_finalize(stmt);
            stmt = nullptr;
            throw;
        }
        ledger::add(ledger::Stage::sqlite,
                    ledger::Metric::metadata_copy_bytes,
                    sizeof(decltype(db.statements)::value_type) + std::strlen(sql));
    }
    ~Statement() {
        if (stmt) {
            ledger::add(ledger::Stage::sqlite,
                        ledger::Metric::sql_fullscan_steps,
                        static_cast<std::uint64_t>(
                            sqlite3_stmt_status(stmt, SQLITE_STMTSTATUS_FULLSCAN_STEP, cached)));
            ledger::add(ledger::Stage::sqlite,
                        ledger::Metric::sql_vm_steps,
                        static_cast<std::uint64_t>(
                            sqlite3_stmt_status(stmt, SQLITE_STMTSTATUS_VM_STEP, cached)));
            if (cached) {
                // reset releases cursors even after a failed step; clear_bindings
                // drops owned payloads and borrowed batch pointers before the next transaction.
                sqlite3_reset(stmt);
                sqlite3_clear_bindings(stmt);
            } else {
                sqlite3_finalize(stmt);
            }
        }
    }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
};

void exec(Db& db, const char* sql, bool uncertain = false) {
    Statement statement(db, sql);
    int result{};
    do {
        result = sqlite3_step(statement.stmt);
    } while (result == SQLITE_ROW);
    if (result != SQLITE_DONE)
        throw StorageError("SQLite: " + std::string(sqlite3_errmsg(db.db)), uncertain);
}

template <class Database> int scalar(Database&& db, const char* sql) {
    Statement statement(db, sql);
    if (sqlite3_step(statement.stmt) != SQLITE_ROW)
        fail("invalid SQLite metadata");
    return sqlite3_column_int(statement.stmt, 0);
}

void validate(sqlite3* db, int application_id, bool project) {
    if (scalar(db, "PRAGMA application_id") != application_id ||
        scalar(db, "PRAGMA user_version") != kSchemaVersion)
        fail("unsupported QCAE SQLite format");
    Statement check(db, "PRAGMA quick_check");
    if (sqlite3_step(check.stmt) != SQLITE_ROW || !sqlite3_column_text(check.stmt, 0) ||
        std::strcmp(reinterpret_cast<const char*>(sqlite3_column_text(check.stmt, 0)), "ok") != 0)
        fail("corrupt QCAE SQLite database");
    Statement schema(db,
                     project ? "SELECT save_token,payload FROM project WHERE id=1"
                             : "SELECT generation,payload FROM workspace WHERE id=1");
    int rc = sqlite3_step(schema.stmt);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE)
        fail("invalid QCAE SQLite schema");
    if (project && rc != SQLITE_ROW)
        fail("invalid project state");
}

struct RowState {
    std::uint64_t generation;
    std::uint64_t rows;
    std::uint64_t bytes;
};

template <class Database> RowState row_state(Database&& db) {
    Statement read(db,
                   "SELECT magic,record_version,generation,row_count,payload_bytes "
                   "FROM record_state WHERE id=1");
    if (sqlite3_step(read.stmt) != SQLITE_ROW || sqlite3_column_type(read.stmt, 0) != SQLITE_TEXT ||
        sqlite3_column_bytes(read.stmt, 0) != static_cast<int>(std::strlen(kRowsMagic)) ||
        std::strcmp(reinterpret_cast<const char*>(sqlite3_column_text(read.stmt, 0)), kRowsMagic) !=
            0 ||
        sqlite3_column_type(read.stmt, 1) != SQLITE_INTEGER ||
        sqlite3_column_int64(read.stmt, 1) != kRecordVersion)
        fail("unsupported record workspace state");
    for (int index = 2; index < 5; ++index)
        if (sqlite3_column_type(read.stmt, index) != SQLITE_INTEGER ||
            sqlite3_column_int64(read.stmt, index) < (index == 2 ? 1 : 0))
            fail("invalid record workspace counters");
    return {static_cast<std::uint64_t>(sqlite3_column_int64(read.stmt, 2)),
            static_cast<std::uint64_t>(sqlite3_column_int64(read.stmt, 3)),
            static_cast<std::uint64_t>(sqlite3_column_int64(read.stmt, 4))};
}

template <class Database> bool row_mode(Database&& db) {
    if (scalar(db, "PRAGMA application_id") != kWorkspaceId)
        fail("unsupported QCAE workspace identity");
    const int version = scalar(db, "PRAGMA user_version");
    if (version != kSchemaVersion && version != kRowsSchemaVersion)
        fail("unsupported QCAE workspace format");
    return version == kRowsSchemaVersion;
}

void validate_workspace(sqlite3* db) {
    if (!row_mode(db)) {
        validate(db, kWorkspaceId, false);
        return;
    }
    row_state(db);
    Statement check(db, "PRAGMA quick_check");
    if (sqlite3_step(check.stmt) != SQLITE_ROW || !sqlite3_column_text(check.stmt, 0) ||
        std::strcmp(reinterpret_cast<const char*>(sqlite3_column_text(check.stmt, 0)), "ok") != 0)
        fail("corrupt QCAE record database");
    Statement rows(db, "SELECT space,identity,value FROM store_rows LIMIT 0");
    if (sqlite3_step(rows.stmt) != SQLITE_DONE)
        fail("invalid QCAE record schema");
}

bool valid_space(StoreSpace space) {
    const auto value = static_cast<std::uint8_t>(space);
    return value >= static_cast<std::uint8_t>(StoreSpace::document_record) &&
           value <= static_cast<std::uint8_t>(StoreSpace::artifact_record);
}

void validate_key(const StoreKey& key, const StoreOptions& options) {
    if (!valid_space(key.space) || key.identity.empty() ||
        key.identity.size() > options.max_key_bytes ||
        key.identity.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        key.identity.find('\0') != std::string::npos)
        fail("invalid record workspace key");
}

void bind_key(sqlite3* db, sqlite3_stmt* stmt, const StoreKey& key) {
    // Both callers borrow keys from the batch until their statement guards clear bindings.
    if (sqlite3_bind_int(stmt, 1, static_cast<int>(key.space)) != SQLITE_OK ||
        sqlite3_bind_text(
            stmt, 2, key.identity.data(), static_cast<int>(key.identity.size()), SQLITE_STATIC) !=
            SQLITE_OK)
        fail("bind record workspace key: " + std::string(sqlite3_errmsg(db)));
    ledger::add(ledger::Stage::sqlite, ledger::Metric::driver_bind_copy_bytes, key.identity.size());
}

void initialize_rows(sqlite3* db) {
    // Called only inside the first successful row transaction on an empty store.
    exec(db, "DROP TABLE workspace");
    exec(db, "PRAGMA user_version=2");
    exec(db,
         "CREATE TABLE record_state(id INTEGER PRIMARY KEY CHECK(id=1),"
         "magic TEXT NOT NULL,record_version INTEGER NOT NULL,"
         "generation INTEGER NOT NULL CHECK(generation>=1),"
         "row_count INTEGER NOT NULL CHECK(row_count>=0),"
         "payload_bytes INTEGER NOT NULL CHECK(payload_bytes>=0),"
         "transaction_id TEXT NOT NULL)");
    exec(db,
         "CREATE TABLE store_rows(space INTEGER NOT NULL CHECK(space BETWEEN 1 AND 7),"
         "identity TEXT NOT NULL CHECK(length(identity)>0),value BLOB NOT NULL,"
         "PRIMARY KEY(space,identity))");
    // Keep large history/record BLOBs out of index-btree keys. WITHOUT ROWID
    // makes value part of the key record, so saving/seeking a cursor can copy
    // an unrelated overflow BLOB. The private rowid is storage only; callers
    // still use (space,identity). Existing WITHOUT ROWID stores remain readable.
}

void initialize(sqlite3* db, int application_id, bool project) {
    exec(db, "PRAGMA journal_mode=WAL");
    exec(db, "PRAGMA synchronous=FULL");
    exec(db, "BEGIN IMMEDIATE");
    try {
        const std::string app = "PRAGMA application_id=" + std::to_string(application_id);
        exec(db, app.c_str());
        exec(db, "PRAGMA user_version=1");
        exec(db,
             project ? "CREATE TABLE project(id INTEGER PRIMARY KEY CHECK(id=1),save_token TEXT "
                       "NOT NULL,payload BLOB NOT NULL)"
                     : "CREATE TABLE workspace(id INTEGER PRIMARY KEY CHECK(id=1),generation "
                       "INTEGER NOT NULL CHECK(generation>=1),payload BLOB NOT NULL)");
        exec(db, "COMMIT");
    } catch (...) {
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        throw;
    }
}

void configure_existing(sqlite3* db) {
    exec(db, "PRAGMA journal_mode=WAL");
    exec(db, "PRAGMA synchronous=FULL");
}

void bind_blob(sqlite3* db,
               sqlite3_stmt* stmt,
               int index,
               const std::string& value,
               sqlite3_destructor_type lifetime = SQLITE_TRANSIENT) {
    if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        sqlite3_bind_blob(stmt, index, value.data(), static_cast<int>(value.size()), lifetime) !=
            SQLITE_OK)
        fail("SQLite bind payload: " + std::string(sqlite3_errmsg(db)));
    ledger::add(ledger::Stage::sqlite, ledger::Metric::driver_bind_copy_bytes, value.size());
}

std::string column_blob(sqlite3_stmt* stmt, int index, std::size_t limit) {
    if (sqlite3_column_type(stmt, index) != SQLITE_BLOB)
        fail("invalid QCAE payload type");
    int bytes = sqlite3_column_bytes(stmt, index);
    if (bytes < 0 || static_cast<std::size_t>(bytes) > limit)
        fail("QCAE payload exceeds quota");
    const void* data = sqlite3_column_blob(stmt, index);
    return bytes ? std::string(static_cast<const char*>(data), static_cast<std::size_t>(bytes))
                 : std::string();
}

void sync_file(const std::string& path) {
    File file(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (file.fd < 0 || ::fsync(file.fd) != 0)
        fail(system_error("sync project snapshot"));
}

void sync_directory(const std::string& path) {
    File file(::open(fs::path(path).parent_path().c_str(), O_RDONLY | O_CLOEXEC));
    if (file.fd < 0 || ::fsync(file.fd) != 0)
        fail(system_error("sync project directory"));
}

struct Lease {
    File sidecar;
    InodeLock inode;
    std::vector<InodeLock> retired_inodes;
};
} // namespace

struct SqliteWorkspaceStore::Impl {
    std::string path;
    StoreOptions options;
    Lease workspace;
    std::unique_ptr<Db> db;
    bool writable{};
    std::map<std::string, Lease> projects;

    Impl(const std::string& requested, StoreOptions opts)
        : path(canonical_path(requested)), options(std::move(opts)) {
        workspace.sidecar = lock_sidecar(path);
        workspace.inode = lock_inode_if_present(path);
        const bool existing = exists(path);
        if (!existing) {
            File created(
                ::open(path.c_str(), O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600));
            if (created.fd < 0)
                fail(system_error("create working database"));
            workspace.inode = lock_inode_file(std::move(created));
        }
        try {
            db =
                std::make_unique<Db>(path, existing ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE);
            if (existing) {
                validate_workspace(db->db);
            } else {
                initialize(db->db, kWorkspaceId, false);
                writable = true;
            }
            ensure_same_inode(path, workspace.inode);
        } catch (...) {
            db.reset();
            if (!existing) {
                try {
                    ensure_same_inode(path, workspace.inode);
                    ::unlink(path.c_str());
                    ::unlink((path + "-wal").c_str());
                    ::unlink((path + "-shm").c_str());
                } catch (const StorageError&) {
                }
            }
            throw;
        }
    }

    void ensure_writer() {
        if (writable)
            return;
        ensure_same_inode(path, workspace.inode);
        auto writer = std::make_unique<Db>(path, SQLITE_OPEN_READWRITE);
        db = std::move(writer);
        configure_existing(db->db);
        ensure_same_inode(path, workspace.inode);
        writable = true;
    }

    void fault(const char* milestone, bool uncertain = false) {
        if (!options.fault)
            return;
        try {
            options.fault(milestone);
        } catch (const std::exception& error) {
            throw StorageError(error.what(), uncertain);
        } catch (...) {
            throw StorageError("storage fault", uncertain);
        }
    }
};

SqliteWorkspaceStore::SqliteWorkspaceStore(const std::string& path, StoreOptions options)
    : impl_(std::make_unique<Impl>(path, std::move(options))) {}
SqliteWorkspaceStore::~SqliteWorkspaceStore() = default;

StoredWorkspace read_legacy_workspace_readonly(const std::string& requested,
                                               std::size_t max_payload_bytes) {
    try {
        const std::string path = canonical_path(requested);
        if (!exists(path))
            fail("legacy workspace source does not exist");
        const auto inode = lock_inode_if_present(path);
        ensure_same_inode(path, inode);
        LegacyReadSnapshot snapshot(path);
        ensure_same_inode(path, inode);
        // Only the disposable copy permits sidecar recovery. The source has
        // never been opened through a SQLite writer.
        Db reader(snapshot.path, SQLITE_OPEN_READWRITE);
        exec(reader.db, "BEGIN");
        try {
            validate(reader.db, kWorkspaceId, false);
            Statement read(reader.db, "SELECT generation,payload FROM workspace WHERE id=1");
            if (sqlite3_step(read.stmt) != SQLITE_ROW ||
                sqlite3_column_type(read.stmt, 0) != SQLITE_INTEGER ||
                sqlite3_column_int64(read.stmt, 0) < 1)
                fail("legacy workspace source has no valid state");
            StoredWorkspace snapshot{static_cast<std::uint64_t>(sqlite3_column_int64(read.stmt, 0)),
                                     column_blob(read.stmt, 1, max_payload_bytes)};
            sqlite3_reset(read.stmt);
            ensure_same_inode(path, inode);
            exec(reader.db, "COMMIT");
            ensure_same_inode(path, inode);
            return snapshot;
        } catch (...) {
            sqlite3_exec(reader.db, "ROLLBACK", nullptr, nullptr, nullptr);
            throw;
        }
    } catch (const StorageError& error) {
        throw StorageError("legacy_readonly_unavailable: " + std::string(error.what()));
    }
}

std::optional<StoredWorkspace> SqliteWorkspaceStore::load() {
    ensure_same_inode(impl_->path, impl_->workspace.inode);
    if (row_mode(impl_->db->db))
        fail("record workspace requires load_rows");
    validate(impl_->db->db, kWorkspaceId, false);
    Statement statement(impl_->db->db, "SELECT generation,payload FROM workspace WHERE id=1");
    int rc = sqlite3_step(statement.stmt);
    if (rc == SQLITE_DONE)
        return std::nullopt;
    if (rc != SQLITE_ROW || sqlite3_column_type(statement.stmt, 0) != SQLITE_INTEGER)
        fail("invalid workspace state");
    sqlite3_int64 generation = sqlite3_column_int64(statement.stmt, 0);
    if (generation < 1)
        fail("invalid workspace generation");
    auto payload = column_blob(statement.stmt, 1, impl_->options.max_payload_bytes);
    return StoredWorkspace{static_cast<std::uint64_t>(generation), std::move(payload)};
}

std::uint64_t SqliteWorkspaceStore::commit(std::uint64_t expected_generation,
                                           const std::string& payload) {
    ensure_same_inode(impl_->path, impl_->workspace.inode);
    if (row_mode(impl_->db->db))
        fail("record workspace requires commit_rows");
    if (payload.size() > impl_->options.max_payload_bytes)
        fail("workspace payload exceeds quota");
    if (expected_generation >=
        static_cast<std::uint64_t>(std::numeric_limits<sqlite3_int64>::max()))
        fail("workspace generation exhausted");
    impl_->ensure_writer();
    sqlite3* db = impl_->db->db;
    exec(db, "BEGIN IMMEDIATE");
    bool committed = false;
    try {
        Statement read(db, "SELECT generation FROM workspace WHERE id=1");
        int rc = sqlite3_step(read.stmt);
        if (rc != SQLITE_ROW && rc != SQLITE_DONE)
            fail("read workspace generation failed");
        std::uint64_t actual =
            rc == SQLITE_DONE ? 0 : static_cast<std::uint64_t>(sqlite3_column_int64(read.stmt, 0));
        if (actual != expected_generation)
            fail("stale workspace generation");
        Statement write(db,
                        "INSERT INTO workspace(id,generation,payload) VALUES(1,?1,?2) "
                        "ON CONFLICT(id) DO UPDATE SET "
                        "generation=excluded.generation,payload=excluded.payload");
        if (sqlite3_bind_int64(write.stmt, 1, static_cast<sqlite3_int64>(actual + 1)) != SQLITE_OK)
            fail("bind workspace generation failed");
        bind_blob(db, write.stmt, 2, payload);
        if (sqlite3_step(write.stmt) != SQLITE_DONE)
            fail("write workspace failed");
        impl_->fault("before_db_commit");
        // COMMIT errors can have uncertain durability; callers must recover before retrying.
        int commit_rc = sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
        if (commit_rc != SQLITE_OK)
            throw StorageError("SQLite COMMIT: " + std::string(sqlite3_errmsg(db)), true);
        committed = true;
        try {
            ensure_same_inode(impl_->path, impl_->workspace.inode);
        } catch (const StorageError& error) {
            throw StorageError(error.what(), true);
        }
        impl_->fault("after_db_commit", true);
        return actual + 1;
    } catch (...) {
        if (!committed)
            sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        throw;
    }
}

LoadedRows SqliteWorkspaceStore::load_rows() {
    ensure_same_inode(impl_->path, impl_->workspace.inode);
    sqlite3* db = impl_->db->db;
    if (!row_mode(db)) {
        auto legacy = load();
        const auto generation = legacy ? legacy->generation : 0;
        return {generation, {}, std::move(legacy)};
    }
    const auto state = row_state(db);
    if (state.rows > impl_->options.max_rows || state.bytes > impl_->options.max_payload_bytes)
        fail("record workspace exceeds quota");
    LoadedRows loaded{state.generation, {}, std::nullopt};
    loaded.rows.reserve(static_cast<std::size_t>(state.rows));
    Statement read(db, "SELECT space,identity,value FROM store_rows ORDER BY space,identity");
    std::uint64_t bytes = 0;
    int rc;
    while ((rc = sqlite3_step(read.stmt)) == SQLITE_ROW) {
        if (loaded.rows.size() >= impl_->options.max_rows ||
            sqlite3_column_type(read.stmt, 0) != SQLITE_INTEGER ||
            sqlite3_column_int(read.stmt, 0) < 1 || sqlite3_column_int(read.stmt, 0) > 7 ||
            sqlite3_column_type(read.stmt, 1) != SQLITE_TEXT)
            fail("invalid record workspace row");
        const int key_bytes = sqlite3_column_bytes(read.stmt, 1);
        if (key_bytes < 1 || static_cast<std::size_t>(key_bytes) > impl_->options.max_key_bytes)
            fail("invalid record workspace key length");
        StoreKey key{static_cast<StoreSpace>(sqlite3_column_int(read.stmt, 0)),
                     std::string(reinterpret_cast<const char*>(sqlite3_column_text(read.stmt, 1)),
                                 static_cast<std::size_t>(key_bytes))};
        validate_key(key, impl_->options);
        auto value = column_blob(read.stmt, 2, impl_->options.max_payload_bytes);
        if (value.size() > impl_->options.max_payload_bytes - bytes)
            fail("record workspace payload exceeds quota");
        bytes += value.size();
        loaded.rows.push_back(
            {std::move(key), std::make_shared<const std::string>(std::move(value))});
    }
    if (rc != SQLITE_DONE || loaded.rows.size() != state.rows || bytes != state.bytes)
        fail("record workspace counters do not match rows");
    return loaded;
}

BatchReceipt SqliteWorkspaceStore::commit_rows(const StoreBatch& batch) {
    ledger::cover(ledger::Stage::sqlite);
    std::uint64_t logical_bytes = sizeof(StoreBatch) + batch.transaction_id.size();
    std::uint64_t key_bytes = batch.transaction_id.size();
    std::uint64_t deletes = 0;
    for (const auto& mutation : batch.mutations) {
        logical_bytes += sizeof(RowMutation) + mutation.key.identity.size() +
                         (mutation.after ? mutation.after->size() : 0);
        key_bytes += sizeof(mutation.key.space) + mutation.key.identity.size();
        deletes += mutation.after ? 0 : 1;
    }
    ledger::add(ledger::Stage::sqlite, ledger::Metric::batch_payload_bytes, logical_bytes);
    ledger::add(ledger::Stage::sqlite, ledger::Metric::batch_key_bytes, key_bytes);
    ledger::add(ledger::Stage::sqlite, ledger::Metric::batch_delete_count, deletes);
    ledger::add(ledger::Stage::sqlite,
                ledger::Metric::metadata_copy_bytes,
                batch.mutations.size() * sizeof(StoreKey) + key_bytes);
    int writes_before = 0, highwater = 0;
    const bool cache_counted =
        sqlite3_db_status(
            impl_->db->db, SQLITE_DBSTATUS_CACHE_WRITE, &writes_before, &highwater, 0) == SQLITE_OK;
    const auto* observe = std::getenv("QCAE_LEDGER_VFS");
    if (observe && std::strcmp(observe, "1") == 0) {
        ledger::add(ledger::Stage::sqlite, ledger::Metric::physical_wal_write_bytes, 0);
        ledger::add(ledger::Stage::sqlite, ledger::Metric::physical_database_write_bytes, 0);
    }
    ensure_same_inode(impl_->path, impl_->workspace.inode);
    const auto& options = impl_->options;
    if (batch.expected_generation >=
        static_cast<std::uint64_t>(std::numeric_limits<sqlite3_int64>::max()))
        fail("record workspace generation exhausted");
    if (batch.transaction_id.empty() || batch.transaction_id.size() > options.max_key_bytes ||
        batch.transaction_id.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        batch.transaction_id.find('\0') != std::string::npos)
        fail("invalid record transaction identity");
    if (batch.mutations.size() > options.max_batch_rows)
        fail("record batch exceeds row quota");
    std::set<StoreKey> keys;
    std::uint64_t payload_bytes = 0;
    std::uint64_t after_rows = 0;
    for (const auto& mutation : batch.mutations) {
        validate_key(mutation.key, options);
        if (!keys.insert(mutation.key).second)
            fail("duplicate record batch key");
        if (mutation.after) {
            if (mutation.after->size() > options.max_payload_bytes - payload_bytes)
                fail("record batch exceeds payload quota");
            payload_bytes += mutation.after->size();
            ++after_rows;
        }
    }
    if (!row_mode(*impl_->db) &&
        scalar(*impl_->db, "SELECT count(*) FROM workspace WHERE id=1") != 0)
        fail("legacy workspace requires explicit migration to a separate destination");
    impl_->ensure_writer();
    sqlite3* db = impl_->db->db;
    exec(*impl_->db, "BEGIN IMMEDIATE");
    bool committed = false;
    try {
        RowState state{0, 0, 0};
        if (row_mode(*impl_->db)) {
            state = row_state(*impl_->db);
        } else {
            Statement legacy(db, "SELECT generation FROM workspace WHERE id=1");
            const int rc = sqlite3_step(legacy.stmt);
            if (rc == SQLITE_ROW)
                fail("legacy workspace requires explicit migration to a separate destination");
            if (rc != SQLITE_DONE)
                fail("read legacy workspace state failed");
        }
        if (state.generation != batch.expected_generation)
            fail("stale record workspace generation");
        if (state.rows > options.max_rows || state.bytes > options.max_payload_bytes)
            fail("record workspace exceeds quota");
        if (state.generation == 0)
            initialize_rows(db);
        Statement previous(*impl_->db,
                           "SELECT length(value) FROM store_rows WHERE space=?1 AND identity=?2");
        Statement upsert(*impl_->db,
                         "INSERT INTO store_rows(space,identity,value) VALUES(?1,?2,?3) "
                         "ON CONFLICT(space,identity) DO UPDATE SET value=excluded.value");
        Statement remove(*impl_->db, "DELETE FROM store_rows WHERE space=?1 AND identity=?2");
        // Count the final batch state before writes so a full store can replace
        // deleted keys in either input order, without transient quota failures.
        for (const auto& mutation : batch.mutations) {
            sqlite3_reset(previous.stmt);
            sqlite3_clear_bindings(previous.stmt);
            bind_key(db, previous.stmt, mutation.key);
            const int rc = sqlite3_step(previous.stmt);
            if (rc != SQLITE_ROW && rc != SQLITE_DONE)
                fail("read existing record failed");
            if (rc == SQLITE_ROW) {
                const auto old_bytes = sqlite3_column_int64(previous.stmt, 0);
                if (sqlite3_column_type(previous.stmt, 0) != SQLITE_INTEGER || old_bytes < 0 ||
                    static_cast<std::uint64_t>(old_bytes) > state.bytes || state.rows == 0)
                    fail("invalid existing record counters");
                state.bytes -= static_cast<std::uint64_t>(old_bytes);
                --state.rows;
            }
            sqlite3_reset(previous.stmt);
        }
        if (after_rows > options.max_rows - state.rows ||
            payload_bytes > options.max_payload_bytes - state.bytes)
            fail("record workspace exceeds quota");
        state.rows += after_rows;
        state.bytes += payload_bytes;
        if (state.rows > static_cast<std::uint64_t>(std::numeric_limits<sqlite3_int64>::max()) ||
            state.bytes > static_cast<std::uint64_t>(std::numeric_limits<sqlite3_int64>::max()))
            fail("record workspace counter exhausted");
        std::uint64_t rows_written = 0;
        for (const auto& mutation : batch.mutations) {
            sqlite3_stmt* write = mutation.after ? upsert.stmt : remove.stmt;
            sqlite3_reset(write);
            sqlite3_clear_bindings(write);
            bind_key(db, write, mutation.key);
            if (mutation.after) {
                bind_blob(db, write, 3, *mutation.after, SQLITE_STATIC);
            }
            if (sqlite3_step(write) != SQLITE_DONE)
                fail("write record failed");
            rows_written += static_cast<std::uint64_t>(sqlite3_changes(db));
        }
        Statement metadata(*impl_->db,
                           "INSERT INTO record_state(id,magic,record_version,generation,row_count,"
                           "payload_bytes,transaction_id) VALUES(1,'QCAE-ROWS',1,?1,?2,?3,?4) "
                           "ON CONFLICT(id) DO UPDATE SET generation=excluded.generation,"
                           "row_count=excluded.row_count,payload_bytes=excluded.payload_bytes,"
                           "transaction_id=excluded.transaction_id");
        if (sqlite3_bind_int64(
                metadata.stmt, 1, static_cast<sqlite3_int64>(state.generation + 1)) != SQLITE_OK ||
            sqlite3_bind_int64(metadata.stmt, 2, static_cast<sqlite3_int64>(state.rows)) !=
                SQLITE_OK ||
            sqlite3_bind_int64(metadata.stmt, 3, static_cast<sqlite3_int64>(state.bytes)) !=
                SQLITE_OK ||
            sqlite3_bind_text(metadata.stmt,
                              4,
                              batch.transaction_id.data(),
                              static_cast<int>(batch.transaction_id.size()),
                              SQLITE_STATIC) != SQLITE_OK ||
            sqlite3_step(metadata.stmt) != SQLITE_DONE)
            fail("write record workspace metadata failed");
        impl_->fault("before_db_commit");
        exec(*impl_->db, "COMMIT", true);
        committed = true;
        int writes_after = 0;
        if (cache_counted &&
            sqlite3_db_status(db, SQLITE_DBSTATUS_CACHE_WRITE, &writes_after, &highwater, 0) ==
                SQLITE_OK)
            ledger::add(ledger::Stage::sqlite,
                        ledger::Metric::sqlite_cache_page_writes,
                        static_cast<std::uint64_t>(writes_after - writes_before));
        else
            ledger::unknown(ledger::Stage::sqlite, ledger::Metric::sqlite_cache_page_writes);
        ledger::add(ledger::Stage::sqlite,
                    ledger::Metric::driver_bind_copy_bytes,
                    batch.transaction_id.size());
        try {
            ensure_same_inode(impl_->path, impl_->workspace.inode);
        } catch (const StorageError& error) {
            throw StorageError(error.what(), true);
        }
        impl_->fault("after_db_commit", true);
        return {state.generation + 1, rows_written, payload_bytes};
    } catch (...) {
        if (!committed) {
            try {
                exec(*impl_->db, "ROLLBACK");
            } catch (...) {
            }
        }
        throw;
    }
}

std::string SqliteWorkspaceStore::acquire_project(const std::string& requested) {
    const std::string path = canonical_path(requested);
    if (path == impl_->path)
        fail("project target aliases working database");
    if (impl_->projects.contains(path))
        return path;
    Lease lease{lock_sidecar(path), lock_inode_if_present(path), {}};
    ensure_same_inode(path, lease.inode);
    impl_->projects.emplace(path, std::move(lease));
    return path;
}

void SqliteWorkspaceStore::release_projects_except(const std::string& path) noexcept {
    for (auto it = impl_->projects.begin(); it != impl_->projects.end();) {
        if (it->first != path)
            it = impl_->projects.erase(it);
        else
            ++it;
    }
}

StoredProject SqliteWorkspaceStore::read_project(const std::string& path) {
    const auto found = impl_->projects.find(path);
    if (found == impl_->projects.end())
        fail("project lease required");
    ensure_same_inode(path, found->second.inode);
    if (!exists(path))
        fail("project_not_found");
    Db db(path, SQLITE_OPEN_READONLY);
    validate(db.db, kProjectId, true);
    Statement statement(db.db, "SELECT save_token,payload FROM project WHERE id=1");
    if (sqlite3_step(statement.stmt) != SQLITE_ROW ||
        sqlite3_column_type(statement.stmt, 0) != SQLITE_TEXT)
        fail("invalid project state");
    const int token_bytes = sqlite3_column_bytes(statement.stmt, 0);
    if (token_bytes < 1 || token_bytes > 4096)
        fail("invalid project save token");
    const unsigned char* token = sqlite3_column_text(statement.stmt, 0);
    std::string save_token(reinterpret_cast<const char*>(token),
                           static_cast<std::size_t>(token_bytes));
    auto payload = column_blob(statement.stmt, 1, impl_->options.max_payload_bytes);
    return {std::move(save_token), std::move(payload)};
}

void SqliteWorkspaceStore::publish_project(const std::string& path, const StoredProject& project) {
    const auto found = impl_->projects.find(path);
    if (found == impl_->projects.end())
        fail("project lease required");
    if (project.payload.size() > impl_->options.max_payload_bytes)
        fail("project payload exceeds quota");
    if (project.save_token.empty() || project.save_token.size() > 4096 ||
        project.save_token.find('\0') != std::string::npos)
        fail("invalid project save token");
    ensure_same_inode(path, found->second.inode);
    if (exists(path)) {
        Db previous(path, SQLITE_OPEN_READONLY);
        validate(previous.db, kProjectId, true);
    }
    std::string pattern = path + ".tmp.XXXXXX";
    File temporary(::mkstemp(pattern.data()));
    if (temporary.fd < 0)
        fail(system_error("create project snapshot"));
    const std::string temp_path = pattern;
    try {
        {
            Db db(temp_path, SQLITE_OPEN_READWRITE);
            initialize(db.db, kProjectId, true);
            {
                Statement write(db.db,
                                "INSERT INTO project(id,save_token,payload) VALUES(1,?1,?2)");
                if (sqlite3_bind_text(write.stmt,
                                      1,
                                      project.save_token.data(),
                                      static_cast<int>(project.save_token.size()),
                                      SQLITE_TRANSIENT) != SQLITE_OK)
                    fail("bind project save token failed");
                bind_blob(db.db, write.stmt, 2, project.payload);
                if (sqlite3_step(write.stmt) != SQLITE_DONE)
                    fail("write project snapshot failed");
            }
            exec(db.db, "PRAGMA wal_checkpoint(TRUNCATE)");
            exec(db.db, "PRAGMA journal_mode=DELETE");
            Statement mode(db.db, "PRAGMA journal_mode");
            if (sqlite3_step(mode.stmt) != SQLITE_ROW ||
                std::strcmp(reinterpret_cast<const char*>(sqlite3_column_text(mode.stmt, 0)),
                            "delete") != 0)
                fail("project snapshot did not leave WAL mode");
        }
        if (exists(temp_path + "-wal"))
            fail("project snapshot still has WAL file");
        // SQLite may leave an unused shared-memory file after switching to DELETE mode.
        if (::unlink((temp_path + "-shm").c_str()) != 0 && errno != ENOENT)
            fail(system_error("remove snapshot shared-memory file"));
        {
            Db verified(temp_path, SQLITE_OPEN_READONLY);
            validate(verified.db, kProjectId, true);
        }
        sync_file(temp_path);
        sync_directory(path);
        InodeLock new_inode = lock_inode_file(std::move(temporary));
        found->second.retired_inodes.reserve(found->second.retired_inodes.size() + 1);
        impl_->fault("before_project_publish");
        ensure_same_inode(path, found->second.inode);
        if (::rename(temp_path.c_str(), path.c_str()) != 0)
            fail(system_error("publish project snapshot"));
        // Keep previous inode guards: a hardlink alias can still name an old snapshot.
        found->second.retired_inodes.push_back(std::move(found->second.inode));
        found->second.inode = std::move(new_inode);
        try {
            sync_directory(path);
            impl_->fault("after_project_publish", true);
        } catch (const StorageError& error) {
            throw StorageError(error.what(), true);
        }
    } catch (...) {
        ::unlink(temp_path.c_str());
        ::unlink((temp_path + "-wal").c_str());
        ::unlink((temp_path + "-shm").c_str());
        throw;
    }
}
} // namespace qcae
