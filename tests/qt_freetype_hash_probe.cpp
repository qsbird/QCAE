// Independent headless causal checks of the pinned, insertion-scoped Qt hash
// source hooks. The fake key is layout-compatible; no font/GUI state is touched.
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>
#include <QtCore/QSet>
#include <QtCore/QString>
#include <dlfcn.h>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <map>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
struct CacheKey {
    std::uint32_t glyph;
    struct {
        std::uint32_t x;
        std::uint32_t y;
    } subPixelPosition;
    friend bool operator==(const CacheKey& a, const CacheKey& b) noexcept {
        return a.glyph == b.glyph && a.subPixelPosition.x == b.subPixelPosition.x &&
               a.subPixelPosition.y == b.subPixelPosition.y;
    }
};
std::size_t qHash(const CacheKey& key, std::size_t seed) noexcept {
    return qHashMulti(seed, key.glyph, key.subPixelPosition.x, key.subPixelPosition.y);
}
using Callback = void (*)(void*, const char*, unsigned, std::uint64_t);
using Install = void (*)(Callback, void*);
struct Counts {
    std::uint64_t calls[4]{};
    std::uint64_t bytes[4]{};
};
std::map<std::string, Counts> facts;
void collect(void*, const char* site, unsigned kind, std::uint64_t bytes) {
    if (!site || kind > 3)
        std::abort();
    auto& count = facts[site];
    ++count.calls[kind];
    count.bytes[kind] += bytes;
}
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
std::uint64_t calls(const char* site, unsigned kind) {
    return facts[site].calls[kind];
}
CacheKey key(std::uint32_t i) {
    return {i, {i % 3, i % 7}};
}
void* value(std::uint32_t i) {
    return reinterpret_cast<void*>(std::uintptr_t(i) + 1);
}
void preserved(const QHash<CacheKey, void*>& hash, std::uint32_t count) {
    require(hash.size() == count, "The original hash size changed");
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto it = hash.constFind(key(i));
        require(it != hash.cend() && *it == value(i), "The original key/value lookup changed");
    }
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fputs("Use probe output.json\n", stderr);
        return 2;
    }
    Install install{};
    try {
        static_assert(sizeof(CacheKey) == 12);
        install = reinterpret_cast<Install>(dlsym(RTLD_DEFAULT, "qcae_qt_sdk_observer_install"));
        const auto marker = reinterpret_cast<const char* (*)()>(
            dlsym(RTLD_DEFAULT, "qcae_qt_sdk_observer_manifest"));
        require(install && marker, "The linked SDK lacks the exact source bridge");
        QFile expected(qEnvironmentVariable("QCAE_FT_QT_MANIFEST"));
        require(expected.open(QIODevice::ReadOnly), "The expected manifest is missing");
        const auto actual = QJsonDocument::fromJson(marker());
        require(actual == QJsonDocument::fromJson(expected.readAll()), "SDK marker mismatch");
        install(collect, nullptr);
        QHash<CacheKey, void*> hash;
        for (std::uint32_t i = 0; i < 1000; ++i) {
            QcaeFtHash::Scope scope(1);
            hash.insert(key(i), value(i));
            scope.finish();
        }
        preserved(hash, 1000);
        require(calls("ft_hash/glyph_operation_complete", 3) == 0,
                "A compatible insertion scope was silently incomplete");
        require(calls("ft_hash/glyph_operation_complete", 0) == 1000,
                "The actual parent insertion count changed");
        require(calls("ft_hash/span_storage_relocate_bytes", 1) > 0 &&
                    calls("ft_hash/data_rehash_node_move", 1) > 0,
                "Actual storage relocation and rehash branches were not reached");
        const auto relocations = calls("ft_hash/data_rehash_node_move", 1);
        const auto child_fields = calls("ft_hash/node_create_move", 1);
        for (std::uint32_t i = 0; i < 100; ++i) {
            QcaeFtHash::Scope scope(1);
            hash.insert(key(i), value(i));
            scope.finish();
        }
        require(calls("ft_hash/node_value_assign", 1) == 100,
                "Duplicate insertion did not observe original value assignments");
        require(calls("ft_hash/node_create_move", 1) == child_fields &&
                    calls("ft_hash/data_rehash_node_move", 1) == relocations,
                "An unchanged-size update was charged as another node creation/rehash");
        const auto old = hash;
        {
            QcaeFtHash::Scope scope(1);
            hash.insert(key(1000), value(1000));
            scope.finish();
        }
        preserved(old, 1000);
        preserved(hash, 1001);
        require(calls("ft_hash/data_detach_node_copy", 1) == 1000,
                "Shared detach did not observe each original copied node exactly once");
        QSet<std::uint32_t> missing;
        for (int i = 0; i < 2; ++i) {
            QcaeFtHash::Scope scope(2);
            missing.insert(65535U);
            scope.finish();
        }
        require(missing.size() == 1 && missing.contains(65535U), "Original set output changed");
        require(calls("ft_hash/missing_operation_complete", 0) == 2 &&
                    calls("ft_hash/set_node_create_move", 1) == 1 &&
                    calls("ft_hash/set_node_existing_no_payload", 0) == 1,
                "New and duplicate empty-value set actions were not distinct");
        // A genuinely executed deep-owning key falls outside the supported type proof.
        QHash<QString, void*> unsupported;
        {
            QcaeFtHash::Scope scope(1);
            unsupported.insert(QStringLiteral("owned UTF16"), value(0));
            scope.finish();
            require(scope.unknown, "A nontrivial QString key was falsely marked covered");
        }
        require(unsupported.size() == 1, "The unsupported-type observer altered the call");
        // Bounded observer exhaustion: the original Span initialization still executes.
        QHash<CacheKey, void*> limited;
        {
            QcaeFtHash::Scope scope(1, 32);
            limited.insert(key(0), value(0));
            scope.finish();
            require(scope.unknown, "Byte-observer exhaustion was falsely marked covered");
        }
        preserved(limited, 1);
        // Qt rejects this impossible bucket count before allocating Span storage.
        // The original exception and empty cache survive the observer unchanged.
        QHash<CacheKey, void*> impossible;
        bool allocation_rejected = false;
        {
            QcaeFtHash::Scope scope(1);
            try {
                impossible.reserve((std::numeric_limits<qsizetype>::max)());
            } catch (const std::bad_alloc&) {
                allocation_rejected = true;
            }
        }
        require(allocation_rejected && impossible.isEmpty(),
                "Qt's original impossible-capacity rejection changed");
        {
            QcaeFtHash::Scope scope(1);
            scope.finish();
        }
        require(calls("ft_hash/observer_byte_limit", 3) > 0 &&
                    calls("ft_hash/unsupported_node_type", 3) > 0 &&
                    calls("ft_hash/unseen_or_multiple_node_action", 3) > 0,
                "A missing child/type/limit negative observation was hidden");
        install(nullptr, nullptr);
        QJsonArray rows;
        for (const auto& [site, count] : facts)
            rows.append(QJsonObject{{"site", QString::fromStdString(site)},
                                    {"entry_calls", QString::number(count.calls[0])},
                                    {"copy_calls", QString::number(count.calls[1])},
                                    {"copy_bytes", QString::number(count.bytes[1])},
                                    {"write_calls", QString::number(count.calls[2])},
                                    {"write_bytes", QString::number(count.bytes[2])},
                                    {"unsupported_calls", QString::number(count.calls[3])}});
        const QJsonObject report{
            {"schema", "qcae.ft-hash-source-headless/1"},
            {"passed", true},
            {"native_GUI_used", false},
            {"growth_and_span_relocation_reached", true},
            {"duplicate_value_assignment_distinct", true},
            {"COW_node_copies_exactly_once", true},
            {"set_duplicate_empty_value_distinct", true},
            {"type_limit_and_missing_child_unknown_preserved", true},
            {"Qt_impossible_capacity_bad_alloc_original_cache_empty", true},
            {"limits", "32-byte observer budget exhaustion, not a Qt allocation-size failure"},
            {"whole_copy_coverage", "unknown"},
            {"actual_sites", rows}};
        QSaveFile output(QString::fromLocal8Bit(argv[1]));
        require(output.open(QIODevice::WriteOnly), "Could not preserve diagnostic output");
        const auto bytes = QJsonDocument(report).toJson(QJsonDocument::Indented);
        require(output.write(bytes) == bytes.size() && output.commit(),
                "Could not commit diagnostic output");
        std::puts("Actual hash growth/detach/duplicates and unknown negatives passed");
        return 0;
    } catch (const std::exception& error) {
        if (install)
            install(nullptr, nullptr);
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
