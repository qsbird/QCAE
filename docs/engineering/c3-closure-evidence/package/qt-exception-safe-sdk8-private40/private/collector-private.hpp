#pragma once
#ifdef QCAE_C3_QT_SDK_MANIFEST
#include "qcae/operation_ledger.hpp"
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLibrary>
#include <QLibraryInfo>
#include <cstdint>
#include <atomic>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace c3_qt_source {
// Raw SDK source observations remain separate from public API upper bounds.
// Combining them without a per-site deduplication proof would count copies twice.
class Bridge {
    using Callback = void (*)(void*, const char*, unsigned, std::uint64_t);
    using Install = void (*)(Callback, void*);
    using Manifest = const char* (*)();
    using Status = std::uint64_t (*)();
    using Loss = void (*)();
    struct Fact {
        std::uint64_t entries{}, copies{}, writes{}, unsupported{};
    };
    QLibrary core_;
    Install install_{};
    Status status_{};
    Loss loss_{};
    QJsonObject manifest_;
    bool qt_{};
    std::set<std::string> sites_;
    std::string run_;
    std::map<std::string, Fact> facts_;
    std::set<std::string> unexpected_sites_;
    std::mutex mutex_;
    std::atomic<bool> exhausted_{};

    static void
    observe(void* context, const char* site, unsigned kind, std::uint64_t bytes) noexcept {
        auto& self = *static_cast<Bridge*>(context);
        try {
            const auto ledger = qcae::ledger::current();
            if (!ledger)
                return;
            std::lock_guard lock(self.mutex_);
            if (ledger->identity().run_id != self.run_)
                return;
            if (!site || !self.sites_.contains(site) || kind > 3) {
                self.exhausted_ = true;
                if (site && self.unexpected_sites_.size() < 4096)
                    self.unexpected_sites_.insert(site);
                return;
            }
            auto& fact = self.facts_[site];
            auto& total = kind == 0   ? fact.entries
                          : kind == 1 ? fact.copies
                          : kind == 2 ? fact.writes
                                      : fact.unsupported;
            const auto increment = kind == 0 || kind == 3 ? 1 : bytes;
            if (increment > std::numeric_limits<std::uint64_t>::max() - total)
                self.exhausted_ = true;
            else
                total += increment;
        } catch (...) {
            self.exhausted_ = true;
            if (self.loss_) self.loss_();
        }
    }

    QJsonArray loaded_qt_images() const {
        QJsonArray images;
#ifdef __APPLE__
        const auto prefix = QLibraryInfo::path(QLibraryInfo::LibrariesPath) + '/';
        for (std::uint32_t index = 0; index < _dyld_image_count(); ++index) {
            const auto path = QString::fromLocal8Bit(_dyld_get_image_name(index));
            if (path.contains("/Qt") && path.contains(".framework/Versions/")) {
                if (!path.startsWith(prefix))
                    throw std::runtime_error("A second Qt framework installation is loaded");
                images.append(path);
            }
        }
        if (images.isEmpty())
            throw std::runtime_error("No loaded Qt framework image was verified");
#endif
        return images;
    }

    QJsonArray loaded_harfbuzz_images() const {
        QJsonArray images;
#ifdef __APPLE__
        const auto expected = QFileInfo(core_.fileName()).canonicalFilePath();
        for (std::uint32_t index = 0; index < _dyld_image_count(); ++index) {
            const auto path = QString::fromLocal8Bit(_dyld_get_image_name(index));
            if (QFileInfo(path).fileName().startsWith("libharfbuzz")) {
                if (QFileInfo(path).canonicalFilePath() != expected)
                    throw std::runtime_error("A different HarfBuzz library is loaded");
                images.append(path);
            }
        }
        if (images.isEmpty())
            throw std::runtime_error("No loaded HarfBuzz image was verified");
#endif
        return images;
    }

