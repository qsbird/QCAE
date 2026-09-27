#include "qcae/edit_session.hpp"
#include "qcae/records.hpp"
#include "qcae/render_projector.hpp"
#include <iostream>
#include <stdexcept>

namespace {
using namespace qcae;
void check(bool value, const char* text) {
    if (!value)
        throw std::runtime_error(text);
}
template <class T> T good(Result<T> result) {
    if (!result.ok())
        throw std::runtime_error(result.error->message);
    return std::move(*result.value);
}
void run() {
    const DocumentRef doc{DocumentId("doc"), DocumentEpoch("epoch")};
    DocumentView empty(make_record_registry(), {doc, 0});
    EditSession seed(empty);
    seed.put(records::Node{EntityId("a"), {0, 0, 0}, {}});
    seed.put(records::Node{EntityId("b"), {10, 0, 0}, {}});
    seed.put(records::GeometryLine{records::GeometryId("g"), {0, 1, 0}, {10, 1, 0}, 1});
    seed.put(records::Material{EntityId("m"), "Steel", 210000, .3});
    const auto initial = seed.prepare().candidate;
    ViewSession view{"view", doc, initial.version().revision, 1, {EntityId("b")}, {}};
    RenderProjector projector;
    auto full = good(projector.update(initial, view));
    check(full.full && full.full->points.size() == 1 && full.full->geometry_lines.size() == 1,
          "initial full with hidden point and geometry");
    EditSession edit(initial);
    edit.update<records::Node>(EntityId("a"), [](auto& node) { node.position[1] = 2; });
    auto prepared = edit.prepare();
    prepared.candidate = prepared.candidate.with_version({doc, 1});
    view.model_revision = prepared.candidate.version().revision;
    ++view.view_revision;
    DirectedRenderChange change{initial.version().revision, view.model_revision, &prepared.changes};
    auto delta = good(projector.update(prepared.candidate, view, {&change, 1}));
    check(!delta.full && delta.delta.points.size() == 1 &&
              delta.delta.points[0].point.position_mm[1] == 2,
          "local point delta");
    check(delta.work.projected_records == 2 && delta.work.full_rebuilds == 0 &&
              delta.work.model_bytes_copied < 128,
          "bounded projection work");
    auto missing = view;
    ++missing.model_revision;
    check(!projector.update(prepared.candidate, missing).ok(), "stale snapshot rejected");
    auto wrong = view;
    wrong.document.epoch = DocumentEpoch("other");
    check(!projector.update(prepared.candidate, wrong).ok(), "foreign epoch rejected");
    EditSession material(prepared.candidate);
    material.update<records::Material>(EntityId("m"),
                                       [](auto& value) { value.young_modulus_mpa = 200000; });
    auto material_change = material.prepare();
    material_change.candidate = material_change.candidate.with_version({doc, 2});
    change = {view.model_revision,
              material_change.candidate.version().revision,
              &material_change.changes};
    view.model_revision = change.revision;
    auto unchanged = good(projector.update(material_change.candidate, view, {&change, 1}));
    check(!unchanged.full && unchanged.delta.points.empty() && !unchanged.work.projected_records,
          "nonvisual change advances versions without model projection");
    view.hidden_ids.clear();
    ++view.view_revision;
    check(good(projector.update(material_change.candidate, view)).full.has_value(),
          "visibility change rebuilds");
    auto old = initial;
    auto wrong_view = view;
    wrong_view.model_revision = initial.version().revision;
    check(!projector.update(old, wrong_view).ok(), "revision rewind rejected");
    RenderContributions custom;
    custom.add(
        {RecordTraits<records::Material>::type_id, "point", [](const Record& record) -> RenderItem {
             const auto& m = record->get<records::Material>();
             return RenderPoint{m.id, {m.young_modulus_mpa, 0, 0}, true};
         }});
    custom.freeze();
    RenderProjector extension(std::move(custom));
    check(good(extension.update(material_change.candidate, view)).full->points.size() == 1,
          "independent registered render contribution works");
    check(record_activity_counters().whole_model_materializations == 0 &&
              record_activity_counters().whole_model_serializations == 0,
          "no legacy model bridge");
}
} // namespace
int main() {
    try {
        run();
        std::cout << "PASS: render projection, locality, versions and static contribution\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
