#include "qcae/model_delta.hpp"
#include <stdexcept>
using namespace qcae;
namespace {
void check(bool value) {
    if (!value)
        throw std::runtime_error("delta test failed");
}
} // namespace
int main() {
    Model complete;
    complete.materials.push_back({EntityId("m"), "Alloy", 70000.0, 0.33});
    complete.nodes.push_back({EntityId("n"), {1, 2, 3}});
    complete.sections.push_back({EntityId("s"), "section", EntityId("m"), 4, 5, 6, 7});
    complete.beams.push_back(
        {EntityId("b"), EntityId("s"), {EntityId("n"), EntityId("n2")}, {0, 1, 0}});
    complete.parts.push_back({EntityId("p"), "part", {EntityId("b")}});
    complete.assemblies.push_back({EntityId("a"), "assembly", {EntityId("p")}});
    complete.sets.push_back({EntityId("set"), "set", {EntityId("n")}});
    complete.includes.push_back({EntityId("inc"), "file.bdf", EntityId("parent"), {EntityId("n")}});
    complete.forces.push_back({EntityId("f"), EntityId("n"), {1, 2, 3}});
    complete.constraints.push_back({EntityId("c"), {EntityId("n")}, "123456"});
    complete.analyses.push_back({EntityId("analysis"),
                                 "case",
                                 {{"nastran", "1", "digest"}, "linear_static"},
                                 {EntityId("f")},
                                 {EntityId("c")}});
    complete.sources.push_back(
        {EntityId("n"), "source", EntityId("inc"), {"nastran", "1", "digest"}, "GRID", 42});
    const auto model_bytes = state_codec::encode_model(complete);
    check(state_codec::decode_model(model_bytes) == complete);
    bool rejected = false;
    try {
        state_codec::decode_model(model_bytes + "x");
    } catch (const state_codec::CodecError&) {
        rejected = true;
    }
    check(rejected);
    rejected = false;
    try {
        state_codec::decode_model(model_bytes.substr(0, model_bytes.size() - 1));
    } catch (const state_codec::CodecError&) {
        rejected = true;
    }
    check(rejected);
    rejected = false;
    try {
        auto future = model_bytes;
        future[18] = 2;
        state_codec::decode_model(future);
    } catch (const state_codec::CodecError&) {
        rejected = true;
    }
    check(rejected);
    Model changed = complete;
    changed.materials[0].name = "Copper";
    changed.nodes[0].position.x = 9;
    changed.sections[0].area_mm2 = 10;
    changed.beams[0].orientation.y = 2;
    changed.parts[0].name = "part2";
    changed.assemblies[0].name = "assembly2";
    changed.sets[0].name = "set2";
    changed.includes[0].path = "other.bdf";
    changed.forces[0].force_n.x = 4;
    changed.constraints[0].dofs = "123";
    changed.analyses[0].name = "case2";
    changed.sources[0].number = 43;
    auto full_delta = model_delta(complete, changed);
    check(full_delta.records.size() == 12);
    state_codec::Writer full_writer;
    write_model_delta(full_writer, full_delta);
    state_codec::Reader full_reader(full_writer.bytes());
    auto full_decoded = read_model_delta(full_reader);
    full_reader.finish();
    check(apply_model_delta(complete, full_decoded, true) == changed);
    check(apply_model_delta(changed, full_decoded, false) == complete);
    Model before;
    before.materials.push_back({EntityId("m1"), "Steel", 210000.0, 0.3});
    before.nodes.push_back({EntityId("n1"), {0, 0, 0}});
    before.parts.push_back({EntityId("p1"), "part", {EntityId("n1")}});
    Model after = before;
    after.materials[0].young_modulus_mpa = 200000.0;
    after.nodes.push_back({EntityId("n2"), {1, 2, 3}});
    after.parts[0].members.push_back(EntityId("n2"));
    auto delta = model_delta(before, after);
    check(delta.records.size() == 3);
    check(apply_model_delta(before, delta, true) == after);
    check(apply_model_delta(after, delta, false) == before);
    state_codec::Writer w;
    write_model_delta(w, delta);
    state_codec::Reader r(w.bytes());
    auto decoded = read_model_delta(r);
    r.finish();
    check(apply_model_delta(before, decoded, true) == after);
    after.nodes.erase(after.nodes.begin());
    auto removal = model_delta(before, after);
    check(apply_model_delta(before, removal, true) == after);
    check(apply_model_delta(after, removal, false) == before);
}
