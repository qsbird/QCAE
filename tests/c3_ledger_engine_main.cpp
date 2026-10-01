#include "qcae/engine_host.hpp"
#include "c3_ledger_contribution.hpp"
#include "c3_qt_source_bridge.hpp"
#include <iostream>

int main(int argc, char** argv) {
#ifdef QCAE_C3_QT_SDK_MANIFEST
    try {
        (void)c3_qt_source::bridge();
    } catch (const std::exception& error) {
        std::cerr << "Qt source observer initialization failed: " << error.what() << '\n';
        return 5;
    }
#endif
    auto contributions = qcae::ipc::default_engine_contributions();
    contributions.push_back(c3_ledger_test::contribution());
    qcae::Limits limits;
    limits.max_entities = 250000;
    return qcae_run_engine(argc, argv, contributions, limits);
}
