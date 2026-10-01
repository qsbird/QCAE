#pragma once
#include "qcae/render_packet.hpp"
#include "qcae/operation_ledger.hpp"
#include <QByteArray>
#include <bit>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <string_view>

namespace qcae::transport {
namespace render_wire_detail {
inline constexpr std::uint64_t max_items = 500000;
struct Writer {
    QByteArray bytes;
    void append(const char* data, qsizetype size) {
        const auto prior_size = bytes.size(), prior_capacity = bytes.capacity();
        bytes.append(data, size);
        // Charge the previous payload conservatively whenever the backing buffer grows.
        ledger::add(ledger::Stage::render_encode,
                    ledger::Metric::model_copy_bytes,
                    static_cast<std::uint64_t>(size) +
                        (bytes.capacity() != prior_capacity ? prior_size : 0));
        ledger::add(ledger::Stage::render_encode, ledger::Metric::encoded_bytes, size);
    }
    void number(std::uint64_t value) {
        char encoded[8];
        for (int i = 0; i < 8; ++i)
            encoded[i] = static_cast<char>((value >> (i * 8)) & 255);
        append(encoded, 8);
    }
    void text(std::string_view value) {
        number(value.size());
        append(value.data(), static_cast<qsizetype>(value.size()));
    }
    void point(const std::array<double, 3>& value) {
        for (double coordinate : value) {
            if (!std::isfinite(coordinate))
                throw std::invalid_argument("Nonfinite render coordinate");
            number(std::bit_cast<std::uint64_t>(coordinate));
        }
    }
};
struct Reader {
    const QByteArray& bytes;
    qsizetype offset{};
    std::uint64_t number() {
        if (bytes.size() - offset < 8)
            throw std::invalid_argument("Truncated render resource");
        std::uint64_t result{};
        for (int i = 0; i < 8; ++i)
            result |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[offset++]))
                      << (i * 8);
        return result;
    }
    std::string text(bool metadata = false) {
        const auto size = number();
        if (size == 0 || size > 1024 || size > static_cast<std::uint64_t>(bytes.size() - offset))
            throw std::invalid_argument("Invalid render identity length");
        std::string value(bytes.constData() + offset, static_cast<std::size_t>(size));
        offset += static_cast<qsizetype>(size);
        // Own each string once: document/view identities are metadata; entity IDs
        // belong to the model payload. The factor covers moves and validation sets.
        ledger::add(ledger::Stage::render_decode,
                    metadata ? ledger::Metric::metadata_copy_bytes
                             : ledger::Metric::model_copy_bytes,
                    size * 8);
        if (value.find('\0') != std::string::npos)
            throw std::invalid_argument("Invalid render identity");
        return value;
    }
    std::array<double, 3> point() {
        std::array<double, 3> result;
        for (auto& coordinate : result) {
            coordinate = std::bit_cast<double>(number());
            if (!std::isfinite(coordinate))
                throw std::invalid_argument("Nonfinite render coordinate");
        }
        return result;
    }
    std::size_t count() {
        const auto value = number();
        if (value > max_items || value > static_cast<std::uint64_t>(bytes.size() - offset) / 8)
            throw std::invalid_argument("Invalid render item count");
        return static_cast<std::size_t>(value);
    }
    bool boolean() {
        const auto value = number();
        if (value > 1)
            throw std::invalid_argument("Invalid render visibility");
        return value != 0;
    }
    void finished() const {
        if (offset != bytes.size())
            throw std::invalid_argument("Trailing render resource bytes");
    }
};
inline void header(Writer& out,
                   const DocumentRef& document,
                   Revision revision,
                   const std::string& view,
                   std::uint64_t view_revision,
                   bool delta,
                   unsigned version) {
    const auto original = delta ? 0x3141544c45444351ULL : 0x3154454b43415051ULL;
    out.number(original + (static_cast<std::uint64_t>(version - 1) << 56));
    out.text(document.id.value);
    out.text(document.epoch.value);
    out.number(revision);
    out.text(view);
    out.number(view_revision);
}
inline unsigned header(Reader& in,
                       DocumentRef& document,
                       Revision& revision,
                       std::string& view,
                       std::uint64_t& view_revision,
                       bool delta) {
    const auto magic = in.number();
    const auto original = delta ? 0x3141544c45444351ULL : 0x3154454b43415051ULL;
    const auto extended = delta ? 0x3241544c45444351ULL : 0x3254454b43415051ULL;
    const auto generic = delta ? 0x3341544c45444351ULL : 0x3354454b43415051ULL;
    if (magic != original && magic != extended && magic != generic)
        throw std::invalid_argument("Unsupported render resource encoding");
    document = {DocumentId(in.text(true)), DocumentEpoch(in.text(true))};
    revision = in.number();
    view = in.text(true);
    view_revision = in.number();
    return magic == original ? 1 : magic == extended ? 2 : 3;
}
inline void point(Writer& out, const RenderPoint& value) {
    out.text(value.entity.value);
    out.point(value.position_mm);
    out.number(value.visible ? 1 : 0);
}
inline RenderPoint point(Reader& in) {
    return {EntityId(in.text()), in.point(), in.boolean()};
}
inline void geometry(Writer& out, const RenderGeometryLine& line) {
    out.text(line.entity.value);
    out.point(line.start_mm);
    out.point(line.end_mm);
}
inline RenderGeometryLine geometry(Reader& in) {
    return {EntityId(in.text()), in.point(), in.point()};
}
template <class T> Result<T> error(const std::exception& error) {
    return {Status::failed,
            std::nullopt,
            Diagnostic{ErrorCode::invalid_input, error.what(), "render_resource"}};
}
} // namespace render_wire_detail

