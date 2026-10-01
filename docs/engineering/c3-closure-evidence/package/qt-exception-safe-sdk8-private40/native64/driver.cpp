// Reuse the exact frozen 64-case driver and source collector once. The SDK8
// callback fence catches collector exceptions; its allocation-free sticky
// status is mandatory in this private collector's final completeness gate.
#include "store-driver-entry-adapter.cpp"
int main(int argc, char** argv) {
    const auto status = reinterpret_cast<std::uint64_t (*)()>(dlsym(RTLD_DEFAULT, "qcae_qt_sdk_observer_status"));
    if (!status) { std::fputs("SDK8 sticky failure status missing\n", stderr); return 2; }
    const int original = qcae_sdk8_original_store_main(argc, argv);
    try {
        const auto output = qEnvironmentVariable("QCAE_FT_SOURCE_REPORT");
        QFile f(output);
        check(f.open(QIODevice::ReadOnly), "Private source report missing");
        auto report = QJsonDocument::fromJson(f.readAll()).object();
        f.close();
        const auto loss = status();
        report.insert("observer_failure_status", QString::number(loss));
        report.insert("collector_complete", original == 0 && loss == 0 && report.value("source_dispatch_valid").toBool());
        report.insert("collector_loss_is_unknown", true);
        report.insert("coverage_complete", false);
        writeReport(output, report);
        std::printf("SDK8_native64 original_exit=%d sticky_status=%llu collector_complete=%d\n", original, static_cast<unsigned long long>(loss), report.value("collector_complete").toBool());
        return original || loss ? 1 : 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 2; }
}