  public:
    Bridge()
        : Bridge(QCAE_C3_QT_SDK_MANIFEST,
                 QLibraryInfo::path(QLibraryInfo::LibrariesPath) +
                     "/QtCore.framework/Versions/A/QtCore",
                 "qcae_qt_sdk_observer_install",
                 "qcae_qt_sdk_observer_manifest",
                 true) {}
    Bridge(const char* manifest_path,
           const QString& library_path,
           const char* install_symbol,
           const char* manifest_symbol,
           bool qt)
        : qt_(qt) {
        QFile expected(QString::fromUtf8(manifest_path));
        if (!expected.open(QIODevice::ReadOnly))
            throw std::runtime_error("The frozen exact-source SDK manifest is missing");
        const auto expected_json = QJsonDocument::fromJson(expected.readAll());
        core_.setFileName(library_path);
        if (!core_.load())
            throw std::runtime_error("The selected SDK library could not be loaded");
        install_ = reinterpret_cast<Install>(core_.resolve(install_symbol));
        const auto manifest = reinterpret_cast<Manifest>(core_.resolve(manifest_symbol));
        if (qt_) {
            status_ = reinterpret_cast<Status>(core_.resolve("qcae_qt_sdk_observer_status"));
            loss_ = reinterpret_cast<Loss>(core_.resolve("qcae_qt_sdk_observer_collector_loss"));
            if (!status_ || !loss_) throw std::runtime_error("SDK8 failure status bridge is missing");
        }
        if (!install_ || !manifest)
            throw std::runtime_error("The loaded SDK has no source observer bridge");
        const auto actual = QJsonDocument::fromJson(QByteArray(manifest()));
        if (!actual.isObject() || !expected_json.isObject() ||
            actual.object() != expected_json.object())
            throw std::runtime_error("The loaded SDK observer does not match its frozen manifest");
        manifest_ = actual.object();
        for (const auto site : manifest_.value("sites").toArray())
            sites_.insert(site.toObject().value("site").toString().toStdString());
        if (sites_.empty())
            throw std::runtime_error("The source observer manifest contains no audited sites");
        if (qt_)
            (void)loaded_qt_images();
        else
            (void)loaded_harfbuzz_images();
        install_(observe, this);
    }
    ~Bridge() noexcept {
        try { if (install_) install_(nullptr, nullptr); }
        catch (...) { exhausted_ = true; if (loss_) loss_(); }
    }
    void begin(const std::string& run) {
        std::lock_guard lock(mutex_);
        run_ = run;
        facts_.clear();
        unexpected_sites_.clear();
        exhausted_ = false;
    }
    QJsonObject snapshot() {
        if (qcae::ledger::current())
            throw std::runtime_error(
                "Source observer report must be constructed outside the sample");
        std::lock_guard lock(mutex_);
        QJsonArray facts;
        QJsonArray unexpected;
        for (const auto& site : unexpected_sites_)
            unexpected.append(QString::fromStdString(site));
        for (const auto& [site, fact] : facts_)
            facts.append(QJsonObject{{"site", QString::fromStdString(site)},
                                     {"actual_entries", QString::number(fact.entries)},
                                     {"copy_upper_bound_bytes", QString::number(fact.copies)},
                                     {"write_upper_bound_bytes", QString::number(fact.writes)},
                                     {"unsupported_calls", QString::number(fact.unsupported)}});
        QJsonObject result{{"run_id", QString::fromStdString(run_)},
                           {"collector_complete", !exhausted_ && (!status_ || status_() == 0)},
                           {"observer_failure_status", QString::number(status_ ? status_() : 0)},
                           {"collector_loss_is_unknown", true},
                           {"unexpected_sites", unexpected},
                           {"coverage_complete", false},
                           {"whole_pipeline_owned_copy_coverage", "unknown"},
                           {"included_in_contract_total", false},
                           {"observer_manifest", manifest_},
                           {"facts", facts}};
        result.insert(qt_ ? "qt_core_loaded_path" : "harfbuzz_loaded_path", core_.fileName());
        result.insert(qt_ ? "loaded_qt_images" : "loaded_harfbuzz_images",
                      qt_ ? loaded_qt_images() : loaded_harfbuzz_images());
        return result;
    }
};
inline Bridge& bridge() {
    static Bridge value;
    return value;
}
#if defined(QCAE_C3_HARFBUZZ_SDK_MANIFEST) && defined(QCAE_C3_HARFBUZZ_LIBRARY)
inline Bridge& harfbuzz_bridge() {
    static Bridge value(QCAE_C3_HARFBUZZ_SDK_MANIFEST,
                        QStringLiteral(QCAE_C3_HARFBUZZ_LIBRARY),
                        "qcae_hb_sdk_observer_install",
                        "qcae_hb_sdk_observer_manifest",
                        false);
    return value;
}
#endif
} // namespace c3_qt_source
#endif
