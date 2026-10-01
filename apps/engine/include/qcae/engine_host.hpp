#pragma once
#include "qcae/engine_contributions.hpp"

int qcae_run_engine(int argc, char** argv);

// The production executable and test-only contributed executable run this same
// socket, lifecycle and persistence host with different trusted startup inputs.
int qcae_run_engine(int argc,
                    char** argv,
                    std::span<const qcae::ipc::EngineContribution> contributions,
                    qcae::Limits limits = {});
