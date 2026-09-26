#pragma once
#include "qcae/model.hpp"
#include "qcae/state_codec.hpp"
#include <optional>
#include <string>
#include <vector>

namespace qcae {
struct ModelDeltaRecord {
    std::uint64_t field{};
    std::uint64_t index{};
    std::optional<std::string> before;
    std::optional<std::string> after;
};
struct ModelDelta {
    std::vector<ModelDeltaRecord> records;
};
ModelDelta model_delta(const Model& before, const Model& after);
Model apply_model_delta(const Model& model, const ModelDelta& delta, bool forward);
void write_model_delta(state_codec::Writer&, const ModelDelta&);
ModelDelta read_model_delta(state_codec::Reader&);
} // namespace qcae
