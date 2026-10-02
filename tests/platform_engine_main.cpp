#include "qcae/engine_host.hpp"

// The package remains compiled ON; this trusted startup input installs only common services.
int main(int argc, char** argv) {
    const auto defaults = qcae::ipc::default_engine_contributions();
    for (const auto& contribution : defaults)
        if (contribution.id == "qcae.model")
            return qcae_run_engine(
                argc, argv, std::span<const qcae::ipc::EngineContribution>(&contribution, 1));
    return 1;
}
