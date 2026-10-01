#pragma once

#include "qcae/operation_ledger.hpp"
#include <sqlite3.h>
#include <cstddef>
#include <mutex>
#include <new>
#include <stdexcept>

namespace qcae::sqlite_ledger {
// Transparent default-VFS delegation. Count xWrite requests after successful writes;
// these are VFS bytes, not claims about a storage device's physical write amplification.
struct File {
    sqlite3_file base{};
    sqlite3_file* inner{};
    int flags{};
};
inline File* file(sqlite3_file* value) {
    return reinterpret_cast<File*>(value);
}
inline int close(sqlite3_file* value) {
    const auto rc = file(value)->inner->pMethods->xClose(file(value)->inner);
    value->pMethods = nullptr;
    return rc;
}
inline int read(sqlite3_file* value, void* bytes, int size, sqlite3_int64 offset) {
    return file(value)->inner->pMethods->xRead(file(value)->inner, bytes, size, offset);
}
inline int write(sqlite3_file* value, const void* bytes, int size, sqlite3_int64 offset) {
    auto* f = file(value);
    const auto rc = f->inner->pMethods->xWrite(f->inner, bytes, size, offset);
    if (rc == SQLITE_OK) {
        const auto metric = (f->flags & SQLITE_OPEN_WAL)
                                ? ledger::Metric::physical_wal_write_bytes
                                : ledger::Metric::physical_database_write_bytes;
        ledger::add(ledger::Stage::sqlite, metric, static_cast<std::uint64_t>(size));
    }
    return rc;
}
inline int truncate(sqlite3_file* value, sqlite3_int64 size) {
    return file(value)->inner->pMethods->xTruncate(file(value)->inner, size);
}
inline int sync(sqlite3_file* value, int flags) {
    return file(value)->inner->pMethods->xSync(file(value)->inner, flags);
}
inline int size(sqlite3_file* value, sqlite3_int64* bytes) {
    return file(value)->inner->pMethods->xFileSize(file(value)->inner, bytes);
}
inline int lock(sqlite3_file* value, int level) {
    return file(value)->inner->pMethods->xLock(file(value)->inner, level);
}
inline int unlock(sqlite3_file* value, int level) {
    return file(value)->inner->pMethods->xUnlock(file(value)->inner, level);
}
inline int reserved(sqlite3_file* value, int* held) {
    return file(value)->inner->pMethods->xCheckReservedLock(file(value)->inner, held);
}
inline int control(sqlite3_file* value, int operation, void* argument) {
    return file(value)->inner->pMethods->xFileControl(file(value)->inner, operation, argument);
}
inline int sector(sqlite3_file* value) {
    return file(value)->inner->pMethods->xSectorSize(file(value)->inner);
}
inline int characteristics(sqlite3_file* value) {
    return file(value)->inner->pMethods->xDeviceCharacteristics(file(value)->inner);
}
inline int shm_map(sqlite3_file* value, int page, int size, int extend, void volatile** out) {
    return file(value)->inner->pMethods->xShmMap(file(value)->inner, page, size, extend, out);
}
inline int shm_lock(sqlite3_file* value, int offset, int count, int flags) {
    return file(value)->inner->pMethods->xShmLock(file(value)->inner, offset, count, flags);
}
inline void shm_barrier(sqlite3_file* value) {
    file(value)->inner->pMethods->xShmBarrier(file(value)->inner);
}
inline int shm_unmap(sqlite3_file* value, int remove) {
    return file(value)->inner->pMethods->xShmUnmap(file(value)->inner, remove);
}
inline int fetch(sqlite3_file* value, sqlite3_int64 offset, int size, void** out) {
    const auto method = file(value)->inner->pMethods->xFetch;
    if (!method) {
        *out = nullptr;
        return SQLITE_OK;
    }
    return method(file(value)->inner, offset, size, out);
}
inline int unfetch(sqlite3_file* value, sqlite3_int64 offset, void* bytes) {
    const auto method = file(value)->inner->pMethods->xUnfetch;
    return method ? method(file(value)->inner, offset, bytes) : SQLITE_OK;
}
inline const sqlite3_io_methods methods1{1,
                                         close,
                                         read,
                                         write,
                                         truncate,
                                         sync,
                                         size,
                                         lock,
                                         unlock,
                                         reserved,
                                         control,
                                         sector,
                                         characteristics,
                                         nullptr,
                                         nullptr,
                                         nullptr,
                                         nullptr,
                                         nullptr,
                                         nullptr};
inline const sqlite3_io_methods methods2{2,
                                         close,
                                         read,
                                         write,
                                         truncate,
                                         sync,
                                         size,
                                         lock,
                                         unlock,
                                         reserved,
                                         control,
                                         sector,
                                         characteristics,
                                         shm_map,
                                         shm_lock,
                                         shm_barrier,
                                         shm_unmap,
                                         nullptr,
                                         nullptr};
inline const sqlite3_io_methods methods3{3,
                                         close,
                                         read,
                                         write,
                                         truncate,
                                         sync,
                                         size,
                                         lock,
                                         unlock,
                                         reserved,
                                         control,
                                         sector,
                                         characteristics,
                                         shm_map,
                                         shm_lock,
                                         shm_barrier,
                                         shm_unmap,
                                         fetch,
                                         unfetch};
inline sqlite3_vfs* parent(sqlite3_vfs* vfs) {
    return static_cast<sqlite3_vfs*>(vfs->pAppData);
}
inline constexpr std::size_t offset = (sizeof(File) + alignof(std::max_align_t) - 1) /
                                      alignof(std::max_align_t) * alignof(std::max_align_t);
inline int open(sqlite3_vfs* vfs, const char* name, sqlite3_file* out, int flags, int* actual) {
    auto* f = new (out) File;
    f->inner = reinterpret_cast<sqlite3_file*>(reinterpret_cast<unsigned char*>(out) + offset);
    f->flags = flags;
    const auto rc = parent(vfs)->xOpen(parent(vfs), name, f->inner, flags, actual);
    if (rc == SQLITE_OK) {
        const auto version = f->inner->pMethods->iVersion;
        out->pMethods = version >= 3 ? &methods3 : version == 2 ? &methods2 : &methods1;
    }
    return rc;
}
inline int remove(sqlite3_vfs* vfs, const char* name, int sync) {
    return parent(vfs)->xDelete(parent(vfs), name, sync);
}
inline int access(sqlite3_vfs* vfs, const char* name, int flags, int* out) {
    return parent(vfs)->xAccess(parent(vfs), name, flags, out);
}
inline int full_path(sqlite3_vfs* vfs, const char* name, int length, char* out) {
    return parent(vfs)->xFullPathname(parent(vfs), name, length, out);
}
inline void* dl_open(sqlite3_vfs* vfs, const char* name) {
    const auto method = parent(vfs)->xDlOpen;
    return method ? method(parent(vfs), name) : nullptr;
}
inline void dl_error(sqlite3_vfs* vfs, int length, char* out) {
    if (const auto method = parent(vfs)->xDlError)
        method(parent(vfs), length, out);
    else if (length > 0)
        *out = '\0';
}
inline void (*dl_symbol(sqlite3_vfs* vfs, void* handle, const char* symbol))(void) {
    const auto method = parent(vfs)->xDlSym;
    return method ? method(parent(vfs), handle, symbol) : nullptr;
}
inline void dl_close(sqlite3_vfs* vfs, void* handle) {
    if (const auto method = parent(vfs)->xDlClose)
        method(parent(vfs), handle);
}
inline int randomness(sqlite3_vfs* vfs, int length, char* out) {
    return parent(vfs)->xRandomness(parent(vfs), length, out);
}
inline int sleep(sqlite3_vfs* vfs, int micros) {
    return parent(vfs)->xSleep(parent(vfs), micros);
}
inline int time(sqlite3_vfs* vfs, double* out) {
    return parent(vfs)->xCurrentTime(parent(vfs), out);
}
inline int error(sqlite3_vfs* vfs, int length, char* out) {
    const auto method = parent(vfs)->xGetLastError;
    if (method)
        return method(parent(vfs), length, out);
    if (length > 0)
        *out = '\0';
    return 0;
}
inline int time64(sqlite3_vfs* vfs, sqlite3_int64* out) {
    const auto method = parent(vfs)->xCurrentTimeInt64;
    if (method)
        return method(parent(vfs), out);
    double julian = 0;
    const auto rc = parent(vfs)->xCurrentTime(parent(vfs), &julian);
    if (rc == SQLITE_OK)
        *out = static_cast<sqlite3_int64>(julian * 86400000.0);
    return rc;
}
inline const char* name() {
    static std::once_flag once;
    static sqlite3_vfs observed{};
    std::call_once(once, [] {
        auto* original = sqlite3_vfs_find(nullptr);
        if (!original || !original->xOpen || !original->xDelete || !original->xAccess ||
            !original->xFullPathname || !original->xRandomness || !original->xSleep ||
            !original->xCurrentTime)
            throw std::runtime_error("Default SQLite VFS is unavailable or incomplete");
        observed = *original;
        // Version 2 avoids delegating process-global system-call replacement through a shim.
        observed.iVersion = original->iVersion >= 2 ? 2 : 1;
        observed.szOsFile = static_cast<int>(offset) + original->szOsFile;
        observed.zName = "qcae-observed-default";
        observed.pAppData = original;
        observed.xOpen = open;
        observed.xDelete = remove;
        observed.xAccess = access;
        observed.xFullPathname = full_path;
        observed.xDlOpen = dl_open;
        observed.xDlError = dl_error;
        observed.xDlSym = dl_symbol;
        observed.xDlClose = dl_close;
        observed.xRandomness = randomness;
        observed.xSleep = sleep;
        observed.xCurrentTime = time;
        observed.xGetLastError = error;
        observed.xCurrentTimeInt64 = observed.iVersion >= 2 ? time64 : nullptr;
        if (sqlite3_vfs_register(&observed, 0) != SQLITE_OK)
            throw std::runtime_error("Unable to register the SQLite observation VFS");
    });
    return observed.zName;
}
} // namespace qcae::sqlite_ledger
