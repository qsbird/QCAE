#include "qcae/render_wire.hpp"
#include <iostream>
#include <stdexcept>

int main() {
    using namespace qcae;
    const auto check = [](bool value, const char* text) {
        if (!value)
            throw std::runtime_error(text);
    };
    try {
        RenderPacket packet;
        packet.document = {DocumentId("document"), DocumentEpoch("epoch")};
        packet.revision = 7;
        packet.view_session_id = "view";
        packet.view_revision = 3;
        packet.points = {{EntityId("n1"), {0, 0, 0}, true}, {EntityId("n2"), {1, 2, 3}, false}};
        packet.beams = {{EntityId("b"), {0, 1}}};
        packet.geometry_lines = {{EntityId("g"), {1, 0, 0}, {2, 0, 0}}};
        const auto bytes = transport::encode_render_packet(packet);
        const auto decoded = transport::decode_render_packet(bytes);
        check(decoded.ok() && transport::encode_render_packet(*decoded.value) == bytes,
              "exact packet roundtrip");
        for (qsizetype size = 0; size < bytes.size(); ++size)
            check(!transport::decode_render_packet(bytes.left(size)).ok(),
                  "truncated packet accepted");
        check(!transport::decode_render_packet(bytes + 'x').ok(), "trailing bytes accepted");
        packet.beams[0].points[1] = 2;
        check(!transport::decode_render_packet(transport::encode_render_packet(packet)).ok(),
              "bad topology accepted");
        packet.beams[0].points[1] = 1;
        packet.geometry_lines[0].entity = EntityId("n1");
        check(!transport::decode_render_packet(transport::encode_render_packet(packet)).ok(),
              "cross-kind identity alias accepted");
        packet.geometry_lines[0].entity = EntityId("g");
        packet.geometry_lines[0].visible = false;
        packet.beams[0].visible = false;
        const auto hidden_bytes = transport::encode_render_packet(packet);
        const auto hidden_packet = transport::decode_render_packet(hidden_bytes);
        check(transport::render_resource_version(hidden_bytes) == 2 && hidden_packet.ok() &&
                  !hidden_packet.value->beams[0].visible &&
                  !hidden_packet.value->geometry_lines[0].visible,
              "version 2 packet preserves hidden line coordinates");
        auto future_packet = hidden_bytes;
        future_packet[7] = '4';
        check(!transport::decode_render_packet(future_packet).ok(), "future encoding accepted");
        RenderDelta delta{
            packet.document, 7, 8, "view", 3, 4, {{0, {EntityId("n1"), {0, 4, 0}, true}}}, {}};
        const auto encoded = transport::encode_render_delta(delta);
        const auto decoded_delta = transport::decode_render_delta(encoded);
        check(decoded_delta.ok() && transport::encode_render_delta(*decoded_delta.value) == encoded,
              "exact delta roundtrip");
        delta.visibility = {{RenderPrimitive::point, 1, EntityId("n2"), true},
                            {RenderPrimitive::beam, 0, EntityId("b"), false},
                            {RenderPrimitive::geometry_line, 0, EntityId("g"), false}};
        const auto visibility_bytes = transport::encode_render_delta(delta);
        const auto visible_delta = transport::decode_render_delta(visibility_bytes);
        check(transport::render_resource_version(visibility_bytes) == 2 && visible_delta.ok() &&
                  visible_delta.value->visibility.size() == 3 &&
                  transport::encode_render_delta(*visible_delta.value) == visibility_bytes,
              "version 2 visibility delta roundtrip");
        delta.visibility.push_back(delta.visibility.front());
        check(!transport::decode_render_delta(transport::encode_render_delta(delta)).ok(),
              "duplicate visibility accepted");
        delta.visibility.pop_back();
        delta.visibility[0].primitive = static_cast<RenderPrimitive>(99);
        check(!transport::decode_render_delta(transport::encode_render_delta(delta)).ok(),
              "unknown primitive accepted");
        delta.visibility.clear();
        delta.points.push_back(delta.points[0]);
        check(!transport::decode_render_delta(transport::encode_render_delta(delta)).ok(),
              "duplicate delta accepted");
        delta.points.pop_back();
        delta.revision = 6;
        check(!transport::decode_render_delta(transport::encode_render_delta(delta)).ok(),
              "revision rewind accepted");
        check(!transport::decode_render_delta(bytes).ok(), "packet decoded as delta");
        auto generic = packet;
        generic.points.push_back({EntityId("n3"), {2, 0, 0}, true});
        generic.points.push_back({EntityId("n4"), {2, 2, 0}, true});
        generic.points.push_back({EntityId("n5"), {0, 2, 0}, true});
        generic.cells = {{EntityId("area"), RenderCellKind::polygon, {0, 2, 3, 4}, true},
                         {EntityId("path"), RenderCellKind::polyline, {0, 1, 2, 3, 4}, false}};
        const auto generic_bytes = transport::encode_render_packet(generic);
        const auto generic_result = transport::decode_render_packet(generic_bytes);
        check(transport::render_resource_version(generic_bytes) == 3 && generic_result.ok() &&
                  generic_result.value->cells[0].points.size() == 4 &&
                  generic_result.value->cells[1].points.size() == 5 &&
                  !generic_result.value->cells[1].visible &&
                  transport::encode_render_packet(*generic_result.value) == generic_bytes,
              "generic variable-arity cell roundtrip");
        for (qsizetype size = 0; size < generic_bytes.size(); ++size)
            check(!transport::decode_render_packet(generic_bytes.left(size)).ok(),
                  "truncated generic packet accepted");
        for (const auto& bad_points : {std::vector<std::size_t>{0, 1},
                                       std::vector<std::size_t>{0, 1, 1, 3},
                                       std::vector<std::size_t>{0, 1, 2, 999}}) {
            generic.cells[0].points = bad_points;
            check(!transport::decode_render_packet(transport::encode_render_packet(generic)).ok(),
                  "invalid generic topology accepted");
        }
        generic.cells[0].points = {0, 2, 3, 4};
        generic.cells[0].kind = static_cast<RenderCellKind>(9);
        check(!transport::decode_render_packet(transport::encode_render_packet(generic)).ok(),
              "unknown generic topology accepted");
        generic.cells[0].kind = RenderCellKind::polygon;
        generic.cells[0].entity = EntityId("n1");
        check(!transport::decode_render_packet(transport::encode_render_packet(generic)).ok(),
              "generic cross-kind identity alias accepted");
        RenderDelta cell_delta;
        cell_delta.document = packet.document;
        cell_delta.view_session_id = "view";
        cell_delta.base_revision = cell_delta.revision = 7;
        cell_delta.base_view_revision = 3;
        cell_delta.view_revision = 4;
        cell_delta.visibility = {{RenderPrimitive::cell, 0, EntityId("area"), false}};
        const auto cell_bytes = transport::encode_render_delta(cell_delta);
        check(transport::render_resource_version(cell_bytes) == 3 &&
                  transport::decode_render_delta(cell_bytes).ok(),
              "generic cell visibility delta");
        {
            auto measured = std::make_shared<ledger::OperationLedger>(
                ledger::Identity{"wire-accounting", "document", "epoch", 7});
            ledger::Scope scope(measured);
            RenderDelta empty;
            empty.document = packet.document;
            empty.revision = empty.base_revision = 7;
            empty.view_session_id = "view";
            empty.view_revision = empty.base_view_revision = 3;
            const auto encoded = transport::encode_render_delta(empty);
            check(transport::decode_render_delta(encoded).ok(), "empty measured delta failed");
            const auto snapshot = measured->snapshot();
            const auto encode = static_cast<std::size_t>(ledger::Stage::render_encode);
            const auto decode = static_cast<std::size_t>(ledger::Stage::render_decode);
            const auto model = static_cast<std::size_t>(ledger::Metric::model_copy_bytes);
            const auto metadata = static_cast<std::size_t>(ledger::Metric::metadata_copy_bytes);
            check(snapshot.covered[encode] && snapshot.values[encode][metadata] == 0 &&
                      snapshot.values[encode][model] >= static_cast<std::uint64_t>(encoded.size()),
                  "encoded payload had an unmeasured or duplicated metadata copy");
            check(snapshot.covered[decode] && snapshot.values[decode][model] == 0 &&
                      snapshot.values[decode][metadata] > 0,
                  "document/view identity copies were charged twice or as model records");
        }
        std::cout << "PASS: binary display resources roundtrip and malformed rejection\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
