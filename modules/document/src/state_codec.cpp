#include "qcae/state_codec.hpp"
#include <bit>
#include <cmath>
#include <limits>

namespace qcae::state_codec {
void Writer::number(std::uint64_t value) {
    if (bytes_.size() > max_bytes - 8)
        throw CodecError("State exceeds 64 MiB");
    for (unsigned i = 0; i < 8; ++i)
        bytes_.push_back(static_cast<char>(value >> (i * 8)));
}
void Writer::real(double value) {
    if (!std::isfinite(value))
        throw CodecError("Nonfinite numeric state");
    number(std::bit_cast<std::uint64_t>(value));
}
void Writer::text(std::string_view value) {
    if (value.size() > max_bytes || bytes_.size() > max_bytes - 8 - value.size())
        throw CodecError("State exceeds 64 MiB");
    number(value.size());
    bytes_.append(value);
}
void Writer::boolean(bool value) {
    number(value ? 1 : 0);
}
Reader::Reader(std::string_view bytes) : bytes_(bytes) {
    if (bytes.size() > max_bytes)
        throw CodecError("State exceeds 64 MiB");
}
std::uint64_t Reader::number() {
    if (remaining() < 8)
        throw CodecError("Truncated state");
    std::uint64_t result{};
    for (unsigned i = 0; i < 8; ++i)
        result |= std::uint64_t(static_cast<unsigned char>(bytes_[offset_++])) << (i * 8);
    return result;
}
double Reader::real() {
    const double result = std::bit_cast<double>(number());
    if (!std::isfinite(result))
        throw CodecError("Nonfinite numeric state");
    return result;
}
std::string Reader::text() {
    const auto size = number();
    if (size > remaining())
        throw CodecError("Truncated string");
    std::string result(bytes_.substr(offset_, static_cast<std::size_t>(size)));
    offset_ += static_cast<std::size_t>(size);
    return result;
}
bool Reader::boolean() {
    const auto value = number();
    if (value > 1)
        throw CodecError("Malformed boolean");
    return value == 1;
}
void Reader::finish() const {
    if (remaining())
        throw CodecError("Trailing state bytes");
}
namespace {
template <class Tag> void put(Writer& w, const Id<Tag>& id) {
    w.text(id.value);
}
template <class Tag> Id<Tag> get_id(Reader& r) {
    return Id<Tag>(r.text());
}
template <class T, class F> void put_vec(Writer& w, const std::vector<T>& values, F write) {
    w.number(values.size());
    for (const auto& value : values)
        write(w, value);
}
template <class T, class F>
std::vector<T> get_vec(Reader& r, F read, std::size_t max_count = 500000) {
    const auto count = r.number();
    if (count > r.remaining() / 8 || count > max_count)
        throw CodecError("Malformed vector count");
    std::vector<T> result;
    result.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t i = 0; i < count; ++i)
        result.push_back(read(r));
    return result;
}
void put_ids(Writer& w, const std::vector<EntityId>& ids) {
    put_vec(w, ids, [](Writer& x, const EntityId& id) { put(x, id); });
}
std::vector<EntityId> get_ids(Reader& r) {
    return get_vec<EntityId>(r, [](Reader& x) { return get_id<EntityTag>(x); });
}
void put_vec3(Writer& w, Vec3 v) {
    w.real(v.x);
    w.real(v.y);
    w.real(v.z);
}
Vec3 get_vec3(Reader& r) {
    return {r.real(), r.real(), r.real()};
}
void put_profile(Writer& w, const ProfileRef& p) {
    w.text(p.profile_id);
    w.text(p.profile_version);
    w.text(p.definition_digest);
}
ProfileRef get_profile(Reader& r) {
    return {r.text(), r.text(), r.text()};
}
void put_target(Writer& w, const TargetBinding& t) {
    put_profile(w, t.profile);
    w.text(t.analysis_kind);
}
TargetBinding get_target(Reader& r) {
    return {get_profile(r), r.text()};
}
void put_record(Writer& w, const Material& v) {
    put(w, v.id);
    w.text(v.name);
    w.real(v.young_modulus_mpa);
    w.boolean(v.poisson_ratio.has_value());
    if (v.poisson_ratio)
        w.real(*v.poisson_ratio);
}
Material get_material(Reader& r) {
    Material v;
    v.id = get_id<EntityTag>(r);
    v.name = r.text();
    v.young_modulus_mpa = r.real();
    if (r.boolean())
        v.poisson_ratio = r.real();
    return v;
}
void put_record(Writer& w, const Node& v) {
    put(w, v.id);
    put_vec3(w, v.position);
}
Node get_node(Reader& r) {
    return {get_id<EntityTag>(r), get_vec3(r)};
}
void put_record(Writer& w, const BeamSection& v) {
    put(w, v.id);
    w.text(v.name);
    put(w, v.material);
    w.real(v.area_mm2);
    w.real(v.i1_mm4);
    w.real(v.i2_mm4);
    w.real(v.torsion_mm4);
}
BeamSection get_section(Reader& r) {
    BeamSection v;
    v.id = get_id<EntityTag>(r);
    v.name = r.text();
    v.material = get_id<EntityTag>(r);
    v.area_mm2 = r.real();
    v.i1_mm4 = r.real();
    v.i2_mm4 = r.real();
    v.torsion_mm4 = r.real();
    return v;
}
void put_record(Writer& w, const Beam& v) {
    put(w, v.id);
    put(w, v.section);
    put(w, v.nodes[0]);
    put(w, v.nodes[1]);
    put_vec3(w, v.orientation);
}
Beam get_beam(Reader& r) {
    Beam v;
    v.id = get_id<EntityTag>(r);
    v.section = get_id<EntityTag>(r);
    v.nodes = {get_id<EntityTag>(r), get_id<EntityTag>(r)};
    v.orientation = get_vec3(r);
    return v;
}
void put_record(Writer& w, const Part& v) {
    put(w, v.id);
    w.text(v.name);
    put_ids(w, v.members);
}
Part get_part(Reader& r) {
    return {get_id<EntityTag>(r), r.text(), get_ids(r)};
}
void put_record(Writer& w, const Assembly& v) {
    put(w, v.id);
    w.text(v.name);
    put_ids(w, v.children);
}
Assembly get_assembly(Reader& r) {
    return {get_id<EntityTag>(r), r.text(), get_ids(r)};
}
void put_record(Writer& w, const EntitySet& v) {
    put(w, v.id);
    w.text(v.name);
    put_ids(w, v.members);
}
EntitySet get_set(Reader& r) {
    return {get_id<EntityTag>(r), r.text(), get_ids(r)};
}
void put_record(Writer& w, const IncludeDocument& v) {
    put(w, v.id);
    w.text(v.path);
    w.boolean(v.parent.has_value());
    if (v.parent)
        put(w, *v.parent);
    put_ids(w, v.members);
}
IncludeDocument get_include(Reader& r) {
    IncludeDocument v;
    v.id = get_id<EntityTag>(r);
    v.path = r.text();
    if (r.boolean())
        v.parent = get_id<EntityTag>(r);
    v.members = get_ids(r);
    return v;
}
void put_record(Writer& w, const NodalForce& v) {
    put(w, v.id);
    put(w, v.node);
    put_vec3(w, v.force_n);
}
NodalForce get_force(Reader& r) {
    return {get_id<EntityTag>(r), get_id<EntityTag>(r), get_vec3(r)};
}
void put_record(Writer& w, const Constraint& v) {
    put(w, v.id);
    put_ids(w, v.nodes);
    w.text(v.dofs);
}
Constraint get_constraint(Reader& r) {
    return {get_id<EntityTag>(r), get_ids(r), r.text()};
}
void put_record(Writer& w, const AnalysisDefinition& v) {
    put(w, v.id);
    w.text(v.name);
    put_target(w, v.target);
    put_ids(w, v.forces);
    put_ids(w, v.constraints);
}
AnalysisDefinition get_analysis(Reader& r) {
    return {get_id<EntityTag>(r), r.text(), get_target(r), get_ids(r), get_ids(r)};
}
void put_record(Writer& w, const SourceIdentifier& v) {
    put(w, v.entity);
    w.text(v.source_model_id);
    put(w, v.include);
    put_profile(w, v.profile);
    w.text(v.name_space);
    w.number(v.number);
}
SourceIdentifier get_source(Reader& r) {
    return {
        get_id<EntityTag>(r), r.text(), get_id<EntityTag>(r), get_profile(r), r.text(), r.number()};
}
} // namespace
void write_model(Writer& w, const Model& model) {
    put_vec(w, model.materials, [](Writer& x, const Material& v) { put_record(x, v); });
    put_vec(w, model.nodes, [](Writer& x, const Node& v) { put_record(x, v); });
    put_vec(w, model.sections, [](Writer& x, const BeamSection& v) { put_record(x, v); });
    put_vec(w, model.beams, [](Writer& x, const Beam& v) { put_record(x, v); });
    put_vec(w, model.parts, [](Writer& x, const Part& v) { put_record(x, v); });
    put_vec(w, model.assemblies, [](Writer& x, const Assembly& v) { put_record(x, v); });
    put_vec(w, model.sets, [](Writer& x, const EntitySet& v) { put_record(x, v); });
    put_vec(w, model.includes, [](Writer& x, const IncludeDocument& v) { put_record(x, v); });
    put_vec(w, model.forces, [](Writer& x, const NodalForce& v) { put_record(x, v); });
    put_vec(w, model.constraints, [](Writer& x, const Constraint& v) { put_record(x, v); });
    put_vec(w, model.analyses, [](Writer& x, const AnalysisDefinition& v) { put_record(x, v); });
    put_vec(w, model.sources, [](Writer& x, const SourceIdentifier& v) { put_record(x, v); });
}
Model read_model(Reader& r, std::size_t max_entities, std::size_t max_sources) {
    Model model;
    auto read_entities = [&]<class T>(std::vector<T>& target, auto decoder) {
        target = get_vec<T>(r, decoder, max_entities);
        max_entities -= target.size();
    };
    read_entities(model.materials, get_material);
    read_entities(model.nodes, get_node);
    read_entities(model.sections, get_section);
    read_entities(model.beams, get_beam);
    read_entities(model.parts, get_part);
    read_entities(model.assemblies, get_assembly);
    read_entities(model.sets, get_set);
    read_entities(model.includes, get_include);
    read_entities(model.forces, get_force);
    read_entities(model.constraints, get_constraint);
    read_entities(model.analyses, get_analysis);
    model.sources = get_vec<SourceIdentifier>(r, get_source, max_sources);
    return model;
}
std::string encode_model(const Model& model) {
    Writer w;
    w.text("QCAE-MODEL");
    w.number(1);
    write_model(w, model);
    return w.take();
}
Model decode_model(std::string_view bytes) {
    Reader r(bytes);
    if (r.text() != "QCAE-MODEL" || r.number() != 1)
        throw CodecError("Unsupported model schema");
    Model model = read_model(r);
    r.finish();
    return model;
}
} // namespace qcae::state_codec
