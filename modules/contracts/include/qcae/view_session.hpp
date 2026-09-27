#pragma once
#include "qcae/types.hpp"
#include <vector>

namespace qcae {
struct ViewSession {
    std::string id;
    DocumentRef document;
    Revision model_revision{};
    std::uint64_t view_revision{1};
    std::vector<EntityId> hidden_ids;
    std::string camera_fingerprint;
};
} // namespace qcae
