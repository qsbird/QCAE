#include "qcae/json_ledger.hpp"

#include <QApplication>
#include <QPlainTextEdit>
#include <QTextDocument>
#include <iostream>
#include <stdexcept>

namespace {
void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
auto observe(const char* name) {
    auto operation =
        std::make_shared<qcae::ledger::OperationLedger>(qcae::ledger::Identity{name, {}, {}, 0});
    qcae::ledger::activate(operation);
    return operation;
}
auto copied(const qcae::ledger::OperationLedger& operation) {
    return operation.snapshot()
        .values[static_cast<std::size_t>(qcae::ledger::Stage::socket_receive)]
               [static_cast<std::size_t>(qcae::ledger::Metric::library_internal_copy_bytes)];
}
void append(QPlainTextEdit& log,
            qcae::transport::json_ledger::PlainTextAppendCopies& copies,
            const QString& text) {
    auto* document = log.document();
    const bool empty = document->isEmpty();
    const auto before = document->characterCount();
    log.appendPlainText(text);
    copies.append(QStringView(text), empty, before, document->characterCount());
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    using qcae::transport::json_ledger::PlainTextAppendCopies;
    try {
        QPlainTextEdit log;
        PlainTextAppendCopies copies;
        check(log.document()->isEmpty() && log.document()->characterCount() == 1,
              "An empty Qt document must have its initial paragraph separator");
        append(log, copies, QStringLiteral("ab")); // Warmup is tracked outside a sample.
        check(log.document()->characterCount() == 3, "Real warmup document shape changed");
        const auto sample = observe("real-plain-text");
        append(log, copies, QStringLiteral("c"));
        check(log.toPlainText() == QStringLiteral("ab\nc") &&
                  log.document()->characterCount() == 5 && copied(*sample) == 18,
              "The runtime append disagrees with guarded conservative prefix counting");
        append(log, copies, QString{});
        check(log.document()->characterCount() == 6 && copied(*sample) == 30,
              "Appending an empty line to nonempty text must retain its separator copy");
        append(log, copies, QString(4096, QLatin1Char('x')));
        check(log.document()->characterCount() == 4103 && copied(*sample).has_value() &&
                  *copied(*sample) > 8192,
              "Long real text lost initialized-prefix or owned-input coverage");
        log.clear();
        append(log, copies, QStringLiteral("after clear"));
        check(!copied(*sample), "An untracked clear must invalidate observer coverage");
        qcae::ledger::activate({});

        QPlainTextEdit multiline;
        PlainTextAppendCopies multiline_copies;
        const auto unsupported = observe("real-multiline-text");
        append(multiline, multiline_copies, QStringLiteral("a\nb"));
        check(multiline.toPlainText() == QStringLiteral("a\nb") && !copied(*unsupported),
              "Real multiline behavior must preserve text while marking the audit unknown");
        qcae::ledger::activate({});
        std::cout << "PASS: real QPlainTextEdit warmup, append/empty/long shape and guards; "
                     "font/layout/paint remain unknown\n";
        return 0;
    } catch (const std::exception& error) {
        qcae::ledger::activate({});
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
