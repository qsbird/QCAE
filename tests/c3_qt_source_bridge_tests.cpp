#include "c3_qt_source_bridge.hpp"
#include <QApplication>
#include <QByteArray>
#include <QFile>
#include <QPlainTextDocumentLayout>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextLayout>
#include <iostream>

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    try {
        if (argc != 2)
            throw std::runtime_error("A new source collector report path is required");
        auto& observer = c3_qt_source::bridge();
        observer.begin("source-collector-one");
#ifdef QCAE_C3_HARFBUZZ_SDK_MANIFEST
        auto& hb_observer = c3_qt_source::harfbuzz_bridge();
        hb_observer.begin("source-collector-one");
#endif
        const auto run = std::make_shared<qcae::ledger::OperationLedger>(
            qcae::ledger::Identity{"source-collector-one", "document", "epoch", 7});
        QString text;
        {
            qcae::ledger::Scope sample(run);
            text = QString::fromUtf8(QByteArray(4096, 'a'));
            text.append(QStringLiteral("梁🙂"));
            auto owned = text;
            owned.append('z');
            if (owned.size() != text.size() + 1)
                throw std::runtime_error("The source collector changed QString semantics");
            QTextDocument document;
            auto* layout = new QPlainTextDocumentLayout(&document);
            document.setDocumentLayout(layout);
            QTextCursor cursor(&document);
            cursor.insertText(text);
            const auto block = document.firstBlock();
            block.layout()->clearLayout();
            layout->ensureBlockLayout(block);
        }
        const auto first = observer.snapshot();
#ifdef QCAE_C3_HARFBUZZ_SDK_MANIFEST
        const auto hb_first = hb_observer.snapshot();
        if (!hb_first.value("collector_complete").toBool() ||
            hb_first.value("facts").toArray().isEmpty() ||
            hb_first.value("run_id") != "source-collector-one" ||
            hb_first.value("coverage_complete").toBool())
            throw std::runtime_error("Actual HarfBuzz facts or incomplete coverage were lost");
#endif
        std::cerr << "source collector complete=" << first.value("collector_complete").toBool()
                  << "; facts=" << first.value("facts").toArray().size() << "; unexpected="
                  << QJsonDocument(first.value("unexpected_sites").toArray())
                         .toJson(QJsonDocument::Compact)
                         .constData()
                  << '\n';
        if (!first.value("collector_complete").toBool() ||
            first.value("facts").toArray().isEmpty() ||
            first.value("run_id") != "source-collector-one" ||
            first.value("coverage_complete").toBool())
            throw std::runtime_error("Source facts or explicit incomplete coverage were lost");
        text.append(QString(4096, QChar('x')));
        if (observer.snapshot() != first)
            throw std::runtime_error("Unarmed Qt work leaked into the sample");
#ifdef QCAE_C3_HARFBUZZ_SDK_MANIFEST
        if (hb_observer.snapshot() != hb_first)
            throw std::runtime_error("Unarmed font shaping leaked into the sample");
        hb_observer.begin("source-collector-two");
        if (hb_observer.snapshot().value("run_id") != "source-collector-two" ||
            !hb_observer.snapshot().value("facts").toArray().isEmpty())
            throw std::runtime_error("HarfBuzz observations were reused across independent runs");
#endif
        observer.begin("source-collector-two");
        const auto second = observer.snapshot();
        if (second.value("run_id") != "source-collector-two" ||
            !second.value("facts").toArray().isEmpty())
            throw std::runtime_error("Source observations were reused across independent runs");
        auto combined = first;
#ifdef QCAE_C3_HARFBUZZ_SDK_MANIFEST
        combined.insert("harfbuzz_source_observation", hb_first);
#endif
        QFile report(QString::fromLocal8Bit(argv[1]));
        if (!report.open(QIODevice::WriteOnly) ||
            report.write(QJsonDocument(combined).toJson()) <= 0)
            throw std::runtime_error("Could not retain the actual source collector evidence");
        std::cout << "PASS: real source copies, manifest and loaded-image binding, sample "
                     "exclusion and run reset\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
