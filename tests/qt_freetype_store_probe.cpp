// Bounded source-observed diagnostic. Original fixed behavior cases execute
// once. Observer-owned buffers are not presented as original Qt payload copies.
#include <dlfcn.h>
#include <map>
#include <mutex>
#include <set>
#include <string>

#define main qcae_fixed_font_probe_main
#include "qt_font_backend_behavior_probe.cpp"
#undef main

namespace {
using SourceCallback = void (*)(void*, const char*, unsigned, std::uint64_t);
using SourceInstall = void (*)(SourceCallback, void*);
using SourceManifest = const char* (*)();
struct Counts {
    std::array<std::uint64_t, 4> calls{};
    std::array<std::uint64_t, 4> bytes{};
};
struct Facts {
    std::mutex mutex;
    std::map<std::string, Counts> rows;
    std::set<std::string> declared;
    std::set<std::string> unexpected;
    bool malformed{};
};
void collect(void* opaque, const char* site, unsigned kind, std::uint64_t bytes) {
    auto& facts = *static_cast<Facts*>(opaque);
    const std::lock_guard guard(facts.mutex);
    if (!site || kind > 3) {
        facts.malformed = true;
        return;
    }
    if (!facts.declared.contains(site))
        facts.unexpected.insert(site);
    auto& row = facts.rows[site];
    ++row.calls[kind];
    row.bytes[kind] += bytes;
}
QJsonObject factsJson(const Facts& facts, const QJsonObject& manifest) {
    QJsonArray rows, unexpected;
    for (const auto& [site, values] : facts.rows) {
        rows.append(QJsonObject{{"site", QString::fromStdString(site)},
                                {"entry_calls", QString::number(values.calls[0])},
                                {"copy_calls", QString::number(values.calls[1])},
                                {"copy_bytes", QString::number(values.bytes[1])},
                                {"write_calls", QString::number(values.calls[2])},
                                {"write_bytes", QString::number(values.bytes[2])},
                                {"unsupported_calls", QString::number(values.calls[3])}});
    }
    for (const auto& site : facts.unexpected)
        unexpected.append(QString::fromStdString(site));
    return {{"schema", "qcae.qt-ft-owned-source-diagnostic/1"},
            {"manifest", manifest},
            {"actual_sites", rows},
            {"unexpected_sites", unexpected},
            {"malformed_callback", facts.malformed},
            {"source_dispatch_valid", !facts.malformed && facts.unexpected.empty()},
            {"coverage_complete", false},
            {"FreeType_internal_copy_coverage", "unknown"},
            {"product_default_changed", false},
            {"observer_buffers_excluded_from_original_source_payload", true},
            {"contract_total_contribution", "none; independent diagnostic facts"}};
}
} // namespace

int main(int argc, char** argv) {
    const auto manifest_path = qEnvironmentVariable("QCAE_FT_QT_MANIFEST");
    const auto output = qEnvironmentVariable("QCAE_FT_SOURCE_REPORT");
    SourceInstall install{};
    try {
        check(!manifest_path.isEmpty() && !output.isEmpty(),
              "Set QCAE_FT_QT_MANIFEST and QCAE_FT_SOURCE_REPORT");
        install =
            reinterpret_cast<SourceInstall>(dlsym(RTLD_DEFAULT, "qcae_qt_sdk_observer_install"));
        const auto marker =
            reinterpret_cast<SourceManifest>(dlsym(RTLD_DEFAULT, "qcae_qt_sdk_observer_manifest"));
        check(install && marker, "The loaded Qt has no source-observer bridge");
        QFile file(manifest_path);
        check(file.open(QIODevice::ReadOnly), "The exact expected manifest is missing");
        const auto bytes = file.readAll();
        const auto expected = QJsonDocument::fromJson(bytes);
        const auto actual = QJsonDocument::fromJson(marker());
        check(expected.isObject() && expected == actual,
              "The loaded Qt marker is different from the expected source manifest");
        Facts facts;
        for (const auto& site : actual.object().value("sites").toArray())
            facts.declared.insert(site.toObject().value("site").toString().toStdString());
        install(collect, &facts);
        const auto result = qcae_fixed_font_probe_main(argc, argv);
        install(nullptr, nullptr);
        auto report = factsJson(facts, actual.object());
        report.insert("manifest_sha256", sha256(bytes));
        report.insert("fixed_probe_exit_code", result);
        writeReport(output, report);
        return result || facts.malformed || !facts.unexpected.empty() ? 1 : 0;
    } catch (const std::exception& error) {
        if (install)
            install(nullptr, nullptr);
        std::fprintf(stderr, "%s\n", error.what());
        return 2;
    }
}
