#pragma once
#include "qcae/render_packet.hpp"
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
    void number(std::uint64_t value) {
        for (int i = 0; i < 8; ++i)
            bytes.append(static_cast<char>((value >> (i * 8)) & 255));
    }
    void text(std::string_view value) {
        number(value.size());
        bytes.append(value.data(), static_cast<qsizetype>(value.size()));
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
    std::string text() {
        const auto size = number();
        if (size == 0 || size > 1024 || size > static_cast<std::uint64_t>(bytes.size() - offset))
            throw std::invalid_argument("Invalid render identity length");
        std::string value(bytes.constData() + offset, static_cast<std::size_t>(size));
        offset += static_cast<qsizetype>(size);
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
                   bool delta) {
    out.number(delta ? 0x3141544c45444351ULL : 0x3154454b43415051ULL);
    out.text(document.id.value);
    out.text(document.epoch.value);
    out.number(revision);
    out.text(view);
    out.number(view_revision);
}
inline void header(Reader& in,
                   DocumentRef& document,
                   Revision& revision,
                   std::string& view,
                   std::uint64_t& view_revision,
                   bool delta) {
    if (in.number() != (delta ? 0x3141544c45444351ULL : 0x3154454b43415051ULL))
        throw std::invalid_argument("Unsupported render resource encoding");
    document = {DocumentId(in.text()), DocumentEpoch(in.text())};
    revision = in.number();
    view = in.text();
    view_revision = in.number();
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

// Version 1: fixed little-endian integers/IEEE doubles and length-prefixed UTF-8 identities.
// This codec is shared by the engine and clients; it has no application/domain dependency.
inline QByteArray encode_render_packet(const RenderPacket& packet) {
    using namespace render_wire_detail;
    Writer out;
    header(
        out, packet.document, packet.revision, packet.view_session_id, packet.view_revision, false);
    out.number(packet.points.size());
    for (const auto& value : packet.points)
        point(out, value);
    out.number(packet.beams.size());
    for (const auto& beam : packet.beams) {
        out.text(beam.entity.value);
        out.number(beam.points[0]);
        out.number(beam.points[1]);
    }
    out.number(packet.geometry_lines.size());
    for (const auto& line : packet.geometry_lines)
        geometry(out, line);
    return out.bytes;
}
inline QByteArray encode_render_delta(const RenderDelta& delta) {
    using namespace render_wire_detail;
    Writer out;
    header(out, delta.document, delta.revision, delta.view_session_id, delta.view_revision, true);
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
    }
    return out.bytes;
}
inline Result<RenderPacket> decode_render_packet(const QByteArray& bytes) {
    using namespace render_wire_detail;
    try {
        Reader in{bytes};
        RenderPacket packet;
        header(in,
               packet.document,
               packet.revision,
               packet.view_session_id,
               packet.view_revision,
               false);
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
            packet.beams.push_back(
                {id, {static_cast<std::size_t>(a), static_cast<std::size_t>(b)}});
        }
        count = in.count();
        packet.geometry_lines.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            auto line = geometry(in);
            unique(line.entity);
            packet.geometry_lines.push_back(std::move(line));
        }
        in.finished();
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
        header(
            in, delta.document, delta.revision, delta.view_session_id, delta.view_revision, true);
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
            if (index >= max_items || !indices.insert(static_cast<std::size_t>(index)).second ||
                !ids.insert(line.entity.value).second)
                throw std::invalid_argument("Invalid duplicate geometry update");
            delta.geometry_lines.push_back({static_cast<std::size_t>(index), std::move(line)});
        }
        in.finished();
        return {Status::success, std::move(delta), std::nullopt};
    } catch (const std::exception& error) {
        return render_wire_detail::error<RenderDelta>(error);
    }
}
} // namespace qcae::transport
