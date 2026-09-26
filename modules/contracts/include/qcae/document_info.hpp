#pragma once

#include "qcae/types.hpp"
#include <cstddef>

namespace qcae {
struct DocumentInfo {
    DocumentRef document;
    Revision revision{};
    std::string content_state;
    std::string name;
    std::string project_id;
    std::string saved_path;
    std::string saved_content_state;
    std::size_t material_count{};
    bool dirty{};
    bool durable{false};
};
} // namespace qcae
