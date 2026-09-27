#include "qcae/engine_host.hpp"

int main(int argc, char** argv) {
    const auto contributions = qcae::ipc::default_engine_contributions();
    return qcae_run_engine(argc, argv, contributions);
}
