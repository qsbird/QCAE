#include <QtCore/QCoreApplication>
#include <QtCore/QHash>
#include <QtCore/QJsonDocument>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <limits>
#include <map>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include "collector-private.hpp"
thread_local bool fail_next_allocation{};
void* operator new(std::size_t n) {
    if (fail_next_allocation) { fail_next_allocation = false; throw std::bad_alloc{}; }
    if (auto* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
namespace {
struct Key {
    std::uint32_t glyph;
    struct { std::uint32_t x, y; } subPixelPosition;
    friend bool operator==(const Key& a, const Key& b) noexcept {
        return a.glyph == b.glyph && a.subPixelPosition.x == b.subPixelPosition.x && a.subPixelPosition.y == b.subPixelPosition.y;
    }
};
std::size_t qHash(const Key& k, std::size_t seed) noexcept { return qHashMulti(seed, k.glyph, k.subPixelPosition.x, k.subPixelPosition.y); }
std::atomic<unsigned> attempts{}, entries{}, completions{}, unknown{}, nodes{}, throw_budget{};
const char* throw_site{};
bool reenter{}, did_reenter{}, throw_first_thread_entry{};
thread_local bool first_thread_entry = true;
QHash<Key, void*> nested;
void check(bool b, const char* message) { if (!b) throw std::runtime_error(message); }
void collect(void*, const char* site, unsigned kind, std::uint64_t) {
    ++attempts;
    if ((!throw_site || std::strcmp(site, throw_site) == 0) && throw_budget.exchange(0)) throw std::bad_alloc{};
    if (throw_first_thread_entry && first_thread_entry) { first_thread_entry = false; throw std::bad_alloc{}; }
    if (std::strcmp(site, "ft_hash/glyph_operation") == 0) ++entries;
    if (std::strcmp(site, "ft_hash/glyph_operation_complete") == 0) { ++completions; if (kind == 3) ++unknown; }
    if (std::strcmp(site, "ft_hash/node_create_move") == 0) {
        ++nodes;
        if (reenter && !did_reenter) {
            did_reenter = true; QcaeFtHash::Scope child(1);
            nested.insert(Key{7, {0, 0}}, reinterpret_cast<void*>(2)); child.finish();
        }
    }
}
void insert(QHash<Key, void*>& hash, unsigned i) {
    QcaeFtHash::Scope scope(1); hash.insert(Key{i, {0, 0}}, reinterpret_cast<void*>(std::uintptr_t(i) + 1)); scope.finish();
}
struct ObserverOwner {
    ObserverOwner() { qcae_qt_sdk_observer_install(collect, this); }
    ~ObserverOwner() noexcept { qcae_qt_sdk_observer_install(nullptr, nullptr); }
};
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc != 2) return 2;
    try {
        static_assert(std::is_nothrow_destructible_v<QcaeQtSdkScope>);
        static_assert(std::is_nothrow_constructible_v<QcaeQtSdkNoexceptScope, const char*>);
        static_assert(std::is_nothrow_destructible_v<QcaeFtHash::Scope>);
        Dl_info image{}; check(dladdr(reinterpret_cast<void*>(&qcae_qt_sdk_observer_emit), &image) != 0, "No actual linked image");
        check(std::strstr(image.dli_fname, "/tranche8b-private/qt-prefix/") != nullptr, "Actual bridge image is not private SDK8");
        std::printf("actual_SDK_image=%s\n", image.dli_fname);
        qcae_qt_sdk_observer_install(collect, nullptr);
        const std::string mode(argv[1]);
        if (mode == "admission_bad_alloc") {
            throw_budget = 1;
            bool escaped = false;
            try {
                QcaeQtSdkNoexceptScope failed("first_callback");
                check(qcae_qt_sdk_observer_depth() == 0, "Admission depth did not roll back");
                { QcaeQtSdkNoexceptScope child("successful_nested_admission"); check(qcae_qt_sdk_observer_depth() == 1, "Nested successful scope depth wrong"); }
                check(qcae_qt_sdk_observer_depth() == 0, "Failed frame stole nested leave");
            } catch (const std::bad_alloc&) { escaped = true; }
            check(!escaped && attempts == 2 && qcae_qt_sdk_observer_depth() == 0, "Callback exception escaped or poisoned nested admission");
            qcae_qt_sdk_observer_install(collect, nullptr);
            { QcaeQtSdkNoexceptScope recovered("reinstall_recovery"); }
            check(attempts == 3 && qcae_qt_sdk_observer_status() == 1, "Recovery lost sticky unknown or remained poisoned");
        } else if (mode == "emit_bad_alloc") {
            QcaeQtSdkNoexceptScope scope("good_admission");
            throw_budget = 1; qcae_qt_sdk_observer_emit("throwing_emit", 1, 5);
            qcae_qt_sdk_observer_emit("later_emit", 1, 5);
            check(attempts == 3 && qcae_qt_sdk_observer_status() == 1, "Dispatch guard did not recover after callback throw");
        } else if (mode == "scope_destructor_bad_alloc") {
            throw_site = "ft_hash/glyph_operation_complete"; throw_budget = 1;
            QHash<Key, void*> original; insert(original, 1);
            check(original.size() == 1 && QcaeFtHash::active == nullptr && qcae_qt_sdk_observer_depth() == 0, "Destructor exception changed original data or active stack");
            check(qcae_qt_sdk_observer_status() == 1, "Destructor callback loss was not sticky unknown");
            insert(original, 2); check(original.size() == 2 && unknown == 1, "Later completion did not retain sticky unknown");
        } else if (mode == "reentry") {
            reenter = true; QHash<Key, void*> original; insert(original, 1);
            check(original.size() == 1 && nested.size() == 1 && entries == 1 && completions == 1 && nodes == 1 && unknown == 0 && QcaeFtHash::active == nullptr, "Nested original work or reentry suppression changed");
            check(qcae_qt_sdk_observer_status() == 0, "Normal reentry spuriously lost collection");
        } else if (mode == "threads" || mode == "thread_exception") {
            throw_first_thread_entry = mode == "thread_exception";
            std::atomic<bool> preserved{true};
            const auto work = [&] { QHash<Key, void*> original; for (unsigned i = 0; i < 100; ++i) insert(original, i); if (original.size() != 100 || QcaeFtHash::active != nullptr || qcae_qt_sdk_observer_depth() != 0) preserved = false; };
            std::thread a(work), b(work); a.join(); b.join();
            check(preserved, "Actual two-thread original data/scope state changed");
            if (throw_first_thread_entry) check(qcae_qt_sdk_observer_status() == 1 && entries == 198 && unknown == 198, "Thread callback exception did not recover with sticky unknown");
            else check(entries == 200 && completions == 200 && nodes == 200 && unknown == 0 && qcae_qt_sdk_observer_status() == 0, "Normal thread facts changed");
        } else if (mode == "observer_destruction") {
            {
                QcaeQtSdkNoexceptScope scope("outer_scope");
                { ObserverOwner owner; qcae_qt_sdk_observer_emit("owner_alive", 0, 0); }
                const auto before = attempts.load(); qcae_qt_sdk_observer_emit("after_owner_destroyed", 0, 0);
                check(attempts == before, "Destroyed collector was called");
            }
            check(qcae_qt_sdk_observer_depth() == 0 && qcae_qt_sdk_observer_status() == 0, "Uninstalled observer changed scope state");
        } else if (mode == "admission_capacity") {
            for (unsigned i = 0; i < 1025; ++i) qcae_qt_sdk_observer_enter("bounded_admission");
            check(qcae_qt_sdk_observer_depth() == 1024 && qcae_qt_sdk_observer_status() == 2, "Bounded stack overflow was not explicit unknown");
            for (unsigned i = 0; i < 1025; ++i) qcae_qt_sdk_observer_leave();
            { QcaeQtSdkNoexceptScope later("capacity_recovered"); }
            check(qcae_qt_sdk_observer_depth() == 0 && attempts == 1025 && qcae_qt_sdk_observer_status() == 2, "Capacity recovery leaked stack or erased unknown");
        } else if (mode == "original_qt_bad_alloc") {
            bool caught = false; QHash<Key, void*> original;
            try { QcaeFtHash::Scope scope(1); original.reserve((std::numeric_limits<qsizetype>::max)()); scope.finish(); } catch (const std::bad_alloc&) { caught = true; }
            check(caught && original.isEmpty() && QcaeFtHash::active == nullptr && qcae_qt_sdk_observer_depth() == 0, "Original Qt bad_alloc/output behavior changed");
            check(unknown == 1 && qcae_qt_sdk_observer_status() == 0, "Original failure was misreported as collector success/loss");
        } else if (mode == "actual_collector_allocation_loss") {
            qcae_qt_sdk_observer_install(nullptr, nullptr);
            c3_qt_source::Bridge bridge;
            bridge.begin("sdk8-allocation-loss");
            auto ledger = std::make_shared<qcae::ledger::OperationLedger>(qcae::ledger::Identity{"sdk8-allocation-loss", "private-doc", "private-epoch", 1});
            {
                qcae::ledger::Scope measured(ledger);
                QcaeQtSdkNoexceptScope scope("ft_hash/glyph_operation");
                fail_next_allocation = true;
                qcae_qt_sdk_observer_emit("ft_hash/node_create_move", 1, 24);
                check(!fail_next_allocation, "Actual collector allocation was not reached");
            }
            const auto snapshot = bridge.snapshot();
            std::printf("collector_snapshot=%s\n", QJsonDocument(snapshot).toJson(QJsonDocument::Compact).constData());
            check(!snapshot.value("collector_complete").toBool() && qcae_qt_sdk_observer_status() == 4, "Allocation loss became known zero or complete");
        } else return 2;
        qcae_qt_sdk_observer_install(nullptr, nullptr);
        check(qcae_qt_sdk_observer_depth() == 0, "Final depth leaked");
        std::printf("mode=%s GREEN attempts=%u entries=%u completion=%u node=%u unknown=%u sticky_status=%llu actual_active_null=%d\n", mode.c_str(), attempts.load(), entries.load(), completions.load(), nodes.load(), unknown.load(), static_cast<unsigned long long>(qcae_qt_sdk_observer_status()), QcaeFtHash::active == nullptr);
        return 0;
    } catch (const std::exception& e) { qcae_qt_sdk_observer_install(nullptr, nullptr); std::fprintf(stderr, "RED: %s\n", e.what()); return 1; }
}
