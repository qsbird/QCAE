#include "qcae/engine_host.hpp"
#include "engine_test_contribution.hpp"

int main(int argc, char** argv) {
    auto contributions = qcae::ipc::default_engine_contributions();
    contributions.push_back(contribution_test::contribution());
    return qcae_run_engine(argc, argv, contributions);
}
