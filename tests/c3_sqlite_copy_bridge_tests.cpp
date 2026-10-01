#include "c3_sqlite_copy_bridge.h"
#include "qcae/operation_ledger.hpp"

#include <sqlite3.h>
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
std::uint64_t metric(const qcae::ledger::OperationLedger& ledger, qcae::ledger::Metric name) {
    const auto value =
        ledger.snapshot().values[static_cast<std::size_t>(qcae::ledger::Stage::sqlite)]
                                [static_cast<std::size_t>(name)];
    check(value.has_value(), "Missing SQLite observation");
    return *value;
}
} // namespace

int main() {
    using namespace qcae::ledger;
    try {
        auto observer = std::make_shared<OperationLedger>(Identity{"bridge", {}, {}, 0});
        activate(observer);
        check(qcae_c3_sqlite_observer_begin(), "Audited SQLite 3.51.0 was not linked");
        std::array<char, 16> copied{};
        qcae_c3_sqlite_memcpy(copied.data(), "abcdefghijklmnop", copied.size());
        qcae_c3_sqlite_memmove(copied.data() + 1, copied.data(), 8);
        check(metric(*observer, Metric::driver_internal_copy_bytes) == 24,
              "Explicit copy/overlapping move bytes were not counted exactly");
        check(std::memcmp(copied.data(), "aabcdefghjklmnop", copied.size()) == 0,
              "Observer altered memcpy/memmove results");

        // This SQLite API performs a struct assignment in the amalgamation.
        // Its frontend LLVM memcpy must remain observed after O2 codegen.
        sqlite3_mem_methods methods{};
        const auto before = metric(*observer, Metric::driver_internal_copy_bytes);
        check(sqlite3_config(SQLITE_CONFIG_GETMALLOC, &methods) == SQLITE_OK,
              "SQLite allocator configuration could not be read");
        check(metric(*observer, Metric::driver_internal_copy_bytes) >= before + sizeof(methods),
              "Compiler-generated aggregate copy escaped the observer");
        activate({});

        sqlite3* database{};
        check(sqlite3_open(":memory:", &database) == SQLITE_OK, "SQLite open failed");
        check(sqlite3_exec(database,
                           "CREATE TABLE bytes(k TEXT PRIMARY KEY,v BLOB)",
                           nullptr,
                           nullptr,
                           nullptr) == SQLITE_OK,
              "SQLite schema initialization failed");
        observer = std::make_shared<OperationLedger>(Identity{"sql", {}, {}, 0});
        activate(observer);
        check(qcae_c3_sqlite_observer_begin(), "SQLite observer begin failed");
        sqlite3_stmt* statement{};
        check(sqlite3_prepare_v2(
                  database, "INSERT INTO bytes VALUES(?1,?2)", -1, &statement, nullptr) ==
                  SQLITE_OK,
              "SQLite insert prepare failed");
        const std::string key = "same-run-key";
        const std::string value(4096, 'x');
        check(sqlite3_bind_text(statement, 1, key.data(), key.size(), SQLITE_TRANSIENT) ==
                      SQLITE_OK &&
                  sqlite3_bind_blob(statement, 2, value.data(), value.size(), SQLITE_TRANSIENT) ==
                      SQLITE_OK &&
                  sqlite3_step(statement) == SQLITE_DONE,
              "Observed SQLite insert failed");
        sqlite3_finalize(statement);
        check(metric(*observer, Metric::driver_internal_copy_bytes) >= key.size() + value.size(),
              "SQLITE_TRANSIENT payload copies were omitted");
        check(metric(*observer, Metric::encoded_bytes) >= key.size() + value.size(),
              "SQLite record encoder output was omitted");
        const auto copy_bytes = metric(*observer, Metric::driver_internal_copy_bytes);
        const auto encoded_bytes = metric(*observer, Metric::encoded_bytes);
        activate({});
        check(sqlite3_prepare_v2(database, "SELECT k,v FROM bytes", -1, &statement, nullptr) ==
                      SQLITE_OK &&
                  sqlite3_step(statement) == SQLITE_ROW &&
                  std::memcmp(sqlite3_column_text(statement, 0), key.data(), key.size()) == 0 &&
                  sqlite3_column_bytes(statement, 1) == static_cast<int>(value.size()) &&
                  std::memcmp(sqlite3_column_blob(statement, 1), value.data(), value.size()) == 0,
              "Instrumentation changed stored SQL contents");
        sqlite3_finalize(statement);
        sqlite3_close(database);
        std::cout << "PASS: SQLite 3.51.0 explicit, aggregate and TRANSIENT copies; actual SQL "
                     "bytes preserved; driver_copy="
                  << copy_bytes << "; record_encoder=" << encoded_bytes << '\n';
        return 0;
    } catch (const std::exception& error) {
        activate({});
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
