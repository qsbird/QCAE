#include "qcae/model_delta.hpp"
#include <algorithm>

namespace qcae {
namespace {
template <class T> std::string encode_record(const T& value, std::uint64_t field) {
    Model model;
#define CASE(n, member)                                                                            \
    if (field == n) {                                                                              \
        model.member.push_back(value);                                                             \
    }
    if constexpr (std::is_same_v<T, Material>)
        model.materials.push_back(value);
    else if constexpr (std::is_same_v<T, Node>)
        model.nodes.push_back(value);
    else if constexpr (std::is_same_v<T, BeamSection>)
        model.sections.push_back(value);
    else if constexpr (std::is_same_v<T, Beam>)
        model.beams.push_back(value);
    else if constexpr (std::is_same_v<T, Part>)
        model.parts.push_back(value);
    else if constexpr (std::is_same_v<T, Assembly>)
        model.assemblies.push_back(value);
    else if constexpr (std::is_same_v<T, EntitySet>)
        model.sets.push_back(value);
    else if constexpr (std::is_same_v<T, IncludeDocument>)
        model.includes.push_back(value);
    else if constexpr (std::is_same_v<T, NodalForce>)
        model.forces.push_back(value);
    else if constexpr (std::is_same_v<T, Constraint>)
        model.constraints.push_back(value);
    else if constexpr (std::is_same_v<T, AnalysisDefinition>)
        model.analyses.push_back(value);
    else if constexpr (std::is_same_v<T, SourceIdentifier>)
        model.sources.push_back(value);
    (void)field;
    return state_codec::encode_model(model);
}
template <class T>
void diff(std::vector<ModelDeltaRecord>& records,
          std::uint64_t field,
          const std::vector<T>& before,
          const std::vector<T>& after) {
    const auto count = std::max(before.size(), after.size());
    for (std::size_t i = 0; i < count; ++i) {
        if (i < before.size() && i < after.size() && before[i] == after[i])
            continue;
        ModelDeltaRecord record;
        record.field = field;
        record.index = i;
        if (i < before.size())
            record.before = encode_record(before[i], field);
        if (i < after.size())
            record.after = encode_record(after[i], field);
        records.push_back(std::move(record));
    }
}
template <class T>
void patch(std::vector<T>& values,
           const ModelDeltaRecord& record,
           const std::optional<std::string>& from,
           const std::optional<std::string>& to,
           const std::vector<T> Model::* member) {
    const auto index = static_cast<std::size_t>(record.index);
    if (from) {
        const Model wrapped = state_codec::decode_model(*from);
        const auto& decoded = wrapped.*member;
        if (index >= values.size() || decoded.size() != 1 || decoded[0] != values[index])
            throw state_codec::CodecError("Delta base mismatch");
    }
    if (to) {
        Model wrapped = state_codec::decode_model(*to);
        const auto& decoded = wrapped.*member;
        if (decoded.size() != 1)
            throw state_codec::CodecError("Malformed delta record");
        if (from)
            values[index] = decoded[0];
        else if (index <= values.size())
            values.insert(values.begin() + index, decoded[0]);
        else
            throw state_codec::CodecError("Delta insertion index");
    } else {
        if (index >= values.size())
            throw state_codec::CodecError("Delta deletion index");
        values.erase(values.begin() + index);
    }
}
void apply_one(Model& model, const ModelDeltaRecord& record, bool forward) {
    const auto& from = forward ? record.before : record.after;
    const auto& to = forward ? record.after : record.before;
    switch (record.field) {
    case 0:
        patch(model.materials, record, from, to, &Model::materials);
        break;
    case 1:
        patch(model.nodes, record, from, to, &Model::nodes);
        break;
    case 2:
        patch(model.sections, record, from, to, &Model::sections);
        break;
    case 3:
        patch(model.beams, record, from, to, &Model::beams);
        break;
    case 4:
        patch(model.parts, record, from, to, &Model::parts);
        break;
    case 5:
        patch(model.assemblies, record, from, to, &Model::assemblies);
        break;
    case 6:
        patch(model.sets, record, from, to, &Model::sets);
        break;
    case 7:
        patch(model.includes, record, from, to, &Model::includes);
        break;
    case 8:
        patch(model.forces, record, from, to, &Model::forces);
        break;
    case 9:
        patch(model.constraints, record, from, to, &Model::constraints);
        break;
    case 10:
        patch(model.analyses, record, from, to, &Model::analyses);
        break;
    case 11:
        patch(model.sources, record, from, to, &Model::sources);
        break;
    default:
        throw state_codec::CodecError("Unknown delta field");
    }
}
} // namespace
ModelDelta model_delta(const Model& before, const Model& after) {
    ModelDelta result;
    diff(result.records, 0, before.materials, after.materials);
    diff(result.records, 1, before.nodes, after.nodes);
    diff(result.records, 2, before.sections, after.sections);
    diff(result.records, 3, before.beams, after.beams);
    diff(result.records, 4, before.parts, after.parts);
    diff(result.records, 5, before.assemblies, after.assemblies);
    diff(result.records, 6, before.sets, after.sets);
    diff(result.records, 7, before.includes, after.includes);
    diff(result.records, 8, before.forces, after.forces);
    diff(result.records, 9, before.constraints, after.constraints);
    diff(result.records, 10, before.analyses, after.analyses);
    diff(result.records, 11, before.sources, after.sources);
    return result;
}
Model apply_model_delta(const Model& model, const ModelDelta& delta, bool forward) {
    Model result = model;
    // Indexes refer to the source vector. Replace first, then remove from the
    // end and insert from the beginning so positions remain meaningful.
    for (const auto& record : delta.records)
        if (record.before && record.after)
            apply_one(result, record, forward);
    if (forward) {
        for (auto it = delta.records.rbegin(); it != delta.records.rend(); ++it)
            if (it->before && !it->after)
                apply_one(result, *it, true);
        for (const auto& record : delta.records)
            if (!record.before && record.after)
                apply_one(result, record, true);
    } else {
        for (auto it = delta.records.rbegin(); it != delta.records.rend(); ++it)
            if (!it->before && it->after)
                apply_one(result, *it, false);
        for (const auto& record : delta.records)
            if (record.before && !record.after)
                apply_one(result, record, false);
    }
    return result;
}
void write_model_delta(state_codec::Writer& w, const ModelDelta& delta) {
    w.number(delta.records.size());
    for (const auto& record : delta.records) {
        w.number(record.field);
        w.number(record.index);
        w.boolean(record.before.has_value());
        if (record.before)
            w.text(*record.before);
        w.boolean(record.after.has_value());
        if (record.after)
            w.text(*record.after);
    }
}
ModelDelta read_model_delta(state_codec::Reader& r) {
    const auto count = r.number();
    if (count > 500000 || count > r.remaining() / 32)
        throw state_codec::CodecError("Malformed delta count");
    ModelDelta result;
    result.records.reserve(count);
    for (std::uint64_t i = 0; i < count; ++i) {
        ModelDeltaRecord record;
        record.field = r.number();
        record.index = r.number();
        if (record.field > 11 || record.index > 500000)
            throw state_codec::CodecError("Malformed delta index");
        if (r.boolean())
            record.before = r.text();
        if (r.boolean())
            record.after = r.text();
        if (!record.before && !record.after)
            throw state_codec::CodecError("Empty delta record");
        result.records.push_back(std::move(record));
    }
    return result;
}
} // namespace qcae
