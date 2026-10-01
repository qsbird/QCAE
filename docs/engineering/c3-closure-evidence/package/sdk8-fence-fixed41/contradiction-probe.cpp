// This is a synthetic ABI contract probe. It does not execute Qt observer work.
#include <QByteArray>
#include <QCoreApplication>
#include <cstdio>
#include <stdexcept>
#include <string>

static const char* fixtureBindingPath() {
    static const QByteArray path = qgetenv("QCAE_FIXTURE_BINDING");
    return path.constData();
}
#define QCAE_C3_QT_SDK_IMAGE_BINDING fixtureBindingPath()
#include QCAE_TEST_COLLECTOR_HEADER

namespace {
void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    if (argc != 5) {
        std::fputs("Use probe manifest fixture-image accept|reject report.json\n", stderr);
        return 2;
    }
    const std::string expected(argv[3]);
    try {
        c3_qt_source::Bridge collector(argv[1],
                                       QString::fromLocal8Bit(argv[2]),
                                       "qcae_qt_sdk_observer_install",
                                       "qcae_qt_sdk_observer_manifest",
                                       true);
        collector.begin("synthetic-fence-contract-fixture");
        auto report = collector.snapshot();
        report.insert("synthetic_ABI_contract_only", true);
        report.insert("actual_Qt_observer_behavior_proven", false);
        QFile output(QString::fromLocal8Bit(argv[4]));
        check(output.open(QIODevice::WriteOnly), "Could not preserve fixture snapshot");
        const auto bytes = QJsonDocument(report).toJson(QJsonDocument::Indented);
        check(output.write(bytes) == bytes.size(), "Could not write complete fixture snapshot");
        output.close();
        check(report.value("observer_failure_fence_version") == QJsonValue(1) &&
                  report.value("observer_failure_status_known") == QJsonValue(true) &&
                  report.value("observer_failure_status") == QJsonValue("0") &&
                  report.value("observer_scope_balanced") == QJsonValue(true) &&
                  report.value("collector_complete") == QJsonValue(true) &&
                  report.value("exception_boundary_complete") == QJsonValue(true),
              "The expected accepted fixture state was not reached");
        check(expected == "accept", "Fixture was accepted when rejection was required");
        std::printf("synthetic_ABI_fixture_accepted=1 exception_boundary_complete=true "
                    "status=0 scope_balanced=true snapshot=%s\n",
                    argv[4]);
        return 0;
    } catch (const std::exception& error) {
        if (expected == "reject" &&
            std::string(error.what()) == "Unsupported SDK exception fence contract") {
            std::printf("synthetic_ABI_fixture_rejected=%s\n", error.what());
            return 0;
        }
        std::fprintf(stderr, "synthetic_ABI_fixture_failure=%s\n", error.what());
        return 1;
    }
}
