#pragma once

#include "qcae/document_info.hpp"
#include "qcae/model.hpp"

namespace qcae {
struct ModelSnapshot : Model {
    DocumentInfo info;
};
} // namespace qcae
