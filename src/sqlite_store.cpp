#include "qcae/sqlite_store.hpp"

#include <sqlite3.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
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
    Db(const std::string& path, int flags) {
        int rc = sqlite3_open_v2(
            path.c_str(), &db, flags | SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_NOFOLLOW, nullptr);
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
        if (db)
            sqlite3_close(db);
    }
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
    Statement(sqlite3* db, const char* sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
            fail("SQLite prepare: " + std::string(sqlite3_errmsg(db)));
    }
    ~Statement() {
        if (stmt)
            sqlite3_finalize(stmt);
    }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
};

int scalar(sqlite3* db, const char* sql) {
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

void bind_blob(sqlite3* db, sqlite3_stmt* stmt, int index, const std::string& value) {
    if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        sqlite3_bind_blob(
            stmt, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT) !=
            SQLITE_OK)
        fail("SQLite bind payload: " + std::string(sqlite3_errmsg(db)));
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
        if (existing) {
            Db reader(path, SQLITE_OPEN_READONLY);
            validate(reader.db, kWorkspaceId, false);
        }
        try {
            db = std::make_unique<Db>(path, SQLITE_OPEN_READWRITE);
            if (existing)
                configure_existing(db->db);
            else
                initialize(db->db, kWorkspaceId, false);
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

std::optional<StoredWorkspace> SqliteWorkspaceStore::load() {
    ensure_same_inode(impl_->path, impl_->workspace.inode);
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
    if (payload.size() > impl_->options.max_payload_bytes)
        fail("workspace payload exceeds quota");
    if (expected_generation >=
        static_cast<std::uint64_t>(std::numeric_limits<sqlite3_int64>::max()))
        fail("workspace generation exhausted");
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
