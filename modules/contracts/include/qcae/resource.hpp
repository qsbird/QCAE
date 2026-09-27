#pragma once
#include "qcae/types.hpp"
#include <cstdint>
#include <string>

namespace qcae {
inline constexpr std::uint32_t resource_chunk_bytes = 128 * 1024;
inline constexpr std::uint64_t resource_max_bytes = 16 * 1024 * 1024;
inline constexpr std::uint32_t resource_max_wire_bytes = 256 * 1024;

struct ResourceVersion {
    DocumentRef document;
    Revision revision{};
    std::string view_session_id;
    std::uint64_t view_revision{};
};
inline bool same_resource_version(const ResourceVersion& a, const ResourceVersion& b) {
    return a.document.id == b.document.id && a.document.epoch == b.document.epoch &&
           a.revision == b.revision && a.view_session_id == b.view_session_id &&
           a.view_revision == b.view_revision;
}
// Immutable content identity. Storage leases and transport buffers are adapter concerns.
struct ResourceManifest {
    std::string resource_id;
    ResourceVersion version;
    std::uint64_t byte_length{};
    std::uint32_t chunk_bytes{resource_chunk_bytes};
    std::uint32_t chunk_count{};
    std::string sha256;
    std::string media_type;
};
} // namespace qcae
