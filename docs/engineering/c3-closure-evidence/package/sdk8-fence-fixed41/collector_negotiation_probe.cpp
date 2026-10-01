// Candidate probe: compile/run separately against each SDK; never mix Qt images.
#include "collector-optional-status.hpp"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QLibrary>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace {
void check(bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(message);
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc < 4 || argc > 5) {
        std::fputs("Use probe mode manifest core-image [expected-rejection-message]\n", stderr);
        return 2;
    }
    const std::string mode(argv[1]);
    try {
        c3_qt_source::Bridge observer(argv[2],
                                      QString::fromLocal8Bit(argv[3]),
                                      "qcae_qt_sdk_observer_install",
                                      "qcae_qt_sdk_observer_manifest",
                                      true);
        check(mode != "expect_reject", "Invalid manifest/image/symbol fixture was accepted");
        observer.begin("candidate-negotiation-one");
        QLibrary core(QString::fromLocal8Bit(argv[3]));
        check(core.load(), "Explicit SDK image could not be loaded");
        using Enter = void (*)(const char*);
        using Emit = void (*)(const char*, unsigned, std::uint64_t);
        using Leave = void (*)();
        using Loss = void (*)() noexcept;
        const auto enter = reinterpret_cast<Enter>(core.resolve("qcae_qt_sdk_observer_enter"));
        const auto emitObservation = reinterpret_cast<Emit>(core.resolve("qcae_qt_sdk_observer_emit"));
        const auto leave = reinterpret_cast<Leave>(core.resolve("qcae_qt_sdk_observer_leave"));
        check(enter && emitObservation && leave, "Original observer ABI is missing");
        QFile manifest(QString::fromLocal8Bit(argv[2]));
        check(manifest.open(QIODevice::ReadOnly), "Manifest input could not be read");
        const auto sites =
            QJsonDocument::fromJson(manifest.readAll()).object().value("sites").toArray();
        check(!sites.isEmpty(), "Manifest has no observed site");
        const auto site = sites.at(0).toObject().value("site").toString().toStdString();
        auto run = std::make_shared<qcae::ledger::OperationLedger>(
            qcae::ledger::Identity{"candidate-negotiation-one", "doc", "epoch", 7});
        {
            qcae::ledger::Scope scope(run);
            enter(site.c_str());
            emitObservation(site.c_str(), 2, 17);
            leave();
        }
        auto first = observer.snapshot();
        check(first.value("collector_complete") == QJsonValue(true),
              "Normal finite collection failed");
        check(!first.value("facts").toArray().isEmpty(), "Actual sample did not reach collection");
        check(first.value("coverage_complete") == QJsonValue(false),
              "Whole coverage was falsely closed");
        if (mode == "legacy_normal") {
            check(first.value("observer_failure_status").isNull() &&
                      first.value("observer_failure_status_known") == QJsonValue(false) &&
                      first.value("exception_boundary_complete") == QJsonValue(false),
                  "Legacy unavailable status became known zero or exception-safe");
        } else {
            check(first.value("observer_failure_status") == QJsonValue("0") &&
                      first.value("observer_failure_status_known") == QJsonValue(true) &&
                      first.value("exception_boundary_complete") == QJsonValue(true),
                  "Manifest/image-bound v1 status was not used");
            if (mode == "v1_sticky_loss") {
                const auto loss =
                    reinterpret_cast<Loss>(core.resolve("qcae_qt_sdk_observer_collector_loss"));
                check(loss != nullptr, "V1 loss symbol is missing");
                loss();
                first = observer.snapshot();
                check(first.value("collector_complete") == QJsonValue(false) &&
                          first.value("observer_failure_status") == QJsonValue("4") &&
                          first.value("exception_boundary_complete") == QJsonValue(false),
                      "A real sticky collector loss was reported complete");
                observer.begin("candidate-negotiation-two");
                check(observer.snapshot().value("collector_complete") == QJsonValue(false),
                      "Beginning another run erased process-sticky loss");
            } else {
                check(mode == "v1_normal", "Unknown candidate probe mode");
            }
        }
        std::printf("candidate_mode=%s result=%s\n",
                    mode.c_str(),
                    QJsonDocument(first).toJson(QJsonDocument::Compact).constData());
        return 0;
    } catch (const std::exception& e) {
        if (mode == "expect_reject" && argc == 5 &&
            std::string(e.what()).find(argv[4]) != std::string::npos) {
            std::printf("candidate_rejected=%s\n", e.what());
            return 0;
        }
        std::fprintf(stderr, "candidate_failure=%s\n", e.what());
        return 1;
    }
}
