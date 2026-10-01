#include <QApplication>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLibrary>
#include <QLibraryInfo>
#include <QPlainTextDocumentLayout>
#include <QPlainTextEdit>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextLayout>
#include <QTreeWidget>
#include <cstdint>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>

namespace {
using Callback = void (*)(void*, const char*, unsigned, std::uint64_t);
using Install = void (*)(Callback, void*);
using Manifest = const char* (*)();
struct Fact {
    std::uint64_t entries{}, copies{}, writes{}, unsupported{};
};
struct Collector {
    std::mutex mutex;
    std::map<std::string, Fact> facts;
    bool exhausted{};
};
void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
void callback(void* context, const char* site, unsigned kind, std::uint64_t bytes) noexcept {
    auto& collector = *static_cast<Collector*>(context);
    try {
        std::lock_guard lock(collector.mutex);
        if (!site || collector.facts.size() >= 4096 || kind > 3) {
            collector.exhausted = true;
            return;
        }
        auto& fact = collector.facts[site];
        auto& total = kind == 0   ? fact.entries
                      : kind == 1 ? fact.copies
                      : kind == 2 ? fact.writes
                                  : fact.unsupported;
        const auto increment = kind == 0 || kind == 3 ? 1 : bytes;
        if (increment > std::numeric_limits<std::uint64_t>::max() - total)
            collector.exhausted = true;
        else
            total += increment;
    } catch (...) {
        collector.exhausted = true;
    }
}
std::uint64_t entries(const Collector& collector, const std::string& name) {
    std::uint64_t total{};
    for (const auto& [site, fact] : collector.facts)
        if (site.find(name) != std::string::npos)
            total += fact.entries;
    return total;
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    Install install{};
    try {
        check(argc == 3, "Usage: qt_source_copy_probe expected-manifest.json report.json");
        QFile manifest_file(QString::fromLocal8Bit(argv[1]));
        check(manifest_file.open(QIODevice::ReadOnly), "The frozen observer manifest is missing");
        const auto expected = QJsonDocument::fromJson(manifest_file.readAll());
        check(expected.isObject(), "The frozen observer manifest is not a JSON object");
        const auto library_path = QLibraryInfo::path(QLibraryInfo::LibrariesPath) +
                                  QStringLiteral("/QtCore.framework/Versions/A/QtCore");
        QLibrary core(library_path);
        check(core.load(), "Could not load the actual selected QtCore framework");
        install = reinterpret_cast<Install>(core.resolve("qcae_qt_sdk_observer_install"));
        const auto manifest =
            reinterpret_cast<Manifest>(core.resolve("qcae_qt_sdk_observer_manifest"));
        check(install && manifest, "This QtCore has no exact-source observer bridge");
        const auto actual = QJsonDocument::fromJson(QByteArray(manifest()));
        check(actual.isObject() && actual.object() == expected.object() &&
                  actual.object().value("qt_version") == QLatin1String("6.11.1"),
              "The loaded SDK does not match the exact source/patch manifest");
        Collector collector;
        install(callback, &collector);
        {
            // ensureBlockLayout is nonvirtual: the source observer must catch
            // its actual layoutBlock call that public overrides cannot see.
            QTextDocument document;
            auto* layout = new QPlainTextDocumentLayout(&document);
            document.setDocumentLayout(layout);
            QTextCursor cursor(&document);
            cursor.insertText(QStringLiteral("梁 αβ🙂"));
            const auto block = document.firstBlock();
            check(block.text() == QStringLiteral("梁 αβ🙂"), "Observed Unicode text changed");
            block.layout()->clearLayout();
            const auto before = entries(collector, "QPlainTextDocumentLayout::layoutBlock");
            layout->ensureBlockLayout(block);
            check(entries(collector, "QPlainTextDocumentLayout::layoutBlock") == before + 1,
                  "The nonvirtual layout path did not report its real source call");

            QPlainTextEdit log;
            log.resize(400, 240);
            log.setReadOnly(true);
            log.appendPlainText(QStringLiteral("Revision 8"));
            log.appendPlainText(QStringLiteral("Steel E = 200000 MPa"));
            QImage image(log.size(), QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::transparent);
            log.render(&image);
            check(log.toPlainText() == QStringLiteral("Revision 8\nSteel E = 200000 MPa"),
                  "Observed log contents changed");
            QTreeWidget tree;
            tree.resize(400, 240);
            tree.setHeaderLabel(QStringLiteral("Entities"));
            new QTreeWidgetItem(&tree, {QStringLiteral("Steel")});
            new QTreeWidgetItem(&tree, {QStringLiteral("梁 αβ🙂")});
            tree.render(&image);
        }
        install(nullptr, nullptr);
        check(!collector.exhausted && entries(collector, "QTextBlock::text") > 0 &&
                  entries(collector, "QTextEngine::validate") > 0 &&
                  entries(collector, "QTextLayout::beginLayout") > 0 &&
                  entries(collector, "QStyledItemDelegate::paint") > 0,
              "Actual SDK text/layout/tree calls were not observed");
        std::uint64_t copied{}, written{}, unsupported{};
        QJsonArray facts;
        for (const auto& [site, fact] : collector.facts) {
            copied += fact.copies;
            written += fact.writes;
            unsupported += fact.unsupported;
            facts.append(QJsonObject{{"site", QString::fromStdString(site)},
                                     {"actual_entries", QString::number(fact.entries)},
                                     {"copy_upper_bound_bytes", QString::number(fact.copies)},
                                     {"write_upper_bound_bytes", QString::number(fact.writes)},
                                     {"unsupported_calls", QString::number(fact.unsupported)}});
        }
        check(copied > 0 && written > 0 && unsupported > 0,
              "Real owned bytes or retained external shaper gaps were lost");
        const QJsonObject report{{"probe_passed", true},
                                 {"whole_pipeline_owned_copy_coverage", "unknown"},
                                 {"coverage_complete", false},
                                 {"qt_core_loaded_path", core.fileName()},
                                 {"observer_manifest", actual.object()},
                                 {"facts", facts}};
        QFile report_file(QString::fromLocal8Bit(argv[2]));
        check(report_file.open(QIODevice::WriteOnly), "Could not preserve SDK probe evidence");
        check(report_file.write(QJsonDocument(report).toJson(QJsonDocument::Indented)) > 0,
              "Could not write SDK probe evidence");
        std::cout << "PASS: exact loaded source bridge, real nonvirtual layout, Unicode, "
                     "paint/tree copy facts; remaining SDK gaps retained\n";
        return 0;
    } catch (const std::exception& error) {
        if (install)
            install(nullptr, nullptr);
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
