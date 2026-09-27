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
        RenderDelta delta{
            packet.document, 7, 8, "view", 3, 4, {{0, {EntityId("n1"), {0, 4, 0}, true}}}, {}};
        const auto encoded = transport::encode_render_delta(delta);
        const auto decoded_delta = transport::decode_render_delta(encoded);
        check(decoded_delta.ok() && transport::encode_render_delta(*decoded_delta.value) == encoded,
              "exact delta roundtrip");
        delta.points.push_back(delta.points[0]);
        check(!transport::decode_render_delta(transport::encode_render_delta(delta)).ok(),
              "duplicate delta accepted");
        delta.points.pop_back();
        delta.revision = 6;
        check(!transport::decode_render_delta(transport::encode_render_delta(delta)).ok(),
              "revision rewind accepted");
        check(!transport::decode_render_delta(bytes).ok(), "packet decoded as delta");
        std::cout << "PASS: binary display resources roundtrip and malformed rejection\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