// Version 3 adds variable-arity display cells. The original point/line versions
// remain readable and are emitted for packets that do not use generic cells.
inline unsigned render_resource_version(const QByteArray& bytes) {
    if (bytes.size() < 8)
        return 0;
    render_wire_detail::Reader in{bytes};
    const auto magic = in.number();
    if (magic == 0x3141544c45444351ULL || magic == 0x3154454b43415051ULL)
        return 1;
    if (magic == 0x3241544c45444351ULL || magic == 0x3254454b43415051ULL)
        return 2;
    if (magic == 0x3341544c45444351ULL || magic == 0x3354454b43415051ULL)
        return 3;
    return 0;
}
inline bool render_packet_requires_v2(const RenderPacket& packet) {
    for (const auto& beam : packet.beams)
        if (!beam.visible)
            return true;
    for (const auto& line : packet.geometry_lines)
        if (!line.visible)
            return true;
    return false;
}
inline bool render_delta_requires_v2(const RenderDelta& delta) {
    if (!delta.visibility.empty())
        return true;
    for (const auto& line : delta.geometry_lines)
        if (!line.line.visible)
            return true;
    return false;
}
inline unsigned render_packet_version(const RenderPacket& packet) {
    return !packet.cells.empty() ? 3 : render_packet_requires_v2(packet) ? 2 : 1;
}
inline unsigned render_delta_version(const RenderDelta& delta) {
    for (const auto& update : delta.visibility)
        if (update.primitive == RenderPrimitive::cell)
            return 3;
    return render_delta_requires_v2(delta) ? 2 : 1;
}
// This codec is shared by the engine and clients; it has no application/domain dependency.
inline QByteArray encode_render_packet(const RenderPacket& packet) {
    using namespace render_wire_detail;
    Writer out;
    const auto version = render_packet_version(packet);
    const bool extended = version >= 2;
    header(out,
           packet.document,
           packet.revision,
           packet.view_session_id,
           packet.view_revision,
           false,
           version);
    out.number(packet.points.size());
    for (const auto& value : packet.points)
        point(out, value);
    out.number(packet.beams.size());
    for (const auto& beam : packet.beams) {
        out.text(beam.entity.value);
        out.number(beam.points[0]);
        out.number(beam.points[1]);
        if (extended)
            out.number(beam.visible ? 1 : 0);
    }
    out.number(packet.geometry_lines.size());
    for (const auto& line : packet.geometry_lines) {
        geometry(out, line);
        if (extended)
            out.number(line.visible ? 1 : 0);
    }
    if (version >= 3) {
        out.number(packet.cells.size());
        for (const auto& cell : packet.cells) {
            out.text(cell.entity.value);
            out.number(static_cast<std::uint64_t>(cell.kind));
            out.number(cell.points.size());
            for (const auto index : cell.points)
                out.number(index);
            out.number(cell.visible ? 1 : 0);
        }
    }
    // All owned bytes, including header fields, were charged to the one encoded
    // payload buffer above. No separate owned metadata container is copied here.
    ledger::add(ledger::Stage::render_encode, ledger::Metric::metadata_copy_bytes, 0);
    ledger::cover(ledger::Stage::render_encode);
    return out.bytes;
}
inline QByteArray encode_render_delta(const RenderDelta& delta) {
    using namespace render_wire_detail;
    Writer out;
    const auto version = render_delta_version(delta);
    const bool extended = version >= 2;
    header(out,
           delta.document,
           delta.revision,
           delta.view_session_id,
           delta.view_revision,
           true,
           version);
    out.number(delta.base_revision);
    out.number(delta.base_view_revision);
    out.number(delta.points.size());
    for (const auto& value : delta.points) {
        out.number(value.index);
        point(out, value.point);
    }
    out.number(delta.geometry_lines.size());
    for (const auto& value : delta.geometry_lines) {
        out.number(value.index);
        geometry(out, value.line);
        if (extended)
            out.number(value.line.visible ? 1 : 0);
    }
    if (extended) {
        out.number(delta.visibility.size());
        for (const auto& value : delta.visibility) {
            out.number(static_cast<std::uint64_t>(value.primitive));
            out.number(value.index);
            out.text(value.entity.value);
            out.number(value.visible ? 1 : 0);
        }
    }
    ledger::add(ledger::Stage::render_encode, ledger::Metric::metadata_copy_bytes, 0);
    ledger::cover(ledger::Stage::render_encode);
    return out.bytes;
}
inline Result<RenderPacket> decode_render_packet(const QByteArray& bytes) {
    using namespace render_wire_detail;
    try {
        Reader in{bytes};
        RenderPacket packet;
        const auto version = header(in,
                                    packet.document,
                                    packet.revision,
                                    packet.view_session_id,
                                    packet.view_revision,
                                    false);
        const bool extended = version >= 2;
        std::set<std::string> ids;
        const auto unique = [&](const EntityId& id) {
            if (!ids.insert(id.value).second)
                throw std::invalid_argument("Duplicate render identity");
        };
        auto count = in.count();
        packet.points.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            auto value = point(in);
            unique(value.entity);
            packet.points.push_back(std::move(value));
        }
        count = in.count();
        packet.beams.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            const auto id = EntityId(in.text());
            unique(id);
            const auto a = in.number(), b = in.number();
            if (a >= packet.points.size() || b >= packet.points.size())
                throw std::invalid_argument("Invalid render beam endpoint");
            packet.beams.push_back({id,
                                    {static_cast<std::size_t>(a), static_cast<std::size_t>(b)},
                                    extended ? in.boolean() : true});
        }
        count = in.count();
        packet.geometry_lines.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            auto line = geometry(in);
            if (extended)
                line.visible = in.boolean();
            unique(line.entity);
            packet.geometry_lines.push_back(std::move(line));
        }
        std::size_t cell_indices{};
        if (version >= 3) {
            count = in.count();
            packet.cells.reserve(count);
            for (std::size_t i = 0; i < count; ++i) {
                RenderCell cell;
                cell.entity = EntityId(in.text());
                unique(cell.entity);
                const auto kind = in.number();
                if (kind > static_cast<std::uint64_t>(RenderCellKind::polygon))
                    throw std::invalid_argument("Invalid display cell kind");
                cell.kind = static_cast<RenderCellKind>(kind);
                const auto arity = in.count();
                if (arity < (cell.kind == RenderCellKind::polyline ? 2U : 3U) ||
                    arity > render_cell_point_limit || arity > max_items * 4 - cell_indices)
                    throw std::invalid_argument("Invalid display cell arity");
                cell_indices += arity;
                cell.points.reserve(arity);
                std::set<std::size_t> indices;
                for (std::size_t j = 0; j < arity; ++j) {
                    const auto index = in.number();
                    if (index >= packet.points.size() || !indices.insert(index).second)
                        throw std::invalid_argument("Invalid display cell point");
                    cell.points.push_back(static_cast<std::size_t>(index));
                }
                cell.visible = in.boolean();
                packet.cells.push_back(std::move(cell));
            }
        }
        in.finished();
        ledger::add(ledger::Stage::render_decode, ledger::Metric::decoded_bytes, bytes.size());
        ledger::add(ledger::Stage::render_decode,
                    ledger::Metric::model_copy_bytes,
                    3 * (packet.points.size() * (3 * sizeof(double) + sizeof(bool)) +
                         packet.beams.size() * (2 * sizeof(std::size_t) + sizeof(bool)) +
                         packet.geometry_lines.size() * (6 * sizeof(double) + sizeof(bool)) +
                         cell_indices * sizeof(std::size_t) +
                         packet.cells.size() * (sizeof(RenderCellKind) + sizeof(bool))));
        ledger::add(ledger::Stage::render_decode,
                    ledger::Metric::metadata_copy_bytes,
                    8 * sizeof(std::uint64_t));
        ledger::cover(ledger::Stage::render_decode);
        return {Status::success, std::move(packet), std::nullopt};
    } catch (const std::exception& error) {
        return render_wire_detail::error<RenderPacket>(error);
    }
}
inline Result<RenderDelta> decode_render_delta(const QByteArray& bytes) {
    using namespace render_wire_detail;
    try {
        Reader in{bytes};
        RenderDelta delta;
        const auto version = header(
            in, delta.document, delta.revision, delta.view_session_id, delta.view_revision, true);
        const bool extended = version >= 2;
        delta.base_revision = in.number();
        delta.base_view_revision = in.number();
        if (delta.revision < delta.base_revision || delta.view_revision < delta.base_view_revision)
            throw std::invalid_argument("Render delta rewinds its version");
        auto count = in.count();
        delta.points.reserve(count);
        std::set<std::size_t> indices;
        std::set<std::string> ids;
        for (std::size_t i = 0; i < count; ++i) {
            const auto index = in.number();
            auto value = point(in);
            if (index >= max_items || !indices.insert(static_cast<std::size_t>(index)).second ||
                !ids.insert(value.entity.value).second)
                throw std::invalid_argument("Invalid duplicate render point update");
            delta.points.push_back({static_cast<std::size_t>(index), std::move(value)});
        }
        count = in.count();
        delta.geometry_lines.reserve(count);
        indices.clear();
        for (std::size_t i = 0; i < count; ++i) {
            const auto index = in.number();
            auto line = geometry(in);
            if (extended)
                line.visible = in.boolean();
            if (index >= max_items || !indices.insert(static_cast<std::size_t>(index)).second ||
                !ids.insert(line.entity.value).second)
                throw std::invalid_argument("Invalid duplicate geometry update");
            delta.geometry_lines.push_back({static_cast<std::size_t>(index), std::move(line)});
        }
        if (extended) {
            count = in.count();
            delta.visibility.reserve(count);
            std::set<std::pair<std::uint64_t, std::size_t>> updates;
            std::set<std::string> visibility_ids;
            for (std::size_t i = 0; i < count; ++i) {
                const auto primitive = in.number(), index = in.number();
                const auto id = EntityId(in.text());
                const auto visible = in.boolean();
                if (primitive > static_cast<std::uint64_t>(version >= 3
                                                               ? RenderPrimitive::cell
                                                               : RenderPrimitive::geometry_line) ||
                    index >= max_items || !updates.emplace(primitive, index).second ||
                    !visibility_ids.insert(id.value).second)
                    throw std::invalid_argument("Invalid duplicate visibility update");
                delta.visibility.push_back({static_cast<RenderPrimitive>(primitive),
                                            static_cast<std::size_t>(index),
                                            id,
                                            visible});
            }
        }
        in.finished();
        ledger::add(ledger::Stage::render_decode, ledger::Metric::decoded_bytes, bytes.size());
        ledger::add(ledger::Stage::render_decode,
                    ledger::Metric::model_copy_bytes,
                    3 * (delta.points.size() * (3 * sizeof(double) + sizeof(bool)) +
                         delta.geometry_lines.size() * (6 * sizeof(double) + sizeof(bool)) +
                         delta.visibility.size() * sizeof(bool)));
        ledger::add(ledger::Stage::render_decode,
                    ledger::Metric::metadata_copy_bytes,
                    8 * sizeof(std::uint64_t) + (delta.points.size() + delta.geometry_lines.size() +
                                                 delta.visibility.size()) *
                                                    4 * sizeof(std::size_t));
        ledger::cover(ledger::Stage::render_decode);
        return {Status::success, std::move(delta), std::nullopt};
    } catch (const std::exception& error) {
        return render_wire_detail::error<RenderDelta>(error);
    }
}
} // namespace qcae::transport
