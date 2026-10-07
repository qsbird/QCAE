#pragma once
#include "qcae/model_codec.hpp"
#include <span>
#include <string_view>

namespace qcae::ipc::detail {
// Additional MYSTRAN 19 compatibility gate for a semantically verified codec
// bundle. It observes exact published bytes and never rewrites the input/profile.
void validate_mystran_input(std::string_view root_resource, std::span<const TextResource>);
} // namespace qcae::ipc::detail
